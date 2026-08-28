#include "internal.h"
#include "forge/diagnostics.h"
#include <ctype.h>
static int compare_hashes(const void *a, const void *b) {
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return left < right ? -1 : left > right ? 1 : 0;
}
/* Diagnostic identity ignores Go event timestamps, elapsed time, progress and
 * event ordering. It is a loop heuristic, never a proof that tests succeeded. */
static uint64_t legacy_diagnostic_hash(const char *raw) {
    uint64_t hashes[512];
    size_t count = 0;
    const char *p = raw;
    while (p && *p && count < sizeof(hashes) / sizeof(*hashes)) {
        const char *end = strchr(p, '\n');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        yyjson_doc *doc = yyjson_read(p, length, 0);
        yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *action = fg_json_str(obj, "Action");
        const char *output = fg_json_str(obj, "Output");
        if (action && (!strcmp(action, "fail") || !strcmp(action, "build-fail"))) {
            const char *package = fg_json_str(obj, "Package"), *test = fg_json_str(obj, "Test");
            fg_buf identity = {0};
            fg_buf_printf(&identity, "%s:%s:%s", action, package ? package : "", test ? test : "");
            if (!identity.failed)
                hashes[count++] = fg_hash(identity.data, identity.len);
            fg_buf_clear(&identity);
        }
        char *decoded =
            output ? fg_render_bytes(output, yyjson_get_len(yyjson_obj_get(obj, "Output"))) : NULL;
        const char *line = decoded ? decoded : action ? "" : p;
        size_t n = decoded ? strlen(decoded) : action ? 0 : length;
        char *text = malloc(n + 1);
        if (text) {
            memcpy(text, line, n);
            text[n] = 0;
            bool relevant = !strncmp(text, "exit_code=", 10) || strstr(text, ".go:") ||
                            strstr(text, ".c:") || strstr(text, ".rs:") || strstr(text, "error") ||
                            strstr(text, "Error") || strstr(text, "expected") ||
                            strstr(text, "actual") || strstr(text, "panic") ||
                            strstr(text, "Assertion");
            while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' '))
                text[--n] = 0;
            if (relevant && count < sizeof(hashes) / sizeof(*hashes))
                hashes[count++] = fg_hash(text, n);
            free(text);
        }
        free(decoded);
        yyjson_doc_free(doc);
        if (!end)
            break;
        p = end + 1;
    }
    qsort(hashes, count, sizeof(*hashes), compare_hashes);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++)
        if (!i || hashes[i] != hashes[i - 1])
            hashes[unique++] = hashes[i];
    return fg_hash(hashes, unique * sizeof(*hashes));
}
/* Go's -json protocol is preferred. Generic diagnostics keep failures and a
 * bounded tail; every raw byte remains in the session's tool artifacts. */
static char *legacy_compress_output(const char *raw, size_t budget, size_t *visible,
                                    forge_error *e) {
    if (!raw || budget < 64) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid output budget");
        return NULL;
    }
    char *rendered_raw = fg_render_bytes(raw, strlen(raw));
    if (!rendered_raw) {
        fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
        return NULL;
    }
    raw = rendered_raw;
    fg_buf result = {0};
    const char *p = raw;
    size_t pass = 0, fail = 0, skipped = 0;
    uint64_t seen[256] = {0};
    size_t count = 0;
    bool go_protocol = false;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *line = p;
        char *owned = NULL;
        yyjson_doc *d = yyjson_read(p, n, 0);
        if (d) {
            yyjson_val *o = yyjson_doc_get_root(d);
            const char *action = fg_json_str(o, "Action"), *output = fg_json_str(o, "Output"),
                       *test = fg_json_str(o, "Test"), *pkg = fg_json_str(o, "Package");
            bool go_event =
                action &&
                (!strcmp(action, "start") || !strcmp(action, "run") || !strcmp(action, "pause") ||
                 !strcmp(action, "cont") || !strcmp(action, "output") || !strcmp(action, "pass") ||
                 !strcmp(action, "skip") || !strcmp(action, "fail") ||
                 !strcmp(action, "build-output") || !strcmp(action, "build-fail"));
            go_protocol = go_protocol || go_event;
            if (go_event && !strcmp(action, "pass"))
                pass++;
            if (go_event && (!strcmp(action, "fail") || !strcmp(action, "build-fail"))) {
                fail++;
                fg_buf_printf(&result, "FAIL %s %s\n", pkg ? pkg : "", test ? test : "");
            }
            if (go_event && output) {
                owned = fg_render_bytes(output, yyjson_get_len(yyjson_obj_get(o, "Output")));
                if (!owned) {
                    yyjson_doc_free(d);
                    fg_buf_clear(&result);
                    free(rendered_raw);
                    fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
                    return NULL;
                }
                line = owned;
                n = strlen(owned);
            } else if (go_event)
                n = 0;
        }
        char *bounded = malloc(n + 1);
        if (!bounded) {
            free(owned);
            if (d)
                yyjson_doc_free(d);
            fg_buf_clear(&result);
            free(rendered_raw);
            fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
            return NULL;
        }
        memcpy(bounded, line, n);
        bounded[n] = 0;
        line = bounded;
        bool relevant =
            n && (strstr(line, "exit_code=") || strstr(line, "FAIL") || strstr(line, "error") ||
                  strstr(line, "Error") || strstr(line, "panic") || strstr(line, "expected") ||
                  strstr(line, "actual") || strstr(line, ".go:") || strstr(line, ".c:") ||
                  strstr(line, ".rs:") || strstr(line, "Assertion"));
        if (relevant) {
            uint64_t hash = fg_hash(line, n);
            bool duplicate = false;
            for (size_t i = 0; i < count; i++)
                if (seen[i] == hash)
                    duplicate = true;
            size_t take = fg_utf8_prefix(line, n, 1024);
            if (!duplicate && result.len + take + 1 < budget - 64) {
                if (count < 256)
                    seen[count++] = hash;
                fg_buf_add(&result, line, take);
                if (!take || line[take - 1] != '\n')
                    fg_buf_puts(&result, "\n");
            } else
                skipped++;
        }
        free(bounded);
        free(owned);
        if (d)
            yyjson_doc_free(d);
        if (!end)
            break;
        p = end + 1;
    }
    if (!go_protocol) {
        size_t raw_size = strlen(raw);
        if (raw_size <= budget) {
            /* A successful status header must not hide useful stdout or generic JSON. */
            fg_buf_clear(&result);
            fg_buf_puts(&result, raw);
        } else {
            size_t tail = FG_MIN(budget / 3, 1024);
            size_t prefix_limit = budget > tail + 48 ? budget - tail - 48 : 0;
            if (result.len > prefix_limit) {
                result.len = fg_utf8_prefix(result.data, result.len, prefix_limit);
                result.data[result.len] = 0;
            }
            const char *tail_start = raw + fg_utf8_forward(raw, raw_size, raw_size - tail);
            fg_buf_puts(&result, "\n[tail; full output saved]\n");
            fg_buf_puts(&result, tail_start);
        }
    } else if (!result.len) {
        size_t n = strlen(raw);
        size_t tail_budget = budget > 80 ? budget - 80 : 0;
        if (n > tail_budget) {
            fg_buf_puts(&result, "[tail; full output saved]\n");
            size_t start = fg_utf8_forward(raw, n, n - tail_budget);
            fg_buf_add(&result, raw + start, n - start);
        } else
            fg_buf_puts(&result, raw);
    }
    if (pass || fail || skipped)
        fg_buf_printf(&result, "\nsummary: pass=%zu fail=%zu omitted=%zu\n", pass, fail, skipped);
    if (result.len > budget) {
        result.len = fg_utf8_prefix(result.data, result.len, budget);
        result.data[result.len] = 0;
    }
    if (visible)
        *visible = result.len;
    free(rendered_raw);
    return fg_buf_take(&result);
}

/* Normalized adapters. Limits bound retained state, not just the final string. */
#define DG_DETAILS 16u
#define DG_FRAMES 16u
#define DG_STREAMS 64u
#define DG_LINE_BYTES (256u * 1024u)
enum dg_field {
    DG_MESSAGE,
    DG_PATH,
    DG_PACKAGE,
    DG_TEST,
    DG_THREAD,
    DG_CODE,
    DG_EXPECTED,
    DG_ACTUAL,
    DG_LEFT,
    DG_RIGHT,
    DG_DISPLAY,
    DG_FIELDS
};
typedef struct {
    char *path, *function;
    uint64_t line, column;
} dg_frame;
typedef struct {
    const char *adapter, *format, *kind, *severity, *column_unit;
    char *text[DG_FIELDS], *details[DG_DETAILS];
    dg_frame frames[DG_FRAMES];
    size_t detail_count, frame_count, occurrences;
    uint64_t line, column;
    int64_t column_origin;
    bool column_present, truncated, bytes_rendered;
    fg_buf identity;
} dg_record;
typedef struct {
    dg_record *active;
    bool pytest, traceback;
    char *function;
} dg_text_state;
typedef struct {
    fg_buf identity, pending;
    char *package, *test;
    dg_text_state text;
} dg_stream;
typedef struct {
    forge_diagnostic_options options;
    dg_record **records;
    size_t count, seen, duplicates, omitted, malformed, clipped_fields, omitted_details;
    size_t unrecognized_json;
    size_t go_pass, go_fail, go_skip, ignored_events, input_bytes, parsed_bytes;
    size_t test_pass, test_fail, package_pass, package_fail;
    const char *adapters[16];
    size_t adapter_count;
    dg_stream streams[DG_STREAMS];
    size_t stream_count;
    bool failed, input_truncated, line_truncated, stream_limited, ansi_removed, bytes_rendered;
} dg_context;

