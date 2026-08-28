#include "internal.h"
#include "forge/watch.h"
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#endif
#endif

#define WATCH_JSON_BASE 768u
#define WATCH_WINDOWS_BUFFER (64u * 1024u)
#define WATCH_LINUX_BUFFER (64u * 1024u)
#define WATCH_WAIT_SLICE_MS 25u

typedef struct {
    char *path;
    unsigned flags;
} watch_event;

typedef struct watch_directory {
    char *path;
    size_t depth;
#ifdef _WIN32
    HANDLE handle;
    OVERLAPPED operation;
    bool pending;
#elif defined(__linux__)
    int descriptor;
#endif
} watch_directory;

struct forge_watch {
    forge_watch_limits limits;
    char root[FG_PATH_MAX];
    watch_directory **directories, **directory_map;
    size_t directory_count, directory_map_size;
    watch_event *events, **ordered;
    size_t *event_map, event_map_size, event_count, event_bytes;
    bool initial, reopen, dropped_unknown, metadata_directory[2];
    unsigned reasons;
    uint64_t dropped, overflows;
    forge_cancel_fn cancelled;
    void *user;
    forge_error *error;
    uint64_t deadline;
    size_t scan_entries, native_events;
    bool cancelled_now, deadline_now, memory_failed;
#ifdef _WIN32
    HANDLE port;
    watch_directory *current;
    size_t native_offset, native_length;
    BY_HANDLE_FILE_INFORMATION root_identity;
    union {
        DWORD align;
        unsigned char bytes[WATCH_WINDOWS_BUFFER];
    } buffer;
#else
    int root_fd;
    struct stat root_identity;
#if defined(__linux__)
    int notify_fd;
    watch_directory **descriptor_map;
    size_t native_offset, native_length;
    union {
        uint64_t align;
        unsigned char bytes[WATCH_LINUX_BUFFER];
    } buffer;
#elif defined(__APPLE__)
    FSEventStreamRef stream;
    CFRunLoopRef run_loop;
    CFStringRef run_mode;
    bool stream_started;
#endif
#endif
};

static void watch_success(forge_error *error) {
    if (error) {
        error->code = FORGE_OK;
        error->message[0] = 0;
    }
}

static uint64_t watch_saturating_add(uint64_t left, uint64_t right) {
    return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static void watch_loss(forge_watch *watch, unsigned reason, uint64_t dropped, bool unknown) {
    watch->reasons |= reason;
    watch->reopen = true;
    watch->dropped = watch_saturating_add(watch->dropped, dropped);
    watch->dropped_unknown |= unknown;
    if (reason & FORGE_WATCH_RESCAN_NATIVE_OVERFLOW)
        watch->overflows = watch_saturating_add(watch->overflows, 1);
}

void forge_watch_invalidate(forge_watch *watch) {
    if (watch)
        watch_loss(watch, FORGE_WATCH_RESCAN_CALLER, 0, true);
}

static bool watch_check(forge_watch *watch) {
    if (watch->cancelled && watch->cancelled(watch->user)) {
        watch->cancelled_now = true;
        fg_error(watch->error, FORGE_ERR_CANCELLED, "Filesystem watch cancelled");
        return false;
    }
    if (watch->deadline && fg_now_ms() >= watch->deadline) {
        watch->deadline_now = true;
        fg_error(watch->error, FORGE_ERR_LIMIT, "Filesystem watch deadline reached");
        return false;
    }
    return true;
}

static bool watch_fail(forge_watch *watch, forge_status code, unsigned reason,
                       const char *message) {
    watch_loss(watch, reason, 0, true);
    if (code == FORGE_ERR_MEMORY)
        watch->memory_failed = true;
    fg_error(watch->error, code, "%s", message);
    return false;
}

static size_t watch_map_size(size_t count) {
    size_t size = 2;
    while (size < count * 2)
        size *= 2;
    return size;
}

static watch_directory *watch_directory_find(const forge_watch *watch, const char *path) {
    size_t index = (size_t)fg_hash(path, strlen(path)) & (watch->directory_map_size - 1);
    while (watch->directory_map[index]) {
        if (!strcmp(watch->directory_map[index]->path, path))
            return watch->directory_map[index];
        index = (index + 1) & (watch->directory_map_size - 1);
    }
    return NULL;
}

static void watch_directory_insert(forge_watch *watch, watch_directory *directory) {
    size_t index =
        (size_t)fg_hash(directory->path, strlen(directory->path)) & (watch->directory_map_size - 1);
    while (watch->directory_map[index])
        index = (index + 1) & (watch->directory_map_size - 1);
    watch->directory_map[index] = directory;
    watch->directories[watch->directory_count++] = directory;
}

static int watch_metadata_name(const char *path) {
#ifdef _WIN32
    if (!_stricmp(path, ".git"))
        return 0;
    if (!_stricmp(path, ".forge"))
        return 1;
#else
    if (!strcmp(path, ".git"))
        return 0;
    if (!strcmp(path, ".forge"))
        return 1;
#endif
    return -1;
}

static bool watch_metadata_descendant(const char *path) {
    const char *slash = strchr(path, '/');
    if (!slash || (size_t)(slash - path) >= 16)
        return false;
    char component[16];
    memcpy(component, path, (size_t)(slash - path));
    component[slash - path] = 0;
    return watch_metadata_name(component) >= 0;
}

static bool watch_path(forge_watch *watch, const char *parent, const char *name,
                       char result[FG_PATH_MAX]) {
    size_t left = strlen(parent), right = strlen(name), separator = left ? 1u : 0u;
    if (!right || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/'))
        return watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                          "Native watch returned an invalid path component");
#ifdef _WIN32
    if (strchr(name, '\\') || strchr(name, ':'))
        return watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                          "Native watch returned an unsupported Windows path");
#endif
    if (!fg_utf8_valid(name, right))
        return watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                          "Filesystem watch requires UTF-8 paths");
    if (right > watch->limits.max_path_bytes ||
        left + separator > watch->limits.max_path_bytes - right ||
        strlen(watch->root) + left + separator + right + 1 >= FG_PATH_MAX)
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                          "Filesystem watch path limit exceeded");
    memcpy(result, parent, left);
    if (separator)
        result[left++] = '/';
    memcpy(result + left, name, right + 1);
    return true;
}

static bool watch_scan_entry(forge_watch *watch) {
    if (!watch_check(watch))
        return false;
    if (watch->scan_entries >= watch->limits.max_scan_entries)
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_SCAN_LIMIT,
                          "Filesystem watch enrollment entry limit exceeded");
    watch->scan_entries++;
    return true;
}

static bool watch_directory_budget(forge_watch *watch, size_t depth) {
    if (depth > watch->limits.max_depth)
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_DEPTH_LIMIT,
                          "Filesystem watch directory depth limit exceeded");
    if (watch->directory_count >= watch->limits.max_directories)
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_DIRECTORY_LIMIT,
                          "Filesystem watch directory limit exceeded");
    return true;
}

