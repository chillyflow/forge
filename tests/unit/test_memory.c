#include "internal.h"
#include "forge/memory.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdalign.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    size_t calls, fail_at, misalign_at, live_bytes, peak_bytes, live_count;
    void *pointers[32], *bases[32];
    size_t sizes[32];
} test_allocator;

static void *test_alloc(size_t bytes, void *user) {
    test_allocator *allocator = user;
    if (++allocator->calls == allocator->fail_at)
        return NULL;
    size_t index = 0;
    while (index < 32 && allocator->pointers[index])
        index++;
    assert(index < 32);
    bool misaligned = allocator->calls == allocator->misalign_at;
    assert(!misaligned || bytes < SIZE_MAX);
    void *base = malloc(bytes + (misaligned ? 1u : 0u));
    assert(base);
    void *result = misaligned ? (unsigned char *)base + 1 : base;
    allocator->pointers[index] = result;
    allocator->bases[index] = base;
    allocator->sizes[index] = bytes;
    allocator->live_count++;
    allocator->live_bytes += bytes;
    if (allocator->live_bytes > allocator->peak_bytes)
        allocator->peak_bytes = allocator->live_bytes;
    return result;
}

static void test_free(void *pointer, void *user) {
    test_allocator *allocator = user;
    size_t index = 0;
    while (index < 32 && allocator->pointers[index] != pointer)
        index++;
    assert(index < 32 && pointer);
    allocator->live_count--;
    allocator->live_bytes -= allocator->sizes[index];
    void *base = allocator->bases[index];
    allocator->pointers[index] = allocator->bases[index] = NULL;
    allocator->sizes[index] = 0;
    free(base);
}

static void expect_stats(forge_arena_stats actual, forge_arena_stats expected) {
    assert(actual.max_committed_bytes == expected.max_committed_bytes);
    assert(actual.committed_bytes == expected.committed_bytes);
    assert(actual.peak_committed_bytes == expected.peak_committed_bytes);
    assert(actual.used_bytes == expected.used_bytes);
    assert(actual.padding_bytes == expected.padding_bytes);
    assert(actual.peak_used_bytes == expected.peak_used_bytes);
    assert(actual.allocation_count == expected.allocation_count);
    assert(actual.block_count == expected.block_count);
}

static void test_arena_alignment_and_lifetimes(void) {
    test_allocator tracker = {0};
    forge_allocator hooks = {test_alloc, test_free, &tracker};
    forge_error error = {0};
    forge_arena *arena = forge_arena_create_with_allocator(256u * 1024u, &hooks, &error);
    assert(arena);
    forge_arena_stats initial = forge_arena_get_stats(arena);
    assert(initial.committed_bytes == tracker.live_bytes && initial.block_count == 0);
    char *first = forge_arena_alloc(arena, 13, &error);
    assert(first && (uintptr_t)first % alignof(long double) == 0);
    assert((uintptr_t)first % alignof(void *) == 0 && (uintptr_t)first % alignof(long long) == 0);
    memcpy(first, "keep original", 13);
    void *tiny = forge_arena_aligned_alloc(arena, 1, 3, &error);
    void *aligned = forge_arena_aligned_alloc(arena, 4096, 17, &error);
    assert(tiny && aligned && (uintptr_t)aligned % 4096 == 0);
    memset(tiny, 0xa5, 3);
    memset(aligned, 0x7f, 17);
    char *large = forge_arena_alloc(arena, 70u * 1024u, &error);
    assert(large);
    memset(large, 0xc3, 70u * 1024u);
    assert(!memcmp(first, "keep original", 13));
    assert(((unsigned char *)tiny)[2] == 0xa5 && ((unsigned char *)aligned)[16] == 0x7f);
    forge_arena_stats used = forge_arena_get_stats(arena);
    assert(used.used_bytes == 13 + 3 + 17 + 70u * 1024u);
    assert(used.allocation_count == 4 && used.block_count == 2);
    assert(used.committed_bytes == tracker.live_bytes);
    assert(tracker.peak_bytes <= used.max_committed_bytes);
    size_t calls = tracker.calls;
    forge_arena_reset(arena);
    forge_arena_stats reset = forge_arena_get_stats(arena);
    assert(reset.used_bytes == 0 && reset.padding_bytes == 0 && reset.allocation_count == 0);
    assert(reset.committed_bytes == used.committed_bytes && reset.block_count == used.block_count);
    assert(reset.peak_used_bytes == used.used_bytes && tracker.calls == calls);
    assert(forge_arena_alloc(arena, 13, &error) == first);
    assert(forge_arena_aligned_alloc(arena, 1, 3, &error) == tiny);
    assert(forge_arena_aligned_alloc(arena, 4096, 17, &error) == aligned);
    assert(forge_arena_alloc(arena, 70u * 1024u, &error) == large);
    assert(tracker.calls == calls); /* Reset reuses storage, not more tiny mallocs. */
    forge_arena_destroy(arena);
    assert(tracker.live_bytes == 0 && tracker.live_count == 0);
}

