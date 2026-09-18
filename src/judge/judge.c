#include "internal.h"
#include "forge/judge.h"
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#define JUDGE_PATH "/v1/systemone"
#define JUDGE_MAX_URL 2048
#define JUDGE_MAX_KEY 512
#define JUDGE_RETRY_BACKOFF_MS 500u

/* Frozen rerank question text. One narrow judgment per candidate; the bad case
 * is described explicitly so a merely topically-related excerpt is not scored
 * as a direct answer. Changing these strings changes the instrument and must
 * be recorded in any campaign identity that tunes thresholds against them. */
static const char *const rerank_instruction =
    "Does the excerpt in `candidates[%zu].snippet` contain the definition, implementation, or "
    "explanation that best answers the query in `state.query`?";
static const char *const rerank_true =
    "The excerpt contains the specific code, definition, or explanation the query asks for.";
static const char *const rerank_false =
    "The excerpt is unrelated to the query, only shares incidental words, or is clearly less "
    "useful than a direct answer.";

struct forge_judge {
    forge_judge_options options;
    char *endpoint, *model, *key_env, *record_dir; /* Owned copies; NULL record_dir disables. */
    size_t timeout_ms, max_candidates;
    size_t calls, failures, candidates_scored, input_tokens, output_tokens;
    size_t last_input_tokens, last_output_tokens;
    double last_latency_ms, total_latency_ms;
    char last_model[64];
    unsigned record_seq;
    /* Server-issued reconciliation headers from the most recent response;
     * empty when the transport cannot provide them (stub, failures). */
    char record_request_id[80], record_response_date[48];
#ifdef _WIN32
    HINTERNET session; /* Lazily opened; NULL until the first call. */
#endif
};

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static void utc_stamp(char out[24]) {
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    if (!g || !strftime(out, 24, "%Y%m%dT%H%M%SZ", g))
        snprintf(out, 24, "%s", "unknown-utc");
}

static char *copy_or_default(const char *value, const char *fallback) {
    const char *source = value && *value ? value : fallback;
    return source ? fg_strdup(source) : NULL;
}

forge_judge *forge_judge_create(const forge_judge_options *o, forge_error *e) {
    if (!o) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Judge options are required");
        return NULL;
    }
    size_t timeout = o->timeout_ms ? o->timeout_ms : FORGE_JUDGE_DEFAULT_TIMEOUT_MS;
    size_t cap = o->max_candidates ? o->max_candidates : FORGE_JUDGE_DEFAULT_MAX_CANDIDATES;
    if (timeout < FORGE_JUDGE_MIN_TIMEOUT_MS || timeout > FORGE_JUDGE_MAX_TIMEOUT_MS) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Judge timeout must be %zu..%zu ms",
                 FORGE_JUDGE_MIN_TIMEOUT_MS, FORGE_JUDGE_MAX_TIMEOUT_MS);
        return NULL;
    }
    if (!cap || cap > FORGE_JUDGE_MAX_CANDIDATES) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Judge candidate cap must be 1..%zu",
                 FORGE_JUDGE_MAX_CANDIDATES);
        return NULL;
    }
    forge_judge *j = calloc(1, sizeof(*j));
    if (!j) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate judge");
        return NULL;
    }
    j->options = *o;
    j->timeout_ms = timeout;
    j->max_candidates = cap;
    j->endpoint = copy_or_default(o->endpoint, FORGE_JUDGE_DEFAULT_ENDPOINT);
    j->model = copy_or_default(o->model, FORGE_JUDGE_DEFAULT_MODEL);
    j->key_env = copy_or_default(o->api_key_env, FORGE_JUDGE_DEFAULT_KEY_ENV);
    j->record_dir = o->record_dir && *o->record_dir ? fg_strdup(o->record_dir) : NULL;
    if (!j->endpoint || !j->model || !j->key_env || (o->record_dir && *o->record_dir && !j->record_dir)) {
        forge_judge_destroy(j);
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate judge configuration");
        return NULL;
    }
    /* Normalize the endpoint: strip trailing slashes and require an explicit
     * http(s) scheme so a typo cannot silently produce a different origin. */
    size_t n = strlen(j->endpoint);
    while (n && j->endpoint[n - 1] == '/')
        j->endpoint[--n] = 0;
    if (strncmp(j->endpoint, "https://", 8) && strncmp(j->endpoint, "http://", 7)) {
        forge_judge_destroy(j);
        fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint must start with https:// or http://");
        return NULL;
    }
    j->options.endpoint = j->endpoint;
    j->options.model = j->model;
    j->options.api_key_env = j->key_env;
    j->options.record_dir = j->record_dir;
    return j;
}