static bool watch_event_add(forge_watch *watch, const char *path, unsigned flags) {
    size_t length = strlen(path);
    if (!fg_utf8_valid(path, length)) {
        watch_loss(watch, FORGE_WATCH_RESCAN_PATH_ENCODING, 1, false);
        return true;
    }
    if (length > watch->limits.max_path_bytes) {
        watch_loss(watch, FORGE_WATCH_RESCAN_PATH_LIMIT, 1, false);
        return true;
    }
    size_t index = (size_t)fg_hash(path, length) & (watch->event_map_size - 1);
    while (watch->event_map[index]) {
        watch_event *event = &watch->events[watch->event_map[index] - 1];
        if (!strcmp(event->path, path)) {
            event->flags |= flags;
            return true;
        }
        index = (index + 1) & (watch->event_map_size - 1);
    }
    if (watch->event_count >= watch->limits.max_events) {
        watch_loss(watch, FORGE_WATCH_RESCAN_EVENT_LIMIT, 1, false);
        return true;
    }
    /* Six bytes per input byte bounds all JSON escaping, including control
     * bytes and Unicode surrogate pairs. This conservative budget also bounds
     * retained path storage before any serializer allocations occur. */
    size_t bound = length * 6 + 64;
    if (bound > watch->limits.max_bytes - WATCH_JSON_BASE - watch->event_bytes) {
        watch_loss(watch, FORGE_WATCH_RESCAN_BYTE_LIMIT, 1, false);
        return true;
    }
    char *copy = fg_strdup(path);
    if (!copy)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate filesystem watch event");
    watch->events[watch->event_count] = (watch_event){copy, flags};
    watch->event_map[index] = ++watch->event_count;
    watch->event_bytes += bound;
    return true;
}

static size_t watch_depth(const char *path) {
    size_t depth = *path ? 1u : 0u;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            depth++;
    return depth;
}

static bool watch_enroll(forge_watch *watch, const char *relative);

static bool watch_handle_event(forge_watch *watch, const char *path, unsigned flags) {
    if (!*path)
        path = ".";
    int metadata = watch_metadata_name(path);
    if (watch_metadata_descendant(path))
        return true;
    if (metadata >= 0 && (flags & FORGE_WATCH_DIRECTORY) && !(flags & FORGE_WATCH_SYMLINK)) {
        watch->metadata_directory[metadata] =
            !(flags & (FORGE_WATCH_DELETED | FORGE_WATCH_RENAMED_FROM));
        return true;
    }
    if (metadata >= 0 && (flags & (FORGE_WATCH_CREATED | FORGE_WATCH_RENAMED_TO)))
        watch->metadata_directory[metadata] = false;
    if (!watch_event_add(watch, path, flags))
        return false;
    if (!(flags & FORGE_WATCH_DIRECTORY) || (flags & FORGE_WATCH_SYMLINK))
        return true;
    if (!strcmp(path, ".") &&
        (flags & (FORGE_WATCH_DELETED | FORGE_WATCH_RENAMED | FORGE_WATCH_RENAMED_FROM))) {
        watch_loss(watch, FORGE_WATCH_RESCAN_ROOT_CHANGED, 0, true);
        return true;
    }
    if (flags & (FORGE_WATCH_DELETED | FORGE_WATCH_RENAMED | FORGE_WATCH_RENAMED_FROM |
                 FORGE_WATCH_RENAMED_TO)) {
        /* Cached descendant paths and queued watch descriptors cannot be
         * safely relabeled from a possibly split/lost rename pair. */
        watch_loss(watch, FORGE_WATCH_RESCAN_TOPOLOGY, 0, true);
        return true;
    }
    if (strcmp(path, ".") && !watch->reopen && !watch_directory_find(watch, path)) {
        watch->reasons |= FORGE_WATCH_RESCAN_SUBTREE;
        if (!watch_enroll(watch, path)) {
            unsigned reason = watch->cancelled_now || watch->deadline_now
                                  ? FORGE_WATCH_RESCAN_DEADLINE
                                  : FORGE_WATCH_RESCAN_IO;
            watch_loss(watch, reason, 0, true);
            return !watch->memory_failed && !watch->cancelled_now;
        }
    }
    return true;
}

forge_watch_limits forge_default_watch_limits(void) {
    return (forge_watch_limits){1024, 1024u * 1024u, 4095, 4096, 64, 1000000, 8192};
}

static bool watch_valid_limits(const forge_watch_limits *limits) {
    return limits->max_events && limits->max_events <= 65536 && limits->max_bytes >= 1024 &&
           limits->max_bytes <= FG_MAX_JSON && limits->max_path_bytes &&
           limits->max_path_bytes < FG_PATH_MAX && limits->max_directories &&
           limits->max_directories <= 65536 && limits->max_depth <= 64 &&
           limits->max_scan_entries && limits->max_scan_entries <= 10000000 &&
           limits->max_native_events && limits->max_native_events <= 1000000;
}

static void watch_begin_call(forge_watch *watch, forge_cancel_fn cancelled, void *user,
                             uint64_t timeout, forge_error *error) {
    watch->cancelled = cancelled;
    watch->user = user;
    watch->error = error;
    watch->deadline = timeout ? watch_saturating_add(fg_now_ms(), timeout) : 0;
    watch->scan_entries = 0;
    watch->native_events = 0;
    watch->cancelled_now = false;
    watch->deadline_now = false;
    watch->memory_failed = false;
}

static void watch_end_call(forge_watch *watch) {
    watch->cancelled = NULL;
    watch->user = NULL;
    watch->error = NULL;
    watch->deadline = 0;
}

/* Platform implementations follow. Each next() consumes at most one native
 * record, except FSEvents' indivisible callback batches. All queue storage is
 * retained across calls, including cancellation and serialization failures. */
#ifdef _WIN32
static bool watch_windows_error(forge_watch *watch, const char *operation) {
    DWORD code = GetLastError();
    watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
    fg_error(watch->error, FORGE_ERR_IO, "%s (Windows error %lu)", operation, (unsigned long)code);
    return false;
}

static bool watch_windows_wide(forge_watch *watch, const char *path,
                               wchar_t result[FG_PATH_MAX + 8]) {
    wchar_t input[FG_PATH_MAX];
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, input, FG_PATH_MAX);
    if (!count)
        return watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                          "Filesystem watch path is not valid UTF-8");
    for (int i = 0; i < count; i++)
        if (input[i] == L'/')
            input[i] = L'\\';
    if (input[0] == L'\\' && input[1] == L'\\') {
        memcpy(result, L"\\\\?\\UNC\\", 8 * sizeof(*result));
        memcpy(result + 8, input + 2, ((size_t)count - 2) * sizeof(*result));
    } else {
        memcpy(result, L"\\\\?\\", 4 * sizeof(*result));
        memcpy(result + 4, input, (size_t)count * sizeof(*result));
    }
    return true;
}

static bool watch_windows_full(forge_watch *watch, const char *relative, char path[FG_PATH_MAX]) {
    int count =
        snprintf(path, FG_PATH_MAX, "%s%s%s", watch->root,
                 *relative && watch->root[strlen(watch->root) - 1] != '\\' ? "\\" : "", relative);
    if (count < 0 || count >= FG_PATH_MAX)
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                          "Filesystem watch full path is too long");
    for (int i = 0; i < count; i++)
        if (path[i] == '/')
            path[i] = '\\';
    return true;
}

static bool watch_windows_directory_info(forge_watch *watch, HANDLE handle,
                                         BY_HANDLE_FILE_INFORMATION *info) {
    if (!GetFileInformationByHandle(handle, info))
        return watch_windows_error(watch, "Cannot inspect watch directory");
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !(info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info->dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)))
        return watch_fail(watch, FORGE_ERR_POLICY, FORGE_WATCH_RESCAN_IO,
                          "Filesystem watch rejects linked or non-directory ancestors");
    return true;
}

static bool watch_windows_same(const BY_HANDLE_FILE_INFORMATION *left,
                               const BY_HANDLE_FILE_INFORMATION *right) {
    return left->dwVolumeSerialNumber == right->dwVolumeSerialNumber &&
           left->nFileIndexHigh == right->nFileIndexHigh &&
           left->nFileIndexLow == right->nFileIndexLow;
}

