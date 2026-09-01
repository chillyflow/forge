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
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define EXECUTABLE_SUFFIX ".exe"
#define remove_directory _rmdir
#else
#include <unistd.h>
#define EXECUTABLE_SUFFIX ""
#define remove_directory rmdir
#endif

/* This suite tests orchestration, not Go correctness. Copies of this executable
 * stand in for go/gofmt, in a trusted sibling outside each temporary workspace.
 * The ordinary integration suite separately uses the real Go toolchain. */
typedef enum { NORMAL, DELAY_POLICY, DELAY_START_EVENT, DENY_POLICY } callback_mode;
typedef struct {
    char base[FG_PATH_MAX], root[FG_PATH_MAX], bin[FG_PATH_MAX];
    fg_tool_context tools;
    fg_session session;
    forge_metrics metrics;
    fg_validation_result result;
    forge_error error;
    callback_mode mode;
    size_t policy_calls, command_starts, command_results;
    char *result_event;
    const char *blocked_artifact;
} fixture;

static char self[FG_PATH_MAX];

static void set_path(const char *value) {
#ifdef _WIN32
    assert(_putenv_s("PATH", value ? value : "") == 0);
#else
    assert((value ? setenv("PATH", value, 1) : unsetenv("PATH")) == 0);
#endif
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void delete_if_present(const char *directory, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, directory, relative));
    if (exists(path))
        assert(remove(path) == 0);
}

static void delete_directory(const char *directory, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, directory, relative));
    assert(remove_directory(path) == 0);
}

static void write_file(const char *directory, const char *relative, const char *text) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, directory, relative));
    assert(fg_write_file(path, text, strlen(text), NULL));
}

static void copy_helper(const char *directory, const char *name) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, directory, name));
    FILE *input = fopen(self, "rb"), *output = fopen(path, "wb");
    assert(input && output);
    char buffer[16384];
    size_t length;
    while ((length = fread(buffer, 1, sizeof(buffer), input)) != 0)
        assert(fwrite(buffer, 1, length, output) == length);
    assert(!ferror(input));
    assert(fclose(input) == 0);
    assert(fclose(output) == 0);
#ifndef _WIN32
    assert(chmod(path, 0700) == 0);
#endif
}

static void exhaust_deadline(fixture *f) {
    /* Give this callback a short remaining budget, then actually consume it.
     * Indexing/snapshot speed cannot cause expiry before the tested callback. */
    f->tools.deadline = fg_now_ms() + 10;
#ifdef _WIN32
    Sleep(40);
#else
    struct timespec remaining = {0, 40000000};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
#endif
    assert(fg_now_ms() >= f->tools.deadline);
}

static bool policy(const char *tool, forge_capability capability, const char *arguments,
                   void *userdata) {
    fixture *f = userdata;
    assert(!strcmp(tool, "run_command") && capability == FORGE_CAP_PROCESS);
    yyjson_doc *doc = yyjson_read(arguments, strlen(arguments), 0);
    assert(doc && yyjson_is_arr(yyjson_obj_get(yyjson_doc_get_root(doc), "argv")));
    yyjson_doc_free(doc);
    f->policy_calls++;
    if (f->mode == DELAY_POLICY)
        exhaust_deadline(f);
    return f->mode != DENY_POLICY;
}

static void event(const forge_event *value, void *userdata) {
    fixture *f = userdata;
    if (!strcmp(value->type, "validation_command_start")) {
        f->command_starts++;
        if (f->mode == DELAY_START_EVENT)
            exhaust_deadline(f);
    } else if (!strcmp(value->type, "validation_command")) {
        f->command_results++;
    } else if (!strcmp(value->type, "validation_result")) {
        free(f->result_event);
        f->result_event = fg_strdup(value->json);
        assert(f->result_event);
    }
}