void forge_judge_destroy(forge_judge *j) {
    if (!j)
        return;
#ifdef _WIN32
    if (j->session)
        WinHttpCloseHandle(j->session);
#endif
    free(j->endpoint);
    free(j->model);
    free(j->key_env);
    free(j->record_dir);
    free(j);
}

size_t forge_judge_budget_ms(const forge_judge *j) {
    return j ? 2 * j->timeout_ms + JUDGE_RETRY_BACKOFF_MS : 0;
}

void forge_judge_metrics(const forge_judge *j, forge_judge_stats *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!j)
        return;
    out->calls = j->calls;
    out->failures = j->failures;
    out->candidates_scored = j->candidates_scored;
    out->input_tokens = j->input_tokens;
    out->output_tokens = j->output_tokens;
    out->last_latency_ms = j->last_latency_ms;
    out->total_latency_ms = j->total_latency_ms;
    snprintf(out->last_model, sizeof(out->last_model), "%s", j->last_model);
}

/* Build the batched request: {state:{query,candidates[]}, model, questions}.
 * One Noul per candidate, keyed c0..cN; keys never reach the model. */
static char *build_request(const forge_judge *j, const char *query, size_t count,
                           const char *const *paths, const char *const *snippets,
                           const char *const *stages, forge_error *e) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = root != NULL;
    if (ok) {
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_val *state = yyjson_mut_obj(doc), *cands = yyjson_mut_arr(doc),
                       *questions = yyjson_mut_obj(doc);
        ok = state && cands && questions && yyjson_mut_obj_add_str(doc, state, "query", query) &&
             yyjson_mut_obj_add_val(doc, root, "state", state) &&
             yyjson_mut_obj_add_str(doc, root, "model", j->model) &&
             yyjson_mut_obj_add_val(doc, root, "questions", questions);
        for (size_t i = 0; ok && i < count; i++) {
            yyjson_mut_val *c = yyjson_mut_obj(doc);
            ok = c && yyjson_mut_obj_add_str(doc, c, "path", paths[i] ? paths[i] : "") &&
                 yyjson_mut_obj_add_str(doc, c, "stage", stages[i] ? stages[i] : "") &&
                 yyjson_mut_obj_add_str(doc, c, "snippet", snippets[i] ? snippets[i] : "");
            ok = ok && yyjson_mut_arr_add_val(cands, c);
            char key[24], instr[512];
            snprintf(key, sizeof(key), "c%zu", i);
            snprintf(instr, sizeof(instr), rerank_instruction, i);
            /* Keys and the formatted instruction live on this stack frame; the
             * doc must own copies (add_val/add_str reference, *_strcpy copy). */
            yyjson_mut_val *key_val = yyjson_mut_strcpy(doc, key);
            yyjson_mut_val *criteria = yyjson_mut_obj(doc), *question = yyjson_mut_obj(doc);
            ok = ok && criteria && question && key_val &&
                 yyjson_mut_obj_add_str(doc, criteria, "true", rerank_true) &&
                 yyjson_mut_obj_add_str(doc, criteria, "false", rerank_false) &&
                 yyjson_mut_obj_add_str(doc, question, "type", "noul") &&
                 yyjson_mut_obj_add_strcpy(doc, question, "instructions", instr) &&
                 yyjson_mut_obj_add_val(doc, question, "criteria", criteria) &&
                 yyjson_mut_obj_add(questions, key_val, question);
        }
        ok = ok && yyjson_mut_obj_add_val(doc, state, "candidates", cands);
    }
    char *json = NULL;
    if (ok) {
        size_t len = 0;
        json = yyjson_mut_write(doc, 0, &len);
        if (json && len > FG_MAX_JSON) {
            free(json);
            json = NULL;
            fg_error(e, FORGE_ERR_LIMIT, "Judge request exceeds the maximum body size");
        }
    }
    if (!ok && !(e && e->code))
        fg_error(e, FORGE_ERR_MEMORY, "Cannot build judge request");
    if (doc)
        yyjson_mut_doc_free(doc);
    return json;
}