static HANDLE watch_windows_lock(forge_watch *watch, const char *path,
                                 BY_HANDLE_FILE_INFORMATION *info) {
    wchar_t wide[FG_PATH_MAX + 8];
    if (!watch_check(watch) || !watch_windows_wide(watch, path, wide))
        return INVALID_HANDLE_VALUE;
    HANDLE handle =
        CreateFileW(wide, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        watch_windows_error(watch, "Cannot anchor watch directory during enrollment");
        return handle;
    }
    if (!watch_windows_directory_info(watch, handle, info)) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

static bool watch_windows_arm(forge_watch *watch, watch_directory *directory) {
    memset(&directory->operation, 0, sizeof(directory->operation));
    if (!ReadDirectoryChangesW(directory->handle, watch->buffer.bytes,
                               (DWORD)sizeof(watch->buffer.bytes), TRUE,
                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                   FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                                   FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
                                   FILE_NOTIFY_CHANGE_SECURITY,
                               NULL, &directory->operation, NULL))
        return watch_windows_error(watch, "Cannot start directory change notifications");
    directory->pending = true;
    return true;
}

static bool watch_windows_add(forge_watch *watch, const char *relative, size_t depth,
                              const BY_HANDLE_FILE_INFORMATION *anchored) {
    if (watch_directory_find(watch, relative))
        return true;
    if (!watch_directory_budget(watch, depth))
        return false;
    watch_directory *directory = calloc(1, sizeof(*directory));
    if (!directory)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate native directory watch");
    directory->handle = INVALID_HANDLE_VALUE;
    directory->depth = depth;
    directory->path = fg_strdup(relative);
    if (!directory->path) {
        free(directory);
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate native watch path");
    }
    /* One recursive native root watch avoids keeping descendant directory
     * handles open, which can prevent renaming their parents on Windows. The
     * bounded directory inventory still excludes all reparse directories. */
    if (*relative) {
        watch_directory_insert(watch, directory);
        return true;
    }
    char full[FG_PATH_MAX];
    wchar_t wide[FG_PATH_MAX + 8];
    bool ok = watch_windows_full(watch, relative, full) && watch_windows_wide(watch, full, wide);
    if (ok) {
        directory->handle = CreateFileW(
            wide, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, NULL);
        if (directory->handle == INVALID_HANDLE_VALUE)
            ok = watch_windows_error(watch, "Cannot open native directory watch");
    }
    BY_HANDLE_FILE_INFORMATION opened;
    if (ok)
        ok = watch_windows_directory_info(watch, directory->handle, &opened);
    if (ok && !watch_windows_same(anchored, &opened))
        ok = watch_fail(watch, FORGE_ERR_CONFLICT, FORGE_WATCH_RESCAN_TOPOLOGY,
                        "Watch directory changed while opening");
    if (ok && !CreateIoCompletionPort(directory->handle, watch->port, (ULONG_PTR)directory, 0))
        ok = watch_windows_error(watch, "Cannot register directory completion port");
    if (ok)
        ok = watch_windows_arm(watch, directory);
    if (!ok) {
        if (directory->handle != INVALID_HANDLE_VALUE)
            CloseHandle(directory->handle);
        free(directory->path);
        free(directory);
        return false;
    }
    watch_directory_insert(watch, directory);
    return true;
}

static bool watch_windows_walk(forge_watch *watch, const char *relative, size_t depth) {
    /* Keep path buffers off the recursion stack: 64 maximum-depth frames of
     * UTF-8/UTF-16 paths exceed the default Windows thread stack otherwise. */
    struct windows_scan {
        char full[FG_PATH_MAX], pattern[FG_PATH_MAX], name[FG_PATH_MAX], next[FG_PATH_MAX];
        wchar_t wide[FG_PATH_MAX + 8];
        WIN32_FIND_DATAW entry;
    } *scan = malloc(sizeof(*scan));
    if (!scan)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate bounded watch enrollment scratch");
    if (!watch_windows_full(watch, relative, scan->full)) {
        free(scan);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE anchor = watch_windows_lock(watch, scan->full, &info);
    if (anchor == INVALID_HANDLE_VALUE) {
        free(scan);
        return false;
    }
    if (!*relative)
        watch->root_identity = info;
    bool ok = watch_windows_add(watch, relative, depth, &info);
    HANDLE enumeration = INVALID_HANDLE_VALUE;
    if (ok) {
        int count = snprintf(scan->pattern, sizeof(scan->pattern), "%s\\*", scan->full);
        if (count < 0 || (size_t)count >= sizeof(scan->pattern))
            ok = watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                            "Watch directory enumeration path is too long");
        else
            ok = watch_windows_wide(watch, scan->pattern, scan->wide);
    }
    if (ok) {
        enumeration = FindFirstFileW(scan->wide, &scan->entry);
        if (enumeration == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
            ok = watch_windows_error(watch, "Cannot enumerate watch directory");
    }
    while (ok && enumeration != INVALID_HANDLE_VALUE) {
        if (wcscmp(scan->entry.cFileName, L".") && wcscmp(scan->entry.cFileName, L"..")) {
            if (!watch_scan_entry(watch)) {
                ok = false;
                break;
            }
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, scan->entry.cFileName, -1,
                                     scan->name, FG_PATH_MAX, NULL, NULL)) {
                ok = watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                                "Watch directory entry is not valid Unicode");
                break;
            }
            bool directory = (scan->entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool reparse = (scan->entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            int metadata = !*relative ? watch_metadata_name(scan->name) : -1;
            if (metadata >= 0 && directory && !reparse)
                watch->metadata_directory[metadata] = true;
            else if (!(ok = watch_path(watch, relative, scan->name, scan->next)))
                break;
            else if (directory && !reparse)
                ok = watch_windows_walk(watch, scan->next, depth + 1);
        }
        if (!ok || !watch_check(watch)) {
            ok = false;
            break;
        }
        if (!FindNextFileW(enumeration, &scan->entry)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                ok = watch_windows_error(watch, "Cannot continue watch directory enumeration");
            break;
        }
    }
    if (enumeration != INVALID_HANDLE_VALUE)
        FindClose(enumeration);
    CloseHandle(anchor);
    free(scan);
    return ok && watch_check(watch);
}

/* Hold each path component without write/delete sharing during enrollment.
 * Long-lived notification handles permit mutations and are checked separately.
 * The temporary anchors avoid walking through a swapped reparse ancestor. */
static bool watch_enroll(forge_watch *watch, const char *relative) {
    char full[FG_PATH_MAX];
    if (!watch_windows_full(watch, relative, full))
        return false;
    size_t length = strlen(full), base = 0;
    if (length >= 3 && full[1] == ':' && full[2] == '\\')
        base = 3;
    else if (length > 2 && full[0] == '\\' && full[1] == '\\') {
        const char *server = strchr(full + 2, '\\');
        if (server && server[1]) {
            const char *share = strchr(server + 1, '\\');
            base = share ? (size_t)(share - full) : length;
        }
    }
    if (!base)
        return watch_fail(watch, FORGE_ERR_ARGUMENT, FORGE_WATCH_RESCAN_IO,
                          "Filesystem watch requires an ordinary absolute directory path");
    HANDLE anchors[130];
    size_t count = 0, position = base;
    bool ok = true;
    while (ok) {
        if (count >= sizeof(anchors) / sizeof(*anchors)) {
            ok = watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_DEPTH_LIMIT,
                            "Watch root and relative path exceed 130 components");
            break;
        }
        char saved = full[position];
        full[position] = 0;
        BY_HANDLE_FILE_INFORMATION info;
        HANDLE anchor = watch_windows_lock(watch, full, &info);
        full[position] = saved;
        if (anchor == INVALID_HANDLE_VALUE) {
            ok = false;
            break;
        }
        anchors[count++] = anchor;
        if (position == length)
            break;
        while (position < length && full[position] == '\\')
            position++;
        while (position < length && full[position] != '\\')
            position++;
    }
    if (ok)
        ok = watch_windows_walk(watch, relative, watch_depth(relative));
    while (count)
        CloseHandle(anchors[--count]);
    return ok;
}

