/* Judge instrumentation (record-only) unit tests.
 *
 * Deterministic and GPU-free: no network, no real model, no language
 * toolchain. The scripted agent harness is the test_agent_changes.c pattern
 * (scripted native model, real agent/context/tools/index/session code, watch
 * seam stubbed) and the judge runs through the deterministic transport seam
 * from test_judge.c. Validation planning and execution are real; the
 * interpreter itself is the standalone stub passed as argv[1] (see
 * tests/support/validation_stub.c), copied beside the fixture as python[.exe]
 * on a private PATH.
 *
 * Covered:
 *  - a judge-free session's metrics.json is byte-identical to the
 *    pre-instrumentation output for a fixed metrics value (golden captured
 *    from the pre-change builder);
 *  - a judge-free scripted run's metrics.json keeps the exact pre-change
 *    field sequence and carries no `judge` object;
 *  - a scripted run that triggers judge feedback records the judge counters
 *    in metrics.json and links the judge_feedback event to its turn and
 *    validation attempt. */

#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "internal.h"
#include "forge/judge.h"
#include "forge/watch.h"
#include <assert.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define test_rmdir _rmdir
#define PYTHON_NAME "python.exe"
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_rmdir rmdir
#define PYTHON_NAME "python3"
#endif

static const char *stub_executable; /* argv[1]: the validation interpreter stub. */

/* ---- Watch seam: the monitor must not pull the native watcher. ---- */
struct forge_watch {
    bool initial, invalidated;
};
forge_watch_limits forge_default_watch_limits(void) {
    forge_watch_limits limits = {64, 65536, 4095, 64, 64, 1024, 1024};
    return limits;
}
forge_watch *forge_watch_create(const char *root, const forge_watch_limits *limits,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout_ms,
                                forge_error *error) {
    (void)root;
    (void)limits;
    (void)cancelled;
    (void)user;
    assert(timeout_ms && timeout_ms <= 30000);
    forge_watch *watch = calloc(1, sizeof(*watch));
    assert(watch);
    watch->initial = true;
    if (error)
        memset(error, 0, sizeof(*error));
    return watch;
}
char *forge_watch_poll(forge_watch *watch, uint64_t timeout_ms, forge_cancel_fn cancelled,
                       void *user, forge_error *error) {
    (void)timeout_ms;
    (void)cancelled;
    (void)user;
    assert(watch);
    fg_buf json = {0};
    fg_buf_printf(&json,
                  "{\"schema_version\":1,\"backend\":\"judge_instrumentation_test_double\","
                  "\"events\":[],\"rescan_required\":%s,\"initial_scan_required\":%s,"
                  "\"reopen_required\":%s,\"timed_out\":false,\"more_pending\":false,"
                  "\"reason_flags\":%u,\"dropped_events\":0,\"dropped_events_unknown\":false,"
                  "\"overflow_count\":0,\"directories\":2,\"path_encoding\":\"utf-8\"}",
                  watch->initial || watch->invalidated ? "true" : "false",
                  watch->initial ? "true" : "false", watch->invalidated ? "true" : "false",
                  watch->invalidated ? FORGE_WATCH_RESCAN_CALLER
                  : watch->initial   ? FORGE_WATCH_RESCAN_INITIAL
                                     : 0u);
    assert(!json.failed);
    watch->initial = false;
    if (error)
        memset(error, 0, sizeof(*error));
    return fg_buf_take(&json);
}
void forge_watch_invalidate(forge_watch *watch) {
    if (watch)
        watch->invalidated = true;
}
void forge_watch_destroy(forge_watch *watch) {
    free(watch);
}

/* ---- Fixture ---- */
typedef struct {
    char base[FG_PATH_MAX], root[FG_PATH_MAX], bin[FG_PATH_MAX], script[FG_PATH_MAX];
    char *old_path;
    forge_agent *agent;
    forge_model *model;
} fixture;

