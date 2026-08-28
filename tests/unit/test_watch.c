#include "internal.h"
#include "forge/watch.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    char root[FG_PATH_MAX];
    char *files[256], *directories[256];
    size_t file_count, directory_count;
} fixture;

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
    return DeleteFileW(wide) != 0;
}
static bool rename_path(const char *from, const char *to) {
    wchar_t left[FG_PATH_MAX], right[FG_PATH_MAX];
    wide_path(from, left);
    wide_path(to, right);
    return MoveFileExW(left, right, 0) != 0;
}
#else
static bool make_directory(const char *path) {
    return mkdir(path, 0700) == 0;
}
static bool remove_directory(const char *path) {
    return rmdir(path) == 0;
}
static bool remove_file(const char *path) {
    return unlink(path) == 0;
}
static bool rename_path(const char *from, const char *to) {
    return rename(from, to) == 0;
}
#endif

static void fixture_start(fixture *f) {
    memset(f, 0, sizeof(*f));
    char base[FG_PATH_MAX], random[33], name[64], canonical[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(base), base);
    assert(length && length < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    assert(snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp") > 0);
#endif
    assert(fg_random_hex(random, 16));
    snprintf(name, sizeof(name), "forge-watch-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(make_directory(f->root));
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
}

static void fixture_directory(fixture *f, const char *relative) {
    for (size_t i = 0; i < f->directory_count; i++)
        if (!strcmp(f->directories[i], relative))
            return;
    assert(f->directory_count < 256);
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    assert(make_directory(path));
    f->directories[f->directory_count++] = fg_strdup(relative);
    assert(f->directories[f->directory_count - 1]);
}

static void fixture_write(fixture *f, const char *relative, const void *bytes, size_t length) {
    char parents[FG_PATH_MAX], path[FG_PATH_MAX];
    assert(strlen(relative) < sizeof(parents));
    strcpy(parents, relative);
    for (char *p = parents; *p; p++)
        if (*p == '/') {
            *p = 0;
            fixture_directory(f, parents);
            *p = '/';
        }
    bool known = false;
    for (size_t i = 0; i < f->file_count; i++)
        known |= !strcmp(f->files[i], relative);
    if (!known) {
        assert(f->file_count < 256);
        f->files[f->file_count++] = fg_strdup(relative);
        assert(f->files[f->file_count - 1]);
    }
    assert(fg_path_join(path, f->root, relative));
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    wide_path(path, wide);
    HANDLE file =
        CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    assert(length <= UINT32_MAX);
    assert(WriteFile(file, bytes, (DWORD)length, &written, NULL) && written == length);
    assert(CloseHandle(file));
#else
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
#endif
}

static void fixture_remove(fixture *f, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    assert(remove_file(path));
}

static void fixture_rename(fixture *f, const char *from, const char *to) {
    char left[FG_PATH_MAX], right[FG_PATH_MAX];
    assert(fg_path_join(left, f->root, from));
    assert(fg_path_join(right, f->root, to));
    assert(rename_path(left, right));
    for (size_t group = 0; group < 2; group++) {
        char **items = group ? f->directories : f->files;
        size_t count = group ? f->directory_count : f->file_count;
        for (size_t i = 0; i < count; i++) {
            size_t length = strlen(from);
            if (!strncmp(items[i], from, length) &&
                (!items[i][length] || items[i][length] == '/')) {
                char next[FG_PATH_MAX];
                assert(snprintf(next, sizeof(next), "%s%s", to, items[i] + length) > 0);
                free(items[i]);
                items[i] = fg_strdup(next);
                assert(items[i]);
            }
        }
    }
}

static void fixture_finish(fixture *f) {
    char path[FG_PATH_MAX];
    for (size_t i = 0; i < f->file_count; i++) {
        assert(fg_path_join(path, f->root, f->files[i]));
        remove_file(path); /* Deletion cases may have removed it already. */
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i > 0; i--) {
        assert(fg_path_join(path, f->root, f->directories[i - 1]));
        assert(remove_directory(path));
        free(f->directories[i - 1]);
    }
    assert(remove_directory(f->root));
}

static forge_watch *create(fixture *f, const forge_watch_limits *limits) {
    forge_error error = {0};
    forge_watch *watch = forge_watch_create(f->root, limits, NULL, NULL, 5000, &error);
    if (!watch)
        fprintf(stderr, "watch create: %s\n", error.message);
    assert(watch && error.code == FORGE_OK);
    return watch;
}

static yyjson_doc *poll_batch(forge_watch *watch, uint64_t timeout, size_t maximum) {
    forge_error error = {0};
    char *json = forge_watch_poll(watch, timeout, NULL, NULL, &error);
    if (!json)
        fprintf(stderr, "watch poll: %s\n", error.message);
    assert(json && error.code == FORGE_OK);
    assert(strlen(json) <= maximum);
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc)
        fprintf(stderr, "%s\n", json);
    assert(doc);
    forge_free(json);
    yyjson_val *root = yyjson_doc_get_root(doc), *events = yyjson_obj_get(root, "events");
    assert(yyjson_is_arr(events));
    const char *previous = NULL;
    size_t i, count;
    yyjson_val *event;
    yyjson_arr_foreach(events, i, count, event) {
        const char *path = fg_json_str(event, "path");
        assert(path && fg_utf8_valid(path, strlen(path)));
        assert(!previous || strcmp(previous, path) < 0); /* Sorted and coalesced. */
        assert(yyjson_is_uint(yyjson_obj_get(event, "flags")));
        previous = path;
    }
    return doc;
}

static bool flag(yyjson_doc *doc, const char *key) {
    return yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), key));
}