static bool watch_platform_create(forge_watch *watch, const char *root) {
    wchar_t input[FG_PATH_MAX], absolute[FG_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, input, FG_PATH_MAX))
        return watch_fail(watch, FORGE_ERR_PARSE, FORGE_WATCH_RESCAN_PATH_ENCODING,
                          "Filesystem watch root is not valid UTF-8");
    for (wchar_t *p = input; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    if (input[0] == L'\\' && input[1] == L'\\' && (input[2] == L'?' || input[2] == L'.'))
        return watch_fail(watch, FORGE_ERR_POLICY, FORGE_WATCH_RESCAN_IO,
                          "Filesystem watch rejects Windows device namespaces");
    DWORD count = GetFullPathNameW(input, FG_PATH_MAX, absolute, NULL);
    if (!count || count >= FG_PATH_MAX ||
        !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1, watch->root, FG_PATH_MAX,
                             NULL, NULL))
        return watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                          "Filesystem watch absolute root is too long");
    size_t length = strlen(watch->root);
    while (length > 3 && watch->root[length - 1] == '\\')
        watch->root[--length] = 0;
    watch->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!watch->port)
        return watch_windows_error(watch, "Cannot create directory completion port");
    return watch_enroll(watch, "");
}

static void watch_platform_root_check(forge_watch *watch) {
    wchar_t path[FG_PATH_MAX + 8];
    if (!watch_windows_wide(watch, watch->root, path))
        return;
    HANDLE handle = CreateFileW(
        path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !watch_windows_same(&watch->root_identity, &info))
        watch_loss(watch, FORGE_WATCH_RESCAN_ROOT_CHANGED, 0, true);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}

static bool watch_windows_ignored(forge_watch *watch, const char *path, DWORD action) {
    if (watch_metadata_descendant(path))
        return true;
    int metadata = watch_metadata_name(path);
    if (metadata < 0)
        return false;
    bool removed = action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME;
    bool directory = watch->metadata_directory[metadata];
    if (!removed) {
        char full[FG_PATH_MAX];
        wchar_t wide[FG_PATH_MAX + 8];
        if (watch_windows_full(watch, path, full) && watch_windows_wide(watch, full, wide)) {
            DWORD attributes = GetFileAttributesW(wide);
            if (attributes != INVALID_FILE_ATTRIBUTES)
                directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
    }
    watch->metadata_directory[metadata] = directory && !removed;
    return directory;
}

static int watch_platform_next(forge_watch *watch, unsigned wait_ms) {
    if (!watch->current) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *operation = NULL;
        BOOL ok = GetQueuedCompletionStatus(watch->port, &bytes, &key, &operation, wait_ms);
        DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (!operation) {
            if (!ok && code != WAIT_TIMEOUT)
                watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
            return 0;
        }
        watch_directory *directory = (watch_directory *)key;
        directory->pending = false;
        if (!ok || !bytes || bytes > sizeof(watch->buffer.bytes)) {
            watch->native_events++;
            watch_loss(watch,
                       ((ok && !bytes) || bytes > sizeof(watch->buffer.bytes) ||
                        code == ERROR_NOTIFY_ENUM_DIR)
                           ? FORGE_WATCH_RESCAN_NATIVE_OVERFLOW
                           : FORGE_WATCH_RESCAN_IO,
                       0, true);
            return 1;
        }
        watch->current = directory;
        watch->native_length = bytes;
        watch->native_offset = 0;
    }
    watch_directory *directory = watch->current;
    size_t offset = watch->native_offset, remaining = watch->native_length - offset;
    size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    watch->native_events++;
    if (remaining < header) {
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        watch->current = NULL;
        return 1;
    }
    FILE_NOTIFY_INFORMATION *event =
        (FILE_NOTIFY_INFORMATION *)(void *)(watch->buffer.bytes + offset);
    if ((event->FileNameLength % sizeof(wchar_t)) || event->FileNameLength > remaining - header ||
        (event->NextEntryOffset &&
         (event->NextEntryOffset < header + event->FileNameLength ||
          event->NextEntryOffset > remaining || event->NextEntryOffset % sizeof(DWORD)))) {
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        watch->current = NULL;
        return 1;
    }
    DWORD action = event->Action, next = event->NextEntryOffset;
    char name[FG_PATH_MAX], path[FG_PATH_MAX];
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, event->FileName,
                                    (int)(event->FileNameLength / sizeof(wchar_t)), name,
                                    FG_PATH_MAX - 1, NULL, NULL);
    bool valid = count > 0, ignored = false;
    if (valid) {
        name[count] = 0;
        valid = !memchr(name, 0, (size_t)count);
        for (int i = 0; i < count; i++)
            if (name[i] == '\\')
                name[i] = '/';
        ignored = valid && watch_windows_ignored(watch, name, action);
        path[0] = 0;
        char *component = name;
        while (valid && !ignored && *component) {
            char *slash = strchr(component, '/');
            if (slash)
                *slash = 0;
            char joined[FG_PATH_MAX];
            valid = watch_path(watch, path, component, joined);
            if (valid)
                strcpy(path, joined);
            if (!slash)
                break;
            component = slash + 1;
            if (!*component)
                valid = false;
        }
    }
    if (next)
        watch->native_offset += next;
    else {
        watch->current = NULL;
        watch->native_offset = watch->native_length = 0;
        if (!watch->reopen)
            watch_windows_arm(watch, directory);
    }
    if (!valid) {
        watch_loss(watch, FORGE_WATCH_RESCAN_PATH_ENCODING, 1, false);
        return 1;
    }
    if (ignored)
        return 1;
    unsigned flags = 0;
    switch (action) {
    case FILE_ACTION_ADDED:
        flags = FORGE_WATCH_CREATED;
        break;
    case FILE_ACTION_REMOVED:
        flags = FORGE_WATCH_DELETED;
        break;
    case FILE_ACTION_MODIFIED:
        flags = FORGE_WATCH_MODIFIED | FORGE_WATCH_METADATA;
        break;
    case FILE_ACTION_RENAMED_OLD_NAME:
        flags = FORGE_WATCH_RENAMED_FROM | FORGE_WATCH_RENAMED | FORGE_WATCH_DELETED;
        break;
    case FILE_ACTION_RENAMED_NEW_NAME:
        flags = FORGE_WATCH_RENAMED_TO | FORGE_WATCH_RENAMED | FORGE_WATCH_CREATED;
        break;
    default:
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 1, false);
        return 1;
    }
    if (watch_directory_find(watch, path))
        flags |= FORGE_WATCH_DIRECTORY;
    int metadata = watch_metadata_name(path);
    if (metadata >= 0 && watch->metadata_directory[metadata])
        flags |= FORGE_WATCH_DIRECTORY;
    char full[FG_PATH_MAX];
    wchar_t wide[FG_PATH_MAX + 8];
    if (!(flags & (FORGE_WATCH_DELETED | FORGE_WATCH_RENAMED_FROM)) &&
        watch_windows_full(watch, path, full) && watch_windows_wide(watch, full, wide)) {
        DWORD attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            flags &= ~(unsigned)(FORGE_WATCH_DIRECTORY | FORGE_WATCH_SYMLINK);
            if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                flags |= FORGE_WATCH_DIRECTORY;
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                flags |= FORGE_WATCH_SYMLINK;
        }
    }
    return watch_handle_event(watch, path, flags) ? 1 : -1;
}

static bool watch_platform_pending(const forge_watch *watch) {
    return watch->current != NULL;
}