static void fixture_start(fixture *f, callback_mode mode) {
    memset(f, 0, sizeof(*f));
    f->mode = mode;
    char parent[FG_PATH_MAX], id[33], name[80];
#ifdef _WIN32
    DWORD length = GetTempPathA(sizeof(parent), parent);
    assert(length && length < sizeof(parent));
#else
    const char *temp = getenv("TMPDIR");
    int length = snprintf(parent, sizeof(parent), "%s", temp && *temp ? temp : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(parent));
#endif
    assert(fg_random_hex(id, 16));
    snprintf(name, sizeof(name), "forge-verification-%s", id);
    assert(fg_path_join(f->base, parent, name));
    assert(fg_mkdir(f->base, &f->error));
    assert(fg_path_join(f->root, f->base, "workspace"));
    assert(fg_path_join(f->bin, f->base, "trusted-bin"));
    assert(fg_mkdir(f->root, &f->error));
    assert(fg_mkdir(f->bin, &f->error));
    copy_helper(f->bin, "go" EXECUTABLE_SUFFIX);
    copy_helper(f->bin, "gofmt" EXECUTABLE_SUFFIX);
    /* This also prevents repository indexing from finding a host git binary. */
    set_path(f->bin);
    write_file(f->root, "go.mod", "module verification.test/fixture\n\ngo 1.22\n");
    write_file(f->root, "main.go", "package fixture\n\nfunc Value() int { return 1 }\n");
    write_file(f->root, "fixture.txt", "original fixture\n");
    f->tools.repo = forge_repo_open(f->root, &f->error);
    assert(f->tools.repo);
    assert(forge_repo_index(f->tools.repo, &f->error) == FORGE_OK);
    assert(fg_workspace(f->root, f->tools.root, &f->error));
    assert(fg_session_start(&f->session, f->root, event, f, &f->error));
    f->tools.session = &f->session;
    f->tools.config.workspace = f->root;
    f->tools.config.allow_exec = true;
    f->tools.config.policy = policy;
    f->tools.config.userdata = f;
    f->tools.config.limits.command_timeout_ms = 5000;
    f->tools.config.limits.wall_timeout_ms = 30000;
    f->tools.config.limits.max_tool_bytes = 4096;
    f->tools.deadline = fg_now_ms() + 30000;
}

static void fixture_finish(fixture *f) {
    assert(fclose(f->session.events) == 0);
    f->session.events = NULL;
    forge_repo_close(f->tools.repo);
    /* Remove only known fixture artifacts; no recursive filesystem deletion. */
    if (f->blocked_artifact)
        delete_directory(f->session.dir, f->blocked_artifact);
    assert(f->result.commands < 16);
    for (unsigned i = 1; i <= 16; i++) {
        char name[96];
        snprintf(name, sizeof(name), "validation/0001-%04u.stdout", i);
        delete_if_present(f->session.dir, name);
        snprintf(name, sizeof(name), "validation/0001-%04u.stderr", i);
        delete_if_present(f->session.dir, name);
    }
    const char *session_files[] = {"validation/0001.plan.json", "validation/0001.json",
                                   "validation/latest.json", "events.jsonl"};
    for (size_t i = 0; i < sizeof(session_files) / sizeof(session_files[0]); i++)
        delete_if_present(f->session.dir, session_files[i]);
    delete_directory(f->session.dir, "tool");
    delete_directory(f->session.dir, "context");
    delete_directory(f->session.dir, "validation");
    assert(remove_directory(f->session.dir) == 0);
    const char *root_files[] = {"go.mod",
                                "main.go",
                                "fixture.txt",
                                ".forge/probe-runs",
                                ".forge/mutate-fixture",
                                ".forge/index.db-wal",
                                ".forge/index.db-shm",
                                ".forge/index.db"};
    for (size_t i = 0; i < sizeof(root_files) / sizeof(root_files[0]); i++)
        delete_if_present(f->root, root_files[i]);
    delete_directory(f->root, ".forge/sessions");
    delete_directory(f->root, ".forge");
    assert(remove_directory(f->root) == 0);
    delete_if_present(f->bin, "go" EXECUTABLE_SUFFIX);
    delete_if_present(f->bin, "gofmt" EXECUTABLE_SUFFIX);
    assert(remove_directory(f->bin) == 0);
    assert(remove_directory(f->base) == 0);
    fg_validation_result_free(&f->result);
    free(f->result_event);
}

static yyjson_doc *read_json(const char *directory, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, directory, relative));
    size_t length = 0;
    char *text = fg_read_file(path, 1024 * 1024, &length, NULL);
    assert(text);
    yyjson_doc *doc = yyjson_read(text, length, 0);
    free(text);
    assert(doc);
    return doc;
}

static bool json_boolean(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_bool(value));
    return yyjson_get_bool(value);
}

static size_t probe_runs(fixture *f) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, ".forge/probe-runs"));
    if (!exists(path))
        return 0;
    size_t length = 0, count = 0;
    char *text = fg_read_file(path, 4096, &length, NULL);
    assert(text);
    for (size_t i = 0; i < length; i++)
        count += text[i] == '\n';
    free(text);
    return count;
}

