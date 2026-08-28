#include "internal.h"
#include "forge/memory.h"
#include <stdalign.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define ARENA_BLOCK_BYTES (64u * 1024u)
#define VIEW_READ_BYTES (64u * 1024u)

/* MSVC's C headers do not supply the C11 max_align_t typedef. Its fundamental
 * C types have the alignment represented by this union; over-aligned storage
 * still goes through the explicit aligned-allocation API. */
#if defined(_MSC_VER) && !defined(__clang__)
typedef union {
    long double number;
    long long integer;
    void *pointer;
    void (*function)(void);
} arena_max_align_t;
#else
typedef max_align_t arena_max_align_t;
#endif
#define ARENA_DEFAULT_ALIGNMENT alignof(arena_max_align_t)

static void memory_success(forge_error *error) {
    if (error) {
        error->code = FORGE_OK;
        error->message[0] = 0;
    }
}

forge_status forge_slice_subslice(forge_slice source, size_t offset, size_t length,
                                  forge_slice *result, forge_error *error) {
    if (!result || (!source.ptr && source.len) || offset > source.len ||
        length > source.len - offset)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Slice range is outside its backing extent");
    forge_slice view = {source.ptr ? source.ptr + offset : NULL, length};
    *result = view;
    memory_success(error);
    return FORGE_OK;
}

bool forge_slice_equal(forge_slice left, forge_slice right) {
    if ((!left.ptr && left.len) || (!right.ptr && right.len) || left.len != right.len)
        return false;
    return !left.len || !memcmp(left.ptr, right.ptr, left.len);
}

typedef struct arena_block {
    struct arena_block *next;
    size_t committed, used;
} arena_block;

static size_t arena_header_size(void) {
    return (sizeof(arena_block) + ARENA_DEFAULT_ALIGNMENT - 1) &
           ~(size_t)(ARENA_DEFAULT_ALIGNMENT - 1);
}

struct forge_arena {
    forge_allocator allocator;
    arena_block *head, *tail, *current;
    forge_arena_stats stats;
};

static void *arena_system_alloc(size_t size, void *user) {
    (void)user;
    return malloc(size);
}

static void arena_system_free(void *allocation, void *user) {
    (void)user;
    free(allocation);
}

forge_arena *forge_arena_create(size_t maximum, forge_error *error) {
    return forge_arena_create_with_allocator(maximum, NULL, error);
}

forge_arena *forge_arena_create_with_allocator(size_t maximum, const forge_allocator *hooks,
                                               forge_error *error) {
    forge_allocator allocator =
        hooks ? *hooks : (forge_allocator){arena_system_alloc, arena_system_free, NULL};
    if (!allocator.alloc || !allocator.free) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Arena allocator requires alloc and free hooks");
        return NULL;
    }
    if (maximum < sizeof(forge_arena)) {
        fg_error(error, FORGE_ERR_LIMIT, "Arena limit cannot hold its control object");
        return NULL;
    }
    forge_arena *arena = allocator.alloc(sizeof(*arena), allocator.user);
    if (!arena) {
        fg_error(error, FORGE_ERR_MEMORY, "Arena control allocation failed");
        return NULL;
    }
    if ((uintptr_t)arena % ARENA_DEFAULT_ALIGNMENT) {
        allocator.free(arena, allocator.user);
        fg_error(error, FORGE_ERR_ARGUMENT,
                 "Arena allocator did not provide max_align_t alignment");
        return NULL;
    }
    memset(arena, 0, sizeof(*arena));
    arena->allocator = allocator;
    arena->stats.max_committed_bytes = maximum;
    arena->stats.committed_bytes = sizeof(*arena);
    arena->stats.peak_committed_bytes = sizeof(*arena);
    memory_success(error);
    return arena;
}

static bool arena_fits(const arena_block *block, size_t alignment, size_t size, size_t *padding) {
    size_t header = arena_header_size();
    size_t capacity = block->committed - header;
    size_t available = capacity - block->used;
    uintptr_t address = (uintptr_t)((const unsigned char *)block + header + block->used);
    size_t remainder = (size_t)(address & (uintptr_t)(alignment - 1));
    *padding = remainder ? alignment - remainder : 0;
    return *padding <= available && size <= available - *padding;
}