static void watch_platform_destroy(forge_watch *watch) {
    size_t pending = 0;
    for (size_t i = 0; i < watch->directory_count; i++) {
        watch_directory *directory = watch->directories[i];
        if (directory->pending) {
            pending++;
            CancelIoEx(directory->handle, &directory->operation);
        }
    }
    /* With an IOCP association the OVERLAPPED status is not finalized until
     * its completion packet is dequeued. Waiting on the file handle through
     * GetOverlappedResult(TRUE) can deadlock. Cancel every request, then drain
     * their actual packets before closing handles or freeing request storage. */
    while (pending) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *operation = NULL;
        GetQueuedCompletionStatus(watch->port, &bytes, &key, &operation, INFINITE);
        if (!operation)
            continue;
        watch_directory *directory = (watch_directory *)key;
        if (directory->pending && operation == &directory->operation) {
            directory->pending = false;
            pending--;
        }
    }
    for (size_t i = 0; i < watch->directory_count; i++) {
        watch_directory *directory = watch->directories[i];
        if (directory->handle != INVALID_HANDLE_VALUE)
            CloseHandle(directory->handle);
    }
    if (watch->port)
        CloseHandle(watch->port);
}

static const char *watch_backend(void) {
    return "ReadDirectoryChangesW";
}
#else
static bool watch_posix_error(forge_watch *watch, const char *operation) {
    int code = errno;
    watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
    fg_error(watch->error, FORGE_ERR_IO, "%s (%s)", operation, strerror(code));
    return false;
}

static int watch_posix_open_components(forge_watch *watch, int fd, const char *path) {
    const char *component = path;
    while (*component) {
        if (!watch_check(watch)) {
            close(fd);
            return -1;
        }
        while (*component == '/')
            component++;
        if (!*component)
            break;
        const char *end = strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : strlen(component);
        char name[FG_PATH_MAX];
        if (length >= sizeof(name)) {
            watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                       "Watch directory component is too long");
            close(fd);
            return -1;
        }
        memcpy(name, component, length);
        name[length] = 0;
        struct stat listed, opened;
        if (fstatat(fd, name, &listed, AT_SYMLINK_NOFOLLOW) != 0) {
            watch_posix_error(watch, "Cannot inspect watch directory component");
            close(fd);
            return -1;
        }
        if (!S_ISDIR(listed.st_mode)) {
            watch_fail(watch, FORGE_ERR_POLICY, FORGE_WATCH_RESCAN_IO,
                       "Filesystem watch rejects linked or non-directory ancestors");
            close(fd);
            return -1;
        }
        int next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            watch_posix_error(watch, "Cannot open watch directory component");
            close(fd);
            return -1;
        }
        bool ok = fstat(next, &opened) == 0;
        if (!ok)
            watch_posix_error(watch, "Cannot inspect opened watch directory");
        if (ok && (listed.st_dev != opened.st_dev || listed.st_ino != opened.st_ino))
            ok = watch_fail(watch, FORGE_ERR_CONFLICT, FORGE_WATCH_RESCAN_TOPOLOGY,
                            "Watch directory changed while opening");
        close(fd);
        if (!ok) {
            close(next);
            return -1;
        }
        fd = next;
        component += length;
    }
    return fd;
}

static bool watch_posix_root(forge_watch *watch, const char *root) {
    int fd = open(root[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return watch_posix_error(watch, "Cannot open watch root ancestor");
    fd = watch_posix_open_components(watch, fd, root);
    if (fd < 0)
        return false;
    watch->root_fd = fd;
    if (fstat(fd, &watch->root_identity) != 0)
        return watch_posix_error(watch, "Cannot inspect watch root");
    /* realpath is used only after every input component has passed NOFOLLOW;
     * compare identity again so canonicalization cannot silently switch roots. */
    char *canonical = realpath(root, NULL);
    if (!canonical)
        return watch_posix_error(watch, "Cannot canonicalize watch root");
    size_t length = strlen(canonical);
    bool ok = length < FG_PATH_MAX && fg_utf8_valid(canonical, length);
    if (!ok)
        watch_fail(watch, FORGE_ERR_LIMIT, FORGE_WATCH_RESCAN_PATH_LIMIT,
                   "Canonical watch root is too long or not UTF-8");
    else
        memcpy(watch->root, canonical, length + 1);
    free(canonical);
    struct stat current;
    if (ok && lstat(watch->root, &current) != 0)
        ok = watch_posix_error(watch, "Cannot recheck watch root");
    if (ok && (!S_ISDIR(current.st_mode) || current.st_dev != watch->root_identity.st_dev ||
               current.st_ino != watch->root_identity.st_ino))
        ok = watch_fail(watch, FORGE_ERR_CONFLICT, FORGE_WATCH_RESCAN_ROOT_CHANGED,
                        "Watch root changed while opening");
    return ok;
}

#if defined(__linux__)
static size_t watch_descriptor_slot(const forge_watch *watch, int descriptor) {
    return ((size_t)(unsigned)descriptor * (size_t)2654435761u) & (watch->directory_map_size - 1);
}

static watch_directory *watch_descriptor_find(const forge_watch *watch, int descriptor) {
    size_t index = watch_descriptor_slot(watch, descriptor);
    while (watch->descriptor_map[index]) {
        if (watch->descriptor_map[index]->descriptor == descriptor)
            return watch->descriptor_map[index];
        index = (index + 1) & (watch->directory_map_size - 1);
    }
    return NULL;
}
#endif

static bool watch_posix_add(forge_watch *watch, const char *relative, size_t depth, int fd) {
    if (watch_directory_find(watch, relative))
        return true;
    if (!watch_directory_budget(watch, depth))
        return false;
    watch_directory *directory = calloc(1, sizeof(*directory));
    if (!directory)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate native directory watch");
    directory->depth = depth;
    directory->path = fg_strdup(relative);
    if (!directory->path) {
        free(directory);
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate native watch path");
    }
#if defined(__linux__)
    /* inotify lacks an fd-based add API. The proc path refers to our already
     * opened NOFOLLOW directory, not an unchecked workspace pathname. The
     * final '/.' allows ONLYDIR|DONT_FOLLOW without rejecting proc's fd link. */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d/.", fd);
    directory->descriptor =
        inotify_add_watch(watch->notify_fd, proc_path,
                          IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
                              IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_DELETE_SELF |
                              IN_UNMOUNT | IN_ONLYDIR | IN_DONT_FOLLOW | IN_EXCL_UNLINK);
    if (directory->descriptor < 0) {
        if (errno == ENOENT)
            watch_fail(watch, FORGE_ERR_UNSUPPORTED, FORGE_WATCH_RESCAN_IO,
                       "Linux directory watch enrollment requires /proc/self/fd");
        else
            watch_posix_error(watch, "Cannot enroll inotify directory (check OS watch limits)");
        free(directory->path);
        free(directory);
        return false;
    }
    if (watch_descriptor_find(watch, directory->descriptor)) {
        free(directory->path);
        free(directory);
        return watch_fail(watch, FORGE_ERR_CONFLICT, FORGE_WATCH_RESCAN_TOPOLOGY,
                          "Multiple watch paths refer to the same native directory");
    }
    size_t slot = watch_descriptor_slot(watch, directory->descriptor);
    while (watch->descriptor_map[slot])
        slot = (slot + 1) & (watch->directory_map_size - 1);
    watch->descriptor_map[slot] = directory;
#else
    (void)fd;
#endif
    watch_directory_insert(watch, directory);
    return true;
}

/* Takes ownership of fd. Only the root and temporary recursion descriptors
 * remain open; inotify retains its own kernel references to watched inodes. */
static bool watch_posix_walk(forge_watch *watch, int fd, const char *relative, size_t depth) {
    bool ok = watch_check(watch) && watch_posix_add(watch, relative, depth, fd);
    if (!ok) {
        close(fd);
        return false;
    }
    DIR *directory = fdopendir(fd);
    if (!directory) {
        close(fd);
        return watch_posix_error(watch, "Cannot enumerate watch directory");
    }
    while (ok) {
        if (!watch_check(watch)) {
            ok = false;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno)
                ok = watch_posix_error(watch, "Cannot continue watch directory enumeration");
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (!watch_scan_entry(watch)) {
            ok = false;
            break;
        }
        struct stat listed;
        if (fstatat(fd, entry->d_name, &listed, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = watch_posix_error(watch, "Cannot inspect watch directory entry");
            break;
        }
        bool child_directory = S_ISDIR(listed.st_mode);
        int metadata = !*relative ? watch_metadata_name(entry->d_name) : -1;
        if (metadata >= 0 && child_directory) {
            watch->metadata_directory[metadata] = true;
            continue;
        }
        char next[FG_PATH_MAX];
        if (!(ok = watch_path(watch, relative, entry->d_name, next)))
            break;
        if (child_directory) {
            int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                ok = watch_posix_error(watch, "Cannot open descendant watch directory");
                break;
            }
            struct stat opened;
            if (fstat(child, &opened) != 0) {
                ok = watch_posix_error(watch, "Cannot inspect descendant watch directory");
                close(child);
            } else if (!S_ISDIR(opened.st_mode) || listed.st_dev != opened.st_dev ||
                       listed.st_ino != opened.st_ino) {
                ok = watch_fail(watch, FORGE_ERR_CONFLICT, FORGE_WATCH_RESCAN_TOPOLOGY,
                                "Descendant watch directory changed while opening");
                close(child);
            } else
                ok = watch_posix_walk(watch, child, next, depth + 1);
        }
    }
    if (closedir(directory) != 0 && ok)
        ok = watch_posix_error(watch, "Cannot close watch directory enumeration");
    return ok && watch_check(watch);
}

static bool watch_enroll(forge_watch *watch, const char *relative) {
    int fd = dup(watch->root_fd);
    if (fd < 0)
        return watch_posix_error(watch, "Cannot duplicate watch root descriptor");
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        close(fd);
        return watch_posix_error(watch, "Cannot protect watch descriptor from inheritance");
    }
    fd = watch_posix_open_components(watch, fd, relative);
    if (fd < 0)
        return false;
    return watch_posix_walk(watch, fd, relative, watch_depth(relative));
}

static void watch_platform_root_check(forge_watch *watch) {
    struct stat current;
    if (lstat(watch->root, &current) != 0 || !S_ISDIR(current.st_mode) ||
        current.st_dev != watch->root_identity.st_dev ||
        current.st_ino != watch->root_identity.st_ino)
        watch_loss(watch, FORGE_WATCH_RESCAN_ROOT_CHANGED, 0, true);
}

#if defined(__linux__)
static bool watch_platform_create(forge_watch *watch, const char *root) {
    if (!watch_posix_root(watch, root))
        return false;
    watch->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch->notify_fd < 0)
        return watch_posix_error(watch, "Cannot create inotify instance");
    watch->descriptor_map = calloc(watch->directory_map_size, sizeof(*watch->descriptor_map));
    if (!watch->descriptor_map)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate inotify descriptor map");
    return watch_enroll(watch, "");
}

