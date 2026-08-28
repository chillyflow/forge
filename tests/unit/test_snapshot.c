#include "internal.h"
#include "core/input_snapshot.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* All fixtures are private temporary directories. These tests execute no
 * subprocesses and do not index the repository or load a model. */
typedef struct {
    char root[FG_PATH_MAX];
    char *files[256], *directories[256];
    size_t file_count, directory_count;
} fixture;

#ifdef _WIN32
static void fixture_wide(const char *path, wchar_t wide[FG_PATH_MAX]) {
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FG_PATH_MAX));
}
static bool fixture_mkdir(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    fixture_wide(path, wide);
    return CreateDirectoryW(wide, NULL) != 0;
}
static bool fixture_unlink(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    fixture_wide(path, wide);
    return DeleteFileW(wide) != 0;
}
static bool fixture_rmdir(const char *path) {
    wchar_t wide[FG_PATH_MAX];
    fixture_wide(path, wide);
    return RemoveDirectoryW(wide) != 0;
}
#else
static bool fixture_mkdir(const char *path) {
    return mkdir(path, 0700) == 0;
}
static bool fixture_unlink(const char *path) {
    return unlink(path) == 0;
}
static bool fixture_rmdir(const char *path) {
    return rmdir(path) == 0;
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
    int length = snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(base));
#endif
    assert(fg_random_hex(random, 16));
    snprintf(name, sizeof(name), "forge-snapshot-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(fixture_mkdir(f->root));
    /* Some hosts' temp directory is reached through a system symlink. The
     * component rejects symlink ancestors intentionally, so use the real root. */
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
}

static void fixture_directory(fixture *f, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    for (size_t i = 0; i < f->directory_count; i++)
        if (!strcmp(f->directories[i], path))
            return;
    assert(f->directory_count < sizeof(f->directories) / sizeof(*f->directories));
    assert(fixture_mkdir(path));
    f->directories[f->directory_count++] = fg_strdup(path);
    assert(f->directories[f->directory_count - 1]);
}

static void fixture_record_file(fixture *f, const char *path) {
    for (size_t i = 0; i < f->file_count; i++)
        if (!strcmp(f->files[i], path))
            return;
    assert(f->file_count < sizeof(f->files) / sizeof(*f->files));
    f->files[f->file_count++] = fg_strdup(path);
    assert(f->files[f->file_count - 1]);
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
    assert(fg_path_join(path, f->root, relative));
    fixture_record_file(f, path);
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    fixture_wide(path, wide);
    HANDLE file =
        CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    assert(length <= UINT32_MAX);
    DWORD written = 0;
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
    assert(fixture_unlink(path));
}

static void fixture_finish(fixture *f) {
    for (size_t i = 0; i < f->file_count; i++) {
        fixture_unlink(f->files[i]); /* A deletion test may already have removed it. */
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i > 0; i--) {
        assert(fixture_rmdir(f->directories[i - 1]));
        free(f->directories[i - 1]);
    }
    assert(fixture_rmdir(f->root));
}

static fg_input_snapshot *take(fixture *f) {
    forge_error error = {0};
    fg_input_snapshot *result =
        fg_input_snapshot_take(f->root, 256, UINT64_C(16) * 1024 * 1024, NULL, NULL, 0, &error);
    if (!result)
        fprintf(stderr, "snapshot: %s\n", error.message);
    assert(result);
    return result;
}

static void changed(fixture *f, fg_input_snapshot **previous) {
    fg_input_snapshot *current = take(f);
    assert(!fg_input_snapshot_equal(*previous, current));
    assert(fg_input_snapshot_hash(*previous) != fg_input_snapshot_hash(current));
    fg_input_snapshot_destroy(*previous);
    *previous = current;
}

static void test_all_file_inputs_and_mutations(void) {
    fixture f;
    fixture_start(&f);
    fg_input_snapshot *before = take(&f);
    fixture_write(&f, "testdata/input.txt", "first", 5);
    changed(&f, &before);
    fixture_write(&f, "testdata/input.txt", "other", 5); /* Same length. */
    changed(&f, &before);
    fixture_write(&f, "fixture.csv", "a,b\n", 4);
    changed(&f, &before);
    const unsigned char binary[] = {'a', 0, 0xff, 'z'};
    fixture_write(&f, "vendor/data.bin", binary, sizeof(binary));
    changed(&f, &before);
    const unsigned char modified[] = {'a', 0, 0xfe, 'z'};
    fixture_write(&f, "vendor/data.bin", modified, sizeof(modified));
    changed(&f, &before);
    fixture_write(&f, ".hidden/input", "hidden", 6);
    changed(&f, &before);
    fixture_write(&f, "empty", "", 0);
    changed(&f, &before);
    fixture_write(&f, "\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac.txt", "unicode", 7);
    changed(&f, &before);
    fixture_remove(&f, "fixture.csv");
    changed(&f, &before);
    fg_input_snapshot *same = take(&f);
    assert(fg_input_snapshot_equal(before, same));
    assert(fg_input_snapshot_hash(before) == fg_input_snapshot_hash(same));
    fg_input_snapshot_destroy(before);
    fg_input_snapshot_destroy(same);
    fixture_finish(&f);
}

