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
#include "forge/watch.h"
#include "tools/edit_journal.h"
#include <assert.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

/* A native notification can arrive after an agent has already planned its next
 * action. These tests deliberately deliver NO change events: known tool edits
 * must invalidate old reads themselves, including files outside the index.
 *
 * This executable supplies the forge_watch symbols referenced by monitor.o;
 * static linking must not pull the native watch.o. Everything above that seam
 * (scripted model, agent, context, tools, index, state and sessions) is real.
 * An empty private PATH prevents repository scans/final diffs from launching
 * an installed Git. There is no real model, GPU or native watcher evidence. */
typedef struct fixture fixture;
typedef enum {
    EDIT_NORMAL,
    EDIT_CANCEL,
    EDIT_CONCURRENT,
    EDIT_PREPARE_IO,
    EDIT_OUTCOME_IO,
    EDIT_DENIED,
    EDIT_NOOP,
    EDIT_CONFLICT
} edit_case;
struct forge_watch {
    fixture *owner;
    bool initial, invalidated;
};
struct fixture {
    char base[FG_PATH_MAX], root[FG_PATH_MAX], bin[FG_PATH_MAX];
    char source[FG_PATH_MAX], script[FG_PATH_MAX], relative[64], old_read[256];
    char *old_path;
    forge_agent *agent;
    forge_model *model;
    uint64_t read_id, read_generation, refreshed_generation;
    size_t watches_created, watches_destroyed, watch_polls;
    size_t plans, outputs, results, accepted;
    bool live_read_checked, stale_read_checked, cancel_after_refresh, cancelled;
    edit_case edit_mode;
    size_t prepared, finished;
};
static fixture *active;

static void set_path(const char *value) {
#ifdef _WIN32
    assert(_putenv_s("PATH", value ? value : "") == 0);
#else
    assert((value ? setenv("PATH", value, 1) : unsetenv("PATH")) == 0);
#endif
}
static bool is_cancelled(void *user) {
    return ((fixture *)user)->cancelled;
}