static void *arena_consume(forge_arena *arena, arena_block *block, size_t padding, size_t size) {
    void *result = (unsigned char *)block + arena_header_size() + block->used + padding;
    block->used += padding + size;
    arena->current = block;
    arena->stats.used_bytes += size;
    arena->stats.padding_bytes += padding;
    arena->stats.allocation_count++;
    if (arena->stats.used_bytes > arena->stats.peak_used_bytes)
        arena->stats.peak_used_bytes = arena->stats.used_bytes;
    return result;
}

void *forge_arena_alloc(forge_arena *arena, size_t size, forge_error *error) {
    return forge_arena_aligned_alloc(arena, ARENA_DEFAULT_ALIGNMENT, size, error);
}

void *forge_arena_aligned_alloc(forge_arena *arena, size_t alignment, size_t size,
                                forge_error *error) {
    if (!arena || !alignment || (alignment & (alignment - 1))) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Arena alignment must be a nonzero power of two");
        return NULL;
    }
#if SIZE_MAX > UINTPTR_MAX
    if (alignment > UINTPTR_MAX) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Arena alignment is not representable by uintptr_t");
        return NULL;
    }
#endif
    if (!size) {
        memory_success(error);
        return NULL;
    }
    for (arena_block *block = arena->current; block; block = block->next) {
        size_t padding;
        if (arena_fits(block, alignment, size, &padding)) {
            void *result = arena_consume(arena, block, padding, size);
            memory_success(error);
            return result;
        }
    }
    size_t header = arena_header_size();
    size_t slack = alignment > ARENA_DEFAULT_ALIGNMENT ? alignment - 1 : 0;
    if (size > SIZE_MAX - header || slack > SIZE_MAX - header - size) {
        fg_error(error, FORGE_ERR_LIMIT, "Arena allocation size/alignment overflows size_t");
        return NULL;
    }
    size_t minimum = header + slack + size;
    size_t remaining = arena->stats.max_committed_bytes - arena->stats.committed_bytes;
    if (minimum > remaining) {
        fg_error(error, FORGE_ERR_LIMIT, "Arena committed-byte limit exceeded");
        return NULL;
    }
    size_t committed = FG_MAX((size_t)ARENA_BLOCK_BYTES, minimum);
    if (committed > remaining)
        committed = remaining;
    arena_block *block = arena->allocator.alloc(committed, arena->allocator.user);
    if (!block) {
        fg_error(error, FORGE_ERR_MEMORY, "Arena block allocation failed");
        return NULL;
    }
    if ((uintptr_t)block % ARENA_DEFAULT_ALIGNMENT) {
        arena->allocator.free(block, arena->allocator.user);
        fg_error(error, FORGE_ERR_ARGUMENT,
                 "Arena allocator did not provide max_align_t alignment");
        return NULL;
    }
    block->next = NULL;
    block->committed = committed;
    block->used = 0;
    size_t padding;
    if (!arena_fits(block, alignment, size, &padding)) {
        arena->allocator.free(block, arena->allocator.user);
        fg_error(error, FORGE_ERR_MEMORY, "Arena block cannot satisfy its alignment contract");
        return NULL;
    }
    /* No arena-owned state changes before every fallible step has succeeded. */
    if (arena->tail)
        arena->tail->next = block;
    else
        arena->head = block;
    arena->tail = block;
    arena->stats.committed_bytes += committed;
    arena->stats.block_count++;
    if (arena->stats.committed_bytes > arena->stats.peak_committed_bytes)
        arena->stats.peak_committed_bytes = arena->stats.committed_bytes;
    void *result = arena_consume(arena, block, padding, size);
    memory_success(error);
    return result;
}

void forge_arena_reset(forge_arena *arena) {
    if (arena) {
        for (arena_block *block = arena->head; block; block = block->next)
            block->used = 0;
        arena->current = arena->head;
        arena->stats.used_bytes = 0;
        arena->stats.padding_bytes = 0;
        arena->stats.allocation_count = 0;
    }
}

void forge_arena_destroy(forge_arena *arena) {
    if (arena) {
        arena_block *block = arena->head;
        while (block) {
            arena_block *next = block->next;
            arena->allocator.free(block, arena->allocator.user);
            block = next;
        }
        forge_allocator allocator = arena->allocator;
        allocator.free(arena, allocator.user);
    }
}