static bool cancelled(void *user) {
    (void)user;
    return false;
}
static void set_path(const char *value) {
#ifdef _WIN32
    assert(_putenv_s("PATH", value ? value : "") == 0);
#else
    assert((value ? setenv("PATH", value, 1) : unsetenv("PATH")) == 0);
#endif
}
static void temp_paths(char base[FG_PATH_MAX], char root[FG_PATH_MAX], const char *tag) {
    char parent[FG_PATH_MAX], id[33], name[80], canonical[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(parent), parent);
    assert(length && length < sizeof(parent));
#else
    const char *temp = getenv("TMPDIR");
    int length = snprintf(parent, sizeof(parent), "%s", temp && *temp ? temp : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(parent));
#endif
    assert(fg_random_hex(id, 16));
    snprintf(name, sizeof(name), "forge-judge-instr-%s-%s", tag, id);
    assert(fg_path_join(base, parent, name) && fg_mkdir(base, NULL));
    assert(fg_workspace(base, canonical, NULL));
    strcpy(base, canonical);
    assert(fg_path_join(root, base, "workspace") && fg_mkdir(root, NULL));
    assert(fg_workspace(root, canonical, NULL));
    strcpy(root, canonical);
}
/* The scripted native model replays OpenAI-shaped assistant messages. */
static char *native_action(const char *name, const char *arguments) {
    char *quoted_name = fg_json_string(name), *quoted_arguments = fg_json_string(arguments);
    fg_buf out = {0};
    bool ok = quoted_name && quoted_arguments &&
              fg_buf_printf(&out,
                            "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"type\":"
                            "\"function\",\"function\":{\"name\":%s,\"arguments\":%s}}]}",
                            quoted_name, quoted_arguments);
    free(quoted_name);
    free(quoted_arguments);
    if (!ok) {
        fg_buf_clear(&out);
        return NULL;
    }
    return fg_buf_take(&out);
}
static void write_script(const fixture *f) {
    char *first = native_action("apply_patch",
                                "{\"path\":\"value.py\",\"old_text\":\"return 1\","
                                "\"new_text\":\"return 0\"}");
    char *validate = native_action("validate_candidate", "{}");
    char *second = native_action("apply_patch",
                                 "{\"path\":\"value.py\",\"old_text\":\"return 0\","
                                 "\"new_text\":\"return 2\"}");
    char *finish = native_action("final",
                                 "{\"answer\":\"Repaired value() and validated the unchanged "
                                 "tests.\"}");
    fg_buf script = {0};
    bool ok = first && validate && second && finish &&
              fg_buf_printf(&script, "[%s,%s,%s,%s,%s]", first, validate, second, validate,
                            finish);
    assert(ok && !script.failed);
    assert(fg_write_file(f->script, script.data, script.len, NULL));
    fg_buf_clear(&script);
    free(first);
    free(validate);
    free(second);
    free(finish);
}
static void create_fixture(fixture *f) {
    memset(f, 0, sizeof(*f));
    temp_paths(f->base, f->root, "run");
    assert(fg_path_join(f->bin, f->base, "bin") && fg_mkdir(f->bin, NULL));
    assert(fg_path_join(f->script, f->base, "actions.json"));
    /* A failing test on a changed module: the repair loop below fails one
     * validation, repairs, and passes the next. */
    char source[FG_PATH_MAX], tests[FG_PATH_MAX];
    assert(fg_path_join(source, f->root, "value.py"));
    assert(fg_path_join(tests, f->root, "test_value.py"));
    const char *source_text = "def value():\n    return 1\n";
    const char *tests_text = "import unittest\nfrom value import value\n\n"
                             "class ValueTests(unittest.TestCase):\n"
                             "    def test_value(self):\n"
                             "        self.assertEqual(value(), 2)\n";
    assert(fg_write_file(source, source_text, strlen(source_text), NULL));
    assert(fg_write_file(tests, tests_text, strlen(tests_text), NULL));
    /* Place the deterministic interpreter first on a private PATH. */
    size_t length = 0;
    char *stub = fg_read_file(stub_executable, 64u * 1024u * 1024u, &length, NULL);
    assert(stub && length);
    char interpreter[FG_PATH_MAX];
    assert(fg_path_join(interpreter, f->bin, PYTHON_NAME));
    assert(fg_write_file(interpreter, stub, length, NULL));
    free(stub);
#ifndef _WIN32
    assert(chmod(interpreter, 0755) == 0);
#endif
    const char *original_path = getenv("PATH");
    f->old_path = original_path ? fg_strdup(original_path) : NULL;
    assert(!original_path || f->old_path);
    set_path(f->bin);
    write_script(f);
}
static forge_status run_scripted(fixture *f, forge_judge *judge) {
    forge_error error = {0};
    forge_model_config mc = forge_default_model_config();
    mc.script_path = f->script;
    f->model = forge_model_load(&mc, &error);
    assert(f->model && error.code == FORGE_OK);
    forge_agent_config ac = {0};
    ac.workspace = f->root;
    ac.model = f->model;
    ac.limits = forge_default_limits();
    ac.limits.max_turns = 10;
    ac.limits.wall_timeout_ms = 60000;
    ac.allow_write = ac.allow_exec = true;
    ac.semantic_output = ac.compact_context = true;
    ac.minimal_agent = true;
    ac.candidate_checkpoint = true;
    ac.bounded_repair = true;
    ac.judge = judge;
    ac.cancelled = cancelled;
    ac.userdata = f;
    f->agent = forge_agent_create(&ac, &error);
    assert(f->agent && error.code == FORGE_OK);
    forge_status status =
        forge_agent_run(f->agent, "Repair value() without changing tests, validate, and finish.",
                        NULL, NULL, &error);
    if (status != FORGE_OK)
        fprintf(stderr, "judge instrumentation run: %s (%s)\n", forge_status_string(status),
                error.message);
    return status;
}
static bool remove_file(const char *relative, void *user) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, (const char *)user, relative));
    return remove(path) == 0;
}
static void remove_session_tree(const char *session) {
    assert(fg_walk(session, "", remove_file, (void *)session, NULL));
    const char *subdirs[] = {"context", "tool", "validation"};
    char path[FG_PATH_MAX];
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(*subdirs); i++) {
        assert(fg_path_join(path, session, subdirs[i]) && test_rmdir(path) == 0);
    }
    assert(test_rmdir(session) == 0);
}
static void remove_fixture(fixture *f) {
    char session[FG_PATH_MAX], path[FG_PATH_MAX];
    snprintf(session, sizeof(session), "%s", forge_agent_session(f->agent));
    forge_agent_destroy(f->agent);
    forge_model_destroy(f->model);
    f->agent = NULL;
    f->model = NULL;
    set_path(f->old_path);
    free(f->old_path);
    remove_session_tree(session);
    assert(fg_path_join(path, f->root, ".forge/sessions") && test_rmdir(path) == 0);
    const char *metadata[] = {".forge/index.db-wal", ".forge/index.db-shm", ".forge/index.db"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(*metadata); i++) {
        assert(fg_path_join(path, f->root, metadata[i]));
        assert(remove(path) == 0 || errno == ENOENT);
    }
    assert(fg_path_join(path, f->root, ".forge") && test_rmdir(path) == 0);
    assert(fg_path_join(path, f->root, "value.py") && remove(path) == 0);
    assert(fg_path_join(path, f->root, "test_value.py") && remove(path) == 0);
    assert(test_rmdir(f->root) == 0);
    assert(fg_path_join(path, f->bin, PYTHON_NAME) && remove(path) == 0);
    /* The stub interpreter records its one-time failure beside itself. */
    assert(fg_path_join(path, f->bin, "python_stub.state"));
    assert(remove(path) == 0 || errno == ENOENT);
    assert(test_rmdir(f->bin) == 0);
    assert(remove(f->script) == 0);
    assert(test_rmdir(f->base) == 0);
}