forge_watch_limits forge_default_watch_limits(void) {
    forge_watch_limits limits = {64, 65536, 4095, 64, 64, 1024, 1024};
    return limits;
}
forge_watch *forge_watch_create(const char *root, const forge_watch_limits *limits,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout_ms,
                                forge_error *error) {
    (void)limits;
    assert(active && root && !strcmp(root, active->root));
    assert(cancelled && user && timeout_ms && timeout_ms <= 30000);
    if (cancelled(user)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Test watcher creation cancelled");
        return NULL;
    }
    forge_watch *watch = calloc(1, sizeof(*watch));
    assert(watch);
    watch->owner = active;
    watch->initial = true;
    active->watches_created++;
    if (error)
        memset(error, 0, sizeof(*error));
    return watch;
}
char *forge_watch_poll(forge_watch *watch, uint64_t timeout_ms, forge_cancel_fn cancelled,
                       void *user, forge_error *error) {
    (void)timeout_ms;
    assert(watch && watch->owner == active && cancelled && user);
    if (cancelled(user)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Test watcher poll cancelled");
        return NULL;
    }
    active->watch_polls++;
    fg_buf json = {0};
    fg_buf_printf(&json,
                  "{\"schema_version\":1,\"backend\":\"agent_changes_test_double\",\"events\":[],"
                  "\"rescan_required\":%s,\"initial_scan_required\":%s,\"reopen_required\":%s,"
                  "\"timed_out\":false,\"more_pending\":false,\"reason_flags\":%u,"
                  "\"dropped_events\":0,\"dropped_events_unknown\":false,\"overflow_count\":0,"
                  "\"directories\":2,\"path_encoding\":\"utf-8\"}",
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
    if (watch) {
        assert(watch->owner == active);
        watch->invalidated = true;
    }
}
void forge_watch_destroy(forge_watch *watch) {
    if (watch) {
        assert(watch->owner == active);
        active->watches_destroyed++;
        free(watch);
    }
}

static uint64_t number(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_uint(value));
    return yyjson_get_uint(value);
}
static bool bool_field(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_bool(value));
    return yyjson_get_bool(value);
}
static void assert_file(const char *path, const char *expected) {
    size_t length = 0;
    char *text = fg_read_file(path, FG_MAX_JSON, &length, NULL);
    assert(text && length == strlen(expected) && !memcmp(text, expected, length));
    free(text);
}
static void edit_artifact(fixture *f, const char *name, char path[FG_PATH_MAX]) {
    assert(fg_path_join(path, forge_agent_session(f->agent), name));
}
static void check_edit(fixture *f, const char *type, yyjson_val *data) {
    char path[FG_PATH_MAX];
    assert(number(data, "tool_call") == 2);
    if (!strcmp(type, "edit_prepared")) {
        assert(!f->prepared && !f->finished);
        assert(!strcmp(fg_json_str(data, "path"), f->relative));
        assert(!strcmp(fg_json_str(data, "state"), "prepared"));
        assert(bool_field(data, "before_exists"));
        const char *before = f->old_read + 3;
        const char *after = !strcmp(f->relative, "sub/main.c")
                                ? "int forge_change_value(void) { return 2; }\n"
                                : "updated data\n";
        assert_file(f->source, before); /* Callback runs before replacement. */
        edit_artifact(f, fg_json_str(data, "before_artifact"), path);
        assert_file(path, before);
        edit_artifact(f, fg_json_str(data, "after_artifact"), path);
        assert_file(path, after);
        edit_artifact(f, fg_json_str(data, "diff_artifact"), path);
        char *diff = fg_read_file(path, FG_MAX_JSON, NULL, NULL);
        assert(diff && strstr(diff, "diff --git ") && strstr(diff, after));
        free(diff);
        f->prepared++;
        if (f->edit_mode == EDIT_CANCEL)
            f->cancelled = true;
        else if (f->edit_mode == EDIT_CONCURRENT)
            assert(fg_write_file(f->source, "concurrent data\n", 16, NULL));
        else if (f->edit_mode == EDIT_OUTCOME_IO) {
            edit_artifact(f, fg_json_str(data, "outcome_artifact"), path);
            assert(fg_write_file(path, "exclusive sentinel", 18, NULL));
        }
    } else {
        assert(f->prepared == 1 && !f->finished);
        bool aborted = f->edit_mode == EDIT_CANCEL || f->edit_mode == EDIT_CONCURRENT;
        assert(!strcmp(fg_json_str(data, "state"), aborted ? "aborted" : "applied"));
        const char *expected = f->edit_mode == EDIT_CANCEL       ? "cancelled"
                               : f->edit_mode == EDIT_CONCURRENT ? "conflict"
                                                                 : "ok";
        assert(!strcmp(fg_json_str(data, "status"), expected));
        if (!aborted)
            assert_file(f->source, !strcmp(f->relative, "sub/main.c")
                                       ? "int forge_change_value(void) { return 2; }\n"
                                       : "updated data\n");
        f->finished++;
    }
}
static void check_context(fixture *f, yyjson_val *event_data) {
    size_t turn = (size_t)number(event_data, "turn");
    f->plans++;
    assert(turn == f->plans && turn <= 3);
    if (turn == 1)
        return;
    const char *artifact = fg_json_str(event_data, "artifact");
    char expected[64], path[FG_PATH_MAX];
    snprintf(expected, sizeof(expected), "context/%04zu.json", turn);
    assert(artifact && !strcmp(artifact, expected));
    assert(fg_path_join(path, forge_agent_session(f->agent), artifact));
    char *json = fg_read_file(path, FG_MAX_JSON, NULL, NULL);
    assert(json);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    free(json);
    assert(doc);
    yyjson_val *segments = yyjson_obj_get(yyjson_doc_get_root(doc), "segments");
    assert(yyjson_is_arr(segments));
    yyjson_val *original = NULL, *repo = NULL;
    size_t i, count;
    yyjson_val *item;
    yyjson_arr_foreach(segments, i, count, item) {
        if (number(item, "kind") == FORGE_SEG_REPO)
            repo = item;
        if (number(item, "kind") != FORGE_SEG_RESULT)
            continue;
        const char *text = fg_json_str(item, "text");
        if (text && !strcmp(text, f->old_read)) {
            assert(!original);
            original = item;
        }
    }
    assert(repo && original);
    assert(number(original, "source_hash") == fg_hash(f->relative, strlen(f->relative)));
    if (turn == 2) {
        assert(!bool_field(original, "stale") && bool_field(original, "selected"));
        assert(bool_field(original, "pinned"));
        f->read_id = number(original, "id");
        f->read_generation = number(original, "generation");
        assert(number(repo, "generation") == f->read_generation);
        f->live_read_checked = true;
    } else {
        assert(f->live_read_checked && number(original, "id") == f->read_id);
        assert(number(original, "generation") == f->read_generation);
        assert(bool_field(original, "stale") && !bool_field(original, "selected"));
        assert(!bool_field(original, "pinned"));
        f->refreshed_generation = number(repo, "generation");
        assert(f->refreshed_generation > f->read_generation);
        assert(forge_agent_metrics(f->agent)->filesystem_events == 0);
        f->stale_read_checked = true;
        /* Cancel after observing the postpatch plan but before the next model
         * action. The edit remains recorded; no final may be accepted. */
        if (f->cancel_after_refresh)
            f->cancelled = true;
    }
    yyjson_doc_free(doc);
}
static void on_event(const forge_event *event, void *user) {
    fixture *f = user;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(doc), "data");
    if (!strcmp(event->type, "context_plan"))
        check_context(f, data);
    else if (!strcmp(event->type, "model_output"))
        f->outputs++;
    else if (!strcmp(event->type, "edit_prepared") || !strcmp(event->type, "edit_result"))
        check_edit(f, event->type, data);
    else if (!strcmp(event->type, "tool_result")) {
        const char *status = fg_json_str(data, "status");
        assert(status && !strcmp(status, "ok"));
        f->results++;
    } else if (!strcmp(event->type, "message")) {
        assert(f->stale_read_checked && !f->cancelled);
        assert(yyjson_is_str(data) && !strcmp(yyjson_get_str(data), "Patch applied."));
        f->accepted++;
    } else if (!strcmp(event->type, "file_change") || !strcmp(event->type, "stale_generation") ||
               !strcmp(event->type, "watch_warning")) {
        /* These would hide an accidental dependency on the monitor to make the
         * old read stale. Our fake never emits a file-change signal. */
        assert(!"Unexpected watcher-driven invalidation");
    }
    yyjson_doc_free(doc);
}