forge_arena_stats forge_arena_get_stats(const forge_arena *arena) {
    return arena ? arena->stats : (forge_arena_stats){0};
}

struct forge_file_view {
    forge_slice bytes;
    forge_file_view_mode mode;
};

static forge_file_view *view_allocate(uint64_t length, size_t maximum, forge_file_view_mode mode,
                                      forge_error *error) {
    if (length > maximum || length > SIZE_MAX || length > PTRDIFF_MAX) {
        fg_error(error, FORGE_ERR_LIMIT, "File view exceeds the byte or addressable-extent limit");
        return NULL;
    }
    forge_file_view *view = calloc(1, sizeof(*view));
    if (!view) {
        fg_error(error, FORGE_ERR_MEMORY, "File view allocation failed");
        return NULL;
    }
    view->mode = mode;
    view->bytes.len = (size_t)length;
    if (mode == FORGE_FILE_VIEW_READ && length) {
        view->bytes.ptr = malloc((size_t)length);
        if (!view->bytes.ptr) {
            free(view);
            fg_error(error, FORGE_ERR_MEMORY, "File view copy allocation failed");
            return NULL;
        }
    }
    return view;
}

#ifdef _WIN32
static bool view_windows_path(const char *path, wchar_t wide[FG_PATH_MAX + 8], forge_error *error) {
    wchar_t input[FG_PATH_MAX], absolute[FG_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, FG_PATH_MAX)) {
        fg_error(error, FORGE_ERR_PARSE, "File view path is not valid UTF-8");
        return false;
    }
    for (wchar_t *p = input; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    if (!wcsncmp(input, L"\\\\?\\", 4) || !wcsncmp(input, L"\\\\.\\", 4)) {
        fg_error(error, FORGE_ERR_POLICY, "File view device namespace paths are unsupported");
        return false;
    }
    DWORD length = GetFullPathNameW(input, FG_PATH_MAX, absolute, NULL);
    if (!length) {
        fg_error(error, FORGE_ERR_IO, "Cannot resolve file view path (Windows error %lu)",
                 (unsigned long)GetLastError());
        return false;
    }
    int bytes = length < FG_PATH_MAX ? WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute,
                                                           -1, NULL, 0, NULL, NULL)
                                     : 0;
    if (!bytes || bytes > FG_PATH_MAX) {
        fg_error(error, FORGE_ERR_LIMIT, "File view absolute path exceeds 4095 bytes");
        return false;
    }
    if (absolute[0] == L'\\' && absolute[1] == L'\\') {
        memcpy(wide, L"\\\\?\\UNC\\", 8 * sizeof(*wide));
        memcpy(wide + 8, absolute + 2, ((size_t)length - 1) * sizeof(*wide));
    } else {
        memcpy(wide, L"\\\\?\\", 4 * sizeof(*wide));
        memcpy(wide + 4, absolute, ((size_t)length + 1) * sizeof(*wide));
    }
    return true;
}