static yyjson_doc *run_paths(fixture *f, const char *const *changed, size_t changed_count,
                             forge_status expected) {
    memset(&f->error, 0, sizeof(f->error));
    forge_status status =
        fg_validation_run(&f->tools, changed, changed_count, &f->metrics, &f->result, &f->error);
    if (status != expected)
        fprintf(stderr, "verification status=%s expected=%s: %s\n", forge_status_string(status),
                forge_status_string(expected), f->error.message);
    assert(status == expected);
    assert(f->result.json && f->result.summary);
    yyjson_doc *doc = yyjson_read(f->result.json, strlen(f->result.json), 0);
    assert(doc);
    yyjson_val *report = yyjson_doc_get_root(doc);
    assert(json_boolean(report, "applicable"));
    assert(json_boolean(report, "passed") == f->result.passed);
    assert(yyjson_get_uint(yyjson_obj_get(report, "commands_run")) == f->result.commands);
    assert(f->metrics.validation_commands == f->result.commands);
    assert(f->metrics.validation_failures == (expected == FORGE_OK ? 0u : 1u));
    assert(!strcmp(fg_json_str(report, "status"), forge_status_string(expected)));
    return doc;
}

static yyjson_doc *run(fixture *f, forge_status expected) {
    const char *changed[] = {"main.go"};
    return run_paths(f, changed, 1, expected);
}

static void expect_no_launch(fixture *f, yyjson_val *report) {
    assert(!f->result.passed && f->result.commands == 0 && probe_runs(f) == 0);
    assert(!json_boolean(report, "checks_passed"));
    yyjson_val *commands = yyjson_obj_get(report, "commands");
    assert(yyjson_arr_size(commands) == 1);
    yyjson_val *command = yyjson_arr_get_first(commands);
    assert(yyjson_get_uint(yyjson_obj_get(command, "id")) == 1);
    assert(!json_boolean(command, "started"));
    assert(yyjson_get_sint(yyjson_obj_get(command, "exit_code")) == -1);
    assert(f->policy_calls == 1 && f->command_results == 1);
}

static void test_deadline_callbacks(void) {
    const callback_mode modes[] = {DELAY_POLICY, DELAY_START_EVENT};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        fixture f;
        fixture_start(&f, modes[i]);
        yyjson_doc *doc = run(&f, FORGE_ERR_CANCELLED);
        expect_no_launch(&f, yyjson_doc_get_root(doc));
        if (modes[i] == DELAY_START_EVENT)
            assert(f.command_starts == 1);
        yyjson_doc_free(doc);
        fixture_finish(&f);
    }
}

static void test_denial_and_resolution_failure(void) {
    fixture f;
    fixture_start(&f, DENY_POLICY);
    yyjson_doc *doc = run(&f, FORGE_ERR_POLICY);
    expect_no_launch(&f, yyjson_doc_get_root(doc));
    assert(f.command_starts == 0);
    yyjson_doc_free(doc);
    fixture_finish(&f);

    fixture_start(&f, NORMAL);
    set_path(f.base); /* No executable exists in this sibling of the workspace. */
    doc = run(&f, FORGE_ERR_NOT_FOUND);
    expect_no_launch(&f, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    fixture_finish(&f);
}

static void test_blocked_plan_diagnostics(void) {
    fixture f;
    fixture_start(&f, NORMAL);
    write_file(f.root, "module.py", "def value():\n    return 1\n");
    write_file(f.root, "test_module.py",
               "import unittest\n\nclass ModuleTest(unittest.TestCase):\n    pass\n");
    assert(forge_repo_index(f.tools.repo, &f.error) == FORGE_OK);

    const char *changed[] = {"module.py"};
    yyjson_doc *doc = run_paths(&f, changed, 1, FORGE_ERR_NOT_FOUND);
    yyjson_val *report = yyjson_doc_get_root(doc);
    assert(!f.result.passed && f.result.commands == 0 && probe_runs(&f) == 0);
    assert(f.policy_calls == 0 && f.command_starts == 0 && f.command_results == 0);
    assert(yyjson_arr_size(yyjson_obj_get(report, "commands")) == 0);
    assert(strstr(f.result.summary, "Missing required tools: python"));
    assert(strstr(f.result.summary, "python_executable_unavailable"));
    assert(strstr(f.result.summary, "python3/python/py was not found"));
    assert(strstr(f.error.message, "python_executable_unavailable"));
    assert(!strcmp(fg_json_str(report, "summary"), f.result.summary));
    yyjson_doc_free(doc);
    delete_if_present(f.root, "module.py");
    delete_if_present(f.root, "test_module.py");
    fixture_finish(&f);
}

static void test_persistence_failure(const char *blocked_artifact) {
    fixture f;
    fixture_start(&f, NORMAL);
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f.session.dir, blocked_artifact));
    assert(fg_mkdir(path, &f.error));
    f.blocked_artifact = blocked_artifact;
    yyjson_doc *doc = run(&f, FORGE_ERR_IO);
    yyjson_val *report = yyjson_doc_get_root(doc);
    assert(!f.result.passed && !json_boolean(report, "evidence_complete"));
    assert(f.result.commands > 0 && probe_runs(&f) == f.result.commands);
    bool output_failure = strstr(blocked_artifact, ".stdout") != NULL;
    assert(json_boolean(report, "checks_passed") == !output_failure);
    if (strcmp(blocked_artifact, "validation/0001.json")) {
        /* In particular, failing latest.json must revise an already-written
         * per-attempt report rather than leaving stale success evidence. */
        yyjson_doc *saved = read_json(f.session.dir, "validation/0001.json");
        yyjson_val *persisted = yyjson_doc_get_root(saved);
        assert(!json_boolean(persisted, "passed") && !json_boolean(persisted, "evidence_complete"));
        assert(!strcmp(fg_json_str(persisted, "status"), "io"));
        assert(yyjson_get_uint(yyjson_obj_get(persisted, "commands_run")) == f.result.commands);
        yyjson_doc_free(saved);
    }
    yyjson_doc_free(doc);
    fixture_finish(&f);
}

