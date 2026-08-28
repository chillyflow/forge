#include "internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

typedef struct {
    char source[FG_PATH_MAX];
    bool native, mutated;
    size_t outputs, accepted, stale;
} fixture;

static void on_event(const forge_event *event, void *user) {
    fixture *f = user;
    yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
    assert(doc);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(doc), "data");
    if (!strcmp(event->type, "repository_scan") && !f->outputs)
        f->native = yyjson_get_bool(yyjson_obj_get(data, "watch_available"));
    if (!strcmp(event->type, "model_output")) {
        f->outputs++;
        if (f->outputs == 2 && f->native) {
            const char *next = "current external source\n";
            assert(fg_write_file(f->source, next, strlen(next), NULL));
            f->mutated = true;
            /* Let native delivery advance before the agent's nonblocking poll.
             * This is a bounded test delay, not a runtime filesystem poll. */
            uint64_t start = fg_now_ms();
            while (fg_now_ms() - start < 300) {
            }
        }
    }
    if (!strcmp(event->type, "stale_generation"))
        f->stale++;
    if (!strcmp(event->type, "message")) {
        assert(yyjson_is_str(data));
        if (f->native)
            assert(!strcmp(yyjson_get_str(data), "Current source inspected."));
        f->accepted++;
    }
    yyjson_doc_free(doc);
}

typedef struct {
    const char *root;
} removal;
static bool remove_file(const char *relative, void *user) {
    removal *r = user;
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, r->root, relative));
    return remove(path) == 0;
}
int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    char base[FG_PATH_MAX], root[FG_PATH_MAX], id[33], name[64], script[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(base), base);
    assert(length && length < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
#endif
    assert(fg_random_hex(id, 16));
    snprintf(name, sizeof(name), "forge-agent-watch-%s", id);
    assert(fg_path_join(root, base, name) && fg_mkdir(root, NULL));
    char canonical[FG_PATH_MAX];
    assert(fg_workspace(root, canonical, NULL));
    snprintf(root, sizeof(root), "%s", canonical);
    fixture f = {0};
    assert(fg_path_join(f.source, root, "source.txt"));
    assert(fg_write_file(f.source, "old source\n", 11, NULL));
    assert(fg_path_join(script, root, "actions.json"));
    const char *actions =
        "[{\"tool\":\"read_file\",\"args\":{\"path\":\"source.txt\",\"start\":1,\"end\":5}},"
        "{\"final\":\"Stale output must not be accepted.\"},"
        "{\"tool\":\"read_file\",\"args\":{\"path\":\"source.txt\",\"start\":1,\"end\":5}},"
        "{\"final\":\"Current source inspected.\"}]";
    assert(fg_write_file(script, actions, strlen(actions), NULL));
    forge_error error = {0};
    forge_model_config mc = forge_default_model_config();
    mc.script_path = script;
    forge_model *model = forge_model_load(&mc, &error);
    assert(model);
    forge_agent_config ac = {0};
    ac.workspace = root;
    ac.model = model;
    ac.limits = forge_default_limits();
    ac.limits.max_turns = 8;
    ac.limits.wall_timeout_ms = 15000;
    ac.semantic_output = ac.compact_context = true;
    forge_agent *agent = forge_agent_create(&ac, &error);
    assert(agent);
    forge_status status =
        forge_agent_run(agent, "Inspect source.txt without editing.", on_event, &f, &error);
    if (status != FORGE_OK)
        fprintf(stderr, "agent: %s\n", error.message);
    assert(status == FORGE_OK);
    if (f.native) {
        assert(f.mutated && f.outputs == 4 && f.accepted == 1 && f.stale == 1);
        const forge_metrics *metrics = forge_agent_metrics(agent);
        assert(metrics->stale_generations == 1 && metrics->filesystem_events > 0);
        char *state = forge_agent_working_state(agent, &error);
        assert(state && strstr(state, "current external source"));
        free(state);
    }
    char session[FG_PATH_MAX];
    snprintf(session, sizeof(session), "%s", forge_agent_session(agent));
    forge_agent_destroy(agent);
    forge_model_destroy(model);
    removal files = {session};
    assert(!strncmp(session, root, strlen(root)));
    assert(session[strlen(root)] == '/' || session[strlen(root)] == '\\');
    assert(fg_walk(session, "", remove_file, &files, &error));
    char path[FG_PATH_MAX];
    const char *folders[] = {"context", "tool", "validation"};
    for (size_t i = 0; i < sizeof(folders) / sizeof(*folders); i++) {
        assert(fg_path_join(path, session, folders[i]));
        assert(test_rmdir(path) == 0);
    }
    assert(test_rmdir(session) == 0);
    assert(fg_path_join(path, root, ".forge/sessions"));
    assert(test_rmdir(path) == 0);
    const char *metadata[] = {".forge/index.db-wal", ".forge/index.db-shm", ".forge/index.db"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(*metadata); i++) {
        assert(fg_path_join(path, root, metadata[i]));
        remove(path);
    }
    assert(fg_path_join(path, root, ".forge"));
    assert(test_rmdir(path) == 0);
    assert(remove(script) == 0 && remove(f.source) == 0);
    assert(test_rmdir(root) == 0);
    puts(f.native ? "Agent rejected stale action after native source change"
                  : "SKIP: native watcher unavailable");
    return f.native ? 0 : 77;
}