static bool view_windows_regular(HANDLE file, BY_HANDLE_FILE_INFORMATION *info,
                                 forge_error *error) {
    if (!GetFileInformationByHandle(file, info)) {
        fg_error(error, FORGE_ERR_IO, "Cannot inspect opened file view (Windows error %lu)",
                 (unsigned long)GetLastError());
        return false;
    }
    if (GetFileType(file) != FILE_TYPE_DISK ||
        (info->dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
        fg_error(error, FORGE_ERR_POLICY,
                 "File views require regular files, not links or special files");
        return false;
    }
    return true;
}

static bool view_windows_same(const BY_HANDLE_FILE_INFORMATION *left,
                              const BY_HANDLE_FILE_INFORMATION *right) {
    return left->dwVolumeSerialNumber == right->dwVolumeSerialNumber &&
           left->nFileIndexHigh == right->nFileIndexHigh &&
           left->nFileIndexLow == right->nFileIndexLow &&
           left->dwFileAttributes == right->dwFileAttributes &&
           left->nFileSizeHigh == right->nFileSizeHigh &&
           left->nFileSizeLow == right->nFileSizeLow &&
           left->ftLastWriteTime.dwHighDateTime == right->ftLastWriteTime.dwHighDateTime &&
           left->ftLastWriteTime.dwLowDateTime == right->ftLastWriteTime.dwLowDateTime;
}

static forge_file_view *view_open_native(const char *path, size_t maximum,
                                         forge_file_view_mode mode, forge_error *error) {
    wchar_t wide[FG_PATH_MAX + 8];
    if (!view_windows_path(path, wide, error))
        return NULL;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fg_error(error, FORGE_ERR_IO, "Cannot inspect file view path (Windows error %lu)",
                 (unsigned long)GetLastError());
        return NULL;
    }
    if (attributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) {
        fg_error(error, FORGE_ERR_POLICY,
                 "File views reject links, directories, and special files");
        return NULL;
    }
    HANDLE file = CreateFileW(
        wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fg_error(error, FORGE_ERR_IO, "Cannot open file view (Windows error %lu)",
                 (unsigned long)GetLastError());
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION before, after;
    bool ok = view_windows_regular(file, &before, error);
    uint64_t length = ok ? ((uint64_t)before.nFileSizeHigh << 32) | before.nFileSizeLow : 0;
    forge_file_view *view = ok ? view_allocate(length, maximum, mode, error) : NULL;
    ok = view != NULL;
    HANDLE mapping = NULL;
    if (ok && length && mode == FORGE_FILE_VIEW_MAP) {
        mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!mapping ||
            !(view->bytes.ptr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, (SIZE_T)length))) {
            fg_error(error, FORGE_ERR_IO, "Cannot map file view (Windows error %lu)",
                     (unsigned long)GetLastError());
            ok = false;
        }
    } else if (ok && length) {
        size_t offset = 0;
        while (ok && offset < view->bytes.len) {
            DWORD count = 0;
            DWORD requested = (DWORD)FG_MIN(view->bytes.len - offset, (size_t)VIEW_READ_BYTES);
            if (!ReadFile(file, (char *)view->bytes.ptr + offset, requested, &count, NULL)) {
                fg_error(error, FORGE_ERR_IO, "Cannot read file view (Windows error %lu)",
                         (unsigned long)GetLastError());
                ok = false;
            } else if (!count) {
                fg_error(error, FORGE_ERR_CONFLICT, "File view was truncated while reading");
                ok = false;
            } else
                offset += (size_t)count;
        }
    }
    if (ok)
        ok = view_windows_regular(file, &after, error);
    if (ok && !view_windows_same(&before, &after)) {
        fg_error(error, FORGE_ERR_CONFLICT, "File changed while opening its view");
        ok = false;
    }
    if (mapping && !CloseHandle(mapping) && ok) {
        fg_error(error, FORGE_ERR_IO, "Cannot close file mapping handle");
        ok = false;
    }
    if (!CloseHandle(file) && ok) {
        fg_error(error, FORGE_ERR_IO, "Cannot close file view handle");
        ok = false;
    }
    if (!ok) {
        forge_file_view_close(view);
        return NULL;
    }
    return view;
}
#else
static bool view_posix_same(const struct stat *left, const struct stat *right) {
#if defined(__APPLE__)
    bool timestamps = left->st_mtimespec.tv_sec == right->st_mtimespec.tv_sec &&
                      left->st_mtimespec.tv_nsec == right->st_mtimespec.tv_nsec &&
                      left->st_ctimespec.tv_sec == right->st_ctimespec.tv_sec &&
                      left->st_ctimespec.tv_nsec == right->st_ctimespec.tv_nsec;
#else
    bool timestamps = left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
                      left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
                      left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
                      left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
#endif
    return timestamps && left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_mode == right->st_mode && left->st_size == right->st_size;
}