static uint64_t number(yyjson_doc *doc, const char *key) {
    return yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), key));
}

static void describe_batch(const char *label, yyjson_doc *doc) {
    char *json = yyjson_write(doc, 0, NULL);
    fprintf(stderr, "%s: %s\n", label, json ? json : "<JSON allocation failed>");
    free(json);
}

static size_t initial(forge_watch *watch) {
    yyjson_doc *doc = poll_batch(watch, 0, FG_MAX_JSON);
    assert(flag(doc, "initial_scan_required") && flag(doc, "rescan_required"));
    assert(!flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_INITIAL);
    size_t directories = (size_t)number(doc, "directories");
    yyjson_doc_free(doc);
    return directories;
}

static void collect(forge_watch *watch, const char *const *paths, size_t count, unsigned required,
                    unsigned *seen) {
    memset(seen, 0, count * sizeof(*seen));
    uint64_t deadline = fg_now_ms() + 4000;
    bool complete = false;
    while (!complete && fg_now_ms() < deadline) {
        yyjson_doc *doc = poll_batch(watch, 100, FG_MAX_JSON);
        yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events"), *event;
        size_t i, length;
        yyjson_arr_foreach(events, i, length,
                           event) for (size_t j = 0; j < count;
                                       j++) if (!strcmp(fg_json_str(event, "path"), paths[j]))
            seen[j] |= (unsigned)yyjson_get_uint(yyjson_obj_get(event, "flags"));
        yyjson_doc_free(doc);
        complete = true;
        for (size_t j = 0; j < count; j++)
            complete &= (seen[j] & required) == required;
    }
    if (!complete)
        for (size_t i = 0; i < count; i++)
            fprintf(stderr, "watch wanted %s mask=%u, saw=%u\n", paths[i], required, seen[i]);
    assert(complete);
}

static void test_initial_timeout_and_files(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "existing/fixture.csv", "before", 6);
    forge_watch *watch = create(&f, NULL);
    assert(initial(watch) == 2);
    /* FSEvents can deliver fixture-creation notifications after the explicit
     * initial batch. Drain them with a bound, then require a real quiet timeout;
     * never assert that a newly created watcher starts with an empty OS queue. */
    uint64_t deadline = fg_now_ms() + 4000, start = 0;
    yyjson_doc *doc = NULL;
    do {
        yyjson_doc_free(doc);
        assert(fg_now_ms() < deadline);
        start = fg_now_ms();
        doc = poll_batch(watch, 30, FG_MAX_JSON);
        if (flag(doc, "initial_scan_required") || flag(doc, "reopen_required"))
            describe_batch("unexpected initial timeout batch", doc);
        assert(!flag(doc, "initial_scan_required") && !flag(doc, "reopen_required"));
    } while (!flag(doc, "timed_out") || flag(doc, "rescan_required") ||
             yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "events")));
    assert(fg_now_ms() - start >= 20 && fg_now_ms() - start < 1500);
    yyjson_doc_free(doc);
    const char *paths[] = {"z.txt", "a.txt", "unicode-\xC2\xB5-\xE9\x9B\xAA.bin"};
    const unsigned char bytes[] = {0, 255, 1, 0, 2};
    for (size_t i = 0; i < 3; i++)
        fixture_write(&f, paths[i], bytes, sizeof(bytes));
    unsigned seen[3];
    collect(watch, paths, 3, FORGE_WATCH_CREATED, seen);
    fixture_write(&f, paths[1], "changed", 7);
    collect(watch, paths + 1, 1, FORGE_WATCH_MODIFIED, seen);
    fixture_rename(&f, "a.txt", "renamed.txt");
    const char *renamed[] = {"a.txt", "renamed.txt"};
