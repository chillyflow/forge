#include "internal.h"
#include "core/candidate_store.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    char root[FG_PATH_MAX];
    char *files[64], *directories[16];
    size_t count, directory_count;
} fixture;
static void start(fixture *f) {
    memset(f, 0, sizeof(*f));
    char base[FG_PATH_MAX], name[96], nonce[33], canonical[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(base), base);
    assert(length && length < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
#endif
    assert(fg_random_hex(nonce, 16));
    snprintf(name, sizeof(name), "forge-candidate-store-%s", nonce);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, NULL));
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
}
static void remember(fixture *f, const char *name) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    for (size_t i = 0; i < f->count; i++)
        if (!strcmp(f->files[i], path))
            return;
    assert(f->count < sizeof(f->files) / sizeof(*f->files));
    f->files[f->count++] = fg_strdup(path);
    assert(f->files[f->count - 1]);
}
static void write_bytes(fixture *f, const char *name, const char *text, size_t length) {
    char path[FG_PATH_MAX];
    remember(f, name);
    assert(fg_path_join(path, f->root, name));
    assert(fg_write_file(path, text, length, NULL));
}
static void write_text(fixture *f, const char *name, const char *text) {
    write_bytes(f, name, text, strlen(text));
}
static void remove_file(fixture *f, const char *name) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    assert(remove(path) == 0);
}
static void directory(fixture *f, const char *name) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    assert(fg_mkdir(path, NULL));
    assert(f->directory_count < sizeof(f->directories) / sizeof(*f->directories));
    f->directories[f->directory_count++] = fg_strdup(path);
    assert(f->directories[f->directory_count - 1]);
}
static void finish(fixture *f) {
    for (size_t i = 0; i < f->count; i++) {
#ifdef _WIN32
        DWORD attributes = GetFileAttributesA(f->files[i]);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
            assert(SetFileAttributesA(f->files[i], attributes & ~FILE_ATTRIBUTE_READONLY));
#endif
        remove(f->files[i]);
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i > 0; i--) {
#ifdef _WIN32
        assert(RemoveDirectoryA(f->directories[i - 1]));
#else
        assert(rmdir(f->directories[i - 1]) == 0);
#endif
        free(f->directories[i - 1]);
    }
#ifdef _WIN32
    assert(RemoveDirectoryA(f->root));
#else
    assert(rmdir(f->root) == 0);
#endif
}
static fg_candidate_store *take(fixture *f) {
    forge_error error = {0};
    fg_candidate_store *s = fg_candidate_store_take(f->root, NULL, NULL, 0, &error);
    if (!s)
        fprintf(stderr, "candidate store: %s\n", error.message);
    assert(s);
    return s;
}
static void context(fg_tool_context *c, fg_session *s, fixture *workspace, fixture *journal) {
    memset(c, 0, sizeof(*c));
    memset(s, 0, sizeof(*s));
    strcpy(c->root, workspace->root);
    c->config.allow_write = true;
    c->session = s;
    c->deadline = fg_now_ms() + 30000;
    strcpy(s->dir, journal->root);
    s->edit_bytes_limit = 256u * 1024u * 1024u;
    char path[FG_PATH_MAX];
    remember(journal, "events.jsonl");
    assert(fg_path_join(path, journal->root, "events.jsonl"));
    s->events = fopen(path, "wb");
    assert(s->events);
}
static void close_context(fg_tool_context *c, fixture *journal) {
    assert(fclose(c->session->events) == 0);
    c->session->events = NULL;
    for (size_t i = 1; i <= c->call_id; i++) {
        char path[128];
        snprintf(path, sizeof(path), "candidate-%06zu.before", i);
        remember(journal, path);
        snprintf(path, sizeof(path), "candidate-%06zu.after", i);
        remember(journal, path);
    }
}
static void contents(fixture *f, const char *name, const void *expected, size_t length) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    size_t actual = 0;
    char *bytes = fg_read_file(path, 100000, &actual, NULL);
    assert(bytes && actual == length && (!length || !memcmp(bytes, expected, length)));
    free(bytes);
}
static void round_trip(void) {
    fixture original, candidate, journal;
    start(&original);
    start(&candidate);
    start(&journal);
    const char binary[] = {'a', 0, (char)0xff, '\r', '\n', 'z'};
    write_text(&original, "dirty.c", "user work already in progress\n");
    write_bytes(&original, "untracked.bin", binary, sizeof(binary));
    write_text(&original, "empty", "");
    directory(&original, ".git");
    write_text(&original, ".git/user-metadata", "must remain untouched\n");
    fg_candidate_store *baseline = take(&original);
    assert(fg_candidate_store_materialize(baseline, candidate.root, NULL));
    remember(&candidate, "dirty.c");
    remember(&candidate, "untracked.bin");
    remember(&candidate, "empty");
    fg_candidate_store *copy = take(&candidate);
    assert(fg_candidate_store_equal(baseline, copy));
    assert(!fg_candidate_store_cost(baseline, copy));
    fg_candidate_store_destroy(copy);
    write_text(&candidate, "dirty.c", "candidate repair\n");
    write_bytes(&candidate, "untracked.bin", binary + 1, sizeof(binary) - 1);
    remove_file(&candidate, "empty");
    write_text(&candidate, "created.txt", "new input\n");
    remember(&original, "created.txt");
    fg_candidate_store *desired = take(&candidate);
    assert(fg_candidate_store_cost(baseline, desired) > 0);
    fg_tool_context tools;
    fg_session session;
    context(&tools, &session, &original, &journal);
    forge_error error = {0};
    assert(fg_candidate_store_apply(baseline, desired, &tools, &error));
    fg_candidate_store *changed = take(&original);
    assert(fg_candidate_store_equal(changed, desired));
    fg_candidate_store_destroy(changed);
    assert(fg_candidate_store_apply(desired, baseline, &tools, &error));
    fg_candidate_store *restored = take(&original);
    assert(fg_candidate_store_equal(restored, baseline));
    contents(&original, "dirty.c", "user work already in progress\n", 30);
    contents(&original, "untracked.bin", binary, sizeof(binary));
    contents(&original, "empty", "", 0);
    contents(&original, ".git/user-metadata", "must remain untouched\n", 22);
    assert(session.sequence >= 8 && session.edit_bytes_reserved > 0);
    close_context(&tools, &journal);
    fg_candidate_store_destroy(restored);
    fg_candidate_store_destroy(baseline);
    fg_candidate_store_destroy(desired);
    finish(&journal);
    finish(&candidate);
    finish(&original);
}
typedef struct {
    size_t calls;
} policy_state;
static bool deny_second(const char *tool, forge_capability cap, const char *arguments, void *user) {
    policy_state *state = user;
    assert(!strcmp(tool, "candidate_restore") && cap == FORGE_CAP_WRITE && arguments);
    return ++state->calls < 2;
}
static void refusal_cases(void) {
    fixture original, candidate, journal;
    start(&original);
    start(&candidate);
    start(&journal);
    write_text(&original, "a", "original a");
    write_text(&original, "z", "original z");
    write_text(&candidate, "a", "candidate a");
    write_text(&candidate, "z", "candidate z");
    fg_candidate_store *baseline = take(&original), *desired = take(&candidate);
    fg_tool_context tools;
    fg_session session;
    context(&tools, &session, &original, &journal);
    policy_state policy = {0};
    tools.config.policy = deny_second;
    tools.config.userdata = &policy;
    forge_error error = {0};
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_POLICY && policy.calls == 2 && tools.call_id == 0);
    fg_candidate_store *current = take(&original);
    assert(fg_candidate_store_equal(current, baseline));
    fg_candidate_store_destroy(current);
    tools.config.policy = NULL;
    tools.config.userdata = NULL;
    tools.config.allow_write = false;
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_POLICY && tools.call_id == 0);
    tools.config.allow_write = true;
    session.edit_bytes_limit = 2500;
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_LIMIT && tools.call_id == 0);
    current = take(&original);
    assert(fg_candidate_store_equal(current, baseline));
    fg_candidate_store_destroy(current);
    /* One-way application fits, but its mandatory restore would not. */
    session.edit_bytes_limit = 5000;
    assert(!fg_candidate_store_apply_reserving_restore(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_LIMIT && tools.call_id == 0);
    current = take(&original);
    assert(fg_candidate_store_equal(current, baseline));
    fg_candidate_store_destroy(current);
    session.edit_bytes_limit = 256u * 1024u * 1024u;
    write_text(&original, "z", "new user modification");
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_CONFLICT && tools.call_id == 0);
    contents(&original, "a", "original a", 10);
    contents(&original, "z", "new user modification", 21);
    assert(!fg_candidate_store_materialize(desired, original.root, &error));
    contents(&original, "a", "original a", 10);
    close_context(&tools, &journal);
    fg_candidate_store_destroy(baseline);
    fg_candidate_store_destroy(desired);
    finish(&journal);
    finish(&candidate);
    finish(&original);
}
typedef struct {
    bool cancelled;
} late_cancel_state;
static bool late_cancel(void *user) {
    return ((late_cancel_state *)user)->cancelled;
}
static void cancel_after_write(const forge_event *event, void *user) {
    if (!strcmp(event->type, "candidate_edit_outcome"))
        ((late_cancel_state *)user)->cancelled = true;
}
static void failed_apply_recovery(void) {
    fixture original, candidate, journal;
    start(&original);
    start(&candidate);
    start(&journal);
    write_text(&original, "a", "original a");
    write_text(&original, "z", "original z");
    write_text(&candidate, "a", "candidate a");
    write_text(&candidate, "z", "candidate z");
    fg_candidate_store *baseline = take(&original), *desired = take(&candidate);
    fg_tool_context tools;
    fg_session session;
    context(&tools, &session, &original, &journal);
    late_cancel_state flag = {0};
    session.callback = cancel_after_write;
    session.userdata = &flag;
    tools.config.cancelled = late_cancel;
    tools.config.userdata = &flag;
    forge_error error = {0};
    assert(!fg_candidate_store_apply_reserving_restore(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_CANCELLED && tools.call_id == 2);
    fg_candidate_store *current = take(&original);
    assert(fg_candidate_store_equal(current, desired));
    fg_candidate_store_destroy(current);
    assert(fg_candidate_store_recover(baseline, desired, &tools, tools.deadline, &error));
    assert(error.code == FORGE_ERR_CANCELLED && tools.call_id == 4);
    current = take(&original);
    assert(fg_candidate_store_equal(current, baseline));
    fg_candidate_store_destroy(current);
    /* A cancelled application followed by an external edit must not silently
     * erase the external input during cleanup. */
    flag.cancelled = false;
    assert(!fg_candidate_store_apply_reserving_restore(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    write_text(&original, "external", "new user work");
    size_t calls = tools.call_id;
    assert(!fg_candidate_store_recover(baseline, desired, &tools, tools.deadline, &error));
    assert(error.code == FORGE_ERR_CONFLICT && tools.call_id == calls);
    contents(&original, "external", "new user work", 13);
    contents(&original, "a", "candidate a", 11);
    remove_file(&original, "external");
    /* A partial application similarly requires explicit journal recovery. */
    write_text(&original, "z", "original z");
    assert(!fg_candidate_store_recover(baseline, desired, &tools, tools.deadline, &error));
    assert(error.code == FORGE_ERR_CONFLICT && tools.call_id == calls);
    contents(&original, "a", "candidate a", 11);
    contents(&original, "z", "original z", 10);
    close_context(&tools, &journal);
    fg_candidate_store_destroy(baseline);
    fg_candidate_store_destroy(desired);
    finish(&journal);
    finish(&candidate);
    finish(&original);
}
static void readonly_materialization(void) {
    fixture original, candidate;
    start(&original);
    start(&candidate);
    write_text(&original, "protected_test.go", "package fixture\n// protected test\n");
    write_text(&original, "editable.go", "package fixture\n");
    char protected_path[FG_PATH_MAX], copied_path[FG_PATH_MAX];
    assert(fg_path_join(protected_path, original.root, "protected_test.go"));
    assert(fg_path_join(copied_path, candidate.root, "protected_test.go"));
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(protected_path);
    assert(attributes != INVALID_FILE_ATTRIBUTES);
    assert(SetFileAttributesA(protected_path, attributes | FILE_ATTRIBUTE_READONLY));
#else
    assert(chmod(protected_path, 0444) == 0);
#endif
    fg_candidate_store *baseline = take(&original);
    assert(fg_candidate_store_materialize(baseline, candidate.root, NULL));
    remember(&candidate, "protected_test.go");
    remember(&candidate, "editable.go");
    fg_candidate_store *copied = take(&candidate);
    /* Protected test permissions are candidate inputs, not an apparent edit
     * caused by Windows' default mode for newly created files. */
    assert(fg_candidate_store_equal(baseline, copied));
    assert(fg_candidate_store_cost(baseline, copied) == 0);
#ifdef _WIN32
    attributes = GetFileAttributesA(copied_path);
    assert(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY));
#else
    struct stat mode;
    assert(stat(copied_path, &mode) == 0 && (mode.st_mode & 0777u) == 0444);
#endif
    contents(&candidate, "protected_test.go", "package fixture\n// protected test\n",
             strlen("package fixture\n// protected test\n"));
    fg_candidate_store_destroy(copied);
    fg_candidate_store_destroy(baseline);
    finish(&candidate);
    finish(&original);
}
static bool cancel(void *unused) {
    (void)unused;
    return true;
}
static void structural_conflict(void) {
    fixture original, candidate, journal;
    start(&original);
    start(&candidate);
    start(&journal);
    write_text(&original, "a", "original a");
    write_text(&original, "z", "existing file");
    write_text(&candidate, "a", "candidate a");
    directory(&candidate, "z");
    write_text(&candidate, "z/child", "nested content");
    fg_candidate_store *baseline = take(&original), *desired = take(&candidate);
    fg_tool_context tools;
    fg_session session;
    context(&tools, &session, &original, &journal);
    forge_error error = {0};
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_UNSUPPORTED && tools.call_id == 0);
    fg_candidate_store *current = take(&original);
    assert(fg_candidate_store_equal(current, baseline));
    fg_candidate_store_destroy(current);
    tools.config.cancelled = cancel;
    assert(!fg_candidate_store_apply(baseline, desired, &tools, &error));
    assert(error.code == FORGE_ERR_CANCELLED && tools.call_id == 0);
    close_context(&tools, &journal);
    fg_candidate_store_destroy(baseline);
    fg_candidate_store_destroy(desired);
    finish(&journal);
    finish(&candidate);
    finish(&original);
}
static void limits_and_links(void) {
    fixture f;
    start(&f);
    write_text(&f, "a", "a");
    forge_error error = {0};
    assert(!fg_candidate_store_take(f.root, cancel, NULL, 0, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    assert(!fg_candidate_store_take(f.root, NULL, NULL, fg_now_ms(), &error));
    assert(error.code == FORGE_ERR_LIMIT);
    char path[FG_PATH_MAX], target[FG_PATH_MAX];
    assert(fg_path_join(path, f.root, "too-large"));
    remember(&f, "too-large");
    FILE *large = fopen(path, "wb");
    assert(large && fseek(large, 64L * 1024 * 1024, SEEK_SET) == 0);
    assert(fputc('x', large) == 'x' && fclose(large) == 0);
    assert(!fg_candidate_store_take(f.root, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    remove_file(&f, "too-large");
    assert(fg_path_join(path, f.root, "link"));
    assert(fg_path_join(target, f.root, "a"));
#ifdef _WIN32
    bool linked = CreateSymbolicLinkA(path, target, 0x2u) != 0;
#else
    bool linked = symlink(target, path) == 0;
#endif
    if (linked) {
        remember(&f, "link");
        assert(!fg_candidate_store_take(f.root, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_POLICY);
    }
    finish(&f);
}
int main(void) {
    round_trip();
    refusal_cases();
    failed_apply_recovery();
    readonly_materialization();
    structural_conflict();
    limits_and_links();
    puts("candidate content store safety passed");
    return 0;
}