forge_diagnostic_options forge_diagnostics_default_options(void) {
    forge_diagnostic_options options = {
        FORGE_DIAGNOSTICS_AUTO, 4u * 1024u * 1024u, 256, 4096, 1024u * 1024u, false};
    return options;
}

static const char *dg_hint_name(forge_diagnostic_adapter adapter) {
    static const char *const names[] = {"auto",  "go_test", "go_vet", "golangci_lint", "gcc",
                                        "clang", "cargo",   "pytest", "generic"};
    return names[(unsigned)adapter];
}

static void dg_mark(dg_context *c, const char *adapter) {
    for (size_t i = 0; i < c->adapter_count; i++)
        if (!strcmp(c->adapters[i], adapter))
            return;
    if (c->adapter_count < sizeof(c->adapters) / sizeof(c->adapters[0]))
        c->adapters[c->adapter_count++] = adapter;
}

static void dg_key(dg_context *c, dg_record *r, const char *label, const char *text,
                   size_t length) {
    if (!fg_buf_printf(&r->identity, "%s:%zu:", label, length) ||
        !fg_buf_add(&r->identity, text, length))
        c->failed = true;
}

static char *dg_copy(dg_context *c, dg_record *r, const char *text, size_t length) {
    char *copy = fg_render_bytes(text, length);
    if (!copy) {
        c->failed = true;
        return NULL;
    }
    size_t n = strlen(copy);
    if (n != length || (length && memcmp(copy, text, length)))
        c->bytes_rendered = r->bytes_rendered = true;
    if (n > c->options.max_text_bytes) {
        copy[fg_utf8_prefix(copy, n, c->options.max_text_bytes)] = 0;
        c->clipped_fields++;
        r->truncated = true;
    }
    return copy;
}

static void dg_set_n(dg_context *c, dg_record *r, enum dg_field field, const char *text, size_t n) {
    if (!text)
        return;
    char label[16];
    snprintf(label, sizeof(label), "field%u", (unsigned)field);
    dg_key(c, r, label, text, n);
    free(r->text[field]);
    r->text[field] = dg_copy(c, r, text, n);
}

static void dg_set(dg_context *c, dg_record *r, enum dg_field field, const char *text) {
    if (text)
        dg_set_n(c, r, field, text, strlen(text));
}

static void dg_json_field(dg_context *c, dg_record *r, enum dg_field field, yyjson_val *object,
                          const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    if (yyjson_is_str(value))
        dg_set_n(c, r, field, yyjson_get_str(value), yyjson_get_len(value));
    else if (value && !yyjson_is_null(value))
        c->malformed++;
}

static dg_record *dg_new(dg_context *c, const char *adapter, const char *format, const char *kind,
                         const char *severity) {
    dg_record *r = calloc(1, sizeof(*r));
    if (!r) {
        c->failed = true;
        return NULL;
    }
    r->adapter = adapter;
    r->format = format;
    r->kind = kind;
    r->severity = severity;
    r->column_unit = "unknown";
    r->column_origin = -1;
    r->occurrences = 1;
    dg_mark(c, adapter);
    return r;
}

static void dg_free(dg_record *r) {
    if (!r)
        return;
    for (size_t i = 0; i < DG_FIELDS; i++)
        free(r->text[i]);
    for (size_t i = 0; i < r->detail_count; i++)
        free(r->details[i]);
    for (size_t i = 0; i < r->frame_count; i++) {
        free(r->frames[i].path);
        free(r->frames[i].function);
    }
    fg_buf_clear(&r->identity);
    free(r);
}

static void dg_detail(dg_context *c, dg_record *r, const char *text, size_t n) {
    if (!r || !n)
        return;
    dg_key(c, r, "detail", text, n);
    if (r->detail_count == DG_DETAILS) {
        c->omitted_details++;
        r->truncated = true;
        return;
    }
    r->details[r->detail_count++] = dg_copy(c, r, text, n);
}

static void dg_frame_add(dg_context *c, dg_record *r, const char *path, uint64_t line,
                         uint64_t column, const char *function) {
    if (!r || !path || !line)
        return;
    dg_key(c, r, "frame_path", path, strlen(path));
    dg_key(c, r, "frame_function", function ? function : "", function ? strlen(function) : 0);
    if (!fg_buf_printf(&r->identity, "frame:%llu:%llu:", (unsigned long long)line,
                       (unsigned long long)column))
        c->failed = true;
    if (r->frame_count == DG_FRAMES) {
        c->omitted_details++;
        r->truncated = true;
        return;
    }
    dg_frame *frame = &r->frames[r->frame_count++];
    frame->path = dg_copy(c, r, path, strlen(path));
    frame->function = function ? dg_copy(c, r, function, strlen(function)) : NULL;
    frame->line = line;
    frame->column = column;
}

static unsigned dg_severity_rank(const char *severity) {
    if (!strcmp(severity, "fatal"))
        return 0;
    if (!strcmp(severity, "error"))
        return 1;
    if (!strcmp(severity, "warning"))
        return 2;
    if (!strcmp(severity, "note"))
        return 3;
    if (!strcmp(severity, "info"))
        return 4;
    return 5;
}

static int dg_compare_records(const dg_record *a, const dg_record *b) {
    unsigned left = dg_severity_rank(a->severity), right = dg_severity_rank(b->severity);
    if (left != right)
        return left < right ? -1 : 1;
    bool a_output = !strcmp(a->kind, "output"), b_output = !strcmp(b->kind, "output");
    if (a_output != b_output)
        return a_output ? 1 : -1;
    const enum dg_field fields[] = {DG_PATH, DG_PACKAGE, DG_TEST, DG_MESSAGE, DG_CODE};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        int order = strcmp(a->text[fields[i]] ? a->text[fields[i]] : "",
                           b->text[fields[i]] ? b->text[fields[i]] : "");
        if (order)
            return order;
        if (fields[i] == DG_PATH) {
            if (a->line != b->line)
                return a->line < b->line ? -1 : 1;
            if (a->column != b->column)
                return a->column < b->column ? -1 : 1;
        }
    }
    size_t n = FG_MIN(a->identity.len, b->identity.len);
    int order = n ? memcmp(a->identity.data, b->identity.data, n) : 0;
    if (order)
        return order;
    return a->identity.len < b->identity.len ? -1 : a->identity.len > b->identity.len ? 1 : 0;
}

static int dg_compare(const void *a, const void *b) {
    return dg_compare_records(*(dg_record *const *)a, *(dg_record *const *)b);
}

static void dg_commit(dg_context *c, dg_record *r) {
    if (!r)
        return;
    c->seen++;
    if (!fg_buf_printf(&r->identity, "meta:%s:%s:%s:%s:%s:%lld:%llu:%llu:%u", r->adapter, r->format,
                       r->kind, r->severity, r->column_unit, (long long)r->column_origin,
                       (unsigned long long)r->line, (unsigned long long)r->column,
                       r->column_present ? 1u : 0u))
        c->failed = true;
    if (c->failed) {
        dg_free(r);
        return;
    }
    for (size_t i = 0; i < c->count; i++) {
        dg_record *old = c->records[i];
        if (old->identity.len == r->identity.len &&
            !memcmp(old->identity.data, r->identity.data, r->identity.len)) {
            old->occurrences++;
            c->duplicates++;
            dg_free(r);
            return;
        }
    }
    if (c->count < c->options.max_diagnostics) {
        c->records[c->count++] = r;
        return;
    }
    size_t worst = 0;
    for (size_t i = 1; i < c->count; i++)
        if (dg_compare_records(c->records[worst], c->records[i]) < 0)
            worst = i;
    if (dg_compare_records(r, c->records[worst]) < 0) {
        c->omitted += c->records[worst]->occurrences;
        dg_free(c->records[worst]);
        c->records[worst] = r;
    } else {
        c->omitted++;
        dg_free(r);
    }
}

static void dg_flush(dg_context *c, dg_text_state *state) {
    dg_commit(c, state->active);
    state->active = NULL;
    free(state->function);
    state->function = NULL;
    state->traceback = false;
}

static const char *dg_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    return p;
}

static bool dg_number(const char **cursor, uint64_t *number) {
    const char *p = *cursor;
    if (*p < '0' || *p > '9')
        return false;
    uint64_t value = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    *cursor = p;
    *number = value;
    return true;
}

/* A drive-letter colon is skipped; neither numbers embedded in a message nor
 * negative/overflowed locations are treated as source coordinates. */