#if !defined(__APPLE__)
    collect(watch, renamed, 2, FORGE_WATCH_RENAMED, seen);
    assert((seen[0] & (FORGE_WATCH_RENAMED_FROM | FORGE_WATCH_DELETED)) ==
           (FORGE_WATCH_RENAMED_FROM | FORGE_WATCH_DELETED));
    assert((seen[1] & (FORGE_WATCH_RENAMED_TO | FORGE_WATCH_CREATED)) ==
           (FORGE_WATCH_RENAMED_TO | FORGE_WATCH_CREATED));
#else
    /* FSEvents does not promise a matched pair or rename direction. */
    bool saw_rename = false;
    uint64_t rename_deadline = fg_now_ms() + 4000;
    while (!saw_rename && fg_now_ms() < rename_deadline) {
        doc = poll_batch(watch, 100, FG_MAX_JSON);
        yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events"), *event;
        size_t i, count;
        yyjson_arr_foreach(events, i, count, event) {
            const char *path = fg_json_str(event, "path");
            if (!strcmp(path, renamed[0]) || !strcmp(path, renamed[1]))
                saw_rename |=
                    (yyjson_get_uint(yyjson_obj_get(event, "flags")) & FORGE_WATCH_RENAMED) != 0;
        }
        yyjson_doc_free(doc);
    }
    assert(saw_rename);
#endif
    fixture_remove(&f, "renamed.txt");
    collect(watch, renamed + 1, 1, FORGE_WATCH_DELETED, seen);
    const char *nested[] = {"existing/fixture.csv"};
    fixture_write(&f, nested[0], "after\0binary", 12);
    collect(watch, nested, 1, FORGE_WATCH_MODIFIED, seen);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

static yyjson_doc *wait_rescan(forge_watch *watch, size_t maximum) {
    uint64_t deadline = fg_now_ms() + 4000;
    while (fg_now_ms() < deadline) {
        yyjson_doc *doc = poll_batch(watch, 100, maximum);
        if (flag(doc, "rescan_required"))
            return doc;
        yyjson_doc_free(doc);
    }
    assert(!"Expected filesystem rescan signal");
    return NULL;
}

static void test_subtree_enrollment_and_rename_recovery(void) {
    fixture f;
    fixture_start(&f);
    forge_watch *watch = create(&f, NULL);
    initial(watch);
    fixture_write(&f, "new/nested/fixture.txt", "input", 5);
    yyjson_doc *doc = wait_rescan(watch, FG_MAX_JSON);
    assert(!flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_SUBTREE);
    assert(number(doc, "directories") == 3);
    yyjson_doc_free(doc);
    const char *path[] = {"new/nested/fixture.txt"};
    fixture_write(&f, path[0], "edited", 6);
    unsigned seen;
    collect(watch, path, 1, FORGE_WATCH_MODIFIED, &seen);
    fixture_rename(&f, "new", "moved");
    doc = wait_rescan(watch, FG_MAX_JSON);
    assert(flag(doc, "reopen_required"));
    yyjson_doc_free(doc);
    doc = poll_batch(watch, 0, FG_MAX_JSON);
    assert(flag(doc, "reopen_required") && flag(doc, "rescan_required"));
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    watch = create(&f, NULL); /* Reopen before the caller's full index. */
    assert(initial(watch) == 3);
    const char *moved[] = {"moved/nested/fixture.txt"};
    fixture_write(&f, moved[0], "recovered", 9);
    collect(watch, moved, 1, FORGE_WATCH_MODIFIED, &seen);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

static void test_metadata_exclusions(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, ".git/objects/cache", "x", 1);
    fixture_write(&f, ".forge/cache/data", "x", 1);
    fixture_write(&f, "nested/.git/data", "x", 1);
    forge_watch *watch = create(&f, NULL);
    assert(initial(watch) == 3);
    fixture_write(&f, ".git/objects/cache", "new", 3);
    fixture_write(&f, ".forge/cache/data", "new", 3);
    yyjson_doc *doc = poll_batch(watch, 80, FG_MAX_JSON);
    yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events"), *event;
    size_t i, count;
    yyjson_arr_foreach(events, i, count, event) {
        const char *path = fg_json_str(event, "path");
        assert(strncmp(path, ".git", 4) && strncmp(path, ".forge", 6));
    }
    yyjson_doc_free(doc);
    const char *nested[] = {"nested/.git/data"};
    fixture_write(&f, nested[0], "changed", 7);
    unsigned seen;
    collect(watch, nested, 1, FORGE_WATCH_MODIFIED, &seen);
    forge_watch_destroy(watch);
    fixture_finish(&f);
    fixture_start(&f);
    watch = create(&f, NULL);
    initial(watch);
    fixture_write(&f, ".git", "gitdir: elsewhere", 17);
    const char *git_file[] = {".git"};
    collect(watch, git_file, 1, FORGE_WATCH_CREATED, &seen);
    forge_watch_destroy(watch);
    forge_watch_limits limits = forge_default_watch_limits();
    limits.max_path_bytes = 4;
    limits.max_directories = 1;
    watch = create(&f, &limits);
    initial(watch);
    fixture_write(&f, ".forge/deep/long-metadata-path.bin", "ignored", 7);
    doc = poll_batch(watch, 100, FG_MAX_JSON);
    assert(!flag(doc, "rescan_required"));
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "events")) == 0);
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