/* ---- Event/metrics readers ---- */
static yyjson_doc *session_event(const char *session, const char *type, size_t occurrence) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, session, "events.jsonl"));
    size_t length = 0;
    char *text = fg_read_file(path, FG_MAX_JSON, &length, NULL);
    assert(text);
    yyjson_doc *found = NULL;
    size_t seen = 0;
    char *p = text;
    while (*p) {
        char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n) {
            yyjson_doc *doc = yyjson_read(p, n, 0);
            assert(doc);
            const char *name = fg_json_str(yyjson_doc_get_root(doc), "type");
            if (name && !strcmp(name, type) && ++seen == occurrence) {
                found = doc;
                doc = NULL;
            }
            yyjson_doc_free(doc);
            if (found)
                break;
        }
        if (!end)
            break;
        p = end + 1;
    }
    free(text);
    return found;
}
static size_t session_event_count(const char *session, const char *type) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, session, "events.jsonl"));
    size_t length = 0;
    char *text = fg_read_file(path, FG_MAX_JSON, &length, NULL);
    assert(text);
    size_t count = 0;
    char *p = text;
    while (*p) {
        char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n) {
            yyjson_doc *doc = yyjson_read(p, n, 0);
            assert(doc);
            const char *name = fg_json_str(yyjson_doc_get_root(doc), "type");
            if (name && !strcmp(name, type))
                count++;
            yyjson_doc_free(doc);
        }
        if (!end)
            break;
        p = end + 1;
    }
    free(text);
    return count;
}
static uint64_t number(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_uint(value));
    return yyjson_get_uint(value);
}
static double real_field(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_real(value) || yyjson_is_num(value));
    return yyjson_get_real(value);
}
static void assert_bytes(const char *actual, const char *expected, const char *label) {
    if (!actual || strcmp(actual, expected)) {
        fprintf(stderr, "%s mismatch\n--- actual ---\n%s\n--- expected ---\n%s\n", label,
                actual ? actual : "(null)", expected);
        assert(!"byte mismatch");
    }
}