static bool dg_location(const char *text, char path[FG_PATH_MAX], uint64_t *line, uint64_t *column,
                        bool *has_column, const char **message, bool frame) {
    const char *start = text;
    if (isalpha((unsigned char)text[0]) && text[1] == ':' && (text[2] == '/' || text[2] == '\\'))
        start += 2;
    const char *colon = strchr(start, ':');
    if (colon) {
        const char *p = colon + 1;
        uint64_t row = 0, col = 0;
        if (colon == text || (size_t)(colon - text) >= FG_PATH_MAX || !dg_number(&p, &row) || !row)
            return false;
        bool has_col = false;
        if (*p == ':' && p[1] >= '0' && p[1] <= '9') {
            p++;
            if (!dg_number(&p, &col))
                return false;
            has_col = true;
        }
        if (*p == ':' && (p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2]))
            return false;
        if (*p == ':')
            p++;
        else if (!frame || (*p && *p != ' ' && *p != '\t'))
            return false;
        memcpy(path, text, (size_t)(colon - text));
        path[colon - text] = 0;
        *line = row;
        *column = col;
        *has_column = has_col;
        *message = dg_space(p);
        return true;
    }
    return false;
}

static const char *dg_level(const char *text, const char **message) {
    static const struct {
        const char *prefix, *level;
    } levels[] = {{"fatal error:", "fatal"}, {"error:", "error"},    {"warning:", "warning"},
                  {"note:", "note"},         {"remark:", "note"},    {"help:", "note"},
                  {"Error:", "error"},       {"Warning:", "warning"}};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        size_t n = strlen(levels[i].prefix);
        if (!strncmp(text, levels[i].prefix, n)) {
            *message = dg_space(text + n);
            return levels[i].level;
        }
    }
    *message = text;
    return "unknown";
}

static const char *dg_json_level(const char *text) {
    if (!text)
        return "unknown";
    if (!strcmp(text, "fatal error") || !strcmp(text, "fatal"))
        return "fatal";
    if (!strcmp(text, "error") || !strcmp(text, "error: internal compiler error"))
        return "error";
    if (!strcmp(text, "warning"))
        return "warning";
    if (!strcmp(text, "note") || !strcmp(text, "help") || !strcmp(text, "failure-note"))
        return "note";
    if (!strcmp(text, "info"))
        return "info";
    return "unknown";
}

/* Extract only literal, labelled values. The two operands of assert_eq or a
 * pytest assertion are NOT presumed to be expected and actual. */
static void dg_labels(dg_context *c, dg_record *r, const char *message) {
    const char *expected = strstr(message, "expected="), *actual = strstr(message, " actual=");
    if (expected && (expected == message || expected[-1] == ' ' || expected[-1] == '\t') &&
        actual && actual > expected + 9) {
        dg_set_n(c, r, DG_EXPECTED, expected + 9, (size_t)(actual - expected - 9));
        dg_set(c, r, DG_ACTUAL, actual + 8);
    } else if (!strncmp(message, "Expected:", 9))
        dg_set(c, r, DG_EXPECTED, dg_space(message + 9));
    else if (!strncmp(message, "Actual:", 7))
        dg_set(c, r, DG_ACTUAL, dg_space(message + 7));
    else if (!strncmp(message, "left:", 5))
        dg_set(c, r, DG_LEFT, dg_space(message + 5));
    else if (!strncmp(message, "right:", 6))
        dg_set(c, r, DG_RIGHT, dg_space(message + 6));
}

static char *dg_clean_line(dg_context *c, const char *raw, size_t length, fg_buf *plain) {
    while (length && raw[length - 1] == '\r')
        length--;
    for (size_t i = 0; i < length;) {
        if ((unsigned char)raw[i] == 27 && i + 1 < length && raw[i + 1] == '[') {
            size_t end = i + 2;
            while (end < length && end - i < 64 &&
                   ((raw[end] >= '0' && raw[end] <= '9') || raw[end] == ';' || raw[end] == ':'))
                end++;
            if (end < length && raw[end] == 'm') {
                c->ansi_removed = true;
                i = end + 1;
                continue;
            }
        }
        if (!fg_buf_add(plain, raw + i, 1)) {
            c->failed = true;
            return NULL;
        }
        i++;
    }
    char *rendered = fg_render_bytes(plain->data, plain->len);
    if (!rendered)
        c->failed = true;
    return rendered;
}

static dg_record *dg_begin_text(dg_context *c, dg_text_state *state, dg_stream *stream,
                                const char *adapter, const char *format, const char *kind,
                                const char *severity, const char *message, const fg_buf *plain) {
    dg_flush(c, state);
    dg_record *r = dg_new(c, adapter, format, kind, severity);
    if (!r)
        return NULL;
    dg_set(c, r, DG_MESSAGE, message);
    dg_set_n(c, r, DG_DISPLAY, plain->data ? plain->data : "", plain->len);
    if (stream) {
        dg_set(c, r, DG_PACKAGE, stream->package);
        dg_set(c, r, DG_TEST, stream->test);
        dg_key(c, r, "go_stream", stream->identity.data, stream->identity.len);
    }
    state->active = r;
    return r;
}

static const char *dg_text_adapter(dg_context *c, dg_stream *stream, const char *fallback) {
    if (stream)
        return "go_test";
    if (!strcmp(fallback, "rust_test") || !strcmp(fallback, "rustc") || !strcmp(fallback, "rust"))
        return c->options.adapter == FORGE_DIAGNOSTICS_CARGO ? "cargo" : fallback;
    if (!strcmp(fallback, "generic"))
        return c->options.adapter == FORGE_DIAGNOSTICS_PYTEST ? "pytest" : fallback;
    if (!strcmp(fallback, "go_test")) {
        if (c->options.adapter == FORGE_DIAGNOSTICS_GO_VET)
            return "go_vet";
        if (c->options.adapter == FORGE_DIAGNOSTICS_GOLANGCI_LINT)
            return "golangci_lint";
        return "go_test";
    }
    switch (c->options.adapter) {
    case FORGE_DIAGNOSTICS_GCC:
        return "gcc";
    case FORGE_DIAGNOSTICS_CLANG:
        return "clang";
    case FORGE_DIAGNOSTICS_CARGO:
        return "cargo";
    default:
        return fallback;
    }
}

static void dg_text_location(dg_context *c, dg_record *r, const char *path, uint64_t line,
                             uint64_t column, bool has_column) {
    dg_set(c, r, DG_PATH, path);
    r->line = line;
    r->column = column;
    r->column_present = has_column;
    /* GCC permits configurable origins/units, so plain text cannot establish
     * them. Clang's default colon format documents byte columns from one. */
    if (!strcmp(r->adapter, "clang")) {
        r->column_unit = "byte";
        r->column_origin = 1;
    }
}