typedef struct {
    size_t calls, after;
} cancellation;
static bool cancel_after(void *user) {
    cancellation *state = user;
    return ++state->calls >= state->after;
}

static bool delay_check(void *user) {
    (void)user;
#ifdef _WIN32
    Sleep(5);
#else
    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
#endif
    return false;
}

static void test_cancel_deadline_and_invalidation(void) {
    fixture f;
    fixture_start(&f);
    forge_error error = {0};
    cancellation cancel = {0, 1};
    assert(!forge_watch_create(f.root, NULL, cancel_after, &cancel, 100, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    assert(!forge_watch_create(f.root, NULL, delay_check, NULL, 1, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    forge_watch *watch = create(&f, NULL);
    initial(watch);
    fixture_write(&f, "retained.txt", "keep", 4);
    cancel = (cancellation){0, 1};
    char *json = forge_watch_poll(watch, 1000, cancel_after, &cancel, &error);
    assert(!json && error.code == FORGE_ERR_CANCELLED);
    const char *path[] = {"retained.txt"};
    unsigned seen;
    collect(watch, path, 1, FORGE_WATCH_CREATED, &seen);
#if !defined(__APPLE__)
    fixture_write(&f, path[0], "retained after partial native drain", 35);
    cancel = (cancellation){0, 2};
    json = forge_watch_poll(watch, 1000, cancel_after, &cancel, &error);
    assert(!json && error.code == FORGE_ERR_CANCELLED);
    collect(watch, path, 1, FORGE_WATCH_MODIFIED, &seen);
#endif
    forge_watch_invalidate(watch);
    yyjson_doc *doc = poll_batch(watch, 0, FG_MAX_JSON);
    assert(flag(doc, "rescan_required") && flag(doc, "reopen_required"));
    assert(flag(doc, "dropped_events_unknown"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_CALLER);
    yyjson_doc_free(doc);
    doc = poll_batch(watch, 0, FG_MAX_JSON);
    assert(flag(doc, "rescan_required") && flag(doc, "reopen_required"));
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

static void test_creation_bounds_and_arguments(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "nested/one.txt", "1", 1);
    fixture_write(&f, "two.txt", "2", 1);
    forge_error error = {0};
    forge_watch_limits limits = forge_default_watch_limits();
    limits.max_directories = 1;
    assert(!forge_watch_create(f.root, &limits, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    limits = forge_default_watch_limits();
    limits.max_depth = 0;
    assert(!forge_watch_create(f.root, &limits, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    limits = forge_default_watch_limits();
    limits.max_path_bytes = 4;
    assert(!forge_watch_create(f.root, &limits, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    limits = forge_default_watch_limits();
    limits.max_scan_entries = 1;
    assert(!forge_watch_create(f.root, &limits, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    limits = forge_default_watch_limits();
    limits.max_events = 0;
    assert(!forge_watch_create(f.root, &limits, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_watch_create(NULL, NULL, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_watch_poll(NULL, 0, NULL, NULL, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    char missing[FG_PATH_MAX], file[FG_PATH_MAX];
    assert(fg_path_join(missing, f.root, "missing"));
    assert(fg_path_join(file, f.root, "two.txt"));
    assert(!forge_watch_create(missing, NULL, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_IO);
    assert(!forge_watch_create(file, NULL, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_POLICY);
    forge_watch_destroy(NULL);
    forge_watch_invalidate(NULL);
    forge_watch *watch = create(&f, NULL);
    initial(watch);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

static void test_event_byte_and_native_work_limits(void) {
    fixture f;
    fixture_start(&f);
    forge_watch_limits limits = forge_default_watch_limits();
    limits.max_events = 1;
    forge_watch *watch = create(&f, &limits);
    initial(watch);
    fixture_write(&f, "one.txt", "1", 1);
    fixture_write(&f, "two.txt", "2", 1);
    yyjson_doc *doc = wait_rescan(watch, limits.max_bytes);
    assert(flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_EVENT_LIMIT);
    assert(number(doc, "dropped_events") >= 1);
    assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "events")) <= 1);
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    limits = forge_default_watch_limits();
    limits.max_bytes = 1024;
    watch = create(&f, &limits);
    initial(watch);
    char long_name[121];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    fixture_write(&f, long_name, "x", 1);
    doc = wait_rescan(watch, limits.max_bytes);
    assert(flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_BYTE_LIMIT);
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    limits = forge_default_watch_limits();
    limits.max_native_events = 1;
    watch = create(&f, &limits);
    initial(watch);
    fixture_write(&f, "one.txt", "edited", 6);
    doc = poll_batch(watch, 100, limits.max_bytes);
    assert(flag(doc, "more_pending") || flag(doc, "reopen_required"));
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    fixture_finish(&f);
}

static void test_depth_and_runtime_enrollment_limits(void) {
    fixture f;
    fixture_start(&f);
    char deepest[256] = "";
    for (size_t i = 0; i < 64; i++) {
        if (i)
            strcat(deepest, "/");
        strcat(deepest, "d");
        fixture_directory(&f, deepest);
    }
    forge_watch *watch = create(&f, NULL);
    assert(initial(watch) == 65);
    char file[256];
    assert(snprintf(file, sizeof(file), "%s/input.bin", deepest) > 0);
    fixture_write(&f, file, "\0\1\2", 3);
    const char *paths[] = {file};
    unsigned seen;
    collect(watch, paths, 1, FORGE_WATCH_CREATED, &seen);
    strcat(deepest, "/d");
    fixture_directory(&f, deepest);
    yyjson_doc *doc = wait_rescan(watch, FG_MAX_JSON);
    assert(flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_DEPTH_LIMIT);
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    fixture_finish(&f);
    for (size_t i = 0; i < 3; i++) {
        fixture_start(&f);
        forge_watch_limits limits = forge_default_watch_limits();
        unsigned reason;
        if (!i) {
            limits.max_directories = 1;
            reason = FORGE_WATCH_RESCAN_DIRECTORY_LIMIT;
        } else if (i == 1) {
            limits.max_scan_entries = 1;
            reason = FORGE_WATCH_RESCAN_SCAN_LIMIT;
        } else {
            limits.max_path_bytes = 4;
            reason = FORGE_WATCH_RESCAN_PATH_LIMIT;
        }
        watch = create(&f, &limits);
        initial(watch);
        fixture_write(&f, "new/deep/input", "x", 1);
        doc = wait_rescan(watch, FG_MAX_JSON);
        assert(flag(doc, "reopen_required"));
        assert(number(doc, "reason_flags") & reason);
        assert(number(doc, "directories") <= limits.max_directories);
        yyjson_doc_free(doc);
        forge_watch_destroy(watch);
        fixture_finish(&f);
    }
}

#ifdef _WIN32
static void make_junction(const char *path, const char *target) {
    assert(make_directory(path));
    wchar_t wide[FG_PATH_MAX], destination[FG_PATH_MAX];
    wide_path(path, wide);
    wide_path(target, destination);
    struct {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[FG_PATH_MAX];
    } data = {0};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    size_t length = wcslen(destination), substitute = length + 4;
    assert(substitute + length + 2 < FG_PATH_MAX);
    memcpy(data.path, L"\\??\\", 4 * sizeof(wchar_t));
    memcpy(data.path + 4, destination, (length + 1) * sizeof(wchar_t));
    memcpy(data.path + substitute + 1, destination, (length + 1) * sizeof(wchar_t));
    data.substitute_length = (WORD)(substitute * sizeof(wchar_t));
    data.print_offset = (WORD)((substitute + 1) * sizeof(wchar_t));
    data.print_length = (WORD)(length * sizeof(wchar_t));
    data.data_length = (WORD)(8 + (substitute + length + 2) * sizeof(wchar_t));
    HANDLE directory = CreateFileW(wide, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    assert(directory != INVALID_HANDLE_VALUE);
    DWORD returned;
    assert(DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data, (DWORD)data.data_length + 8,
                           NULL, 0, &returned, NULL));
    assert(CloseHandle(directory));
}
#endif

static void test_link_directories_are_not_followed(void) {
    fixture inside, outside;
    fixture_start(&inside);
    fixture_start(&outside);
    fixture_write(&outside, "nested/private.txt", "outside", 7);
    char link[FG_PATH_MAX];
    assert(fg_path_join(link, inside.root, "linked"));
#ifdef _WIN32
    make_junction(link, outside.root);
#else
    assert(symlink(outside.root, link) == 0);
#endif
    forge_watch_limits limits = forge_default_watch_limits();
    limits.max_directories = 1;
    forge_watch *watch = create(&inside, &limits);
    assert(initial(watch) == 1);
    fixture_write(&outside, "nested/private.txt", "changed", 7);
    yyjson_doc *doc = poll_batch(watch, 80, FG_MAX_JSON);
    yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events"), *event;
    size_t i, count;
    yyjson_arr_foreach(events, i, count, event)
        assert(strncmp(fg_json_str(event, "path"), "linked/", 7));
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
    forge_error error = {0};
    assert(!forge_watch_create(link, NULL, NULL, NULL, 1000, &error));
    assert(error.code == FORGE_ERR_POLICY);
    char child[FG_PATH_MAX];
    assert(fg_path_join(child, link, "nested"));
    assert(!forge_watch_create(child, NULL, NULL, NULL, 1000, &error));
    assert(error.code == FORGE_ERR_POLICY);
#ifdef _WIN32
    assert(remove_directory(link));
#else
    assert(remove_file(link));
#endif
    fixture_finish(&inside);
    fixture_finish(&outside);
}

static void test_root_move_and_handle_cleanup(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "nested/file", "x", 1);
    forge_watch *watch = create(&f, NULL);
    initial(watch);
    char moved[FG_PATH_MAX];
    int count = snprintf(moved, sizeof(moved), "%s-moved", f.root);
    assert(count > 0 && (size_t)count < sizeof(moved));
    assert(rename_path(f.root, moved));
    strcpy(f.root, moved);
    yyjson_doc *doc = poll_batch(watch, 0, FG_MAX_JSON);
    assert(flag(doc, "reopen_required"));
    assert(number(doc, "reason_flags") & FORGE_WATCH_RESCAN_ROOT_CHANGED);
    yyjson_doc_free(doc);
    forge_watch_destroy(watch);
#ifdef _WIN32
    DWORD before, after;
    assert(GetProcessHandleCount(GetCurrentProcess(), &before));
#endif
    for (size_t i = 0; i < 32; i++) {
        watch = create(&f, NULL);
        initial(watch);
        forge_watch_destroy(watch);
        forge_watch_limits limits = forge_default_watch_limits();
        limits.max_directories = 1;
        forge_error error = {0};
        assert(!forge_watch_create(f.root, &limits, NULL, NULL, 1000, &error));
        assert(error.code == FORGE_ERR_LIMIT);
    }
#ifdef _WIN32
    assert(GetProcessHandleCount(GetCurrentProcess(), &after));
    assert(after <= before + 2);
#endif
    fixture_finish(&f);
}

int main(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    fputs("watch: files and timeout\n", stderr);
    test_initial_timeout_and_files();
    fputs("watch: subtree enrollment\n", stderr);
    test_subtree_enrollment_and_rename_recovery();
    fputs("watch: exclusions\n", stderr);
    test_metadata_exclusions();
    fputs("watch: cancellation\n", stderr);
    test_cancel_deadline_and_invalidation();
    fputs("watch: creation bounds\n", stderr);
    test_creation_bounds_and_arguments();
    fputs("watch: queue bounds\n", stderr);
    test_event_byte_and_native_work_limits();
    fputs("watch: depth and runtime bounds\n", stderr);
    test_depth_and_runtime_enrollment_limits();
    fputs("watch: directory links\n", stderr);
    test_link_directories_are_not_followed();
    fputs("watch: cleanup\n", stderr);
    test_root_move_and_handle_cleanup();
    puts("watch tests passed");
    return 0;
}