static int watch_platform_next(forge_watch *watch, unsigned wait_ms) {
    if (watch->native_offset == watch->native_length) {
        struct pollfd descriptor = {watch->notify_fd, POLLIN, 0};
        int available = poll(&descriptor, 1, (int)wait_ms);
        if (available < 0) {
            if (errno != EINTR)
                watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
            return 0;
        }
        if (!available)
            return 0;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
            return 0;
        }
        ssize_t count = read(watch->notify_fd, watch->buffer.bytes, sizeof(watch->buffer.bytes));
        if (count <= 0) {
            if (!count || (errno != EAGAIN && errno != EINTR))
                watch_loss(watch, FORGE_WATCH_RESCAN_IO, 0, true);
            return 0;
        }
        watch->native_offset = 0;
        watch->native_length = (size_t)count;
    }
    size_t remaining = watch->native_length - watch->native_offset;
    watch->native_events++;
    struct inotify_event native;
    if (remaining < sizeof(native)) {
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        watch->native_offset = watch->native_length;
        return 1;
    }
    memcpy(&native, watch->buffer.bytes + watch->native_offset, sizeof(native));
    if ((size_t)native.len > remaining - sizeof(native)) {
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        watch->native_offset = watch->native_length;
        return 1;
    }
    const char *name = (const char *)watch->buffer.bytes + watch->native_offset + sizeof(native);
    watch->native_offset += sizeof(native) + native.len;
    if (native.mask & IN_Q_OVERFLOW) {
        watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        return 1;
    }
    watch_directory *directory = watch_descriptor_find(watch, native.wd);
    if (!directory) {
        watch_loss(watch, FORGE_WATCH_RESCAN_TOPOLOGY, 1, true);
        return 1;
    }
    char path[FG_PATH_MAX];
    if (native.len) {
        if (!memchr(name, 0, native.len)) {
            watch_loss(watch, FORGE_WATCH_RESCAN_PATH_ENCODING, 1, false);
            return 1;
        }
        int metadata = !*directory->path ? watch_metadata_name(name) : -1;
        if (metadata >= 0 && (native.mask & IN_ISDIR)) {
            watch->metadata_directory[metadata] = !(native.mask & (IN_DELETE | IN_MOVED_FROM));
            return 1;
        }
        if (!watch_path(watch, directory->path, name, path)) {
            watch_loss(watch, FORGE_WATCH_RESCAN_PATH_ENCODING, 1, false);
            return 1;
        }
    } else
        strcpy(path, directory->path);
    unsigned flags = 0;
    if (native.mask & IN_CREATE)
        flags |= FORGE_WATCH_CREATED;
    if (native.mask & (IN_MODIFY | IN_CLOSE_WRITE))
        flags |= FORGE_WATCH_MODIFIED;
    if (native.mask & (IN_DELETE | IN_DELETE_SELF))
        flags |= FORGE_WATCH_DELETED;
    if (native.mask & IN_ATTRIB)
        flags |= FORGE_WATCH_METADATA;
    if (native.mask & IN_MOVED_FROM)
        flags |= FORGE_WATCH_RENAMED | FORGE_WATCH_RENAMED_FROM | FORGE_WATCH_DELETED;
    if (native.mask & IN_MOVED_TO)
        flags |= FORGE_WATCH_RENAMED | FORGE_WATCH_RENAMED_TO | FORGE_WATCH_CREATED;
    if (native.mask & IN_MOVE_SELF)
        flags |= FORGE_WATCH_RENAMED;
    if ((native.mask & IN_ISDIR) || !native.len)
        flags |= FORGE_WATCH_DIRECTORY;
    if (native.mask & (IN_UNMOUNT | IN_IGNORED)) {
        watch_loss(watch,
                   *directory->path ? FORGE_WATCH_RESCAN_TOPOLOGY : FORGE_WATCH_RESCAN_ROOT_CHANGED,
                   0, true);
        flags |= FORGE_WATCH_DIRECTORY;
    }
    return watch_handle_event(watch, path, flags) ? 1 : -1;
}

static bool watch_platform_pending(const forge_watch *watch) {
    return watch->native_offset < watch->native_length;
}

static void watch_platform_destroy(forge_watch *watch) {
    if (watch->notify_fd >= 0)
        close(watch->notify_fd);
    if (watch->root_fd >= 0)
        close(watch->root_fd);
    free(watch->descriptor_map);
}

