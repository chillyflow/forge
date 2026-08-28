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
#include "forge/index.h"
#include "forge/watch.h"
#include "core/input_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <wchar.h>
#else
#include <unistd.h>
#endif

/* These are coordinator tests, NOT native watcher evidence. The executable
 * supplies all forge_watch symbols used by monitor.o; static linking must not
 * pull watch.o. Repository indexing and input snapshots are the real code.
 * PATH is changed only inside this test process and restored after each case;
 * an empty trusted directory prevents launching an installed Git executable. */
typedef struct fixture fixture;
struct forge_watch {
    fixture *owner;
    bool alive, initial, reopen;
};
typedef enum {
    FAKE_EMPTY,
    FAKE_EVENT,
    FAKE_REOPEN,
    FAKE_BAD_JSON,
    FAKE_IO_ERROR,
    FAKE_MEMORY_ERROR,
    FAKE_DEADLINE_PROBE
} fake_batch_kind;
struct fixture {
    char base[FG_PATH_MAX], root[FG_PATH_MAX], bin[FG_PATH_MAX];
    char *files[128], *directories[128], *old_path;
    size_t file_count, directory_count;
    forge_repo *repo;
    fg_repo_monitor *monitor;
    fg_repo_change change;
    forge_error error;
    forge_watch watches[16];
    size_t create_calls, created, destroyed, live, poll_calls, user_checks;
    forge_status create_error;
    fake_batch_kind initial_kind, next_kind;
    const char *event_path;
    unsigned event_flags;
    uint64_t deadline, release_after_full_attempt, cancel_on_full_attempt;
    size_t after_scan_deliveries;
    bool user_cancelled, deadline_callback_observed, repeat_after_scan, index_cancellation_seen;
};

static fixture *active;
static const char *old_source = "int monitor_value(void) { return 1; }\n";
static const char *new_source = "int monitor_value(void) { return 2; }\n";

static void sleep_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec remaining = {(time_t)(milliseconds / 1000),
                                 (long)(milliseconds % 1000) * 1000000};
    while (nanosleep(&remaining, &remaining) != 0)
        assert(errno == EINTR);
#endif
}

static void set_path(const char *value) {
#ifdef _WIN32
    assert(_putenv_s("PATH", value ? value : "") == 0);
#else
    assert((value ? setenv("PATH", value, 1) : unsetenv("PATH")) == 0);
#endif
}

#ifdef _WIN32
static void wide_path(const char *path, wchar_t wide[FG_PATH_MAX]) {
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FG_PATH_MAX));
}
static bool make_directory(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    wide_path(path, wide);
    return CreateDirectoryW(wide, NULL) != 0;
}
static bool remove_directory(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    wide_path(path, wide);
    return RemoveDirectoryW(wide) != 0;
}
static bool remove_file(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    wide_path(path, wide);
    return DeleteFileW(wide) || GetLastError() == ERROR_FILE_NOT_FOUND;
}
#else
static bool make_directory(const char *path) {
    return mkdir(path, 0700) == 0;
}
static bool remove_directory(const char *path) {
    return rmdir(path) == 0;
}
static bool remove_file(const char *path) {
    return unlink(path) == 0 || errno == ENOENT;
}
#endif

static void fixture_path(fixture *f, const char *relative, char path[FG_PATH_MAX]) {
    assert(relative && *relative && !strstr(relative, ".."));
    assert(fg_path_join(path, f->root, relative));
    /* All mutation/cleanup targets are explicit children of this private root. */
    assert(!strncmp(path, f->root, strlen(f->root)) && path[strlen(f->root)] == '/');
}

static void record_file(fixture *f, const char *path) {
    for (size_t i = 0; i < f->file_count; i++)
        if (!strcmp(f->files[i], path))
            return;
    assert(f->file_count < sizeof(f->files) / sizeof(*f->files));
    f->files[f->file_count++] = fg_strdup(path);
    assert(f->files[f->file_count - 1]);
}

static void fixture_directory(fixture *f, const char *relative) {
    char path[FG_PATH_MAX];
    fixture_path(f, relative, path);
    for (size_t i = 0; i < f->directory_count; i++)
        if (!strcmp(f->directories[i], path))
            return;
    assert(f->directory_count < sizeof(f->directories) / sizeof(*f->directories));
    assert(make_directory(path));
    f->directories[f->directory_count++] = fg_strdup(path);
    assert(f->directories[f->directory_count - 1]);
}