static void dg_text_line(dg_context *c, dg_text_state *state, dg_stream *stream, const char *raw,
                         size_t length) {
    if (c->failed)
        return;
    fg_buf plain = {0};
    char *owned = dg_clean_line(c, raw, length, &plain);
    if (!owned) {
        fg_buf_clear(&plain);
        return;
    }
    const char *text = dg_space(owned), *message = NULL;
    size_t n = strlen(text);
    char path[FG_PATH_MAX];
    uint64_t line = 0, column = 0;
    bool has_column = false;
    dg_record *r = state->active;
    bool forced_generic = c->options.adapter == FORGE_DIAGNOSTICS_GENERIC;
    if (!*text || !strcmp(text, "stdout:") || !strcmp(text, "stderr:"))
        goto done;
    if (!forced_generic && strstr(text, " FAILURES ") && text[0] == '=') {
        dg_flush(c, state);
        state->pytest = true;
        dg_mark(c, "pytest");
        goto done;
    }
    if (state->pytest && !strncmp(text, "===", 3)) {
        dg_flush(c, state);
        state->pytest = false;
        r = NULL;
    }
    if (!forced_generic && (state->pytest || c->options.adapter == FORGE_DIAGNOSTICS_PYTEST) &&
        !strncmp(text, "___", 3)) {
        const char *begin = text;
        while (*begin == '_' || *begin == ' ')
            begin++;
        size_t end = n;
        while (end && (text[end - 1] == '_' || text[end - 1] == ' '))
            end--;
        if (text + end > begin) {
            r = dg_begin_text(c, state, stream, "pytest", "pytest_text", "test_failure", "error",
                              "pytest failure", &plain);
            if (r)
                dg_set_n(c, r, DG_TEST, begin, (size_t)(text + end - begin));
            goto done;
        }
    }
    if (!forced_generic && (!strncmp(text, "FAILED ", 7) || !strncmp(text, "ERROR ", 6))) {
        const char *identity = text + (text[0] == 'F' ? 7 : 6);
        const char *node = strstr(identity, "::"), *end = strstr(identity, " - ");
        if (node && (!end || node < end)) {
            r = dg_begin_text(c, state, stream, "pytest", "pytest_summary", "test_failure", "error",
                              end ? end + 3 : "pytest failure", &plain);
            if (r) {
                dg_set_n(c, r, DG_TEST, identity,
                         end ? (size_t)(end - identity) : strlen(identity));
                dg_set_n(c, r, DG_PATH, identity, (size_t)(node - identity));
            }
            goto done;
        }
    }
    if (!forced_generic && r && !strcmp(r->adapter, "pytest") && text[0] == 'E' &&
        (!text[1] || text[1] == ' ' || text[1] == '\t')) {
        const char *detail = dg_space(text + 1);
        if (*detail) {
            if (!strncmp(detail, "AssertionError", 14) || !strncmp(detail, "assert ", 7) ||
                strstr(detail, "Error:"))
                dg_set(c, r, DG_MESSAGE, detail);
            dg_detail(c, r, detail, strlen(detail));
            dg_labels(c, r, detail);
        }
        goto done;
    }
    if (!forced_generic && r && !strcmp(r->adapter, "pytest") && text[0] == '>') {
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && !strncmp(text, "Traceback (most recent call last):", 34)) {
        r = dg_begin_text(c, state, stream, dg_text_adapter(c, stream, "generic"),
                          "python_traceback", "runtime", "unknown", text, &plain);
        state->traceback = true;
        goto done;
    }
    if (!forced_generic && r && (state->traceback || !strcmp(r->adapter, "pytest")) &&
        !strncmp(text, "File \"", 6)) {
        const char *close = strchr(text + 6, '"');
        if (close && !strncmp(close, "\", line ", 8)) {
            const char *number = close + 8;
            if ((size_t)(close - text - 6) < sizeof(path) && dg_number(&number, &line) && line) {
                memcpy(path, text + 6, (size_t)(close - text - 6));
                path[close - text - 6] = 0;
                const char *function = !strncmp(number, ", in ", 5) ? number + 5 : NULL;
                dg_frame_add(c, r, path, line, 0, function);
                if (!r->text[DG_PATH])
                    dg_text_location(c, r, path, line, 0, false);
                goto done;
            }
        }
    }
    if (!forced_generic && r && state->traceback &&
        (strstr(text, "Error:") || !strncmp(text, "AssertionError", 14))) {
        dg_set(c, r, DG_MESSAGE, text);
        r->severity = "error";
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && r && !strcmp(r->format, "rust_panic")) {
        if (!strncmp(text, "assertion ", 10) || !strcmp(text, "stack backtrace:")) {
            if (!strncmp(text, "assertion ", 10))
                dg_set(c, r, DG_MESSAGE, text);
            dg_detail(c, r, text, n);
            goto done;
        }
        const char *number = text;
        uint64_t frame_index = 0;
        if (dg_number(&number, &frame_index) && *number == ':' && number[1] == ' ') {
            free(state->function);
            state->function = fg_strdup(dg_space(number + 1));
            if (!state->function)
                c->failed = true;
            dg_detail(c, r, text, n);
            goto done;
        }
    }
    if (!forced_generic && !strncmp(text, "test ", 5)) {
        const char *end = strstr(text + 5, " ... FAILED");
        if (end) {
            r = dg_begin_text(c, state, stream, dg_text_adapter(c, stream, "rust_test"),
                              "rust_test_text", "test_failure", "error", "test failed", &plain);
            if (r)
                dg_set_n(c, r, DG_TEST, text + 5, (size_t)(end - text - 5));
            goto done;
        }
    }
    if (!forced_generic && !strncmp(text, "thread '", 8)) {
        const char *end = strstr(text + 8, "' panicked at ");
        if (end) {
            const char *location = dg_space(end + strlen("' panicked at "));
            r = dg_begin_text(c, state, stream, dg_text_adapter(c, stream, "rust"), "rust_panic",
                              "runtime", "error", text, &plain);
            if (r) {
                dg_set_n(c, r, DG_THREAD, text + 8, (size_t)(end - text - 8));
                if (dg_location(location, path, &line, &column, &has_column, &message, true))
                    dg_text_location(c, r, path, line, column, has_column);
            }
            goto done;
        }
    }
    if (!forced_generic && (!strncmp(text, "error[", 6) || !strncmp(text, "warning[", 8))) {
        const char *start = strchr(text, '['), *end = strstr(start, "]:");
        if (end) {
            r = dg_begin_text(c, state, stream, dg_text_adapter(c, stream, "rustc"), "rustc_text",
                              "compiler", text[0] == 'e' ? "error" : "warning", dg_space(end + 2),
                              &plain);
            if (r)
                dg_set_n(c, r, DG_CODE, start + 1, (size_t)(end - start - 1));
            goto done;
        }
    }
    if (!forced_generic && r && !strncmp(text, "--> ", 4) &&
        dg_location(text + 4, path, &line, &column, &has_column, &message, true)) {
        dg_text_location(c, r, path, line, column, has_column);
        goto done;
    }
    if (!forced_generic && r && (!strncmp(text, "at ", 3) || stream) &&
        dg_location(!strncmp(text, "at ", 3) ? text + 3 : text, path, &line, &column, &has_column,
                    &message, true) &&
        (!*message || !strncmp(message, "+0x", 3))) {
        dg_frame_add(c, r, path, line, column, state->function);
        goto done;
    }
    if (!forced_generic && r &&
        (!strncmp(text, "Expected:", 9) || !strncmp(text, "Actual:", 7) ||
         !strncmp(text, "left:", 5) || !strncmp(text, "right:", 6))) {
        dg_labels(c, r, text);
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && dg_location(text, path, &line, &column, &has_column, &message, false)) {
        const char *body = NULL, *level = dg_level(message, &body);
        bool go =
            strstr(path, ".go") && (stream || c->options.adapter == FORGE_DIAGNOSTICS_GO_TEST ||
                                    c->options.adapter == FORGE_DIAGNOSTICS_GO_VET ||
                                    c->options.adapter == FORGE_DIAGNOSTICS_GOLANGCI_LINT);
        bool python = strstr(path, ".py") &&
                      (state->pytest || c->options.adapter == FORGE_DIAGNOSTICS_PYTEST ||
                       !strncmp(body, "AssertionError", 14));
        if (python && r && !strcmp(r->adapter, "pytest")) {
            dg_text_location(c, r, path, line, column, has_column);
            dg_frame_add(c, r, path, line, column, NULL);
            if (!strcmp(r->text[DG_MESSAGE], "pytest failure"))
                dg_set(c, r, DG_MESSAGE, body);
            goto done;
        }
        {
            bool recognized = go || python || strcmp(level, "unknown");
            const char *adapter = python ? "pytest"
                                  : recognized
                                      ? dg_text_adapter(c, stream, go ? "go_test" : "compiler")
                                      : "generic";
            const char *kind = python ? "test_failure"
                               : go && !stream && c->options.adapter != FORGE_DIAGNOSTICS_GO_TEST
                                   ? "lint"
                               : recognized ? "compiler"
                                            : "diagnostic";
            r = dg_begin_text(c, state, stream, adapter,
                              python       ? "pytest_text"
                              : go         ? "go_text"
                              : recognized ? "compiler_text"
                                           : "location_text",
                              kind, python ? "error" : level, body, &plain);
            if (r) {
                dg_text_location(c, r, path, line, column, has_column);
                dg_labels(c, r, body);
                const char *option = strstr(body, " [-W");
                if (option && n && text[n - 1] == ']')
                    dg_set_n(c, r, DG_CODE, option + 2, strlen(option + 2) - 1);
                if (!strcmp(adapter, "golangci_lint")) {
                    const char *tag = strrchr(body, '(');
                    size_t body_length = strlen(body);
                    if (tag && body_length && body[body_length - 1] == ')')
                        dg_set_n(c, r, DG_CODE, tag + 1, (size_t)(body + body_length - tag - 2));
                }
            }
            goto done;
        }
    }
    if (!forced_generic && r && owned != text &&
        (!strcmp(r->adapter, "pytest") || state->traceback)) {
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && r &&
        (strchr(text, '|') || text[0] == '^' || !strncmp(text, "= note:", 7) ||
         !strncmp(text, "= help:", 7))) {
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && r && stream && (strchr(text, '(') || !strncmp(text, "goroutine ", 10))) {
        free(state->function);
        state->function = fg_strdup(text);
        if (!state->function)
            c->failed = true;
        dg_detail(c, r, text, n);
        goto done;
    }
    if (!forced_generic && !strncmp(text, "--- FAIL: ", 10)) {
        const char *end = strchr(text + 10, ' ');
        r = dg_begin_text(c, state, stream, "go_test", "go_test_text", "test_failure", "error",
                          "test failed", &plain);
        if (r)
            dg_set_n(c, r, DG_TEST, text + 10, end ? (size_t)(end - text - 10) : n - 10);
        goto done;
    }
    {
        const char *body = NULL, *level = dg_level(text, &body);
        bool panic = !strncmp(text, "panic:", 6) || !strncmp(text, "AssertionError", 14);
        bool status = !strncmp(text, "exit_code=", 10);
        const char *kind = status                     ? "status"
                           : panic                    ? "runtime"
                           : strcmp(level, "unknown") ? "diagnostic"
                                                      : "output";
        const char *adapter = stream ? "go_test" : "generic";
        const char *format = stream ? "go_test_json" : "generic_text";
        if (!forced_generic && c->options.adapter == FORGE_DIAGNOSTICS_CARGO &&
            strcmp(level, "unknown")) {
            adapter = "cargo";
            format = "rustc_text";
            kind = "compiler";
        }
        r = dg_begin_text(c, state, stream, adapter, format, kind,
                          panic    ? "error"
                          : status ? "info"
                                   : level,
                          body, &plain);
        if (r)
            dg_labels(c, r, body);
    }
done:
    free(owned);
    fg_buf_clear(&plain);
}

static const char *dg_tag(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    const char *text = yyjson_get_str(value);
    return text && strlen(text) == yyjson_get_len(value) ? text : NULL;
}