static const char *watch_backend(void) {
    return "inotify";
}
#elif defined(__APPLE__)
static void watch_fsevents_callback(ConstFSEventStreamRef stream, void *info, size_t count,
                                    void *event_paths, const FSEventStreamEventFlags native_flags[],
                                    const FSEventStreamEventId event_ids[]) {
    (void)stream;
    (void)event_ids;
    forge_watch *watch = info;
    char **paths = event_paths;
    size_t root_length = strlen(watch->root);
    for (size_t i = 0; i < count; i++) {
        if (!watch_check(watch)) {
            /* Callback strings belong to the OS and expire on return. Keep
             * accepted events, explicitly mark the unprocessed tail as lost. */
            watch_loss(watch, FORGE_WATCH_RESCAN_DEADLINE, (uint64_t)(count - i), false);
            return;
        }
        if (watch->native_events >= watch->limits.max_native_events) {
            watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_WORK_LIMIT, (uint64_t)(count - i), false);
            return;
        }
        watch->native_events++;
        FSEventStreamEventFlags native = native_flags[i];
        if (native &
            (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
             kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagEventIdsWrapped))
            watch_loss(watch, FORGE_WATCH_RESCAN_NATIVE_OVERFLOW, 0, true);
        if (native & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount |
                      kFSEventStreamEventFlagUnmount))
            watch_loss(watch, FORGE_WATCH_RESCAN_ROOT_CHANGED, 0, true);
        if (native & kFSEventStreamEventFlagHistoryDone)
            continue;
        const char *path = paths[i];
        if (!path || strncmp(path, watch->root, root_length) ||
            (root_length > 1 && path[root_length] && path[root_length] != '/')) {
            watch_loss(watch, FORGE_WATCH_RESCAN_ROOT_CHANGED, 1, true);
            continue;
        }
        path += root_length;
        if (*path == '/')
            path++;
        if (watch_metadata_descendant(path))
            continue;
        int metadata = watch_metadata_name(path);
        if (metadata >= 0 && (native & kFSEventStreamEventFlagItemIsDir) &&
            !(native & kFSEventStreamEventFlagItemIsSymlink)) {
            watch->metadata_directory[metadata] = !(native & kFSEventStreamEventFlagItemRemoved);
            continue;
        }
        size_t path_length = strlen(path);
        if (path_length > watch->limits.max_path_bytes ||
            root_length + path_length + 1 >= FG_PATH_MAX) {
            watch_loss(watch, FORGE_WATCH_RESCAN_PATH_LIMIT, 1, false);
            continue;
        }
        if (!fg_utf8_valid(path, path_length)) {
            watch_loss(watch, FORGE_WATCH_RESCAN_PATH_ENCODING, 1, false);
            continue;
        }
        unsigned flags = 0;
        if (native & kFSEventStreamEventFlagItemCreated)
            flags |= FORGE_WATCH_CREATED;
        if (native & kFSEventStreamEventFlagItemRemoved)
            flags |= FORGE_WATCH_DELETED;
        if (native & kFSEventStreamEventFlagItemModified)
            flags |= FORGE_WATCH_MODIFIED;
        if (native & kFSEventStreamEventFlagItemRenamed)
            flags |= FORGE_WATCH_RENAMED;
        if (native & kFSEventStreamEventFlagItemIsDir)
            flags |= FORGE_WATCH_DIRECTORY;
        if (native & kFSEventStreamEventFlagItemIsSymlink)
            flags |= FORGE_WATCH_SYMLINK;
        if (native & (kFSEventStreamEventFlagItemInodeMetaMod |
                      kFSEventStreamEventFlagItemChangeOwner | kFSEventStreamEventFlagItemXattrMod))
            flags |= FORGE_WATCH_METADATA;
        if (!flags)
            flags = FORGE_WATCH_METADATA;
        if (!watch_handle_event(watch, path, flags)) {
            watch_loss(watch, FORGE_WATCH_RESCAN_MEMORY, (uint64_t)(count - i - 1), false);
            return;
        }
    }
}