static void fixture_write(fixture *f, const char *relative, const void *bytes, size_t length) {
    char path[FG_PATH_MAX], parents[FG_PATH_MAX];
    assert(strlen(relative) < sizeof(parents));
    strcpy(parents, relative);
    for (char *p = parents; *p; p++)
        if (*p == '/') {
            *p = 0;
            fixture_directory(f, parents);
            *p = '/';
        }
    fixture_path(f, relative, path);
    record_file(f, path);
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    wide_path(path, wide);
    HANDLE file =
        CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE && length <= UINT32_MAX);
    DWORD written = 0;
    assert(WriteFile(file, bytes, (DWORD)length, &written, NULL) && written == length);
    assert(CloseHandle(file));
#else
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
#endif
}

static void fixture_hardlink(fixture *f, const char *existing, const char *alias) {
    char source[FG_PATH_MAX], target[FG_PATH_MAX];
    fixture_path(f, existing, source);
    fixture_path(f, alias, target);
    /* Both names stay on the fixture volume; actual hard-link support is
     * required, while change delivery still uses the fake watcher. */
#ifdef _WIN32
    wchar_t source_wide[FG_PATH_MAX], target_wide[FG_PATH_MAX];
    wide_path(source, source_wide);
    wide_path(target, target_wide);
    assert(CreateHardLinkW(target_wide, source_wide, NULL));
#else
    assert(link(source, target) == 0);
#endif
    record_file(f, target);
}

static bool user_cancel(void *user) {
    fixture *f = user;
    f->user_checks++;
    if (f->cancel_on_full_attempt) {
        forge_index_stats stats;
        assert(forge_repo_get_index_stats(f->repo, &stats));
        if (stats.full_attempts >= f->cancel_on_full_attempt) {
            f->index_cancellation_seen = true;
            return true;
        }
    }
    return f->user_cancelled;
}

static forge_index_stats index_stats(fixture *f) {
    forge_index_stats stats;
    assert(forge_repo_get_index_stats(f->repo, &stats));
    return stats;
}

static char *batch(bool initial, bool reopen, const char *path, unsigned flags) {
    fg_buf json = {0};
    fg_buf_puts(&json,
                "{\"schema_version\":1,\"backend\":\"coordinator_test_double\",\"events\":[");
    if (path) {
        char *quoted = fg_json_string(path);
        assert(quoted);
        fg_buf_printf(&json, "{\"path\":%s,\"flags\":%u}", quoted, flags);
        free(quoted);
    }
    fg_buf_printf(&json,
                  "],\"rescan_required\":%s,\"initial_scan_required\":%s,"
                  "\"reopen_required\":%s,\"timed_out\":false,\"more_pending\":false,"
                  "\"reason_flags\":%u,\"dropped_events\":0,\"dropped_events_unknown\":%s,"
                  "\"overflow_count\":0,\"directories\":1,\"path_encoding\":\"utf-8\"}",
                  initial || reopen ? "true" : "false", initial ? "true" : "false",
                  reopen ? "true" : "false",
                  reopen    ? FORGE_WATCH_RESCAN_IO
                  : initial ? FORGE_WATCH_RESCAN_INITIAL
                            : 0u,
                  reopen ? "true" : "false");
    assert(!json.failed);
    return fg_buf_take(&json);
}

forge_watch_limits forge_default_watch_limits(void) {
    /* Not used by the coordinator, but keep this fake's public surface complete. */
    forge_watch_limits limits = {64, 65536, 4095, 64, 64, 1024, 1024};
    return limits;
}

forge_watch *forge_watch_create(const char *root, const forge_watch_limits *limits,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout,
                                forge_error *error) {
    (void)limits;
    fixture *f = active;
    assert(f && root && !strcmp(root, f->root));
    assert(cancelled && user && timeout && timeout <= 30000);
    f->create_calls++;
    if (cancelled(user)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Fake watcher creation cancelled");
        return NULL;
    }
    if (f->create_error) {
        fg_error(error, f->create_error, "Injected native watcher creation failure");
        return NULL;
    }
    assert(f->created < sizeof(f->watches) / sizeof(*f->watches));
    forge_watch *watch = &f->watches[f->created++];
    *watch = (forge_watch){f, true, true, false};
    f->live++;
    if (error)
        memset(error, 0, sizeof(*error));
    return watch;
}