/* ---- Test 1: judge-free metrics.json is byte-identical to the pre-change
 * builder's output. The golden was captured from the pre-instrumentation
 * build (commit 17440762) for exactly this metrics value. ---- */
static forge_metrics golden_metrics(void) {
    forge_metrics m = {0};
    m.simulated = true;
    m.prompt_tokens = 1234;
    m.generated_tokens = 567;
    m.cached_tokens = 89;
    m.turns = 3;
    m.tool_calls = 2;
    m.files_modified = 1;
    m.validation_commands = 2;
    m.validation_failures = 1;
    m.duration_ms = 4567.5;
    m.tool_ms = 12.25;
    return m;
}
/* Captured from the pre-instrumentation builder (commit 17440762): the
 * exact bytes fg_metrics_json produced for golden_metrics(). */
static const char *const metrics_golden =
    "{\n"
    "    \"schema_version\": 1,\n"
    "    \"status\": \"ok\",\n"
    "    \"simulated\": true,\n"
    "    \"prompt_tokens\": 1234,\n"
    "    \"generated_tokens\": 567,\n"
    "    \"cached_tokens\": 89,\n"
    "    \"prefill_tokens\": 0,\n"
    "    \"turns\": 3,\n"
    "    \"tool_calls\": 2,\n"
    "    \"raw_tool_bytes\": 0,\n"
    "    \"visible_tool_bytes\": 0,\n"
    "    \"files_modified\": 1,\n"
    "    \"context_evictions\": 0,\n"
    "    \"loop_warnings\": 0,\n"
    "    \"grammar_fast_tokens\": 0,\n"
    "    \"grammar_fallback_tokens\": 0,\n"
    "    \"think_tokens\": 0,\n"
    "    \"forced_actions\": 0,\n"
    "    \"action_stops\": 0,\n"
    "    \"action_select_tokens\": 0,\n"
    "    \"action_argument_tokens\": 0,\n"
    "    \"patch_tokens\": 0,\n"
    "    \"final_tokens\": 0,\n"
    "    \"memory_tokens\": 0,\n"
    "    \"forced_action_progress_tokens\": 0,\n"
    "    \"raw_tool_tokens\": 0,\n"
    "    \"visible_tool_tokens\": 0,\n"
    "    \"files_opened\": 0,\n"
    "    \"validation_commands\": 2,\n"
    "    \"validation_failures\": 1,\n"
    "    \"generation_arena_peak_bytes\": 0,\n"
    "    \"repo_full_scans\": 0,\n"
    "    \"repo_delta_scans\": 0,\n"
    "    \"filesystem_events\": 0,\n"
    "    \"watch_reopens\": 0,\n"
    "    \"stale_generations\": 0,\n"
    "    \"index_cold_parses\": 0,\n"
    "    \"index_incremental_parses\": 0,\n"
    "    \"index_cache_hits\": 0,\n"
    "    \"index_cache_evictions\": 0,\n"
    "    \"peak_index_source_bytes\": 0,\n"
    "    \"peak_index_nodes\": 0,\n"
    "    \"checkpoint_lookups\": 0,\n"
    "    \"checkpoint_hits\": 0,\n"
    "    \"checkpoint_misses\": 0,\n"
    "    \"checkpoint_captures\": 0,\n"
    "    \"checkpoint_evictions\": 0,\n"
    "    \"checkpoint_restored_tokens\": 0,\n"
    "    \"checkpoint_reused_tokens\": 0,\n"
    "    \"checkpoint_additional_tokens\": 0,\n"
    "    \"checkpoint_peak_bytes\": 0,\n"
    "    \"load_ms\": 0.0,\n"
    "    \"prefill_ms\": 0.0,\n"
    "    \"decode_ms\": 0.0,\n"
    "    \"sampling_ms\": 0.0,\n"
    "    \"duration_ms\": 4567.5,\n"
    "    \"tool_ms\": 12.25,\n"
    "    \"validation_ms\": 0.0,\n"
    "    \"index_ms\": 0.0,\n"
    "    \"checkpoint_probe_ms\": 0.0,\n"
    "    \"checkpoint_capture_ms\": 0.0,\n"
    "    \"checkpoint_restore_ms\": 0.0\n"
    "}";