static bool dg_coordinate(dg_context *c, yyjson_val *object, const char *key, bool positive,
                          uint64_t *result) {
    yyjson_val *value = yyjson_obj_get(object, key);
    if (!value || yyjson_is_null(value))
        return false;
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > UINT32_MAX ||
        (positive && !yyjson_get_uint(value))) {
        c->malformed++;
        return false;
    }
    *result = yyjson_get_uint(value);
    return true;
}

static void dg_json_location(dg_context *c, dg_record *r, yyjson_val *position,
                             const char *file_key, const char *line_key, const char *column_key,
                             const char *unit, int64_t origin) {
    if (!yyjson_is_obj(position))
        return;
    dg_json_field(c, r, DG_PATH, position, file_key);
    dg_coordinate(c, position, line_key, true, &r->line);
    r->column_present = dg_coordinate(c, position, column_key, false, &r->column);
    r->column_unit = unit;
    r->column_origin = origin;
}

/* The primary location is normalized. Other source ranges stay in the raw
 * artifact; retain their identity so distinct evidence never deduplicates
 * merely because an unrendered location differs. */
static void dg_omit_location(dg_context *c, dg_record *r, yyjson_val *value) {
    char *json = yyjson_val_write(value, 0, NULL);
    if (!json) {
        c->failed = true;
        return;
    }
    dg_key(c, r, "unrendered_location", json, strlen(json));
    free(json);
    c->omitted_details++;
    r->truncated = true;
}

static void dg_child_details(dg_context *c, dg_record *r, yyjson_val *children, unsigned depth) {
    if (!children || yyjson_is_null(children))
        return;
    if (!yyjson_is_arr(children)) {
        c->malformed++;
        return;
    }
    if (depth == 8) {
        c->omitted_details += yyjson_arr_size(children);
        r->truncated = true;
        return;
    }
    size_t i, count;
    yyjson_val *child;
    yyjson_arr_foreach(children, i, count, child) {
        yyjson_val *message = yyjson_obj_get(child, "message");
        if (yyjson_is_str(message))
            dg_detail(c, r, yyjson_get_str(message), yyjson_get_len(message));
        else
            c->malformed++;
        yyjson_val *locations = yyjson_obj_get(child, "locations");
        yyjson_val *spans = yyjson_obj_get(child, "spans");
        size_t j, n;
        yyjson_val *location;
        yyjson_arr_foreach(locations, j, n, location) dg_omit_location(c, r, location);
        yyjson_arr_foreach(spans, j, n, location) dg_omit_location(c, r, location);
        dg_child_details(c, r, yyjson_obj_get(child, "children"), depth + 1);
    }
}

static bool dg_rust_json(dg_context *c, yyjson_val *message, yyjson_val *envelope,
                         const char *adapter) {
    yyjson_val *body = yyjson_obj_get(message, "message"),
               *spans = yyjson_obj_get(message, "spans");
    const char *level = dg_tag(message, "level");
    if (!yyjson_is_str(body) || !level || !yyjson_is_arr(spans)) {
        c->malformed++;
        return false;
    }
    dg_record *r = dg_new(c, adapter, !strcmp(adapter, "cargo") ? "cargo_json" : "rustc_json",
                          "compiler", dg_json_level(level));
    if (!r)
        return true;
    dg_set_n(c, r, DG_MESSAGE, yyjson_get_str(body), yyjson_get_len(body));
    dg_key(c, r, "level", level, strlen(level));
    dg_json_field(c, r, DG_CODE, yyjson_obj_get(message, "code"), "code");
    if (envelope)
        dg_json_field(c, r, DG_PACKAGE, envelope, "package_id");
    size_t i, count;
    yyjson_val *span;
    bool primary = false;
    yyjson_arr_foreach(spans, i, count, span) {
        if (!primary && yyjson_get_bool(yyjson_obj_get(span, "is_primary"))) {
            dg_json_location(c, r, span, "file_name", "line_start", "column_start",
                             "unicode_scalar", 1);
            primary = true;
        } else
            dg_omit_location(c, r, span);
        yyjson_val *label = yyjson_obj_get(span, "label");
        if (yyjson_is_str(label))
            dg_detail(c, r, yyjson_get_str(label), yyjson_get_len(label));
        yyjson_val *lines = yyjson_obj_get(span, "text"), *source;
        size_t j, length;
        yyjson_arr_foreach(lines, j, length, source) {
            yyjson_val *text = yyjson_obj_get(source, "text");
            if (yyjson_is_str(text))
                dg_detail(c, r, yyjson_get_str(text), yyjson_get_len(text));
        }
    }
    dg_child_details(c, r, yyjson_obj_get(message, "children"), 0);
    dg_commit(c, r);
    return true;
}

static bool dg_gcc_json(dg_context *c, yyjson_val *object) {
    const char *kind = dg_tag(object, "kind");
    yyjson_val *message = yyjson_obj_get(object, "message"),
               *locations = yyjson_obj_get(object, "locations");
    if (!kind || !yyjson_is_str(message) || !yyjson_is_arr(locations))
        return false;
    dg_record *r = dg_new(c, "gcc", "gcc_json", "compiler", dg_json_level(kind));
    if (!r)
        return true;
    dg_set_n(c, r, DG_MESSAGE, yyjson_get_str(message), yyjson_get_len(message));
    dg_json_field(c, r, DG_CODE, object, "option");
    yyjson_val *caret = yyjson_obj_get(yyjson_arr_get_first(locations), "caret");
    uint64_t origin = 0;
    bool known_origin = dg_coordinate(c, object, "column-origin", false, &origin);
    const char *column_key = yyjson_obj_get(caret, "byte-column")      ? "byte-column"
                             : yyjson_obj_get(caret, "display-column") ? "display-column"
                                                                       : "column";
    const char *unit = !strcmp(column_key, "byte-column")      ? "byte"
                       : !strcmp(column_key, "display-column") ? "display"
                                                               : "unknown";
    dg_json_location(c, r, caret, "file", "line", column_key, unit,
                     known_origin ? (int64_t)origin : -1);
    for (size_t i = 1; i < yyjson_arr_size(locations); i++)
        dg_omit_location(c, r, yyjson_arr_get(locations, i));
    dg_child_details(c, r, yyjson_obj_get(object, "children"), 0);
    dg_commit(c, r);
    return true;
}

static bool dg_lint_json(dg_context *c, yyjson_val *issue) {
    yyjson_val *message = yyjson_obj_get(issue, "Text"), *position = yyjson_obj_get(issue, "Pos");
    if (!yyjson_is_str(message) || !yyjson_is_obj(position) || !dg_tag(issue, "FromLinter")) {
        c->malformed++;
        return false;
    }
    dg_record *r = dg_new(c, "golangci_lint", "golangci_json", "lint",
                          dg_json_level(dg_tag(issue, "Severity")));
    if (!r)
        return true;
    dg_set_n(c, r, DG_MESSAGE, yyjson_get_str(message), yyjson_get_len(message));
    dg_json_field(c, r, DG_CODE, issue, "FromLinter");
    dg_json_location(c, r, position, "Filename", "Line", "Column", "byte", 1);
    size_t i, count;
    yyjson_val *source;
    yyjson_arr_foreach(yyjson_obj_get(issue, "SourceLines"), i, count,
                       source) if (yyjson_is_str(source))
        dg_detail(c, r, yyjson_get_str(source), yyjson_get_len(source));
    dg_commit(c, r);
    return true;
}

static dg_stream *dg_go_stream(dg_context *c, yyjson_val *object, bool create) {
    yyjson_val *package = yyjson_obj_get(object, "Package"), *test = yyjson_obj_get(object, "Test");
    if (!package)
        package = yyjson_obj_get(object, "ImportPath");
    if ((package && !yyjson_is_str(package)) || (test && !yyjson_is_str(test))) {
        c->malformed++;
        return NULL;
    }
    fg_buf identity = {0};
    const char *p = package ? yyjson_get_str(package) : "", *t = test ? yyjson_get_str(test) : "";
    size_t pn = package ? yyjson_get_len(package) : 0, tn = test ? yyjson_get_len(test) : 0;
    fg_buf_printf(&identity, "%zu:", pn);
    fg_buf_add(&identity, p, pn);
    fg_buf_printf(&identity, ":%zu:", tn);
    fg_buf_add(&identity, t, tn);
    if (identity.failed) {
        c->failed = true;
        fg_buf_clear(&identity);
        return NULL;
    }
    for (size_t i = 0; i < c->stream_count; i++)
        if (c->streams[i].identity.len == identity.len &&
            !memcmp(c->streams[i].identity.data, identity.data, identity.len)) {
            fg_buf_clear(&identity);
            return &c->streams[i];
        }
    if (!create || c->stream_count == DG_STREAMS) {
        if (create)
            c->stream_limited = true;
        fg_buf_clear(&identity);
        return NULL;
    }
    dg_stream *stream = &c->streams[c->stream_count++];
    stream->identity = identity;
    stream->package = fg_render_bytes(p, pn);
    stream->test = fg_render_bytes(t, tn);
    if (!stream->package || !stream->test)
        c->failed = true;
    return stream;
}