char *forge_watch_poll(forge_watch *watch, uint64_t timeout, forge_cancel_fn cancelled, void *user,
                       forge_error *error) {
    assert(watch && watch->alive && watch->owner == active && cancelled && user);
    fixture *f = watch->owner;
    f->poll_calls++;
    fake_batch_kind kind = watch->initial ? f->initial_kind : f->next_kind;
    if (!watch->initial)
        f->next_kind = FAKE_EMPTY;
    if (kind == FAKE_DEADLINE_PROBE) {
        assert(timeout == 0 && !f->user_cancelled);
        assert(f->deadline <= fg_now_ms() + 5000);
        while (fg_now_ms() < f->deadline)
            sleep_ms(1);
        /* A bare user_cancel callback returns false here. Only the monitor's
         * wrapper can communicate its absolute deadline to a timeout-zero poll. */
        assert(cancelled(user));
        f->deadline_callback_observed = true;
        fg_error(error, FORGE_ERR_CANCELLED, "Fake poll observed monitor deadline");
        return NULL;
    }
    if (cancelled(user)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Fake watcher poll cancelled");
        return NULL;
    }
    if (kind == FAKE_IO_ERROR || kind == FAKE_MEMORY_ERROR) {
        fg_error(error, kind == FAKE_IO_ERROR ? FORGE_ERR_IO : FORGE_ERR_MEMORY,
                 "Injected native watcher poll failure");
        return NULL;
    }
    if (kind == FAKE_BAD_JSON)
        return fg_strdup("not a JSON watch batch");
    if (kind == FAKE_REOPEN)
        watch->reopen = true;
    const char *path = kind == FAKE_EVENT ? f->event_path : NULL;
    unsigned flags = f->event_flags ? f->event_flags : FORGE_WATCH_MODIFIED;
    if (!watch->initial && f->release_after_full_attempt &&
        index_stats(f).full_attempts >= f->release_after_full_attempt) {
        /* The event is released only AFTER a real full index has run. Mutating
         * here models an event queued while that index was completing; no
         * wall-clock delivery assumptions or native notification APIs are used. */
        fixture_write(f, "main.c", new_source, strlen(new_source));
        f->after_scan_deliveries++;
        path = "main.c";
        flags = FORGE_WATCH_MODIFIED;
        f->release_after_full_attempt = f->repeat_after_scan ? index_stats(f).full_attempts + 1 : 0;
    }
    char *json = batch(watch->initial, watch->reopen, path, flags);
    watch->initial = false;
    if (error)
        memset(error, 0, sizeof(*error));
    return json;
}

void forge_watch_invalidate(forge_watch *watch) {
    if (watch) {
        assert(watch->alive);
        watch->reopen = true;
    }
}

void forge_watch_destroy(forge_watch *watch) {
    if (!watch)
        return;
    assert(watch->owner == active && watch->alive && active->live);
    watch->alive = false;
    active->live--;
    active->destroyed++;
}

static void fixture_start(fixture *f) {
    assert(!active);
    memset(f, 0, sizeof(*f));
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
    snprintf(name, sizeof(name), "forge-monitor-%s", id);
    assert(fg_path_join(f->base, parent, name) && make_directory(f->base));
    assert(fg_workspace(f->base, canonical, NULL));
    strcpy(f->base, canonical);
    assert(fg_path_join(f->root, f->base, "workspace") && make_directory(f->root));
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
    assert(fg_path_join(f->bin, f->base, "empty-path") && make_directory(f->bin));
    const char *original_path = getenv("PATH");
    f->old_path = original_path ? fg_strdup(original_path) : NULL;
    assert(!original_path || f->old_path);
    set_path(f->bin);
    fixture_write(f, "main.c", old_source, strlen(old_source));
    fixture_write(f, "fixture.csv", "a,b\n", 4);
    const unsigned char binary[] = {0, 0xff, 1, 2};
    fixture_write(f, "blob.bin", binary, sizeof(binary));
    fixture_write(f, "no_extension", "first", 5);
    f->repo = forge_repo_open(f->root, &f->error);
    assert(f->repo && f->error.code == FORGE_OK);
    active = f;
}