static void test_success_and_input_mutation(void) {
    for (unsigned mutate = 0; mutate < 2; mutate++) {
        fixture f;
        fixture_start(&f, NORMAL);
        if (mutate)
            write_file(f.root, ".forge/mutate-fixture", "yes\n");
        uint64_t generation = forge_repo_generation(f.tools.repo);
        yyjson_doc *doc = run(&f, mutate ? FORGE_ERR_CONFLICT : FORGE_OK);
        yyjson_val *report = yyjson_doc_get_root(doc);
        assert(f.result.commands > 0 && probe_runs(&f) == f.result.commands);
        assert(f.result.passed == !mutate);
        assert(f.result.inputs_changed == (mutate != 0));
        assert(json_boolean(report, "inputs_changed") == (mutate != 0));
        assert(json_boolean(report, "evidence_complete"));
        /* fixture.txt is outside the indexed source extensions: only the full
         * input snapshot catches this modification, not the source generation. */
        assert(forge_repo_generation(f.tools.repo) == generation);
        size_t i, count;
        yyjson_val *command;
        yyjson_arr_foreach(yyjson_obj_get(report, "commands"), i, count, command) {
            assert(json_boolean(command, "started"));
            assert(yyjson_get_sint(yyjson_obj_get(command, "exit_code")) == 0);
        }
        assert(f.result_event);
        yyjson_doc *event_doc = yyjson_read(f.result_event, strlen(f.result_event), 0);
        assert(event_doc);
        yyjson_val *event_report = yyjson_obj_get(yyjson_doc_get_root(event_doc), "data");
        assert(json_boolean(event_report, "passed") == !mutate);
        assert(json_boolean(event_report, "inputs_changed") == (mutate != 0));
        yyjson_doc_free(event_doc);
        yyjson_doc *saved = read_json(f.session.dir, "validation/latest.json");
        assert(json_boolean(yyjson_doc_get_root(saved), "passed") == !mutate);
        yyjson_doc_free(saved);
        yyjson_doc_free(doc);
        fixture_finish(&f);
    }
}

static int helper(int argc, char **argv) {
    FILE *marker = fopen(".forge/probe-runs", "ab");
    if (!marker)
        return 91;
    bool written = fputs("run\n", marker) >= 0;
    if (fclose(marker) != 0 || !written)
        return 92;
    bool compile_only = false;
    for (int i = 2; i < argc; i++)
        compile_only |= !strcmp(argv[i], "-run");
    if (!strcmp(argv[1], "test") && !compile_only && exists(".forge/mutate-fixture")) {
        FILE *file = fopen("fixture.txt", "wb");
        if (!file)
            return 93;
        written = fputs("modified by validation helper\n", file) >= 0;
        if (fclose(file) != 0 || !written)
            return 94;
    }
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Keep assertion failures in test output instead of opening a CRT dialog. */
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc > 1 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "test") || !strcmp(argv[1], "vet")))
        return helper(argc, argv);
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, self, sizeof(self));
    assert(length && length < sizeof(self));
#else
    assert(realpath(argv[0], self));
#endif
    const char *path = getenv("PATH");
    char *original_path = path ? fg_strdup(path) : NULL;
    assert(!path || original_path);
    test_deadline_callbacks();
    test_denial_and_resolution_failure();
    test_blocked_plan_diagnostics();
    test_persistence_failure("validation/0001-0001.stdout");
    test_persistence_failure("validation/0001.json");
    test_persistence_failure("validation/latest.json");
    test_success_and_input_mutation();
    set_path(original_path);
    free(original_path);
    puts("verification orchestration tests passed");
    return 0;
}