static void dg_go_output(dg_context *c, dg_stream *stream, const char *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length && !c->failed) {
        const char *newline = memchr(bytes + offset, '\n', length - offset);
        size_t n = newline ? (size_t)(newline - (bytes + offset)) : length - offset;
        size_t take = FG_MIN(n, DG_LINE_BYTES - stream->pending.len);
        if (take < n)
            c->line_truncated = true;
        if (!fg_buf_add(&stream->pending, bytes + offset, take))
            c->failed = true;
        offset += n;
        if (newline) {
            dg_text_line(c, &stream->text, stream, stream->pending.data, stream->pending.len);
            stream->pending.len = 0;
            if (stream->pending.data)
                stream->pending.data[0] = 0;
            offset++;
        }
    }
}

static bool dg_structured(dg_context *c, yyjson_val *object) {
    if (c->options.adapter == FORGE_DIAGNOSTICS_GENERIC)
        return false;
    if (yyjson_is_arr(object)) {
        yyjson_val *first = yyjson_arr_get_first(object);
        bool gcc =
            first && dg_tag(first, "kind") && yyjson_is_arr(yyjson_obj_get(first, "locations"));
        if (!gcc && !(c->options.adapter == FORGE_DIAGNOSTICS_GCC && !yyjson_arr_size(object)))
            return false;
        dg_mark(c, "gcc");
        size_t i, count;
        yyjson_val *item;
        yyjson_arr_foreach(object, i, count, item) if (!dg_gcc_json(c, item)) {
            c->malformed++;
            c->omitted++;
        }
        return true;
    }
    if (!yyjson_is_obj(object))
        return false;
    yyjson_val *issues = yyjson_obj_get(object, "Issues");
    if (issues) {
        if (!yyjson_is_arr(issues) && !yyjson_is_null(issues)) {
            c->malformed++;
            return false;
        }
        dg_mark(c, "golangci_lint");
        size_t i, count;
        yyjson_val *issue;
        yyjson_arr_foreach(issues, i, count, issue) if (!dg_lint_json(c, issue)) c->omitted++;
        return true;
    }
    const char *reason = dg_tag(object, "reason");
    if (reason && !strcmp(reason, "compiler-message"))
        return dg_rust_json(c, yyjson_obj_get(object, "message"), object, "cargo");
    if (reason &&
        (!strcmp(reason, "compiler-artifact") || !strcmp(reason, "build-script-executed"))) {
        dg_mark(c, "cargo");
        c->ignored_events++;
        return true;
    }
    if (reason && !strcmp(reason, "build-finished")) {
        yyjson_val *success = yyjson_obj_get(object, "success");
        if (!yyjson_is_bool(success)) {
            c->malformed++;
            return false;
        }
        dg_mark(c, "cargo");
        if (yyjson_get_bool(success))
            c->ignored_events++;
        else {
            dg_record *r = dg_new(c, "cargo", "cargo_json", "build_failure", "error");
            if (r) {
                dg_set(c, r, DG_MESSAGE, "cargo build failed");
                dg_commit(c, r);
            }
        }
        return true;
    }
    const char *message_type = dg_tag(object, "$message_type");
    if (message_type && !strcmp(message_type, "diagnostic"))
        return dg_rust_json(c, object, NULL,
                            c->options.adapter == FORGE_DIAGNOSTICS_CARGO ? "cargo" : "rustc");
    const char *action = dg_tag(object, "Action");
    static const char *const actions[] = {"start", "run",          "pause",     "cont",
                                          "pass",  "bench",        "fail",      "output",
                                          "skip",  "build-output", "build-fail"};
    bool go = false;
    for (size_t i = 0; action && i < sizeof(actions) / sizeof(actions[0]); i++)
        go |= !strcmp(action, actions[i]);
    if (!go)
        return false;
    bool output = !strcmp(action, "output") || !strcmp(action, "build-output");
    yyjson_val *text = yyjson_obj_get(object, "Output");
    yyjson_val *package = yyjson_obj_get(object, "Package"), *test = yyjson_obj_get(object, "Test");
    if (!package)
        package = yyjson_obj_get(object, "ImportPath");
    if ((output && !yyjson_is_str(text)) || (package && !yyjson_is_str(package)) ||
        (test && !yyjson_is_str(test))) {
        c->malformed++;
        return false;
    }
    dg_mark(c, "go_test");
    dg_stream *stream = dg_go_stream(c, object, output);
    bool has_test = yyjson_is_str(yyjson_obj_get(object, "Test")) &&
                    yyjson_get_len(yyjson_obj_get(object, "Test")) != 0;
    if (output) {
        if (stream)
            dg_go_output(c, stream, yyjson_get_str(text), yyjson_get_len(text));
        else
            c->omitted++;
    } else if (!strcmp(action, "pass")) {
        c->go_pass++;
        if (has_test)
            c->test_pass++;
        else
            c->package_pass++;
    } else if (!strcmp(action, "skip"))
        c->go_skip++;
    else if (!strcmp(action, "fail") || !strcmp(action, "build-fail")) {
        c->go_fail++;
        if (has_test)
            c->test_fail++;
        else
            c->package_fail++;
        if (stream) {
            if (stream->pending.len) {
                dg_text_line(c, &stream->text, stream, stream->pending.data, stream->pending.len);
                stream->pending.len = 0;
            }
            dg_flush(c, &stream->text);
        }
        dg_record *r = dg_new(c, "go_test", "go_test_json",
                              !strcmp(action, "build-fail") ? "build_failure"
                              : has_test                    ? "test_failure"
                                                            : "package_failure",
                              "error");
        if (r) {
            dg_set(c, r, DG_MESSAGE,
                   !strcmp(action, "build-fail") ? "build failed"
                   : has_test                    ? "test failed"
                                                 : "package failed");
            dg_json_field(c, r, DG_PACKAGE, object,
                          yyjson_obj_get(object, "Package") ? "Package" : "ImportPath");
            dg_json_field(c, r, DG_TEST, object, "Test");
            dg_json_field(c, r, DG_CODE, object, "FailedBuild");
            dg_commit(c, r);
        }
    } else
        c->ignored_events++;
    return true;
}

static bool dg_add_string(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
                          const char *value) {
    return value ? yyjson_mut_obj_add_strcpy(doc, object, key, value)
                 : yyjson_mut_obj_add_null(doc, object, key);
}

static char *dg_record_json(dg_context *c, dg_record *r) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        c->failed = true;
        return NULL;
    }
    yyjson_mut_val *object = yyjson_mut_obj(doc), *location = yyjson_mut_obj(doc);
    yyjson_mut_val *details = yyjson_mut_arr(doc), *stack = yyjson_mut_arr(doc);
    bool ok = object && location && details && stack;
    yyjson_mut_doc_set_root(doc, object);
    char fingerprint[17];
    snprintf(fingerprint, sizeof(fingerprint), "%016llx",
             (unsigned long long)fg_hash(r->identity.data, r->identity.len));
    ok = ok && yyjson_mut_obj_add_strcpy(doc, object, "fingerprint", fingerprint) &&
         yyjson_mut_obj_add_str(doc, object, "adapter", r->adapter) &&
         yyjson_mut_obj_add_str(doc, object, "format", r->format) &&
         yyjson_mut_obj_add_str(doc, object, "kind", r->kind) &&
         yyjson_mut_obj_add_str(doc, object, "severity", r->severity);
    static const char *const fields[] = {"message",  "path",   "package", "test",  "thread", "code",
                                         "expected", "actual", "left",    "right", "display"};
    for (size_t i = 0; i < DG_FIELDS; i++)
        if (i != DG_PATH)
            ok = dg_add_string(doc, object, fields[i], r->text[i]) && ok;
    ok = dg_add_string(doc, location, "path", r->text[DG_PATH]) && ok;
    ok = (r->line ? yyjson_mut_obj_add_uint(doc, location, "line", r->line)
                  : yyjson_mut_obj_add_null(doc, location, "line")) &&
         ok;
    ok = (r->column_present ? yyjson_mut_obj_add_uint(doc, location, "column", r->column)
                            : yyjson_mut_obj_add_null(doc, location, "column")) &&
         ok;
    ok = yyjson_mut_obj_add_str(doc, location, "column_unit", r->column_unit) && ok;
    ok = (r->column_origin >= 0
              ? yyjson_mut_obj_add_sint(doc, location, "column_origin", r->column_origin)
              : yyjson_mut_obj_add_null(doc, location, "column_origin")) &&
         ok;
    ok = yyjson_mut_obj_add_val(doc, object, "location", location) && ok;
    for (size_t i = 0; i < r->detail_count; i++)
        ok = r->details[i] && yyjson_mut_arr_add_strcpy(doc, details, r->details[i]) && ok;
    for (size_t i = 0; i < r->frame_count; i++) {
        dg_frame *frame = &r->frames[i];
        yyjson_mut_val *entry = yyjson_mut_obj(doc);
        ok = entry && dg_add_string(doc, entry, "path", frame->path) &&
             dg_add_string(doc, entry, "function", frame->function) &&
             yyjson_mut_obj_add_uint(doc, entry, "line", frame->line) &&
             (frame->column ? yyjson_mut_obj_add_uint(doc, entry, "column", frame->column)
                            : yyjson_mut_obj_add_null(doc, entry, "column")) &&
             yyjson_mut_arr_add_val(stack, entry) && ok;
    }
    ok = yyjson_mut_obj_add_val(doc, object, "details", details) &&
         yyjson_mut_obj_add_val(doc, object, "stack", stack) &&
         yyjson_mut_obj_add_uint(doc, object, "occurrences", r->occurrences) &&
         yyjson_mut_obj_add_bool(doc, object, "truncated", r->truncated) &&
         yyjson_mut_obj_add_bool(doc, object, "bytes_rendered", r->bytes_rendered) && ok;
    char *json = ok ? yyjson_mut_write(doc, 0, NULL) : NULL;
    if (!json)
        c->failed = true;
    yyjson_mut_doc_free(doc);
    return json;
}