static void fixture_finish(fixture *f) {
    fg_repo_change_free(&f->change);
    fg_repo_monitor_destroy(f->monitor);
    f->monitor = NULL;
    assert(f->live == 0 && f->destroyed == f->created);
    forge_repo_close(f->repo);
    f->repo = NULL;
    /* No recursive removal: delete only tracked fixture paths and known SQLite
     * artifacts, then remove tracked directories in reverse creation order.
     * RemoveDirectoryW unlinks the test junction itself; it never walks it. */
    for (size_t i = 0; i < f->file_count; i++) {
        assert(remove_file(f->files[i]));
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i; i--) {
        assert(remove_directory(f->directories[i - 1]));
        free(f->directories[i - 1]);
    }
    const char *metadata[] = {".forge/index.db-wal", ".forge/index.db-shm", ".forge/index.db"};
    char path[FG_PATH_MAX];
    for (size_t i = 0; i < sizeof(metadata) / sizeof(*metadata); i++) {
        fixture_path(f, metadata[i], path);
        assert(remove_file(path));
    }
    fixture_path(f, ".forge", path);
    assert(remove_directory(path));
    assert(remove_directory(f->root));
    assert(remove_directory(f->bin));
    assert(remove_directory(f->base));
    set_path(f->old_path);
    free(f->old_path);
    active = NULL;
}

static bool create_monitor(fixture *f, bool require_native, uint64_t budget) {
    memset(&f->error, 0, sizeof(f->error));
    f->deadline = fg_now_ms() + budget;
    f->monitor = fg_repo_monitor_create(f->repo, f->root, user_cancel, f, f->deadline,
                                        require_native, &f->change, &f->error);
    return f->monitor != NULL;
}

static void start_monitor(fixture *f, bool require_native) {
    bool created = create_monitor(f, require_native, 30000);
    if (!created)
        fprintf(stderr, "monitor create: %s\n", f->error.message);
    assert(created && f->error.code == FORGE_OK);
    assert(f->change.json && f->change.full_scan);
}

static forge_status poll_monitor(fixture *f, bool force_full) {
    fg_repo_change_free(&f->change);
    memset(&f->error, 0, sizeof(f->error));
    return fg_repo_monitor_poll(f->monitor, 0, force_full, &f->change, &f->error);
}

static yyjson_doc *change_document(fixture *f) {
    assert(f->change.json);
    yyjson_doc *doc = yyjson_read(f->change.json, strlen(f->change.json), 0);
    assert(doc && yyjson_is_obj(yyjson_doc_get_root(doc)));
    return doc;
}

static bool change_flag(fixture *f, const char *key) {
    yyjson_doc *doc = change_document(f);
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), key);
    assert(yyjson_is_bool(value));
    bool result = yyjson_get_bool(value);
    yyjson_doc_free(doc);
    return result;
}

static uint64_t change_count(fixture *f, const char *key) {
    yyjson_doc *doc = change_document(f);
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), key);
    assert(yyjson_is_uint(value));
    uint64_t result = yyjson_get_uint(value);
    yyjson_doc_free(doc);
    return result;
}

static void expect_index_source(fixture *f, const char *path, const char *source) {
    char *json = forge_repo_index_describe(f->repo, path, &f->error);
    assert(json);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    const char *hash = fg_json_str(yyjson_doc_get_root(doc), "source_hash");
    char expected[32];
    snprintf(expected, sizeof(expected), "%016llx",
             (unsigned long long)fg_hash(source, strlen(source)));
    assert(hash && !strcmp(hash, expected));
    yyjson_doc_free(doc);
    free(json);
}

static void expect_source(fixture *f, const char *source) {
    expect_index_source(f, "main.c", source);
}