static void test_metrics_json_without_judge_is_byte_identical(void) {
    char base[FG_PATH_MAX], root[FG_PATH_MAX];
    temp_paths(base, root, "golden");
    forge_error error = {0};
    fg_session session = {0};
    assert(fg_session_start(&session, root, NULL, NULL, &error));
    forge_metrics m = golden_metrics();
    assert(fg_session_finish(&session, &m, FORGE_OK, &error));
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, session.dir, "metrics.json"));
    char *file = fg_read_file(path, FG_MAX_JSON, NULL, NULL);
    assert(file);
    assert_bytes(file, metrics_golden, "judge-free metrics.json");
    assert(!strstr(file, "\"judge\""));
    char *direct = fg_metrics_json(&m, FORGE_OK);
    assert(direct);
    assert_bytes(direct, metrics_golden, "fg_metrics_json");
    /* The done event data is the same document. */
    yyjson_doc *done = session_event(session.dir, "done", 1);
    assert(done);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(done), "data");
    char *data_json = yyjson_val_write(data, YYJSON_WRITE_PRETTY, NULL);
    assert(data_json);
    assert_bytes(data_json, metrics_golden, "done event data");
    free(data_json);
    yyjson_doc_free(done);
    free(direct);
    free(file);
    remove_session_tree(session.dir);
    assert(fg_path_join(path, root, ".forge/sessions") && test_rmdir(path) == 0);
    assert(fg_path_join(path, root, ".forge") && test_rmdir(path) == 0);
    assert(test_rmdir(root) == 0);
    assert(test_rmdir(base) == 0);
}