static void source_spelling(const char *relative, bool backslashes, char out[64]) {
    assert(strlen(relative) < 64);
    strcpy(out, relative);
    if (backslashes)
        for (char *p = out; *p; p++)
            if (*p == '/')
                *p = '\\';
}
static void write_script(fixture *f, bool read_backslashes, bool patch_backslashes, const char *old,
                         const char *replacement) {
    char read_path[64], patch_path[64];
    source_spelling(f->relative, read_backslashes, read_path);
    source_spelling(f->relative, patch_backslashes, patch_path);
    char *read_json = fg_json_string(read_path), *patch_json = fg_json_string(patch_path);
    char *old_json = fg_json_string(old), *replacement_json = fg_json_string(replacement);
    assert(read_json && patch_json && old_json && replacement_json);
    fg_buf script = {0};
    fg_buf_printf(&script,
                  "[{\"tool\":\"read_file\",\"args\":{\"path\":%s,\"start\":1,\"end\":5}},"
                  "{\"tool\":\"apply_patch\",\"args\":{\"path\":%s,\"old_text\":%s,"
                  "\"new_text\":%s}},{\"final\":\"Patch applied.\"}]",
                  read_json, patch_json, old_json, replacement_json);
    free(read_json);
    free(patch_json);
    free(old_json);
    free(replacement_json);
    assert(!script.failed && fg_write_file(f->script, script.data, script.len, NULL));
    fg_buf_clear(&script);
}
static void create_fixture(fixture *f, bool indexed, bool read_backslashes, bool patch_backslashes,
                           bool cancel_after_refresh) {
    assert(!active);
    memset(f, 0, sizeof(*f));
    char parent[FG_PATH_MAX], id[33], name[80], canonical[FG_PATH_MAX], sub[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(parent), parent);
    assert(length && length < sizeof(parent));
#else
    const char *temp = getenv("TMPDIR");
    int length = snprintf(parent, sizeof(parent), "%s", temp && *temp ? temp : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(parent));
#endif
    assert(fg_random_hex(id, 16));
    snprintf(name, sizeof(name), "forge-agent-changes-%s", id);
    assert(fg_path_join(f->base, parent, name) && fg_mkdir(f->base, NULL));
    assert(fg_workspace(f->base, canonical, NULL));
    strcpy(f->base, canonical);
    assert(fg_path_join(f->root, f->base, "workspace") && fg_mkdir(f->root, NULL));
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
    assert(fg_path_join(sub, f->root, "sub") && fg_mkdir(sub, NULL));
    assert(fg_path_join(f->bin, f->base, "empty-path") && fg_mkdir(f->bin, NULL));
    assert(fg_path_join(f->script, f->base, "actions.json"));
    snprintf(f->relative, sizeof(f->relative), "sub/%s", indexed ? "main.c" : "note.txt");
    assert(fg_path_join(f->source, f->root, f->relative));
    const char *old = indexed ? "int forge_change_value(void) { return 1; }\n" : "original data\n";
    const char *replacement =
        indexed ? "int forge_change_value(void) { return 2; }\n" : "updated data\n";
    snprintf(f->old_read, sizeof(f->old_read), "1: %s", old);
    assert(fg_write_file(f->source, old, strlen(old), NULL));
    write_script(f, read_backslashes, patch_backslashes, old, replacement);
    const char *original_path = getenv("PATH");
    f->old_path = original_path ? fg_strdup(original_path) : NULL;
    assert(!original_path || f->old_path);
    set_path(f->bin);
    f->cancel_after_refresh = cancel_after_refresh;
    active = f;
}
static bool inside(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(root, path, n) && (path[n] == '/' || path[n] == '\\');
}
typedef struct {
    const char *session;
} cleanup;
static bool remove_session_file(const char *relative, void *user) {
    cleanup *c = user;
    char path[FG_PATH_MAX];
    assert(relative && *relative && !strstr(relative, ".."));
    assert(fg_path_join(path, c->session, relative) && inside(c->session, path));
    return remove(path) == 0;
}
static void destroy_fixture(fixture *f) {
    assert(active == f && f->watches_created == 1 && f->watches_destroyed == 1);
    char session[FG_PATH_MAX], path[FG_PATH_MAX];
    snprintf(session, sizeof(session), "%s", forge_agent_session(f->agent));
    assert(inside(f->root, session) && inside(f->base, f->root));
    forge_agent_destroy(f->agent);
    forge_model_destroy(f->model);
    f->agent = NULL;
    f->model = NULL;
    set_path(f->old_path);
    free(f->old_path);
    cleanup c = {session};
    assert(fg_walk(session, "", remove_session_file, &c, NULL));
    const char *session_dirs[] = {"context", "tool", "validation"};
    for (size_t i = 0; i < sizeof(session_dirs) / sizeof(*session_dirs); i++) {
        assert(fg_path_join(path, session, session_dirs[i]) && inside(f->root, path));
        assert(test_rmdir(path) == 0);
    }
    assert(test_rmdir(session) == 0);
    assert(fg_path_join(path, f->root, ".forge/sessions") && test_rmdir(path) == 0);
    const char *metadata[] = {".forge/index.db-wal", ".forge/index.db-shm", ".forge/index.db"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(*metadata); i++) {
        assert(fg_path_join(path, f->root, metadata[i]) && inside(f->root, path));
        assert(remove(path) == 0 || errno == ENOENT);
    }
    assert(fg_path_join(path, f->root, ".forge") && test_rmdir(path) == 0);
    assert(inside(f->root, f->source) && remove(f->source) == 0);
    assert(fg_path_join(path, f->root, "sub") && test_rmdir(path) == 0);
    assert(test_rmdir(f->root) == 0);
    assert(inside(f->base, f->script) && remove(f->script) == 0);
    assert(inside(f->base, f->bin) && test_rmdir(f->bin) == 0);
    assert(test_rmdir(f->base) == 0);
    active = NULL;
}
static void run_case(bool indexed, bool read_backslashes, bool patch_backslashes,
                     bool cancel_after_refresh) {
    fixture f;
    create_fixture(&f, indexed, read_backslashes, patch_backslashes, cancel_after_refresh);
    forge_error error = {0};
    forge_model_config mc = forge_default_model_config();
    mc.script_path = f.script;
    f.model = forge_model_load(&mc, &error);
    assert(f.model);
    forge_agent_config ac = {0};
    ac.workspace = f.root;
    ac.model = f.model;
    ac.limits = forge_default_limits();
    ac.limits.max_turns = 3;
    ac.limits.wall_timeout_ms = 15000;
    ac.allow_write = ac.semantic_output = ac.compact_context = true;
    ac.cancelled = is_cancelled;
    ac.userdata = &f;
    f.agent = forge_agent_create(&ac, &error);
    assert(f.agent);
    forge_status status =
        forge_agent_run(f.agent, "Update the requested file.", on_event, &f, &error);
    forge_status expected = cancel_after_refresh ? FORGE_ERR_CANCELLED : FORGE_OK;
    if (status != expected)
        fprintf(stderr, "agent changes (%s, read=%s, patch=%s, cancel=%d): %s\n", f.relative,
                read_backslashes ? "backslash" : "slash", patch_backslashes ? "backslash" : "slash",
                cancel_after_refresh, error.message);
    assert(status == expected);
    assert(f.live_read_checked && f.stale_read_checked && f.plans == 3 && f.results == 2);
    assert(f.outputs == (cancel_after_refresh ? 2u : 3u));
    assert(f.accepted == (cancel_after_refresh ? 0u : 1u));
    assert(f.prepared == 1 && f.finished == 1);
    const forge_metrics *metrics = forge_agent_metrics(f.agent);
    assert(metrics->simulated && metrics->tool_calls == 2 && metrics->files_modified == 1);
    assert(metrics->filesystem_events == 0 && metrics->watch_reopens == 0);
    assert(metrics->stale_generations == 0 && metrics->validation_commands == 0);
    assert(f.watch_polls >= 5);
    char *actual = fg_read_file(f.source, 1024, NULL, &error);
    assert(actual);
    assert(!strcmp(actual,
                   indexed ? "int forge_change_value(void) { return 2; }\n" : "updated data\n"));
    free(actual);
    destroy_fixture(&f);
}
static void on_edit_error(const forge_event *event, void *user) {
    fixture *f = user;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(doc), "data");
    if (!strcmp(event->type, "edit_prepared") || !strcmp(event->type, "edit_result"))
        check_edit(f, event->type, data);
    else if (!strcmp(event->type, "tool_call") && f->edit_mode == EDIT_PREPARE_IO &&
             !strcmp(fg_json_str(data, "tool"), "apply_patch")) {
        char path[FG_PATH_MAX];
        edit_artifact(f, "tool/000002.after", path);
        assert(fg_write_file(path, "exclusive sentinel", 18, NULL));
    } else if (!strcmp(event->type, "message"))
        f->accepted++;
    else if (!strcmp(event->type, "model_output"))
        f->outputs++;
    yyjson_doc_free(doc);
}
static void check_edit_budget(fixture *f) {
    fg_session session = {0};
    snprintf(session.dir, sizeof(session.dir), "%s", forge_agent_session(f->agent));
    session.events = tmpfile();
    assert(session.events);
    session.edit_bytes_limit = 1;
    fg_tool_context context = {0};
    context.session = &session;
    context.call_id = 900;
    forge_error error = {0};
    fg_edit_record record = {0};
    assert(!fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                            (forge_slice){"after", 5}, &record, &error));
    assert(error.code == FORGE_ERR_LIMIT && !session.edit_bytes_reserved && !record.prepared);
    session.edit_bytes_limit = 1024u * 1024u;
    memset(&error, 0, sizeof(error));
    assert(fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                           (forge_slice){"after", 5}, &record, &error));
    size_t reserved = session.edit_bytes_reserved;
    assert(reserved && record.prepared);
    assert(fg_edit_finish(&context, &record, false, FORGE_ERR_CONFLICT, &error));
    session.edit_bytes_limit = 2 * reserved;
    assert(!fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                            (forge_slice){"after", 5}, &record, &error));
    assert(error.code == FORGE_ERR_IO && !record.prepared && !context.evidence_failed);
    assert(session.edit_bytes_reserved == 2 * reserved); /* Failed writes keep their reservation. */
    context.call_id++;
    assert(!fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                            (forge_slice){"after", 5}, &record, &error));
    assert(error.code == FORGE_ERR_LIMIT && session.edit_bytes_reserved == 2 * reserved);
    char path[FG_PATH_MAX];
    edit_artifact(f, "tool/000900.before", path);
    assert_file(path, "before");
    edit_artifact(f, "tool/000901.before", path);
    assert(!fg_read_file(path, 1024, NULL, NULL));
    assert(fclose(session.events) == 0);
    /* A complete manifest is still not proof of preparation when the event
     * stream refuses its write; no target replacement is authorized. */
    edit_artifact(f, "tool/000900.before", path);
    session.events = fopen(path, "rb");
    assert(session.events);
    session.edit_bytes_limit = 1024u * 1024u;
    context.call_id = 902;
    memset(&error, 0, sizeof(error));
    assert(!fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                            (forge_slice){"after", 5}, &record, &error));
    assert(error.code == FORGE_ERR_IO && !record.prepared && context.evidence_failed);
    assert(fclose(session.events) == 0);
    session.events = tmpfile();
    assert(session.events);
    context.call_id = 903;
    context.evidence_failed = false;
    memset(&error, 0, sizeof(error));
    assert(fg_edit_prepare(&context, "budget.txt", true, (forge_slice){"before", 6},
                           (forge_slice){"after", 5}, &record, &error));
    assert(fclose(session.events) == 0);
    session.events = fopen(path, "rb");
    assert(session.events);
    assert(!fg_edit_finish(&context, &record, false, FORGE_ERR_CONFLICT, &error));
    assert(error.code == FORGE_ERR_IO && context.evidence_failed && record.finished);
    assert(fclose(session.events) == 0);
}
static void run_edit_error(edit_case mode) {
    fixture f;
    create_fixture(&f, false, false, false, false);
    f.edit_mode = mode;
    if (mode == EDIT_NOOP || mode == EDIT_CONFLICT)
        write_script(&f, false, false, mode == EDIT_CONFLICT ? "not in file" : "original data\n",
                     "original data\n");
    forge_error error = {0};
    forge_model_config mc = forge_default_model_config();
    mc.script_path = f.script;
    f.model = forge_model_load(&mc, &error);
    assert(f.model);
    forge_agent_config ac = {0};
    ac.workspace = f.root;
    ac.model = f.model;
    ac.limits = forge_default_limits();
    ac.limits.max_turns = (mode == EDIT_CANCEL || mode == EDIT_OUTCOME_IO) ? 3 : 2;
    ac.limits.wall_timeout_ms = 15000;
    ac.allow_write = mode != EDIT_DENIED;
    ac.cancelled = is_cancelled;
    ac.userdata = &f;
    f.agent = forge_agent_create(&ac, &error);
    assert(f.agent);
    forge_status status =
        forge_agent_run(f.agent, "Exercise edit evidence failure.", on_edit_error, &f, &error);
    forge_status expected = mode == EDIT_CANCEL       ? FORGE_ERR_CANCELLED
                            : mode == EDIT_OUTCOME_IO ? FORGE_ERR_IO
                                                      : FORGE_ERR_LIMIT;
    if (status != expected)
        fprintf(stderr, "edit failure case %d: %s\n", (int)mode, error.message);
    assert(status == expected && f.outputs == 2 && !f.accepted);
    bool prepared = mode == EDIT_CANCEL || mode == EDIT_CONCURRENT || mode == EDIT_OUTCOME_IO;
    assert(f.prepared == (prepared ? 1u : 0u));
    assert(f.finished == (mode == EDIT_CANCEL || mode == EDIT_CONCURRENT ? 1u : 0u));
    bool applied = mode == EDIT_OUTCOME_IO;
    assert(forge_agent_metrics(f.agent)->files_modified == (applied ? 1u : 0u));
    assert_file(f.source, applied                   ? "updated data\n"
                          : mode == EDIT_CONCURRENT ? "concurrent data\n"
                                                    : "original data\n");
    char path[FG_PATH_MAX];
    if (applied || mode == EDIT_PREPARE_IO) {
        edit_artifact(&f, applied ? "tool/000002.edit-result.json" : "tool/000002.after", path);
        assert_file(path, "exclusive sentinel");
    }
    if (!prepared) {
        edit_artifact(&f, "tool/000002.edit.json", path);
        assert(!fg_read_file(path, 1024, NULL, NULL));
    }
    char *state = forge_agent_working_state(f.agent, &error);
    yyjson_doc *doc = state ? yyjson_read(state, strlen(state), 0) : NULL;
    assert(doc);
    yyjson_val *changes = yyjson_obj_get(yyjson_doc_get_root(doc), "observed_changes");
    assert(yyjson_arr_size(changes) == (applied ? 1u : 0u));
    if (applied)
        assert(!strcmp(fg_json_str(yyjson_arr_get(changes, 0), "path"), f.relative));
    yyjson_doc_free(doc);
    free(state);
    if (mode == EDIT_DENIED)
        check_edit_budget(&f);
    destroy_fixture(&f);
}
int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    for (unsigned indexed = 0; indexed < 2; indexed++)
        for (unsigned read_backslashes = 0; read_backslashes < 2; read_backslashes++)
            for (unsigned patch_backslashes = 0; patch_backslashes < 2; patch_backslashes++)
                run_case(indexed != 0, read_backslashes != 0, patch_backslashes != 0, false);
    run_case(false, true, true, true);
    for (edit_case mode = EDIT_CANCEL; mode <= EDIT_CONFLICT; mode++)
        run_edit_error(mode);
    puts("Agent known-change and edit-evidence tests passed (no watch delivery)");
    return 0;
}