static void test_stable_path_order_and_framing(void) {
    fixture a, b;
    fixture_start(&a);
    fixture_start(&b);
    fixture_write(&a, "z/last.txt", "z", 1);
    fixture_write(&a, "b.bin", "\0x", 2);
    fixture_write(&a, "a.csv", "a", 1);
    fixture_write(&b, "a.csv", "a", 1);
    fixture_write(&b, "b.bin", "\0x", 2);
    fixture_write(&b, "z/last.txt", "z", 1);
    fg_input_snapshot *left = take(&a), *right = take(&b);
    assert(fg_input_snapshot_equal(left, right));
    assert(fg_input_snapshot_hash(left) == fg_input_snapshot_hash(right));
    fixture_remove(&b, "b.bin");
    fixture_write(&b, "renamed.bin", "\0x", 2);
    fg_input_snapshot *renamed = take(&b);
    assert(!fg_input_snapshot_equal(left, renamed));
    fg_input_snapshot_destroy(left);
    fg_input_snapshot_destroy(right);
    fg_input_snapshot_destroy(renamed);
    fixture_finish(&a);
    fixture_finish(&b);
}

static void test_metadata_exclusions_are_exact(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "input.txt", "x", 1);
    fixture_write(&f, ".git/config", "repository metadata", 19);
    fixture_write(&f, ".forge/sessions/output.bin", "\0binary", 7);
    forge_error error = {0};
    fg_input_snapshot *before = fg_input_snapshot_take(f.root, 1, 1, NULL, NULL, 0, &error);
    assert(before); /* Excluded files do not consume either content/file budget. */
    fixture_write(&f, ".git/config", "updated metadata", 16);
    fixture_write(&f, ".forge/another.json", "{}", 2);
    fg_input_snapshot *same = take(&f);
    assert(fg_input_snapshot_equal(before, same));
    fg_input_snapshot_destroy(same);
    fixture_write(&f, "testdata/.forge/fixture.txt", "nested", 6);
    changed(&f, &before);
    fixture_write(&f, "vendor/.git/fixture.csv", "a,b", 3);
    changed(&f, &before);
    fixture_write(&f, ".gitignore", "ignored.txt", 11);
    changed(&f, &before);
    fixture_write(&f, "ignored.txt", "still an input", 14);
    changed(&f, &before);
    fg_input_snapshot_destroy(before);
    fixture_finish(&f);

    fixture_start(&f);
    fixture_write(&f, ".git", "gitdir: elsewhere", 17);
    before = take(&f);
    fixture_write(&f, ".git", "gitdir: changed", 15);
    changed(&f, &before); /* A worktree .git file is not an excluded directory. */
    fixture_write(&f, ".forge", "ordinary file", 13);
    changed(&f, &before);
    fg_input_snapshot_destroy(before);
    fixture_finish(&f);
}

typedef struct {
    size_t calls, at;
    bool transient;
} cancellation;
static bool cancel_snapshot(void *user) {
    cancellation *cancel = user;
    cancel->calls++;
    return cancel->transient ? cancel->calls == cancel->at : cancel->calls >= cancel->at;
}