/* Parse {model, answers:{c0:{noul:..},..}, usage:{..}}; all-or-nothing. */
static forge_status parse_response(forge_judge *j, const char *body, size_t len, size_t count,
                                   double *scores, forge_error *e) {
    yyjson_doc *doc = yyjson_read(body, len, 0);
    if (!doc)
        return fg_error(e, FORGE_ERR_PARSE, "Judge response is not valid JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *model = root ? yyjson_obj_get(root, "model") : NULL;
    yyjson_val *answers = root ? yyjson_obj_get(root, "answers") : NULL;
    forge_status status = FORGE_OK;
    if (!answers || !yyjson_is_obj(answers)) {
        status = fg_error(e, FORGE_ERR_PARSE, "Judge response is missing answers");
    }
    for (size_t i = 0; status == FORGE_OK && i < count; i++) {
        char key[24];
        snprintf(key, sizeof(key), "c%zu", i);
        yyjson_val *answer = yyjson_obj_get(answers, key);
        yyjson_val *noul = answer ? yyjson_obj_get(answer, "noul") : NULL;
        if (!noul || !yyjson_is_num(noul)) {
            status = fg_error(e, FORGE_ERR_PARSE, "Judge response is missing answer %s", key);
            break;
        }
        double v = yyjson_get_num(noul);
        if (!(v >= 0.0 && v <= 1.0)) {
            status = fg_error(e, FORGE_ERR_PARSE, "Judge answer %s is outside [0,1]", key);
            break;
        }
        scores[i] = v;
    }
    if (status == FORGE_OK) {
        if (model && yyjson_is_str(model))
            snprintf(j->last_model, sizeof(j->last_model), "%s", yyjson_get_str(model));
        yyjson_val *usage = yyjson_obj_get(root, "usage");
        yyjson_val *in = usage ? yyjson_obj_get(usage, "input_tokens") : NULL;
        yyjson_val *out = usage ? yyjson_obj_get(usage, "output_tokens") : NULL;
        j->last_input_tokens = in && yyjson_is_uint(in) ? (size_t)yyjson_get_uint(in) : 0;
        j->last_output_tokens = out && yyjson_is_uint(out) ? (size_t)yyjson_get_uint(out) : 0;
        j->input_tokens += j->last_input_tokens;
        j->output_tokens += j->last_output_tokens;
    }
    yyjson_doc_free(doc);
    return status;
}

/* Best-effort raw recording: never fails the call it documents. */
static void record_run(forge_judge *j, const char *request, const char *response,
                       size_t response_len, forge_status status, double latency_ms,
                       const char *error_message) {
    if (!j->record_dir)
        return;
    forge_error ignored = {0};
    (void)fg_mkdir(j->record_dir, &ignored);
    char utc[24], path[FG_PATH_MAX];
    utc_stamp(utc);
    snprintf(path, sizeof(path), "%s/judge-%s-%04u.json", j->record_dir, utc, j->record_seq);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    bool ok = root != NULL;
    if (ok) {
        yyjson_mut_doc_set_root(doc, root);
        ok = yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
             yyjson_mut_obj_add_str(doc, root, "utc", utc) &&
             yyjson_mut_obj_add_real(doc, root, "latency_ms", latency_ms) &&
             yyjson_mut_obj_add_str(doc, root, "status", status == FORGE_OK ? "ok" : "error") &&
             yyjson_mut_obj_add_str(doc, root, "error", error_message ? error_message : "") &&
             yyjson_mut_obj_add_str(doc, root, "request_id", j->record_request_id) &&
             yyjson_mut_obj_add_str(doc, root, "response_date", j->record_response_date);
        yyjson_doc *request_doc = ok ? yyjson_read(request, strlen(request), 0) : NULL;
        yyjson_val *request_val = request_doc ? yyjson_doc_get_root(request_doc) : NULL;
        yyjson_mut_val *request_copy =
            ok && request_val ? yyjson_val_mut_copy(doc, request_val) : NULL;
        if (ok && request_copy)
            ok = yyjson_mut_obj_add_val(doc, root, "request", request_copy);
        else if (ok)
            ok = yyjson_mut_obj_add_str(doc, root, "request", request ? request : "");
        yyjson_doc *response_doc = ok && response ? yyjson_read(response, response_len, 0) : NULL;
        yyjson_val *response_val = response_doc ? yyjson_doc_get_root(response_doc) : NULL;
        yyjson_mut_val *response_copy =
            ok && response_val ? yyjson_val_mut_copy(doc, response_val) : NULL;
        if (ok && response_copy)
            ok = yyjson_mut_obj_add_val(doc, root, "response", response_copy);
        else if (ok)
            ok = yyjson_mut_obj_add_str(doc, root, "response", response ? response : "");
        if (request_doc)
            yyjson_doc_free(request_doc);
        if (response_doc)
            yyjson_doc_free(response_doc);
    }
    if (ok) {
        size_t len = 0;
        char *json = yyjson_mut_write(doc, 1, &len);
        if (json) {
            (void)fg_write_file(path, json, len, &ignored);
            free(json);
        }
    }
    if (doc)
        yyjson_mut_doc_free(doc);
    j->record_seq++;
}

#ifdef _WIN32
/* Best-effort response-header capture for the raw records: the server-issued
 * request id makes every call reconcilable against the provider's console.
 * Absent headers leave empty strings; this never fails the call. */
static void capture_response_headers(forge_judge *j, HINTERNET req) {
    wchar_t value[128];
    j->record_request_id[0] = 0;
    j->record_response_date[0] = 0;
    DWORD size = sizeof(value);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, L"x-typesafe-request-id", value, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, j->record_request_id,
                                    (int)sizeof(j->record_request_id), NULL, NULL);
        if (n <= 0)
            j->record_request_id[0] = 0;
    }
    size = sizeof(value);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE, WINHTTP_HEADER_NAME_BY_INDEX, value, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, j->record_response_date,
                                    (int)sizeof(j->record_response_date), NULL, NULL);
        if (n <= 0)
            j->record_response_date[0] = 0;
    }
}