static bool watch_platform_create(forge_watch *watch, const char *root) {
    if (!watch_posix_root(watch, root))
        return false;
    CFStringRef path =
        CFStringCreateWithCString(kCFAllocatorDefault, watch->root, kCFStringEncodingUTF8);
    if (!path)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate FSEvents root path");
    const void *paths[] = {path};
    CFArrayRef array = CFArrayCreate(kCFAllocatorDefault, paths, 1, &kCFTypeArrayCallBacks);
    CFRelease(path);
    if (!array)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate FSEvents root array");
    FSEventStreamContext context = {0, watch, NULL, NULL, NULL};
    watch->stream =
        FSEventStreamCreate(kCFAllocatorDefault, watch_fsevents_callback, &context, array,
                            kFSEventStreamEventIdSinceNow, 0.01,
                            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot |
                                kFSEventStreamCreateFlagNoDefer);
    CFRelease(array);
    if (!watch->stream)
        return watch_fail(watch, FORGE_ERR_IO, FORGE_WATCH_RESCAN_IO,
                          "Cannot create FSEvents stream");
    watch->run_loop = CFRunLoopGetCurrent();
    CFRetain(watch->run_loop);
    watch->run_mode = CFStringCreateWithFormat(kCFAllocatorDefault, NULL,
                                               CFSTR("org.forge.watch.%p"), (void *)watch);
    if (!watch->run_mode)
        return watch_fail(watch, FORGE_ERR_MEMORY, FORGE_WATCH_RESCAN_MEMORY,
                          "Cannot allocate private filesystem watch run-loop mode");
    /* Run-loop scheduling is intentionally used instead of a dispatch worker:
     * callbacks run only inside the caller's bounded poll on this same thread. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    FSEventStreamScheduleWithRunLoop(watch->stream, watch->run_loop, watch->run_mode);
#pragma clang diagnostic pop
    if (!FSEventStreamStart(watch->stream))
        return watch_fail(watch, FORGE_ERR_IO, FORGE_WATCH_RESCAN_IO,
                          "Cannot start FSEvents stream");
    watch->stream_started = true;
    return watch_enroll(watch, "");
}

static int watch_platform_next(forge_watch *watch, unsigned wait_ms) {
    size_t before = watch->native_events;
    CFRunLoopRunInMode(watch->run_mode, (CFTimeInterval)wait_ms / 1000.0, true);
    if (watch->memory_failed || watch->cancelled_now)
        return -1;
    return watch->native_events != before ? 1 : 0;
}

static bool watch_platform_pending(const forge_watch *watch) {
    (void)watch;
    return false;
}

static void watch_platform_destroy(forge_watch *watch) {
    if (watch->stream) {
        if (watch->stream_started)
            FSEventStreamStop(watch->stream);
        FSEventStreamInvalidate(watch->stream);
        FSEventStreamRelease(watch->stream);
    }
    if (watch->run_mode)
        CFRelease(watch->run_mode);
    if (watch->run_loop)
        CFRelease(watch->run_loop);
    if (watch->root_fd >= 0)
        close(watch->root_fd);
}

static const char *watch_backend(void) {
    return "FSEvents";
}
#else
static bool watch_platform_create(forge_watch *watch, const char *root) {
    (void)root;
    return watch_fail(watch, FORGE_ERR_UNSUPPORTED, FORGE_WATCH_RESCAN_IO,
                      "Native filesystem watching is unsupported on this platform");
}

static int watch_platform_next(forge_watch *watch, unsigned wait_ms) {
    (void)watch;
    (void)wait_ms;
    return 0;
}

static bool watch_platform_pending(const forge_watch *watch) {
    (void)watch;
    return false;
}

static void watch_platform_destroy(forge_watch *watch) {
    if (watch->root_fd >= 0)
        close(watch->root_fd);
}

static const char *watch_backend(void) {
    return "unsupported";
}
#endif
#endif

forge_watch *forge_watch_create(const char *root, const forge_watch_limits *requested,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout_ms,
                                forge_error *error) {
    forge_watch_limits limits = requested ? *requested : forge_default_watch_limits();
    if (!root || !*root || !watch_valid_limits(&limits)) {
        fg_error(error, FORGE_ERR_ARGUMENT,
                 "Filesystem watch requires a root and valid bounded limits");
        return NULL;
    }
    if (strlen(root) >= FG_PATH_MAX) {
        fg_error(error, FORGE_ERR_LIMIT, "Filesystem watch root exceeds 4095 bytes");
        return NULL;
    }
    if (!fg_utf8_valid(root, strlen(root))) {
        fg_error(error, FORGE_ERR_PARSE, "Filesystem watch root is not UTF-8");
        return NULL;
    }
    forge_watch *watch = calloc(1, sizeof(*watch));
    if (!watch) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate filesystem watcher");
        return NULL;
    }
#ifndef _WIN32
    watch->root_fd = -1;
#if defined(__linux__)
    watch->notify_fd = -1;
#endif
#endif
    watch->limits = limits;
    watch->initial = true;
    watch->event_map_size = watch_map_size(limits.max_events);
    watch->directory_map_size = watch_map_size(limits.max_directories);
    watch_begin_call(watch, cancelled, user, timeout_ms, error);
    watch->directories = calloc(limits.max_directories, sizeof(*watch->directories));
    watch->directory_map = calloc(watch->directory_map_size, sizeof(*watch->directory_map));
    watch->events = calloc(limits.max_events, sizeof(*watch->events));
    watch->ordered = calloc(limits.max_events, sizeof(*watch->ordered));
    watch->event_map = calloc(watch->event_map_size, sizeof(*watch->event_map));
    if (!watch->directories || !watch->directory_map || !watch->events || !watch->ordered ||
        !watch->event_map) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate bounded filesystem watch queues");
        forge_watch_destroy(watch);
        return NULL;
    }
    bool ok = watch_check(watch) && watch_platform_create(watch, root) && watch_check(watch);
    if (!ok) {
        forge_watch_destroy(watch);
        return NULL;
    }
    watch_end_call(watch);
    watch_success(error);
    return watch;
}

static int watch_event_compare(const void *left, const void *right) {
    const watch_event *a = *(const watch_event *const *)left;
    const watch_event *b = *(const watch_event *const *)right;
    return strcmp(a->path, b->path);
}

static char *watch_json(forge_watch *watch, bool timed_out, bool more_pending, forge_error *error) {
    for (size_t i = 0; i < watch->event_count; i++)
        watch->ordered[i] = &watch->events[i];
    qsort(watch->ordered, watch->event_count, sizeof(*watch->ordered), watch_event_compare);
    fg_buf json = {0};
    unsigned reasons = watch->reasons | (watch->initial ? FORGE_WATCH_RESCAN_INITIAL : 0u);
    bool ok = fg_buf_printf(&json, "{\"schema_version\":1,\"backend\":\"%s\",\"events\":[",
                            watch_backend());
    for (size_t i = 0; ok && i < watch->event_count; i++) {
        watch_event *event = watch->ordered[i];
        char *path = fg_json_string(event->path);
        ok = path &&
             fg_buf_printf(&json, "%s{\"path\":%s,\"flags\":%u}", i ? "," : "", path, event->flags);
        free(path);
    }
    if (ok)
        ok = fg_buf_printf(
            &json,
            "],\"rescan_required\":%s,\"initial_scan_required\":%s,\"reopen_required\":%s,"
            "\"timed_out\":%s,\"more_pending\":%s,\"reason_flags\":%u,\"dropped_events\":%llu,"
            "\"dropped_events_unknown\":%s,\"overflow_count\":%llu,\"directories\":%zu,"
            "\"path_encoding\":\"utf-8\"}",
            reasons || watch->reopen ? "true" : "false", watch->initial ? "true" : "false",
            watch->reopen ? "true" : "false", timed_out ? "true" : "false",
            more_pending ? "true" : "false", reasons, (unsigned long long)watch->dropped,
            watch->dropped_unknown ? "true" : "false", (unsigned long long)watch->overflows,
            watch->directory_count);
    if (!ok) {
        fg_buf_clear(&json);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot serialize filesystem watch batch");
        return NULL;
    }
    if (json.len > watch->limits.max_bytes) {
        fg_buf_clear(&json);
        fg_error(error, FORGE_ERR_LIMIT, "Filesystem watch JSON exceeds its byte limit");
        return NULL;
    }
    char *result = fg_buf_take(&json);
    if (!result) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot finalize filesystem watch batch");
        return NULL;
    }
    for (size_t i = 0; i < watch->event_count; i++) {
        free(watch->events[i].path);
        watch->events[i] = (watch_event){0};
    }
    memset(watch->event_map, 0, watch->event_map_size * sizeof(*watch->event_map));
    watch->event_count = watch->event_bytes = 0;
    watch->initial = false;
    if (!watch->reopen)
        watch->reasons = 0;
    watch_success(error);
    return result;
}

char *forge_watch_poll(forge_watch *watch, uint64_t timeout_ms, forge_cancel_fn cancelled,
                       void *user, forge_error *error) {
    if (!watch) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Filesystem watch poll requires a watcher");
        return NULL;
    }
#if defined(__APPLE__)
    if (watch->run_loop != CFRunLoopGetCurrent()) {
        fg_error(error, FORGE_ERR_ARGUMENT, "FSEvents must be polled on its creating thread");
        return NULL;
    }
#endif
    watch_begin_call(watch, cancelled, user, timeout_ms, error);
    bool more_pending = false, ok = watch_check(watch);
    if (ok)
        watch_platform_root_check(watch);
    while (ok && !watch->deadline_now) {
        if (watch->native_events >= watch->limits.max_native_events) {
            more_pending = true;
            break;
        }
        if (watch->reopen && !watch_platform_pending(watch)) {
            watch->dropped_unknown = true;
            break;
        }
        unsigned wait_ms = 0;
        if (timeout_ms && !watch->initial && !watch->reasons && !watch->event_count) {
            uint64_t now = fg_now_ms();
            if (now >= watch->deadline) {
                watch->deadline_now = true;
                break;
            }
            wait_ms = (unsigned)FG_MIN(watch->deadline - now, WATCH_WAIT_SLICE_MS);
        }
        int received = watch_platform_next(watch, wait_ms);
        if (received < 0) {
            ok = false;
            break;
        }
        if (!received && (!timeout_ms || watch->initial || watch->reasons || watch->event_count ||
                          watch->reopen))
            break;
        ok = watch_check(watch);
    }
    more_pending |= watch_platform_pending(watch);
    if (watch->cancelled_now || watch->memory_failed || (!ok && !watch->deadline_now)) {
        watch_end_call(watch);
        return NULL;
    }
    bool timed_out =
        watch->deadline_now || (!timeout_ms && !watch->initial && !watch->event_count &&
                                !watch->reasons && !watch->reopen && !more_pending);
    char *result = watch_json(watch, timed_out, more_pending, error);
    watch_end_call(watch);
    return result;
}

void forge_watch_destroy(forge_watch *watch) {
    if (!watch)
        return;
    watch_platform_destroy(watch);
    for (size_t i = 0; i < watch->directory_count; i++) {
        free(watch->directories[i]->path);
        free(watch->directories[i]);
    }
    for (size_t i = 0; i < watch->event_count; i++)
        free(watch->events[i].path);
    free(watch->directories);
    free(watch->directory_map);
    free(watch->events);
    free(watch->ordered);
    free(watch->event_map);
    free(watch);
}