static void test_snapshot_fallback_and_noops(void) {
    fixture f;
    fixture_start(&f);
    f.create_error = FORGE_ERR_UNSUPPORTED;
    start_monitor(&f, false);
    assert(!f.change.native && !change_flag(&f, "watch_available"));
    uint64_t generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(!f.change.changed && f.change.generation == generation);
    assert(!change_flag(&f, "followup_scan_required"));
    const unsigned char binary[] = {0, 0xfe, 1, 2};
    const struct {
        const char *path;
        const void *bytes;
        size_t length;
    } edits[] = {{"fixture.csv", "c,d\n", 4},
                 {"blob.bin", binary, sizeof(binary)},
                 {"no_extension", "other", 5},
                 {".hidden/input", "hidden", 6},
                 {"vendor/input.dat", "vendor", 6}};
    for (size_t i = 0; i < sizeof(edits) / sizeof(*edits); i++) {
        fixture_write(&f, edits[i].path, edits[i].bytes, edits[i].length);
        assert(poll_monitor(&f, false) == FORGE_OK);
        assert(f.change.changed && f.change.full_scan && f.change.generation > generation);
        assert(!f.change.native && !change_flag(&f, "followup_scan_required"));
        generation = f.change.generation;
        assert(poll_monitor(&f, false) == FORGE_OK);
        assert(!f.change.changed && f.change.generation == generation);
    }
    char path[FG_PATH_MAX];
    fixture_path(&f, "fixture.csv", path);
    assert(remove_file(path));
    assert(poll_monitor(&f, false) == FORGE_OK && f.change.changed);
    generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK && !f.change.changed);
    assert(f.change.generation == generation && !f.poll_calls && !f.created);
    expect_source(&f, old_source);
    fixture_finish(&f);
}

static void fixture_too_deep(fixture *f) {
    char relative[FG_PATH_MAX] = "";
    for (size_t depth = 0; depth <= FG_INPUT_SNAPSHOT_MAX_DEPTH; depth++) {
        strcat(relative, depth ? "/n" : "n");
        fixture_directory(f, relative);
    }
}