/* HTTPS JSON POST via WinHTTP (Schannel TLS, system certificate store, no new
 * dependency). Returns IO for transient failures (network errors, 408/429/529
 * and 5xx) so the caller can retry once; POLICY/PARSE/ARGUMENT are final. */
static forge_status http_transport(forge_judge *j, const char *request, size_t request_len,
                                   char **response, size_t *response_len, forge_error *e) {
    const char *key = getenv(j->key_env);
    if (!key || !*key)
        return fg_error(e, FORGE_ERR_POLICY, "Judge API key is not set in %s", j->key_env);
    if (strlen(key) > JUDGE_MAX_KEY)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge API key is unreasonably long");
    if (request_len > FG_MAX_JSON)
        return fg_error(e, FORGE_ERR_LIMIT, "Judge request exceeds the maximum body size");
    char url[JUDGE_MAX_URL];
    int n = snprintf(url, sizeof(url), "%s%s", j->endpoint, JUDGE_PATH);
    if (n < 0 || (size_t)n >= sizeof(url))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint URL is too long");
    wchar_t wurl[JUDGE_MAX_URL], wkey[JUDGE_MAX_KEY + 1];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, JUDGE_MAX_URL))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint is not valid UTF-8");
    if (!MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, JUDGE_MAX_KEY + 1))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge API key is not valid UTF-8");
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint could not be parsed");
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && uc.nScheme != INTERNET_SCHEME_HTTP)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint must use http or https");
    if (!uc.dwHostNameLength || uc.dwHostNameLength >= JUDGE_MAX_URL || !uc.dwUrlPathLength ||
        uc.dwUrlPathLength >= JUDGE_MAX_URL)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Judge endpoint could not be parsed");
    /* CrackUrl component pointers are not guaranteed null-terminated (the host
     * otherwise runs into the path and WinHttpConnect rejects it). */
    wchar_t whost[JUDGE_MAX_URL], wpath[JUDGE_MAX_URL];
    memcpy(whost, uc.lpszHostName, (size_t)uc.dwHostNameLength * sizeof(wchar_t));
    whost[uc.dwHostNameLength] = 0;
    memcpy(wpath, uc.lpszUrlPath, (size_t)uc.dwUrlPathLength * sizeof(wchar_t));
    wpath[uc.dwUrlPathLength] = 0;
    forge_judge *m = (forge_judge *)j;
    if (!m->session) {
        m->session = WinHttpOpen(L"Forge/judge", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m->session)
            return fg_error(e, FORGE_ERR_IO, "WinHttpOpen failed (%lu)", GetLastError());
        int t = (int)FG_MIN(j->timeout_ms, (size_t)INT_MAX);
        WinHttpSetTimeouts(m->session, t, t, t, t);
    }
    HINTERNET connect = WinHttpConnect(m->session, whost, uc.nPort, 0);
    if (!connect)
        return fg_error(e, FORGE_ERR_IO, "WinHttpConnect failed (%lu)", GetLastError());
    HINTERNET req = WinHttpOpenRequest(connect, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    forge_status status = FORGE_OK;
    wchar_t auth[JUDGE_MAX_KEY + 40];
    if (!req) {
        status = fg_error(e, FORGE_ERR_IO, "WinHttpOpenRequest failed (%lu)", GetLastError());
    } else {
        swprintf(auth, sizeof(auth) / sizeof(auth[0]), L"Authorization: Bearer %s", wkey);
        if (!WinHttpAddRequestHeaders(req, auth, (DWORD)-1,
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) ||
            !WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1,
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            status = fg_error(e, FORGE_ERR_IO, "WinHttpAddRequestHeaders failed (%lu)",
                              GetLastError());
        } else if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)request,
                                       (DWORD)request_len, (DWORD)request_len, 0)) {
            status = fg_error(e, FORGE_ERR_IO, "WinHttpSendRequest failed (%lu)", GetLastError());
        } else if (!WinHttpReceiveResponse(req, NULL)) {
            status = fg_error(e, FORGE_ERR_IO, "WinHttpReceiveResponse failed (%lu)",
                              GetLastError());
        }
    }
    if (status == FORGE_OK) {
        capture_response_headers(j, req);
        DWORD code = 0, size = sizeof(code);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            status = fg_error(e, FORGE_ERR_IO, "WinHttpQueryHeaders failed (%lu)", GetLastError());
        } else if (code < 200 || code >= 300) {
            fg_buf detail = {0};
            if (code == 401 || code == 422) {
                char chunk[256];
                DWORD read = 0;
                if (WinHttpReadData(req, chunk, sizeof(chunk) - 1, &read) && read) {
                    chunk[read] = 0;
                    (void)fg_buf_puts(&detail, chunk);
                }
            }
            status = code == 401
                         ? fg_error(e, FORGE_ERR_POLICY, "Judge service rejected the API key (401)")
                     : code == 422
                         ? fg_error(e, FORGE_ERR_PARSE, "Judge request rejected (422): %s",
                                    detail.data ? detail.data : "")
                     : code == 408 || code == 429 || code == 529 || code >= 500
                         ? fg_error(e, FORGE_ERR_IO, "Judge service transient failure (%lu)", code)
                         : fg_error(e, FORGE_ERR_IO, "Judge service returned status %lu", code);
            fg_buf_clear(&detail);
        }
    }
    if (status == FORGE_OK) {
        fg_buf body = {0};
        bool ok = true;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(req, &available)) {
                ok = fg_error(e, FORGE_ERR_IO, "WinHttpQueryDataAvailable failed (%lu)",
                              GetLastError()) == FORGE_OK;
                break;
            }
            if (!available)
                break;
            if (body.len + available > FG_MAX_JSON) {
                fg_error(e, FORGE_ERR_LIMIT, "Judge response exceeds the maximum body size");
                ok = false;
                break;
            }
            char *space = realloc(body.data, body.len + available + 1);
            if (!space) {
                fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate judge response");
                ok = false;
                break;
            }
            body.data = space;
            DWORD read = 0;
            if (!WinHttpReadData(req, body.data + body.len, available, &read)) {
                ok = fg_error(e, FORGE_ERR_IO, "WinHttpReadData failed (%lu)", GetLastError()) ==
                     FORGE_OK;
                break;
            }
            body.len += read;
            body.data[body.len] = 0;
        }
        if (ok && !body.data) {
            body.data = fg_strdup("");
            ok = body.data != NULL;
            if (!ok)
                fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate judge response");
        }
        if (ok) {
            *response = body.data;
            *response_len = body.len;
            body.data = NULL;
        }
        free(body.data);
        status = ok ? FORGE_OK : ((e && e->code) ? e->code : FORGE_ERR_IO);
    }
    if (req)
        WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    return status;
}
#endif