/* ---- Test 2: a judge-free run keeps the pre-change field sequence. ---- */
static void test_judge_free_run_metrics_are_unchanged(void) {
    fixture f;
    create_fixture(&f);
    assert(run_scripted(&f, NULL) == FORGE_OK);
    const char *session = forge_agent_session(f.agent);
    assert(session_event_count(session, "judge_feedback") == 0);
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, session, "metrics.json"));
    char *text = fg_read_file(path, FG_MAX_JSON, NULL, NULL);
    assert(text);
    assert(!strstr(text, "\"judge\""));
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    assert(doc);
    /* Field-compare against the pre-instrumentation shape: the exact key
     * sequence the pre-change builder wrote, with no additions. */
    static const char *const fields[] = {
        "schema_version", "status", "simulated", "prompt_tokens", "generated_tokens",
        "cached_tokens", "prefill_tokens", "turns", "tool_calls", "raw_tool_bytes",
        "visible_tool_bytes", "files_modified", "context_evictions", "loop_warnings",
        "grammar_fast_tokens", "grammar_fallback_tokens", "think_tokens", "forced_actions",
        "action_stops", "action_select_tokens", "action_argument_tokens", "patch_tokens",
        "final_tokens", "memory_tokens", "forced_action_progress_tokens", "raw_tool_tokens",
        "visible_tool_tokens", "files_opened", "validation_commands", "validation_failures",
        "generation_arena_peak_bytes", "repo_full_scans", "repo_delta_scans", "filesystem_events",
        "watch_reopens", "stale_generations", "index_cold_parses", "index_incremental_parses",
        "index_cache_hits", "index_cache_evictions", "peak_index_source_bytes", "peak_index_nodes",
        "checkpoint_lookups", "checkpoint_hits", "checkpoint_misses", "checkpoint_captures",
        "checkpoint_evictions", "checkpoint_restored_tokens", "checkpoint_reused_tokens",
        "checkpoint_additional_tokens", "checkpoint_peak_bytes", "load_ms", "prefill_ms",
        "decode_ms", "sampling_ms", "duration_ms", "tool_ms", "validation_ms", "index_ms",
        "checkpoint_probe_ms", "checkpoint_capture_ms", "checkpoint_restore_ms"};
    size_t index = 0;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(yyjson_doc_get_root(doc), &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = yyjson_get_str(key);
        assert(index < sizeof(fields) / sizeof(*fields));
        assert(name && !strcmp(name, fields[index]));
        index++;
    }
    assert(index == sizeof(fields) / sizeof(*fields));
    yyjson_doc_free(doc);
    free(text);
    remove_fixture(&f);
}

/* ---- Test 3: a run that triggers judge feedback records counters and links
 * the event to its episode. ---- */
static const char *const judge_response =
    "{\"model\":\"jev-feedback-test\",\"answers\":{"
    "\"failure_family\":{\"type\":\"choice\",\"choice\":\"code_defect\",\"probabilities\":{"
    "\"code_defect\":0.8,\"test_or_requirement_mismatch\":0.05,"
    "\"environment_or_dependency\":0.05,\"missing_evidence\":0.05,\"unknown\":0.05},"
    "\"confidence\":0.78},"
    "\"evidence_gap\":{\"type\":\"noul\",\"noul\":0.2},"
    "\"repair_readiness\":{\"type\":\"score\",\"score\":2.6,\"legend\":{\"0\":\"No actionable "
    "signal\",\"1\":\"Needs more inspection\",\"2\":\"Concrete suspect\","
    "\"3\":\"Actionable repair likely\"},\"probabilities\":{\"0\":0.0,\"1\":0.05,\"2\":0.3,"
    "\"3\":0.65},\"confidence\":0.7},"
    "\"next_action\":{\"type\":\"choice\",\"choice\":\"patch_code\",\"probabilities\":{"
    "\"inspect_source\":0.1,\"patch_code\":0.8,\"run_validation\":0.05,\"defer\":0.05},"
    "\"confidence\":0.76}},"
    "\"usage\":{\"input_tokens\":222,\"output_tokens\":0}}";