static void fixture_unsafe_entry(fixture *f) {
    char path[FG_PATH_MAX];
    fixture_path(f, "unsafe", path);
#ifdef _WIN32
    fixture_directory(f, "unsafe");
    wchar_t wide[FG_PATH_MAX], target[FG_PATH_MAX];
    wide_path(path, wide);
    wide_path(f->bin, target);
    for (wchar_t *p = target; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    struct {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[FG_PATH_MAX];
    } data = {0};
    size_t length = wcslen(target), substitute = length + 4;
    assert(substitute + length + 2 < FG_PATH_MAX);
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(data.path, L"\\??\\", 4 * sizeof(wchar_t));
    memcpy(data.path + 4, target, (length + 1) * sizeof(wchar_t));
    memcpy(data.path + substitute + 1, target, (length + 1) * sizeof(wchar_t));
    data.substitute_length = (WORD)(substitute * sizeof(wchar_t));
    data.print_offset = (WORD)((substitute + 1) * sizeof(wchar_t));
    data.print_length = (WORD)(length * sizeof(wchar_t));
    data.data_length = (WORD)(8 + (substitute + length + 2) * sizeof(wchar_t));
    HANDLE directory = CreateFileW(wide, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    assert(directory != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    assert(DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data, (DWORD)data.data_length + 8,
                           NULL, 0, &returned, NULL));
    assert(CloseHandle(directory));
#else
    assert(mkfifo(path, 0600) == 0);
    record_file(f, path);
#endif
}

static void test_incomplete_and_refused_snapshots(void) {
    for (unsigned unsafe = 0; unsafe < 2; unsafe++) {
        fixture f;
        fixture_start(&f);
        f.create_error = FORGE_ERR_UNSUPPORTED;
        if (unsafe)
            fixture_unsafe_entry(&f);
        else
            fixture_too_deep(&f);
        assert(!create_monitor(&f, false, 30000));
        assert(f.error.code == (unsafe ? FORGE_ERR_POLICY : FORGE_ERR_LIMIT));
        assert(!f.change.json && !f.live && index_stats(&f).full_attempts == 0);
        fixture_finish(&f);

        fixture_start(&f);
        f.create_error = FORGE_ERR_UNSUPPORTED;
        start_monitor(&f, false);
        uint64_t generation = forge_repo_generation(f.repo);
        if (unsafe)
            fixture_unsafe_entry(&f);
        else
            fixture_too_deep(&f);
        assert(poll_monitor(&f, false) == (unsafe ? FORGE_ERR_POLICY : FORGE_ERR_LIMIT));
        assert(!f.change.json && forge_repo_generation(f.repo) == generation);
        fixture_finish(&f);
    }
}

static void test_native_noops_and_unindexed_signal(void) {
    fixture f;
    fixture_start(&f);
    start_monitor(&f, true);
    uint64_t generation = forge_repo_generation(f.repo);
    uint64_t full_attempts = index_stats(&f).full_attempts;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(!f.change.changed && !f.change.full_scan && !f.change.delta_scan);
    assert(f.change.generation == generation && index_stats(&f).full_attempts == full_attempts);
    fixture_write(&f, "fixture.csv", "new\n", 4);
    f.next_kind = FAKE_EVENT;
    f.event_path = "fixture.csv";
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(f.change.changed && f.change.generation > generation && f.change.events == 1);
    generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(!f.change.changed && f.change.generation == generation);
    fixture_finish(&f);
}

static void test_hardlink_alias_forces_full_scan(void) {
    fixture f;
    fixture_start(&f);
    fixture_hardlink(&f, "main.c", "alias.c");
    start_monitor(&f, true);
    expect_index_source(&f, "main.c", old_source);
    expect_index_source(&f, "alias.c", old_source);
    forge_index_stats before = index_stats(&f);
    uint64_t generation = forge_repo_generation(f.repo);
    size_t calls = f.poll_calls;

    /* Truncation writes through the shared file, unlike atomic replacement.
     * Only alias.c receives a signal: main.c must refresh without its own. */
    fixture_write(&f, "alias.c", new_source, strlen(new_source));
    f.next_kind = FAKE_EVENT;
    f.event_path = "alias.c";
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(f.change.changed && f.change.full_scan && !f.change.delta_scan);
    assert(f.change.events == 1 && f.change.generation > generation);
    assert(index_stats(&f).full_attempts == before.full_attempts + 1);
    assert(index_stats(&f).delta_attempts == before.delta_attempts);
    assert(f.poll_calls == calls + 2 && !change_flag(&f, "followup_scan_required"));
    expect_index_source(&f, "main.c", new_source);
    expect_index_source(&f, "alias.c", new_source);

    generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(!f.change.changed && !f.change.full_scan && !f.change.delta_scan);
    assert(f.change.generation == generation);
    fixture_finish(&f);
}

static void test_removed_hardlink_alias_forces_full_scan(void) {
    for (unsigned replace = 0; replace < 2; replace++) {
        fixture f;
        fixture_start(&f);
        fixture_hardlink(&f, "main.c", "alias.c");
        start_monitor(&f, true);
        expect_index_source(&f, "main.c", old_source);
        expect_index_source(&f, "alias.c", old_source);
        forge_index_stats before = index_stats(&f);
        uint64_t generation = forge_repo_generation(f.repo);
        size_t calls = f.poll_calls;

        /* A queued write may outlive the edited alias. Current link counts
         * cannot identify main.c as affected once alias.c has been removed. */
        fixture_write(&f, "alias.c", new_source, strlen(new_source));
        char path[FG_PATH_MAX];
        fixture_path(&f, "alias.c", path);
        assert(remove_file(path));
        if (replace) {
            /* Reusing the pathname for an unrelated file hides the missing
             * target too; the coalesced deletion signal must force a scan. */
            fixture_write(&f, "alias.c", old_source, strlen(old_source));
            f.event_flags = FORGE_WATCH_MODIFIED | FORGE_WATCH_DELETED | FORGE_WATCH_CREATED;
        }
        f.next_kind = FAKE_EVENT;
        f.event_path = "alias.c";
        assert(poll_monitor(&f, false) == FORGE_OK);
        assert(f.change.changed && f.change.full_scan && !f.change.delta_scan);
        assert(f.change.events == 1 && f.change.generation > generation);
        assert(index_stats(&f).full_attempts == before.full_attempts + 1);
        assert(index_stats(&f).delta_attempts == before.delta_attempts);
        assert(f.poll_calls == calls + 2 && !change_flag(&f, "followup_scan_required"));
        expect_index_source(&f, "main.c", new_source);
        if (replace)
            expect_index_source(&f, "alias.c", old_source);
        else {
            forge_error missing = {0};
            char *json = forge_repo_index_describe(f.repo, "alias.c", &missing);
            assert(!json && missing.code == FORGE_ERR_NOT_FOUND);
            free(json);
        }
        generation = f.change.generation;
        assert(poll_monitor(&f, false) == FORGE_OK);
        assert(!f.change.changed && !f.change.full_scan && !f.change.delta_scan);
        assert(f.change.generation == generation);
        fixture_finish(&f);
    }
}

static void test_events_queued_during_full_index(void) {
    fixture f;
    fixture_start(&f);
    start_monitor(&f, true);
    uint64_t generation = forge_repo_generation(f.repo);
    uint64_t scans = index_stats(&f).full_attempts;
    f.release_after_full_attempt = scans + 1;
    assert(poll_monitor(&f, true) == FORGE_OK);
    assert(f.after_scan_deliveries == 1 && f.change.changed && f.change.full_scan);
    assert(f.change.generation > generation && f.change.events == 1);
    assert(change_flag(&f, "followup_scan_required") && change_count(&f, "after_scan_events") == 1);
    expect_source(&f, old_source); /* The first scan preceded the injected change. */
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(f.change.changed && f.change.full_scan && index_stats(&f).full_attempts == scans + 2);
    assert(!change_flag(&f, "followup_scan_required"));
    expect_source(&f, new_source);
    generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(!f.change.changed && f.change.generation == generation);

    /* Continually queued signals require future scans, not an unbounded retry
     * loop inside one poll. The caller must not accept this as a stable view. */
    f.repeat_after_scan = true;
    f.release_after_full_attempt = index_stats(&f).full_attempts + 1;
    for (unsigned i = 0; i < 3; i++) {
        size_t calls = f.poll_calls;
        assert(poll_monitor(&f, i == 0) == FORGE_OK);
        assert(f.change.changed && change_flag(&f, "followup_scan_required"));
        assert(f.poll_calls - calls == 2);
    }
    fixture_finish(&f);
}

static void test_cancellation_and_absolute_deadline(void) {
    fixture f;
    fixture_start(&f);
    f.user_cancelled = true;
    assert(!create_monitor(&f, true, 30000));
    assert(f.error.code == FORGE_ERR_CANCELLED && !f.create_calls && !f.created);
    assert(index_stats(&f).full_attempts == 0);
    f.user_cancelled = false;
    f.deadline = fg_now_ms() - 1;
    f.monitor = fg_repo_monitor_create(f.repo, f.root, user_cancel, &f, f.deadline, true, &f.change,
                                       &f.error);
    assert(!f.monitor && f.error.code == FORGE_ERR_CANCELLED && !f.create_calls);
    fixture_finish(&f);

    fixture_start(&f);
    start_monitor(&f, true);
    size_t calls = f.poll_calls;
    f.user_cancelled = true;
    assert(poll_monitor(&f, false) == FORGE_ERR_CANCELLED && f.poll_calls == calls);
    assert(!f.change.json);
    fixture_finish(&f);

    /* Index cancellation is forwarded through monitor creation and polling,
     * including cleanup of an already-created fake native watcher. */
    fixture_start(&f);
    f.cancel_on_full_attempt = 1;
    assert(!create_monitor(&f, true, 30000));
    assert(f.error.code == FORGE_ERR_CANCELLED && f.index_cancellation_seen);
    assert(f.created == 1 && f.destroyed == 1 && !f.change.json);
    fixture_finish(&f);

    fixture_start(&f);
    start_monitor(&f, true);
    uint64_t generation = forge_repo_generation(f.repo);
    f.cancel_on_full_attempt = index_stats(&f).full_attempts + 1;
    assert(poll_monitor(&f, true) == FORGE_ERR_CANCELLED && f.index_cancellation_seen);
    assert(!f.change.json && f.live == 1 && forge_repo_generation(f.repo) == generation);
    fixture_finish(&f);

    fixture_start(&f);
    assert(create_monitor(&f, true, 2000));
    f.next_kind = FAKE_DEADLINE_PROBE;
    assert(poll_monitor(&f, false) == FORGE_ERR_CANCELLED);
    assert(f.deadline_callback_observed && !f.user_cancelled && f.user_checks);
    assert(!f.change.json && f.live == 1);
    calls = f.poll_calls;
    assert(poll_monitor(&f, false) == FORGE_ERR_CANCELLED && f.poll_calls == calls);
    fixture_finish(&f);
}

static void test_creation_poll_and_reopen_cleanup(void) {
    const forge_status failures[] = {FORGE_ERR_UNSUPPORTED, FORGE_ERR_IO, FORGE_ERR_MEMORY};
    for (size_t i = 0; i < sizeof(failures) / sizeof(*failures); i++) {
        fixture f;
        fixture_start(&f);
        f.create_error = failures[i];
        assert(!create_monitor(&f, true, 30000) && f.error.code == failures[i]);
        assert(f.create_calls == 1 && !f.created && !f.change.json);
        fixture_finish(&f);
    }
    const fake_batch_kind initial_failures[] = {FAKE_REOPEN, FAKE_BAD_JSON, FAKE_IO_ERROR,
                                                FAKE_MEMORY_ERROR};
    const forge_status statuses[] = {FORGE_ERR_LIMIT, FORGE_ERR_PARSE, FORGE_ERR_IO,
                                     FORGE_ERR_MEMORY};
    for (size_t i = 0; i < sizeof(initial_failures) / sizeof(*initial_failures); i++) {
        fixture f;
        fixture_start(&f);
        f.initial_kind = initial_failures[i];
        assert(!create_monitor(&f, true, 30000) && f.error.code == statuses[i]);
        assert(f.created == 1 && f.destroyed == 1 && !f.live && !f.change.json);
        fixture_finish(&f);
    }
    for (size_t i = 1; i < sizeof(initial_failures) / sizeof(*initial_failures); i++) {
        fixture f;
        fixture_start(&f);
        start_monitor(&f, true);
        f.next_kind = initial_failures[i];
        assert(poll_monitor(&f, false) == statuses[i] && !f.change.json);
        assert(f.live == 1); /* Caller-owned monitor survives until explicit destroy. */
        fixture_finish(&f);
    }

    fixture f;
    fixture_start(&f);
    start_monitor(&f, true);
    f.next_kind = FAKE_REOPEN;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(f.change.reopened && f.change.full_scan && f.created == 2 && f.destroyed == 1);
    assert(f.live == 1 && change_flag(&f, "watch_available"));
    fixture_finish(&f);

    fixture_start(&f);
    start_monitor(&f, true);
    f.create_error = FORGE_ERR_UNSUPPORTED;
    f.next_kind = FAKE_REOPEN;
    assert(poll_monitor(&f, false) == FORGE_ERR_UNSUPPORTED);
    assert(!f.change.json && !f.live && f.created == f.destroyed);
    fixture_finish(&f);

    fixture_start(&f);
    start_monitor(&f, false);
    f.create_error = FORGE_ERR_UNSUPPORTED;
    f.next_kind = FAKE_REOPEN;
    assert(poll_monitor(&f, false) == FORGE_OK);
    assert(f.change.reopened && f.change.changed && !f.change.native && !f.live);
    assert(!change_flag(&f, "watch_available"));
    uint64_t generation = f.change.generation;
    assert(poll_monitor(&f, false) == FORGE_OK && !f.change.changed);
    assert(f.change.generation == generation);
    fixture_finish(&f);

    fg_repo_monitor_destroy(NULL);
    fg_repo_change_free(NULL);
    forge_watch_destroy(NULL);
}

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_snapshot_fallback_and_noops();
    test_incomplete_and_refused_snapshots();
    test_native_noops_and_unindexed_signal();
    test_hardlink_alias_forces_full_scan();
    test_removed_hardlink_alias_forces_full_scan();
    test_events_queued_during_full_index();
    test_cancellation_and_absolute_deadline();
    test_creation_poll_and_reopen_cleanup();
    puts("monitor coordinator tests passed (fake watcher; real index and input snapshots)");
    return 0;
}