static forge_status judge_transport(forge_judge *j, const char *request, size_t request_len,
                                    char **response, size_t *response_len, forge_error *e) {
    if (j->options.transport)
        return j->options.transport(request, request_len, response, response_len,
                                    j->options.transport_userdata, e);
#ifdef _WIN32
    return http_transport(j, request, request_len, response, response_len, e);
#else
    return fg_error(e, FORGE_ERR_UNSUPPORTED,
                    "Hosted judge transport is unavailable on this platform");
#endif
}

forge_status forge_judge_rerank(forge_judge *j, const char *query, size_t count,
                                const char *const *paths, const char *const *snippets,
                                const char *const *stages, double *scores, forge_error *e) {
    if (!j || !query || (count && (!paths || !snippets || !stages || !scores)))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid judge rerank arguments");
    if (!count)
        return FORGE_OK;
    if (count > j->max_candidates)
        return fg_error(e, FORGE_ERR_LIMIT, "Judge rerank exceeds the candidate cap (%zu)",
                        j->max_candidates);
    if (j->options.cancelled && j->options.cancelled(j->options.userdata))
        return fg_error(e, FORGE_ERR_CANCELLED, "Judge call cancelled");
    char *body = build_request(j, query, count, paths, snippets, stages, e);
    if (!body)
        return e && e->code ? e->code : FORGE_ERR_MEMORY;
    char *response = NULL;
    size_t response_len = 0;
    forge_status status = FORGE_OK;
    uint64_t start = fg_now_ms();
    for (unsigned attempt = 0;; attempt++) {
        j->record_request_id[0] = 0; /* Headers belong to the last attempt only. */
        j->record_response_date[0] = 0;
        status = judge_transport(j, body, strlen(body), &response, &response_len, e);
        if (status == FORGE_OK || attempt > 0 || status != FORGE_ERR_IO)
            break;
        sleep_ms(JUDGE_RETRY_BACKOFF_MS);
        if (j->options.cancelled && j->options.cancelled(j->options.userdata)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Judge call cancelled");
            break;
        }
        if (e) {
            e->code = FORGE_OK;
            e->message[0] = 0;
        }
    }
    double latency = (double)(fg_now_ms() - start);
    j->calls++;
    j->last_latency_ms = latency;
    j->total_latency_ms += latency;
    j->last_input_tokens = 0;
    j->last_output_tokens = 0;
    if (status == FORGE_OK)
        status = parse_response(j, response, response_len, count, scores, e);
    if (status == FORGE_OK)
        j->candidates_scored += count;
    if (status != FORGE_OK)
        j->failures++;
    record_run(j, body, response, response_len, status, latency,
               status == FORGE_OK ? "" : (e ? e->message : ""));
    free(response);
    free(body);
    return status;
}

forge_status forge_judge_rerank_retrieval(void *userdata, const char *query, size_t count,
                                          const char *const *paths, const char *const *snippets,
                                          const char *const *stages, double *scores,
                                          forge_rerank_info *info, forge_error *e) {
    forge_judge *j = userdata;
    forge_error local = {0};
    forge_status status = forge_judge_rerank(j, query, count, paths, snippets, stages, scores,
                                             &local);
    if (e && local.code)
        *e = local;
    if (status == FORGE_OK && info) {
        info->model = j->last_model[0] ? j->last_model : NULL;
        info->input_tokens = j->last_input_tokens;
        info->output_tokens = j->last_output_tokens;
        info->latency_ms = j->last_latency_ms;
        info->reported = true;
    }
    return status;
}