static void test_arena_transactional_failures(void) {
    test_allocator tracker = {0};
    forge_allocator hooks = {test_alloc, test_free, &tracker};
    forge_error error = {0};
    forge_arena *arena = forge_arena_create_with_allocator(256u * 1024u, &hooks, &error);
    assert(arena);
    char *sentinel = forge_arena_alloc(arena, 8, &error);
    assert(sentinel);
    memcpy(sentinel, "sentinel", 8);
    forge_arena_stats before = forge_arena_get_stats(arena);
    size_t bytes = tracker.live_bytes, calls = tracker.calls;
    assert(!forge_arena_aligned_alloc(arena, 0, 1, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(!forge_arena_aligned_alloc(arena, 3, 1, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(!forge_arena_alloc(arena, SIZE_MAX, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(!forge_arena_aligned_alloc(arena, (SIZE_MAX >> 1) + 1, 64, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(!forge_arena_alloc(arena, 256u * 1024u, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(tracker.calls == calls && tracker.live_bytes == bytes);
    tracker.fail_at = tracker.calls + 1;
    assert(!forge_arena_alloc(arena, 70u * 1024u, &error));
    assert(error.code == FORGE_ERR_MEMORY);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(tracker.live_bytes == bytes && !memcmp(sentinel, "sentinel", 8));
    tracker.fail_at = 0;
    assert(forge_arena_alloc(arena, 70u * 1024u, &error));
    assert(error.code == FORGE_OK);
    before = forge_arena_get_stats(arena);
    assert(!forge_arena_alloc(arena, 0, &error) && error.code == FORGE_OK);
    assert(!forge_arena_aligned_alloc(arena, 64, 0, &error) && error.code == FORGE_OK);
    expect_stats(forge_arena_get_stats(arena), before);
    forge_arena_destroy(arena);
    assert(!tracker.live_count && !tracker.live_bytes);
}

static void test_arena_limits_and_bad_construction(void) {
    forge_error error = {0};
    assert(!forge_arena_create(0, &error) && error.code == FORGE_ERR_LIMIT);
    assert(!forge_arena_create(1, &error) && error.code == FORGE_ERR_LIMIT);
    test_allocator tracker = {0};
    forge_allocator hooks = {test_alloc, NULL, &tracker};
    assert(!forge_arena_create_with_allocator(1024, &hooks, &error));
    assert(error.code == FORGE_ERR_ARGUMENT && tracker.calls == 0);
    hooks.free = test_free;
    tracker.fail_at = 1;
    assert(!forge_arena_create_with_allocator(1024, &hooks, &error));
    assert(error.code == FORGE_ERR_MEMORY && tracker.live_bytes == 0);
    tracker.fail_at = 0;
    forge_arena *arena = forge_arena_create_with_allocator(1024, &hooks, &error);
    assert(arena);
    assert(forge_arena_alloc(arena, 1, &error));
    forge_arena_stats stats = forge_arena_get_stats(arena);
    assert(stats.committed_bytes == 1024 && tracker.live_bytes == 1024);
    while (forge_arena_aligned_alloc(arena, 1, 1, &error)) {
        stats = forge_arena_get_stats(arena);
        assert(stats.committed_bytes <= 1024 && tracker.live_bytes <= 1024);
    }
    assert(error.code == FORGE_ERR_LIMIT);
    expect_stats(forge_arena_get_stats(arena), stats);
    forge_arena_reset(arena);
    assert(forge_arena_alloc(arena, 1, &error));
    forge_arena_destroy(arena);
    assert(tracker.live_count == 0);
    assert(!forge_arena_alloc(NULL, 1, &error) && error.code == FORGE_ERR_ARGUMENT);
    forge_arena_reset(NULL);
    forge_arena_destroy(NULL);
    expect_stats(forge_arena_get_stats(NULL), (forge_arena_stats){0});
}

static void test_arena_allocator_alignment_contract(void) {
    test_allocator tracker = {0};
    tracker.misalign_at = 1;
    forge_allocator hooks = {test_alloc, test_free, &tracker};
    forge_error error = {0};
    assert(!forge_arena_create_with_allocator(4096, &hooks, &error));
    assert(error.code == FORGE_ERR_ARGUMENT && tracker.live_count == 0);
    tracker.misalign_at = tracker.calls + 2;
    forge_arena *arena = forge_arena_create_with_allocator(4096, &hooks, &error);
    assert(arena);
    forge_arena_stats before = forge_arena_get_stats(arena);
    assert(!forge_arena_alloc(arena, 8, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    expect_stats(forge_arena_get_stats(arena), before);
    assert(tracker.live_bytes == before.committed_bytes && tracker.live_count == 1);
    assert(forge_arena_alloc(arena, 8, &error));
    forge_arena_destroy(arena);
    assert(tracker.live_count == 0 && tracker.live_bytes == 0);
}

static void test_slices_are_borrowed_and_bounded(void) {
    const unsigned char bytes[] = {'a', 0, 'b', 0xff};
    forge_slice_t source = {(const char *)bytes, sizeof(bytes)};
    forge_slice result = {NULL, 0};
    forge_error error = {0};
    assert(forge_slice_subslice(source, 1, 2, &result, &error) == FORGE_OK);
    assert(result.ptr == (const char *)bytes + 1 && result.len == 2);
    assert(forge_slice_equal(result, (forge_slice){(const char *)bytes + 1, 2}));
    assert(!forge_slice_equal(result, (forge_slice){(const char *)bytes, 2}));
    forge_slice before = result;
    assert(forge_slice_subslice(source, SIZE_MAX, 1, &result, &error) == FORGE_ERR_ARGUMENT);
    assert(result.ptr == before.ptr && result.len == before.len);
    assert(forge_slice_subslice(source, 3, 2, &result, &error) == FORGE_ERR_ARGUMENT);
    assert(result.ptr == before.ptr && result.len == before.len);
    assert(forge_slice_subslice((forge_slice){NULL, 1}, 0, 1, &result, &error) ==
           FORGE_ERR_ARGUMENT);
    assert(result.ptr == before.ptr && result.len == before.len);
    assert(forge_slice_subslice(source, 0, 1, NULL, &error) == FORGE_ERR_ARGUMENT);
    assert(forge_slice_subslice(source, sizeof(bytes), 0, &result, &error) == FORGE_OK);
    assert(result.ptr == (const char *)bytes + sizeof(bytes) && result.len == 0);
    assert(forge_slice_equal(result, (forge_slice){NULL, 0}));
    assert(!forge_slice_equal((forge_slice){NULL, 1}, (forge_slice){NULL, 1}));
    assert(forge_slice_subslice((forge_slice){NULL, 0}, 0, 0, &result, &error) == FORGE_OK);
    assert(!result.ptr && !result.len);
}

typedef struct {
    char root[FG_PATH_MAX];
    char *files[16];
    size_t count;
} file_fixture;

#ifdef _WIN32
static void file_wide(const char *path, wchar_t wide[FG_PATH_MAX]) {
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FG_PATH_MAX));
}
#endif

static void fixture_start(file_fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    char base[FG_PATH_MAX], random[33], name[64];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(base), base);
    assert(length && length < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    int length = snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
    assert(length > 0 && (size_t)length < sizeof(base));
#endif
    assert(fg_random_hex(random, 16));
    snprintf(name, sizeof(name), "forge-memory-%s", random);
    assert(fg_path_join(fixture->root, base, name));
    assert(fg_mkdir(fixture->root, NULL));
}

static void fixture_write(file_fixture *fixture, const char *name, const void *bytes, size_t length,
                          char path[FG_PATH_MAX]) {
    assert(fg_path_join(path, fixture->root, name));
    bool recorded = false;
    for (size_t i = 0; i < fixture->count; i++)
        recorded |= !strcmp(fixture->files[i], path);
    if (!recorded) {
        assert(fixture->count < 16);
        fixture->files[fixture->count++] = fg_strdup(path);
        assert(fixture->files[fixture->count - 1]);
    }
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    file_wide(path, wide);
    HANDLE file =
        CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    assert(length <= UINT32_MAX);
    assert(WriteFile(file, bytes, (DWORD)length, &written, NULL) && written == length);
    assert(CloseHandle(file));
#else
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
#endif
}

static void fixture_finish(file_fixture *fixture) {
    for (size_t i = 0; i < fixture->count; i++) {
#ifdef _WIN32
        wchar_t wide[FG_PATH_MAX];
        file_wide(fixture->files[i], wide);
        assert(DeleteFileW(wide));
#else
        assert(unlink(fixture->files[i]) == 0);
#endif
        free(fixture->files[i]);
    }
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    file_wide(fixture->root, wide);
    assert(RemoveDirectoryW(wide));
#else
    assert(rmdir(fixture->root) == 0);
#endif
}

static void test_file_views_binary_empty_and_lifetime(void) {
    file_fixture fixture;
    fixture_start(&fixture);
    char path[FG_PATH_MAX];
    const unsigned char binary[] = {'a', 0, 0xff, 'z'};
    fixture_write(&fixture, "\xc3\xa9-\xe6\x97\xa5.bin", binary, sizeof(binary), path);
    forge_error error = {0};
    forge_file_view *mapped =
        forge_file_view_open(path, sizeof(binary), FORGE_FILE_VIEW_MAP, &error);
    forge_file_view *read =
        forge_file_view_open(path, sizeof(binary), FORGE_FILE_VIEW_READ, &error);
    if (!mapped || !read)
        fprintf(stderr, "view: %s\n", error.message);
    assert(mapped && read);
    assert(forge_slice_equal(forge_file_view_slice(mapped),
                             (forge_slice){(const char *)binary, sizeof(binary)}));
    assert(forge_slice_equal(forge_file_view_slice(read), forge_file_view_slice(mapped)));
    forge_slice tail;
    assert(forge_slice_subslice(forge_file_view_slice(mapped), 2, 2, &tail, &error) == FORGE_OK);
    assert((unsigned char)tail.ptr[0] == 0xffu && tail.ptr[1] == 'z');
    forge_file_view_close(mapped);
    /* No file/mapping handle survives READ. Its private bytes stay unchanged
     * when the filesystem source is rewritten or shrunk. Never test that by
     * dereferencing an externally truncated mapping: POSIX may raise SIGBUS. */
    fixture_write(&fixture, "\xc3\xa9-\xe6\x97\xa5.bin", "n", 1, path);
    assert(forge_slice_equal(forge_file_view_slice(read),
                             (forge_slice){(const char *)binary, sizeof(binary)}));
    forge_file_view_close(read);
    mapped = forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error);
    assert(mapped && forge_slice_equal(forge_file_view_slice(mapped), (forge_slice){"n", 1}));
    forge_file_view_close(mapped);
    fixture_write(&fixture, "empty", "", 0, path);
    for (int mode = FORGE_FILE_VIEW_MAP; mode <= FORGE_FILE_VIEW_READ; mode++) {
        forge_file_view *empty = forge_file_view_open(path, 0, (forge_file_view_mode)mode, &error);
        assert(empty);
        forge_slice view = forge_file_view_slice(empty);
        assert(!view.ptr && !view.len);
        forge_file_view_close(empty);
    }
    forge_file_view_close(NULL);
    forge_slice empty = forge_file_view_slice(NULL);
    assert(!empty.ptr && !empty.len);
    fixture_finish(&fixture);
}

static void test_file_views_streaming_limits_and_cleanup(void) {
    file_fixture fixture;
    fixture_start(&fixture);
    char path[FG_PATH_MAX];
    size_t length = 200000;
    unsigned char *data = malloc(length);
    assert(data);
    for (size_t i = 0; i < length; i++)
        data[i] = (unsigned char)(i % 251);
    fixture_write(&fixture, "large.bin", data, length, path);
    forge_error error = {0};
    for (int mode = FORGE_FILE_VIEW_MAP; mode <= FORGE_FILE_VIEW_READ; mode++) {
        assert(!forge_file_view_open(path, length - 1, (forge_file_view_mode)mode, &error));
        assert(error.code == FORGE_ERR_LIMIT);
        assert(!forge_file_view_open(path, 0, (forge_file_view_mode)mode, &error));
        assert(error.code == FORGE_ERR_LIMIT);
        forge_file_view *view =
            forge_file_view_open(path, length, (forge_file_view_mode)mode, &error);
        assert(view);
        assert(forge_slice_equal(forge_file_view_slice(view),
                                 (forge_slice){(const char *)data, length}));
        forge_file_view_close(view);
    }
#ifdef _WIN32
    DWORD before = 0, after = 0;
    assert(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (size_t i = 0; i < 32; i++) {
        assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error));
        assert(error.code == FORGE_ERR_LIMIT);
        forge_file_view *view = forge_file_view_open(path, length, FORGE_FILE_VIEW_MAP, &error);
        assert(view);
        forge_file_view_close(view);
    }
    assert(GetProcessHandleCount(GetCurrentProcess(), &after) && after == before);
    wchar_t wide[FG_PATH_MAX];
    file_wide(path, wide);
    HANDLE held =
        CreateFileW(wide, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(held != INVALID_HANDLE_VALUE);
    assert(!forge_file_view_open(path, length, FORGE_FILE_VIEW_READ, &error));
    assert(error.code == FORGE_ERR_IO);
    assert(CloseHandle(held));
#else
    if (geteuid() != 0) {
        assert(chmod(path, 0000) == 0);
        assert(!forge_file_view_open(path, length, FORGE_FILE_VIEW_READ, &error));
        assert(error.code == FORGE_ERR_IO);
        assert(chmod(path, 0600) == 0);
    }
#endif
    forge_file_view *recovered = forge_file_view_open(path, length, FORGE_FILE_VIEW_READ, &error);
    assert(recovered);
    forge_file_view_close(recovered);
    free(data);
    fixture_finish(&fixture);
}

static void test_file_views_reject_invalid_inputs(void) {
    forge_error error = {0};
    assert(!forge_file_view_open(NULL, 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_file_view_open("", 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_file_view_open(".", 1, (forge_file_view_mode)0, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    char path[FG_PATH_MAX + 1];
    memset(path, 'x', sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    file_fixture fixture;
    fixture_start(&fixture);
    assert(!forge_file_view_open(fixture.root, 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(fg_path_join(path, fixture.root, "missing"));
    assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_IO);
    char target[FG_PATH_MAX];
    fixture_write(&fixture, "target", "x", 1, target);
    assert(fg_path_join(path, fixture.root, "link"));
#ifdef _WIN32
    wchar_t wide_target[FG_PATH_MAX], wide_path[FG_PATH_MAX];
    file_wide(target, wide_target);
    file_wide(path, wide_path);
    bool linked = CreateSymbolicLinkW(wide_path, wide_target, 0x2u) != 0;
    if (!linked && GetLastError() == ERROR_INVALID_PARAMETER)
        linked = CreateSymbolicLinkW(wide_path, wide_target, 0) != 0;
    if (linked) {
        assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error));
        assert(error.code == FORGE_ERR_POLICY);
        assert(DeleteFileW(wide_path));
    } else {
        DWORD code = GetLastError();
        assert(code == ERROR_PRIVILEGE_NOT_HELD || code == ERROR_ACCESS_DENIED ||
               code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_PARAMETER);
        puts("SKIP file-view symbolic-link test: creation permission unavailable");
    }
    assert(!forge_file_view_open("\\\\.\\NUL", 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_POLICY);
#else
    assert(symlink(target, path) == 0);
    assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_MAP, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
    assert(!forge_file_view_open(path, 1, FORGE_FILE_VIEW_READ, &error));
    assert(error.code == FORGE_ERR_POLICY);
    assert(unlink(path) == 0);
#endif
    fixture_finish(&fixture);
}

int main(void) {
    test_arena_alignment_and_lifetimes();
    test_arena_transactional_failures();
    test_arena_limits_and_bad_construction();
    test_arena_allocator_alignment_contract();
    test_slices_are_borrowed_and_bounded();
    test_file_views_binary_empty_and_lifetime();
    test_file_views_streaming_limits_and_cleanup();
    test_file_views_reject_invalid_inputs();
    puts("memory tests passed");
    return 0;
}