static bool dg_incomplete(const dg_context *c) {
    return c->input_truncated || c->line_truncated || c->stream_limited || c->omitted ||
           c->malformed || c->clipped_fields || c->omitted_details;
}

static char *dg_report(dg_context *c) {
    qsort(c->records, c->count, sizeof(*c->records), dg_compare);
    for (size_t i = 1; i < c->adapter_count; i++) {
        const char *name = c->adapters[i];
        size_t j = i;
        while (j && strcmp(c->adapters[j - 1], name) > 0) {
            c->adapters[j] = c->adapters[j - 1];
            j--;
        }
        c->adapters[j] = name;
    }
    fg_buf diagnostics = {0};
    size_t retained = 0;
    for (size_t i = 0; i < c->count && !c->failed; i++) {
        char *json = dg_record_json(c, c->records[i]);
        if (!json)
            break;
        size_t length = strlen(json);
        if (length + (retained ? 1u : 0u) <= c->options.max_json_bytes - 2048 - diagnostics.len) {
            if (retained)
                fg_buf_puts(&diagnostics, ",");
            fg_buf_add(&diagnostics, json, length);
            retained++;
        } else
            c->omitted += c->records[i]->occurrences;
        free(json);
    }
    fg_buf result = {0};
    fg_buf_printf(
        &result,
        "{\"schema_version\":1,\"adapter_hint\":\"%s\",\"input_bytes\":%zu,\"parsed_bytes\":%zu,"
        "\"input_truncated\":%s,\"incomplete\":%s,\"line_truncated\":%s,\"stream_limit_reached\":%"
        "s,"
        "\"ansi_removed\":%s,\"bytes_rendered\":%s,\"adapters\":[",
        dg_hint_name(c->options.adapter), c->input_bytes, c->parsed_bytes,
        c->input_truncated ? "true" : "false", dg_incomplete(c) ? "true" : "false",
        c->line_truncated ? "true" : "false", c->stream_limited ? "true" : "false",
        c->ansi_removed ? "true" : "false", c->bytes_rendered ? "true" : "false");
    for (size_t i = 0; i < c->adapter_count; i++)
        fg_buf_printf(&result, "%s\"%s\"", i ? "," : "", c->adapters[i]);
    fg_buf_printf(
        &result,
        "],\"summary\":{\"diagnostics_seen\":%zu,\"records_retained\":%zu,\"duplicate_"
        "occurrences\":%zu,"
        "\"omitted_diagnostics\":%zu,\"malformed_records\":%zu,\"unrecognized_json_records\":%zu,"
        "\"clipped_fields\":%zu,"
        "\"omitted_details\":%zu,\"ignored_events\":%zu,\"go_pass_events\":%zu,\"go_fail_events\":%"
        "zu,"
        "\"go_skip_events\":%zu,\"test_pass_events\":%zu,\"test_fail_events\":%zu,"
        "\"package_pass_events\":%zu,\"package_fail_events\":%zu},"
        "\"limits\":{\"max_input_bytes\":%zu,\"max_diagnostics\":%zu,\"max_text_bytes\":%zu,"
        "\"max_json_bytes\":%zu},\"diagnostics\":[",
        c->seen, retained, c->duplicates, c->omitted, c->malformed, c->unrecognized_json,
        c->clipped_fields, c->omitted_details, c->ignored_events, c->go_pass, c->go_fail,
        c->go_skip, c->test_pass, c->test_fail, c->package_pass, c->package_fail,
        c->options.max_input_bytes, c->options.max_diagnostics, c->options.max_text_bytes,
        c->options.max_json_bytes);
    fg_buf_add(&result, diagnostics.data ? diagnostics.data : "", diagnostics.len);
    fg_buf_puts(&result, "]}");
    c->failed |= diagnostics.failed || result.failed || result.len > c->options.max_json_bytes;
    fg_buf_clear(&diagnostics);
    if (c->failed) {
        fg_buf_clear(&result);
        return NULL;
    }
    return fg_buf_take(&result);
}

static void dg_generic_json(dg_context *c, dg_text_state *state, const char *bytes, size_t length) {
    size_t take = FG_MIN(length, DG_LINE_BYTES);
    if (take < length)
        c->line_truncated = true;
    c->unrecognized_json++;
    fg_buf plain = {0};
    char *text = dg_clean_line(c, bytes, take, &plain);
    if (text)
        dg_begin_text(c, state, NULL, "generic", "generic_json", "output", "unknown", text, &plain);
    free(text);
    fg_buf_clear(&plain);
}

static void dg_parse_line(dg_context *c, dg_text_state *state, const char *bytes, size_t length) {
    size_t n = FG_MIN(length, DG_LINE_BYTES);
    if (n < length)
        c->line_truncated = true;
    yyjson_doc *doc = yyjson_read(bytes, n, 0);
    yyjson_val *object = doc ? yyjson_doc_get_root(doc) : NULL;
    bool handled = doc && dg_structured(c, object);
    if (!handled) {
        if (doc)
            dg_generic_json(c, state, bytes, n);
        else {
            size_t start = 0;
            while (start < n && isspace((unsigned char)bytes[start]))
                start++;
            if (c->options.adapter != FORGE_DIAGNOSTICS_GENERIC && start < n &&
                (bytes[start] == '{' || bytes[start] == '['))
                c->malformed++;
            dg_text_line(c, state, NULL, bytes, n);
        }
    }
    yyjson_doc_free(doc);
}