typedef struct {
    size_t calls;
    char request[65536];
    size_t request_len;
} judge_stub;
static forge_status judge_transport(const char *request, size_t request_len, char **response,
                                    size_t *response_len, void *userdata, forge_error *e) {
    judge_stub *s = userdata;
    if (s->calls == 0) {
        assert(request_len < sizeof(s->request));
        memcpy(s->request, request, request_len);
        s->request[request_len] = 0;
        s->request_len = request_len;
    }
    s->calls++;
    size_t length = strlen(judge_response);
    char *copy = malloc(length + 1);
    if (!copy)
        return fg_error(e, FORGE_ERR_MEMORY, "stub allocation failed");
    memcpy(copy, judge_response, length + 1);
    *response = copy;
    *response_len = length;
    return FORGE_OK;
}
static void test_judge_run_records_counters_and_links_feedback(void) {
    fixture f;
    create_fixture(&f);
    judge_stub stub = {0};
    forge_judge_options options = {0};
    options.transport = judge_transport;
    options.transport_userdata = &stub;
    options.timeout_ms = 500;
    forge_error error = {0};
    forge_judge *judge = forge_judge_create(&options, &error);
    assert(judge && error.code == FORGE_OK);
    assert(run_scripted(&f, judge) == FORGE_OK);
    const char *session = forge_agent_session(f.agent);

    /* The failed candidate validation produced exactly one linked event. */
    assert(session_event_count(session, "judge_feedback") == 1);
    yyjson_doc *feedback = session_event(session, "judge_feedback", 1);
    assert(feedback);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(feedback), "data");
    assert(data);
    assert(number(data, "turn") == 2); /* The scripted validate_candidate ran on turn 2. */
    assert(number(data, "candidate_attempts") == 1);
    assert(number(data, "validation_id") == 1);
    assert(!strcmp(fg_json_str(data, "model"), "jev-feedback-test"));
    assert(!strcmp(fg_json_str(data, "failure_family"), "code_defect"));
    assert(!strcmp(fg_json_str(data, "next_action"), "patch_code"));
    assert(number(data, "input_tokens") == 222 && number(data, "output_tokens") == 0);
    yyjson_doc_free(feedback);

    /* metrics.json carries the counters forge_judge_metrics reports. */
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, session, "metrics.json"));
    char *text = fg_read_file(path, FG_MAX_JSON, NULL, NULL);
    assert(text);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    assert(doc);
    yyjson_val *recorded = yyjson_obj_get(yyjson_doc_get_root(doc), "judge");
    assert(recorded);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 0 && stats.candidates_scored == 0);
    assert(stats.input_tokens == 222 && stats.output_tokens == 0);
    assert(!strcmp(stats.last_model, "jev-feedback-test"));
    assert(number(recorded, "calls") == stats.calls);
    assert(number(recorded, "failures") == stats.failures);
    assert(number(recorded, "candidates_scored") == stats.candidates_scored);
    assert(number(recorded, "input_tokens") == stats.input_tokens);
    assert(number(recorded, "output_tokens") == stats.output_tokens);
    assert(real_field(recorded, "last_latency_ms") == stats.last_latency_ms);
    assert(real_field(recorded, "total_latency_ms") == stats.total_latency_ms);
    assert(stats.total_latency_ms >= stats.last_latency_ms);
    assert(!strcmp(fg_json_str(recorded, "last_model"), stats.last_model));
    yyjson_doc_free(doc);
    free(text);

    /* The judge saw the failed episode it was asked about. */
    assert(stub.calls == 1 && stub.request_len);
    assert(strstr(stub.request, "\"value.py\""));
    assert(strstr(stub.request, "Repair value() without changing tests, validate, and finish."));
    assert(strstr(stub.request, "AssertionError"));
    forge_judge_destroy(judge);
    remove_fixture(&f);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    assert(argc > 1);
    stub_executable = argv[1];
    test_metrics_json_without_judge_is_byte_identical();
    test_judge_free_run_metrics_are_unchanged();
    test_judge_run_records_counters_and_links_feedback();
    puts("Judge instrumentation tests passed (scripted model, stub interpreter, no network)");
    return 0;
}