static forge_file_view *view_open_native(const char *path, size_t maximum,
                                         forge_file_view_mode mode, forge_error *error) {
    struct stat listed, before, after;
    if (lstat(path, &listed) != 0) {
        fg_error(error, FORGE_ERR_IO, "Cannot inspect file view path: %s", strerror(errno));
        return NULL;
    }
    if (!S_ISREG(listed.st_mode)) {
        fg_error(error, FORGE_ERR_POLICY,
                 "File views reject links, directories, and special files");
        return NULL;
    }
    int file = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) {
        fg_error(error, errno == ELOOP ? FORGE_ERR_POLICY : FORGE_ERR_IO,
                 "Cannot open file view: %s", strerror(errno));
        return NULL;
    }
    bool ok = fstat(file, &before) == 0;
    if (!ok)
        fg_error(error, FORGE_ERR_IO, "Cannot inspect opened file view: %s", strerror(errno));
    if (ok && !S_ISREG(before.st_mode)) {
        fg_error(error, FORGE_ERR_POLICY, "Opened file view is not a regular file");
        ok = false;
    }
    if (ok && !view_posix_same(&listed, &before)) {
        fg_error(error, FORGE_ERR_CONFLICT, "File changed before opening its view");
        ok = false;
    }
    if (ok && before.st_size < 0) {
        fg_error(error, FORGE_ERR_IO, "File view has an invalid size");
        ok = false;
    }
    forge_file_view *view =
        ok ? view_allocate((uint64_t)before.st_size, maximum, mode, error) : NULL;
    ok = view != NULL;
    if (ok && view->bytes.len && mode == FORGE_FILE_VIEW_MAP) {
        void *mapped = mmap(NULL, view->bytes.len, PROT_READ, MAP_PRIVATE, file, 0);
        if (mapped == MAP_FAILED) {
            fg_error(error, FORGE_ERR_IO, "Cannot map file view: %s", strerror(errno));
            ok = false;
        } else
            view->bytes.ptr = mapped;
    } else if (ok && view->bytes.len) {
        size_t offset = 0;
        while (ok && offset < view->bytes.len) {
            size_t requested = FG_MIN(view->bytes.len - offset, (size_t)VIEW_READ_BYTES);
            ssize_t count = read(file, (char *)view->bytes.ptr + offset, requested);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                fg_error(error, FORGE_ERR_IO, "Cannot read file view: %s", strerror(errno));
                ok = false;
            } else if (!count) {
                fg_error(error, FORGE_ERR_CONFLICT, "File view was truncated while reading");
                ok = false;
            } else
                offset += (size_t)count;
        }
    }
    if (ok && fstat(file, &after) != 0) {
        fg_error(error, FORGE_ERR_IO, "Cannot recheck opened file view: %s", strerror(errno));
        ok = false;
    }
    if (ok && !view_posix_same(&before, &after)) {
        fg_error(error, FORGE_ERR_CONFLICT, "File changed while opening its view");
        ok = false;
    }
    if (close(file) != 0 && ok) {
        fg_error(error, FORGE_ERR_IO, "Cannot close file view descriptor: %s", strerror(errno));
        ok = false;
    }
    if (!ok) {
        forge_file_view_close(view);
        return NULL;
    }
    return view;
}
#endif

forge_file_view *forge_file_view_open(const char *path, size_t maximum, forge_file_view_mode mode,
                                      forge_error *error) {
    if (!path || !*path || (mode != FORGE_FILE_VIEW_MAP && mode != FORGE_FILE_VIEW_READ)) {
        fg_error(error, FORGE_ERR_ARGUMENT,
                 "File view requires a path and an explicit storage mode");
        return NULL;
    }
    if (strlen(path) > FORGE_FILE_VIEW_MAX_PATH_BYTES) {
        fg_error(error, FORGE_ERR_LIMIT, "File view path exceeds 4095 bytes");
        return NULL;
    }
    forge_file_view *view = view_open_native(path, maximum, mode, error);
    if (view)
        memory_success(error);
    return view;
}

forge_slice forge_file_view_slice(const forge_file_view *view) {
    return view ? view->bytes : (forge_slice){NULL, 0};
}

void forge_file_view_close(forge_file_view *view) {
    if (view) {
        if (view->bytes.ptr && view->mode == FORGE_FILE_VIEW_MAP) {
#ifdef _WIN32
            UnmapViewOfFile(view->bytes.ptr);
#else
            munmap((void *)view->bytes.ptr, view->bytes.len);
#endif
        } else if (view->mode == FORGE_FILE_VIEW_READ)
            free((void *)view->bytes.ptr);
        free(view);
    }
}