char *forge_diagnostics_parse(const char *bytes, size_t length,
                              const forge_diagnostic_options *options, forge_error *error) {
    dg_context c = {0};
    c.options = options ? *options : forge_diagnostics_default_options();
    if ((!bytes && length) || (unsigned)c.options.adapter > FORGE_DIAGNOSTICS_GENERIC ||
        !c.options.max_input_bytes || c.options.max_input_bytes > FG_MAX_JSON ||
        !c.options.max_diagnostics || c.options.max_diagnostics > 4096 ||
        c.options.max_text_bytes < 64 || c.options.max_text_bytes > 16384 ||
        c.options.max_json_bytes < 4096 || c.options.max_json_bytes > FG_MAX_JSON) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid diagnostic input or limits");
        return NULL;
    }
    c.records = calloc(c.options.max_diagnostics, sizeof(*c.records));
    if (!c.records) {
        fg_error(error, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
        return NULL;
    }
    c.input_bytes = length;
    c.parsed_bytes = FG_MIN(length, c.options.max_input_bytes);
    c.input_truncated = c.options.input_truncated || c.parsed_bytes != length;
    const char *input = bytes ? bytes : "";
    yyjson_doc *whole = yyjson_read(input, c.parsed_bytes, 0);
    dg_text_state state = {0};
    if (whole) {
        if (!dg_structured(&c, yyjson_doc_get_root(whole)))
            dg_generic_json(&c, &state, input, c.parsed_bytes);
    } else {
        size_t offset = 0;
        while (offset < c.parsed_bytes && !c.failed) {
            const char *end = memchr(input + offset, '\n', c.parsed_bytes - offset);
            size_t n = end ? (size_t)(end - (input + offset)) : c.parsed_bytes - offset;
            dg_parse_line(&c, &state, input + offset, n);
            offset += n + (end ? 1u : 0u);
        }
    }
    yyjson_doc_free(whole);
    dg_flush(&c, &state);
    for (size_t i = 0; i < c.stream_count; i++) {
        dg_stream *stream = &c.streams[i];
        if (stream->pending.len)
            dg_text_line(&c, &stream->text, stream, stream->pending.data, stream->pending.len);
        dg_flush(&c, &stream->text);
    }
    char *result = c.failed ? NULL : dg_report(&c);
    for (size_t i = 0; i < c.count; i++)
        dg_free(c.records[i]);
    free(c.records);
    for (size_t i = 0; i < c.stream_count; i++) {
        fg_buf_clear(&c.streams[i].identity);
        fg_buf_clear(&c.streams[i].pending);
        free(c.streams[i].package);
        free(c.streams[i].test);
    }
    if (!result)
        fg_error(error, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
    return result;
}

static char *dg_render_value(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    return yyjson_is_str(value) ? fg_render_bytes(yyjson_get_str(value), yyjson_get_len(value))
                                : NULL;
}

static bool dg_render_record(fg_buf *line, yyjson_val *record) {
    char *display = dg_render_value(record, "display"),
         *message = dg_render_value(record, "message");
    char *test = dg_render_value(record, "test"), *package = dg_render_value(record, "package");
    yyjson_val *location = yyjson_obj_get(record, "location");
    char *path = dg_render_value(location, "path");
    const char *severity = dg_tag(record, "severity"), *kind = dg_tag(record, "kind");
    if (!message || !severity || !kind) {
        free(display);
        free(message);
        free(test);
        free(package);
        free(path);
        return false;
    }
    if (display && *display)
        fg_buf_puts(line, display);
    else {
        if (!strcmp(kind, "test_failure") || !strcmp(kind, "build_failure") ||
            !strcmp(kind, "package_failure")) {
            fg_buf_puts(line, "FAIL ");
            if (package && *package)
                fg_buf_printf(line, "%s ", package);
            if (test && *test)
                fg_buf_printf(line, "%s: ", test);
        }
        if (path && *path) {
            fg_buf_puts(line, path);
            yyjson_val *row = yyjson_obj_get(location, "line"),
                       *column = yyjson_obj_get(location, "column");
            if (yyjson_is_uint(row))
                fg_buf_printf(line, ":%llu", (unsigned long long)yyjson_get_uint(row));
            if (yyjson_is_uint(column))
                fg_buf_printf(line, ":%llu", (unsigned long long)yyjson_get_uint(column));
            fg_buf_puts(line, ": ");
        }
        fg_buf_printf(line, "%s: %s", severity, message);
    }
    while (line->len && (line->data[line->len - 1] == '\n' || line->data[line->len - 1] == '\r'))
        line->data[--line->len] = 0;
    uint64_t occurrences = yyjson_get_uint(yyjson_obj_get(record, "occurrences"));
    if (occurrences > 1)
        fg_buf_printf(line, " [x%llu]", (unsigned long long)occurrences);
    fg_buf_puts(line, "\n");
    const char *labels[] = {"expected", "actual", "left", "right"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        char *value = dg_render_value(record, labels[i]);
        if (value) {
            fg_buf_printf(line, "  %s=%s\n", labels[i], value);
            free(value);
        }
    }
    size_t i, count;
    yyjson_val *detail;
    yyjson_arr_foreach(yyjson_obj_get(record, "details"), i, count, detail) {
        if (i == DG_DETAILS)
            break;
        if (yyjson_is_str(detail)) {
            char *value = fg_render_bytes(yyjson_get_str(detail), yyjson_get_len(detail));
            if (!value)
                line->failed = true;
            else {
                fg_buf_printf(line, "  %s\n", value);
                free(value);
            }
        }
    }
    yyjson_val *frame;
    yyjson_arr_foreach(yyjson_obj_get(record, "stack"), i, count, frame) {
        if (i == DG_FRAMES)
            break;
        char *frame_path = dg_render_value(frame, "path"),
             *function = dg_render_value(frame, "function");
        if (frame_path) {
            fg_buf_printf(line, "  at %s:%llu", frame_path,
                          (unsigned long long)yyjson_get_uint(yyjson_obj_get(frame, "line")));
            if (function && *function)
                fg_buf_printf(line, " (%s)", function);
            fg_buf_puts(line, "\n");
        }
        free(frame_path);
        free(function);
    }
    free(display);
    free(message);
    free(test);
    free(package);
    free(path);
    return !line->failed;
}

char *forge_diagnostics_render(const char *json, size_t length, size_t budget, size_t *visible,
                               forge_error *error) {
    if (visible)
        *visible = 0;
    if (!json || !length || length > FG_MAX_JSON || budget < 64 || budget > FG_MAX_JSON) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid diagnostic render request");
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(json, length, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *records = yyjson_obj_get(root, "diagnostics"),
               *summary = yyjson_obj_get(root, "summary");
    if (yyjson_get_uint(yyjson_obj_get(root, "schema_version")) != 1 || !yyjson_is_arr(records) ||
        yyjson_arr_size(records) > 4096 || !yyjson_is_obj(summary) ||
        !yyjson_is_bool(yyjson_obj_get(root, "incomplete"))) {
        yyjson_doc_free(doc);
        fg_error(error, FORGE_ERR_PARSE, "Invalid normalized diagnostic document");
        return NULL;
    }
    fg_buf result = {0};
    bool clipped = false, invalid = false;
    size_t i, count;
    yyjson_val *record;
    yyjson_arr_foreach(records, i, count, record) {
        fg_buf line = {0};
        if (!dg_render_record(&line, record))
            invalid = true;
        if (!invalid) {
            size_t take = fg_utf8_prefix(line.data, line.len, budget - result.len);
            fg_buf_add(&result, line.data, take);
            if (take < line.len)
                clipped = true;
        }
        fg_buf_clear(&line);
        if (invalid || clipped)
            break;
    }
    uint64_t pass = yyjson_get_uint(yyjson_obj_get(summary, "go_pass_events"));
    uint64_t fail = yyjson_get_uint(yyjson_obj_get(summary, "go_fail_events"));
    bool incomplete = yyjson_get_bool(yyjson_obj_get(root, "incomplete"));
    fg_buf footer = {0};
    if (pass || fail)
        fg_buf_printf(&footer, "\nGo events: pass=%llu fail=%llu\n", (unsigned long long)pass,
                      (unsigned long long)fail);
    if (clipped || incomplete || footer.len > budget - result.len) {
        fg_buf marked = {0};
        fg_buf_puts(&marked, "\n[diagnostic view incomplete]\n");
        fg_buf_add(&marked, footer.data, footer.len);
        marked.failed |= footer.failed;
        fg_buf_clear(&footer);
        footer = marked;
    }
    if (footer.len) {
        size_t tail = fg_utf8_prefix(footer.data, footer.len, budget);
        size_t prefix = fg_utf8_prefix(result.data, result.len, budget - tail);
        if (result.data)
            result.data[prefix] = 0;
        result.len = prefix;
        fg_buf_add(&result, footer.data, tail);
    }
    bool failed = result.failed || footer.failed;
    fg_buf_clear(&footer);
    yyjson_doc_free(doc);
    if (invalid || failed) {
        fg_buf_clear(&result);
        fg_error(error, invalid ? FORGE_ERR_PARSE : FORGE_ERR_MEMORY,
                 invalid ? "Invalid diagnostic record" : "Diagnostic render allocation failed");
        return NULL;
    }
    if (visible)
        *visible = result.len;
    return fg_buf_take(&result);
}

uint64_t fg_diagnostic_hash(const char *raw) {
    if (!raw)
        return legacy_diagnostic_hash(NULL);
    forge_diagnostic_options options = forge_diagnostics_default_options();
    options.max_diagnostics = 512;
    char *json = forge_diagnostics_parse(raw, strlen(raw), &options, NULL);
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    if (!doc) {
        free(json);
        return legacy_diagnostic_hash(raw);
    }
    uint64_t hashes[512];
    size_t n = 0, i, count;
    yyjson_val *record;
    yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "diagnostics"), i, count, record) {
        const char *kind = dg_tag(record, "kind"), *fingerprint = dg_tag(record, "fingerprint");
        if (kind && fingerprint && strcmp(kind, "output") && n < 512)
            hashes[n++] = fg_hash(fingerprint, strlen(fingerprint));
    }
    qsort(hashes, n, sizeof(*hashes), compare_hashes);
    size_t unique = 0;
    for (size_t j = 0; j < n; j++)
        if (!j || hashes[j] != hashes[j - 1])
            hashes[unique++] = hashes[j];
    uint64_t result = fg_hash(hashes, unique * sizeof(*hashes));
    yyjson_doc_free(doc);
    free(json);
    return result;
}

char *fg_compress_output(const char *raw, size_t budget, size_t *visible, forge_error *error) {
    if (!raw || budget < 64) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid output budget");
        return NULL;
    }
    char *json = forge_diagnostics_parse(raw, strlen(raw), NULL, error);
    if (!json)
        return NULL;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *adapters = yyjson_obj_get(root, "adapters"),
               *summary = yyjson_obj_get(root, "summary");
    bool go_only = yyjson_arr_size(adapters) == 1 &&
                   !strcmp(yyjson_get_str(yyjson_arr_get_first(adapters)), "go_test");
    bool generic_only = yyjson_arr_size(adapters) == 0 ||
                        (yyjson_arr_size(adapters) == 1 &&
                         !strcmp(yyjson_get_str(yyjson_arr_get_first(adapters)), "generic"));
    /* Preserve the original small-output contract, and its byte-exact Go
     * output-only clipping behavior for embedders. Named multi-record streams
     * use the normalized ranking/deduplication pipeline. */
    bool go_output_only = go_only && !yyjson_get_uint(yyjson_obj_get(summary, "go_pass_events")) &&
                          !yyjson_get_uint(yyjson_obj_get(summary, "go_fail_events")) &&
                          !yyjson_get_uint(yyjson_obj_get(summary, "duplicate_occurrences")) &&
                          yyjson_arr_size(yyjson_obj_get(root, "diagnostics")) <= 1;
    bool raw_bounded = !yyjson_get_bool(yyjson_obj_get(root, "input_truncated"));
    char *result = ((generic_only || go_output_only) && raw_bounded) || budget > FG_MAX_JSON
                       ? legacy_compress_output(raw, budget, visible, error)
                       : forge_diagnostics_render(json, strlen(json), budget, visible, error);
    yyjson_doc_free(doc);
    free(json);
    return result;
}