static void test_limits_cancellation_and_recovery(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "a.txt", "abc", 3);
    fixture_write(&f, "b.txt", "defg", 4);
    forge_error error = {0};
    fg_input_snapshot *exact = fg_input_snapshot_take(f.root, 2, 7, NULL, NULL, 0, &error);
    assert(exact);
    assert(!fg_input_snapshot_take(f.root, 1, 7, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    assert(!fg_input_snapshot_take(f.root, 2, 6, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    assert(!fg_input_snapshot_take(f.root, 2, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    cancellation cancel = {0, 1, false};
    assert(!fg_input_snapshot_take(f.root, 2, 7, cancel_snapshot, &cancel, 0, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    assert(!fg_input_snapshot_take(f.root, 2, 7, NULL, NULL, 1, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    fg_input_snapshot *recovered = take(&f);
    assert(fg_input_snapshot_equal(exact, recovered));
    fg_input_snapshot_destroy(exact);
    fg_input_snapshot_destroy(recovered);
    fixture_finish(&f);

    fixture_start(&f);
    size_t length = 2u * 1024u * 1024u;
    unsigned char *bytes = malloc(length);
    assert(bytes);
    for (size_t i = 0; i < length; i++)
        bytes[i] = (unsigned char)(i % 251);
    fixture_write(&f, "large.bin", bytes, length);
    exact = fg_input_snapshot_take(f.root, 1, (uint64_t)length, NULL, NULL, 0, &error);
    assert(exact);
    cancel = (cancellation){0, 20, true};
    assert(
        !fg_input_snapshot_take(f.root, 1, (uint64_t)length, cancel_snapshot, &cancel, 0, &error));
    assert(error.code == FORGE_ERR_CANCELLED && cancel.calls >= 20);
    bytes[length - 1] ^= 1u;
    fixture_write(&f, "large.bin", bytes, length);
    changed(&f, &exact); /* Detect a change beyond many streaming blocks. */
    free(bytes);
    fg_input_snapshot_destroy(exact);
    fixture_finish(&f);
}

static void test_depth_and_path_bounds(void) {
    fixture f;
    fixture_start(&f);
    char relative[FG_PATH_MAX] = "";
    for (size_t i = 0; i < FG_INPUT_SNAPSHOT_MAX_DEPTH; i++) {
        if (i)
            strcat(relative, "/");
        strcat(relative, "d");
        fixture_directory(&f, relative);
    }
    char leaf[FG_PATH_MAX];
    assert(fg_path_join(leaf, relative, "x"));
    fixture_write(&f, leaf, "x", 1);
    fg_input_snapshot *at_limit = take(&f);
    fg_input_snapshot_destroy(at_limit);
    strcat(relative, "/d");
    fixture_directory(&f, relative);
    forge_error error = {0};
    assert(!fg_input_snapshot_take(f.root, 256, 1024, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    fixture_finish(&f);
    char too_long[FG_PATH_MAX + 1];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    assert(!fg_input_snapshot_take(too_long, 1, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
}

static bool fixture_symlink(const char *target, const char *path, bool directory) {
#ifdef _WIN32
    wchar_t wide_target[FG_PATH_MAX], wide_path[FG_PATH_MAX];
    fixture_wide(target, wide_target);
    fixture_wide(path, wide_path);
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(wide_path, wide_target, flags | 0x2u))
        return true;
    if (GetLastError() == ERROR_INVALID_PARAMETER &&
        CreateSymbolicLinkW(wide_path, wide_target, flags))
        return true;
    DWORD code = GetLastError();
    assert(code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_ACCESS_DENIED ||
           code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED);
    return false;
#else
    (void)directory;
    assert(symlink(target, path) == 0);
    return true;
#endif
}

static void test_links_and_special_files_are_denied(void) {
    fixture inside, outside;
    fixture_start(&inside);
    fixture_start(&outside);
    fixture_write(&outside, "outside.txt", "must not follow", 15);
    char target[FG_PATH_MAX], link[FG_PATH_MAX];
    assert(fg_path_join(target, outside.root, "outside.txt"));
    assert(fg_path_join(link, inside.root, "link.txt"));
    forge_error error = {0};
    if (fixture_symlink(target, link, false)) {
        assert(!fg_input_snapshot_take(inside.root, 10, 100, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_POLICY);
        assert(fixture_unlink(link));
        assert(fg_path_join(link, inside.root, "linked_directory"));
        assert(fixture_symlink(outside.root, link, true));
        assert(!fg_input_snapshot_take(inside.root, 10, 100, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_POLICY);
        assert(!fg_input_snapshot_take(link, 10, 100, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_POLICY); /* Reject a linked root as well. */
#ifdef _WIN32
        assert(fixture_rmdir(link));
#else
        assert(fixture_unlink(link));
#endif
        assert(fg_path_join(link, inside.root, ".forge"));
        assert(fixture_symlink(outside.root, link, true));
        assert(!fg_input_snapshot_take(inside.root, 10, 100, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_POLICY); /* Exclusion cannot hide a link itself. */
#ifdef _WIN32
        assert(fixture_rmdir(link));
#else
        assert(fixture_unlink(link));
#endif
    } else
        puts("SKIP snapshot symlink checks: this host does not permit creating symlinks");
#ifndef _WIN32
    assert(fg_path_join(link, inside.root, "pipe"));
    assert(mkfifo(link, 0600) == 0);
    assert(!fg_input_snapshot_take(inside.root, 10, 100, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(fixture_unlink(link));
#endif
    fg_input_snapshot *recovered = take(&inside);
    fg_input_snapshot_destroy(recovered);
    fixture_finish(&inside);
    fixture_finish(&outside);
}

#ifdef _WIN32
/* Junctions are reparse points that ordinary directory owners can create even
 * when the host denies symbolic-link creation. Exercise the rejection path
 * without invoking mklink or requiring administrative privileges. */
static void test_windows_junction_denial(void) {
    fixture inside, outside;
    fixture_start(&inside);
    fixture_start(&outside);
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, inside.root, "junction"));
    assert(fixture_mkdir(path));
    wchar_t wide_path[FG_PATH_MAX], wide_target[FG_PATH_MAX];
    fixture_wide(path, wide_path);
    fixture_wide(outside.root, wide_target);
    struct {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[FG_PATH_MAX];
    } data = {0};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    size_t target_length = wcslen(wide_target), substitute_length = target_length + 4;
    assert(substitute_length + target_length + 2 < FG_PATH_MAX);
    memcpy(data.path, L"\\??\\", 4 * sizeof(wchar_t));
    memcpy(data.path + 4, wide_target, (target_length + 1) * sizeof(wchar_t));
    memcpy(data.path + substitute_length + 1, wide_target, (target_length + 1) * sizeof(wchar_t));
    data.substitute_length = (WORD)(substitute_length * sizeof(wchar_t));
    data.print_offset = (WORD)((substitute_length + 1) * sizeof(wchar_t));
    data.print_length = (WORD)(target_length * sizeof(wchar_t));
    data.data_length = (WORD)(8 + (substitute_length + target_length + 2) * sizeof(wchar_t));
    HANDLE directory = CreateFileW(wide_path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    assert(directory != INVALID_HANDLE_VALUE);
    DWORD returned = 0;
    assert(DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data, (DWORD)data.data_length + 8,
                           NULL, 0, &returned, NULL));
    assert(CloseHandle(directory));
    forge_error error = {0};
    assert(!fg_input_snapshot_take(inside.root, 10, 100, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(!fg_input_snapshot_take(path, 10, 100, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(fixture_rmdir(path));
    fixture_finish(&inside);
    fixture_finish(&outside);
}
#endif

static void test_read_errors_and_invalid_arguments(void) {
    forge_error error = {0};
    assert(!fg_input_snapshot_equal(NULL, NULL));
    assert(fg_input_snapshot_hash(NULL) == 0);
    fg_input_snapshot_destroy(NULL);
    assert(!fg_input_snapshot_take(NULL, 1, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!fg_input_snapshot_take(".", 0, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!fg_input_snapshot_take(".", 1, 0, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    fixture f;
    fixture_start(&f);
    char missing[FG_PATH_MAX], path[FG_PATH_MAX];
    assert(fg_path_join(missing, f.root, "missing"));
    assert(!fg_input_snapshot_take(missing, 1, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_IO);
    fixture_write(&f, "locked.txt", "x", 1);
    assert(fg_path_join(path, f.root, "locked.txt"));
    assert(!fg_input_snapshot_take(path, 1, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_POLICY);
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    fixture_wide(path, wide);
    HANDLE held =
        CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    assert(!fg_input_snapshot_take(f.root, 1, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_IO);
    assert(CloseHandle(held));
#else
    if (geteuid() != 0) {
        assert(chmod(path, 0000) == 0);
        assert(!fg_input_snapshot_take(f.root, 1, 1, NULL, NULL, 0, &error));
        assert(error.code == FORGE_ERR_IO);
        assert(chmod(path, 0600) == 0);
    }
#endif
    fg_input_snapshot *recovered = take(&f);
    assert(!fg_input_snapshot_equal(recovered, NULL));
    assert(!fg_input_snapshot_equal(NULL, recovered));
    fg_input_snapshot_destroy(recovered);
    fixture_finish(&f);
}

int main(void) {
    test_all_file_inputs_and_mutations();
    test_stable_path_order_and_framing();
    test_metadata_exclusions_are_exact();
    test_limits_cancellation_and_recovery();
    test_depth_and_path_bounds();
    test_links_and_special_files_are_denied();
#ifdef _WIN32
    test_windows_junction_denial();
#endif
    test_read_errors_and_invalid_arguments();
    puts("input snapshot tests passed");
    return 0;
}
