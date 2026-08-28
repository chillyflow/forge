#include "internal.h"
#include "input_snapshot.h"
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SNAPSHOT_BLOCK_BYTES (64u * 1024u)
#define SNAPSHOT_HASH_OFFSET UINT64_C(14695981039346656037)
#define SNAPSHOT_HASH_PRIME UINT64_C(1099511628211)

typedef struct {
    char *path;
    uint64_t length, hash;
} input_file;

struct fg_input_snapshot {
    input_file *files;
    size_t count, capacity;
    uint64_t bytes, hash;
};

typedef struct {
    fg_input_snapshot *snapshot;
    size_t max_files, root_length, relative_length;
    uint64_t max_bytes, deadline;
    forge_cancel_fn cancelled;
    void *user;
    forge_error *error;
    char root[FG_PATH_MAX], relative[FG_PATH_MAX];
    unsigned char block[SNAPSHOT_BLOCK_BYTES];
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX + 8];
    HANDLE anchors[FG_INPUT_SNAPSHOT_MAX_DEPTH + 1];
    size_t anchor_count;
#endif
} snapshot_scan;

static bool snapshot_fail(snapshot_scan *scan, forge_status status, const char *message) {
    fg_error(scan->error, status, "%s: %s", message, scan->relative_length ? scan->relative : ".");
    return false;
}

static bool snapshot_check(snapshot_scan *scan) {
    if (scan->cancelled && scan->cancelled(scan->user))
        return snapshot_fail(scan, FORGE_ERR_CANCELLED, "Input snapshot cancelled");
    if (scan->deadline && fg_now_ms() >= scan->deadline)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot deadline reached");
    return true;
}

static uint64_t snapshot_hash_bytes(uint64_t hash, const void *bytes, size_t length) {
    const unsigned char *data = bytes;
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= SNAPSHOT_HASH_PRIME;
    }
    return hash;
}

/* Explicit byte order and length framing make aggregate identities independent
 * of native endianness, struct padding, traversal order, and absolute root. */
static uint64_t snapshot_hash_uint(uint64_t hash, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(value & 255u);
        value >>= 8;
    }
    return snapshot_hash_bytes(hash, bytes, sizeof(bytes));
}

static bool snapshot_metadata(const snapshot_scan *scan, const char *name) {
    if (scan->relative_length)
        return false;
#ifdef _WIN32
    return !_stricmp(name, ".git") || !_stricmp(name, ".forge");
#else
    return !strcmp(name, ".git") || !strcmp(name, ".forge");
#endif
}

static bool snapshot_push(snapshot_scan *scan, const char *name) {
    size_t length = strlen(name), separator = scan->relative_length ? 1u : 0u;
    if (length >= FG_PATH_MAX || scan->relative_length + separator + length >= FG_PATH_MAX ||
        scan->root_length + scan->relative_length + separator + length + 1 >= FG_PATH_MAX)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot path exceeds 4095 bytes");
    if (separator)
        scan->relative[scan->relative_length++] = '/';
    memcpy(scan->relative + scan->relative_length, name, length + 1);
    scan->relative_length += length;
    return true;
}

static void snapshot_pop(snapshot_scan *scan, size_t previous) {
    scan->relative_length = previous;
    scan->relative[previous] = 0;
}

static bool snapshot_file_budget(snapshot_scan *scan, uint64_t length) {
    if (scan->snapshot->count >= scan->max_files)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot file limit exceeded");
    if (length > scan->max_bytes - scan->snapshot->bytes)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot byte limit exceeded");
    return true;
}

static bool snapshot_capture(snapshot_scan *scan, uint64_t *length, uint64_t *hash, size_t count) {
    if ((uint64_t)count > scan->max_bytes - scan->snapshot->bytes)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot byte limit exceeded");
    scan->snapshot->bytes += (uint64_t)count;
    *length += (uint64_t)count;
    *hash = snapshot_hash_bytes(*hash, scan->block, count);
    return snapshot_check(scan);
}

static bool snapshot_add(snapshot_scan *scan, uint64_t length, uint64_t hash) {
    fg_input_snapshot *snapshot = scan->snapshot;
    if (snapshot->count == snapshot->capacity) {
        size_t next = snapshot->capacity > SIZE_MAX / 2 ? scan->max_files : snapshot->capacity * 2;
        if (next < 16)
            next = 16;
        if (next > scan->max_files)
            next = scan->max_files;
        if (next <= snapshot->capacity || next > SIZE_MAX / sizeof(*snapshot->files))
            return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot record limit exceeded");
        input_file *files = realloc(snapshot->files, next * sizeof(*files));
        if (!files)
            return snapshot_fail(scan, FORGE_ERR_MEMORY, "Input snapshot allocation failed");
        snapshot->files = files;
        snapshot->capacity = next;
    }
    char *path = fg_strdup(scan->relative);
    if (!path)
        return snapshot_fail(scan, FORGE_ERR_MEMORY, "Input snapshot path allocation failed");
    snapshot->files[snapshot->count++] = (input_file){path, length, hash};
    return true;
}

#ifdef _WIN32
static bool snapshot_windows_error(snapshot_scan *scan, const char *operation) {
    DWORD code = GetLastError();
    fg_error(scan->error, FORGE_ERR_IO, "%s (Windows error %lu): %s", operation,
             (unsigned long)code, scan->relative_length ? scan->relative : ".");
    return false;
}

/* Full paths use the native Unicode/extended-length API. The UTF-8 byte bound is
 * still enforced independently; ANSI code-page conversion never identifies a
 * different file or silently drops a name. scan->wide is scratch, not retained. */
static bool snapshot_windows_path(snapshot_scan *scan, const char *path) {
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, scan->wide + 8, FG_PATH_MAX);
    if (!count)
        return snapshot_fail(scan, FORGE_ERR_PARSE, "Input snapshot path is not valid UTF-8");
    wchar_t *source = scan->wide + 8;
    for (int i = 0; i < count; i++)
        if (source[i] == L'/')
            source[i] = L'\\';
    if (source[0] == L'\\' && source[1] == L'\\') {
        memmove(scan->wide + 8, source + 2, ((size_t)count - 2) * sizeof(*source));
        memcpy(scan->wide, L"\\\\?\\UNC\\", 8 * sizeof(*source));
    } else {
        memmove(scan->wide + 4, source, (size_t)count * sizeof(*source));
        memcpy(scan->wide, L"\\\\?\\", 4 * sizeof(*source));
    }
    return true;
}

static bool snapshot_windows_full(snapshot_scan *scan, bool pattern) {
    char path[FG_PATH_MAX];
    int length =
        snprintf(path, sizeof(path), "%s%s%s%s", scan->root,
                 scan->root_length && scan->root[scan->root_length - 1] == '\\' ? "" : "\\",
                 scan->relative, pattern ? (scan->relative_length ? "\\*" : "*") : "");
    if (length < 0 || (size_t)length >= sizeof(path))
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot path exceeds 4095 bytes");
    return snapshot_windows_path(scan, path);
}

static HANDLE snapshot_windows_open(snapshot_scan *scan, const char *path, bool directory) {
    if (!snapshot_check(scan) || !snapshot_windows_path(scan, path))
        return INVALID_HANDLE_VALUE;
    HANDLE handle =
        CreateFileW(scan->wide, directory ? FILE_READ_ATTRIBUTES : GENERIC_READ, FILE_SHARE_READ,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT |
                        (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN),
                    NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        snapshot_windows_error(scan, "Cannot open input snapshot entry");
        return handle;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) {
        snapshot_windows_error(scan, "Cannot inspect input snapshot entry");
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) ||
        !!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) {
        snapshot_fail(scan, FORGE_ERR_POLICY, "Input snapshot rejects links and special files");
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

static HANDLE snapshot_windows_entry(snapshot_scan *scan, bool directory) {
    char path[FG_PATH_MAX];
    int length = snprintf(
        path, sizeof(path), "%s%s%s", scan->root,
        scan->root_length && scan->root[scan->root_length - 1] == '\\' ? "" : "\\", scan->relative);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot path exceeds 4095 bytes");
        return INVALID_HANDLE_VALUE;
    }
    return snapshot_windows_open(scan, path, directory);
}

static bool snapshot_windows_same(const BY_HANDLE_FILE_INFORMATION *before,
                                  const BY_HANDLE_FILE_INFORMATION *after) {
    return before->dwVolumeSerialNumber == after->dwVolumeSerialNumber &&
           before->nFileIndexHigh == after->nFileIndexHigh &&
           before->nFileIndexLow == after->nFileIndexLow &&
           before->dwFileAttributes == after->dwFileAttributes &&
           before->nFileSizeHigh == after->nFileSizeHigh &&
           before->nFileSizeLow == after->nFileSizeLow &&
           before->ftLastWriteTime.dwHighDateTime == after->ftLastWriteTime.dwHighDateTime &&
           before->ftLastWriteTime.dwLowDateTime == after->ftLastWriteTime.dwLowDateTime;
}

static bool snapshot_windows_file(snapshot_scan *scan) {
    HANDLE handle = snapshot_windows_entry(scan, false);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION before, after;
    bool ok = GetFileInformationByHandle(handle, &before) != 0;
    if (!ok)
        snapshot_windows_error(scan, "Cannot inspect input file");
    uint64_t expected = ok ? ((uint64_t)before.nFileSizeHigh << 32) | before.nFileSizeLow : 0;
    if (ok)
        ok = snapshot_file_budget(scan, expected);
    uint64_t length = 0, hash = SNAPSHOT_HASH_OFFSET;
    while (ok) {
        if (!snapshot_check(scan)) {
            ok = false;
            break;
        }
        DWORD count = 0;
        if (!ReadFile(handle, scan->block, (DWORD)sizeof(scan->block), &count, NULL)) {
            ok = snapshot_windows_error(scan, "Cannot read input file");
            break;
        }
        if (!count)
            break;
        ok = snapshot_capture(scan, &length, &hash, (size_t)count);
    }
    if (ok)
        ok = snapshot_check(scan);
    if (ok && !GetFileInformationByHandle(handle, &after))
        ok = snapshot_windows_error(scan, "Cannot recheck input file");
    if (ok && (!snapshot_windows_same(&before, &after) || expected != length))
        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT, "Input file changed during snapshot");
    if (!CloseHandle(handle) && ok)
        ok = snapshot_windows_error(scan, "Cannot close input file");
    return ok && snapshot_add(scan, length, hash);
}

static bool snapshot_windows_walk(snapshot_scan *scan, HANDLE directory, size_t depth) {
    BY_HANDLE_FILE_INFORMATION before, after;
    if (!snapshot_check(scan))
        return false;
    if (!GetFileInformationByHandle(directory, &before))
        return snapshot_windows_error(scan, "Cannot inspect input directory");
    if (!snapshot_windows_full(scan, true))
        return false;
    WIN32_FIND_DATAW entry;
    HANDLE listing = FindFirstFileW(scan->wide, &entry);
    if (listing == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND)
            return snapshot_windows_error(scan, "Cannot enumerate input directory");
    }
    bool ok = true, more = listing != INVALID_HANDLE_VALUE;
    while (ok && more) {
        if (!snapshot_check(scan)) {
            ok = false;
            break;
        }
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) {
            char name[MAX_PATH * 4];
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.cFileName, -1, name,
                                     (int)sizeof(name), NULL, NULL)) {
                ok = snapshot_fail(scan, FORGE_ERR_PARSE, "Input filename is not valid Unicode");
                break;
            }
            bool is_directory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool excluded = is_directory && snapshot_metadata(scan, name);
            size_t previous = scan->relative_length;
            if (entry.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))
                ok = snapshot_fail(scan, FORGE_ERR_POLICY,
                                   "Input snapshot rejects links and special files");
            else if (!excluded && snapshot_push(scan, name)) {
                if (is_directory) {
                    if (depth >= FG_INPUT_SNAPSHOT_MAX_DEPTH)
                        ok = snapshot_fail(scan, FORGE_ERR_LIMIT,
                                           "Input snapshot directory depth exceeded");
                    else {
                        HANDLE child = snapshot_windows_entry(scan, true);
                        ok = child != INVALID_HANDLE_VALUE;
                        if (ok) {
                            ok = snapshot_windows_walk(scan, child, depth + 1);
                            if (!CloseHandle(child) && ok)
                                ok = snapshot_windows_error(scan, "Cannot close input directory");
                        }
                    }
                } else
                    ok = snapshot_windows_file(scan);
                snapshot_pop(scan, previous);
            } else if (!excluded)
                ok = false;
        }
        if (ok && !FindNextFileW(listing, &entry)) {
            DWORD code = GetLastError();
            if (code != ERROR_NO_MORE_FILES)
                ok = snapshot_windows_error(scan, "Cannot continue input directory enumeration");
            more = false;
        }
    }
    if (listing != INVALID_HANDLE_VALUE && !FindClose(listing) && ok)
        ok = snapshot_windows_error(scan, "Cannot close input directory enumeration");
    if (ok && !GetFileInformationByHandle(directory, &after))
        ok = snapshot_windows_error(scan, "Cannot recheck input directory");
    if (ok && !snapshot_windows_same(&before, &after))
        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT, "Input directory changed during snapshot");
    return ok && snapshot_check(scan);
}

static bool snapshot_windows_root(snapshot_scan *scan, const char *root) {
    wchar_t input[FG_PATH_MAX], absolute[FG_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1, input, FG_PATH_MAX))
        return snapshot_fail(scan, FORGE_ERR_PARSE, "Workspace path is not valid UTF-8");
    /* Device namespaces do not identify an ordinary workspace root. Paths below
     * a drive or UNC share receive our own extended-length prefix later. */
    if (!wcsncmp(input, L"\\\\?\\", 4) || !wcsncmp(input, L"\\\\.\\", 4))
        return snapshot_fail(scan, FORGE_ERR_POLICY, "Device namespace workspace is unsupported");
    DWORD count = GetFullPathNameW(input, FG_PATH_MAX, absolute, NULL);
    if (!count)
        return snapshot_windows_error(scan, "Cannot resolve input snapshot workspace");
    if (count >= FG_PATH_MAX)
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Workspace path exceeds 4095 bytes");
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, absolute, -1, scan->root, FG_PATH_MAX,
                             NULL, NULL))
        return snapshot_fail(scan, FORGE_ERR_LIMIT, "Workspace path exceeds UTF-8 path limit");
    scan->root_length = strlen(scan->root);
    for (size_t i = 0; i < scan->root_length; i++)
        if (scan->root[i] == '/')
            scan->root[i] = '\\';
    size_t base = 0;
    if (scan->root_length >= 3 && scan->root[1] == ':' && scan->root[2] == '\\')
        base = 3;
    else if (scan->root_length > 2 && scan->root[0] == '\\' && scan->root[1] == '\\') {
        const char *server = strchr(scan->root + 2, '\\');
        const char *share = server ? strchr(server + 1, '\\') : NULL;
        if (server && server[1])
            base = share ? (size_t)(share - scan->root) : scan->root_length;
    }
    if (!base)
        return snapshot_fail(scan, FORGE_ERR_POLICY,
                             "Workspace must identify a drive or UNC directory");
    while (scan->root_length > base && scan->root[scan->root_length - 1] == '\\')
        scan->root[--scan->root_length] = 0;
    /* Hold every ancestor without write/delete sharing while native full paths
     * are used. Each is opened as a reparse point, inspected, and never followed.
     * Descendant directory handles are held in the same way by the walker. */
    size_t position = base;
    for (;;) {
        if (scan->anchor_count >= sizeof(scan->anchors) / sizeof(*scan->anchors))
            return snapshot_fail(scan, FORGE_ERR_LIMIT, "Workspace ancestor depth exceeded");
        char saved = scan->root[position];
        scan->root[position] = 0;
        HANDLE directory = snapshot_windows_open(scan, scan->root, true);
        scan->root[position] = saved;
        if (directory == INVALID_HANDLE_VALUE)
            return false;
        scan->anchors[scan->anchor_count++] = directory;
        if (position == scan->root_length)
            break;
        while (position < scan->root_length && scan->root[position] == '\\')
            position++;
        while (position < scan->root_length && scan->root[position] != '\\')
            position++;
    }
    return snapshot_windows_walk(scan, scan->anchors[scan->anchor_count - 1], 0);
}
#else
static bool snapshot_posix_error(snapshot_scan *scan, const char *operation) {
    int code = errno;
    fg_error(scan->error, FORGE_ERR_IO, "%s (%s): %s", operation, strerror(code),
             scan->relative_length ? scan->relative : ".");
    return false;
}

static bool snapshot_posix_same(const struct stat *before, const struct stat *after) {
#if defined(__APPLE__)
    bool timestamps = before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec &&
                      before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
                      before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec &&
                      before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
#else
    bool timestamps = before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
                      before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
                      before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
                      before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
#endif
    return timestamps && before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
           before->st_mode == after->st_mode && before->st_size == after->st_size &&
           before->st_nlink == after->st_nlink;
}

static bool snapshot_posix_file(snapshot_scan *scan, int parent, const char *name,
                                const struct stat *listed) {
    if (!snapshot_check(scan))
        return false;
    if (listed->st_size < 0)
        return snapshot_fail(scan, FORGE_ERR_IO, "Input file has an invalid size");
    if (!snapshot_file_budget(scan, (uint64_t)listed->st_size))
        return false;
    /* NONBLOCK prevents a file swapped for a FIFO from blocking before fstat;
     * NOFOLLOW and descriptor-relative opens prevent link traversal. */
    int file = openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (file < 0) {
        if (errno == ELOOP)
            return snapshot_fail(scan, FORGE_ERR_POLICY, "Input snapshot rejects symbolic links");
        return snapshot_posix_error(scan, "Cannot open input file");
    }
    struct stat before, after;
    bool ok = fstat(file, &before) == 0;
    if (!ok)
        snapshot_posix_error(scan, "Cannot inspect input file");
    if (ok && !S_ISREG(before.st_mode))
        ok = snapshot_fail(scan, FORGE_ERR_POLICY, "Input snapshot rejects special files");
    if (ok && !snapshot_posix_same(listed, &before))
        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT, "Input file changed before it could be read");
    uint64_t length = 0, hash = SNAPSHOT_HASH_OFFSET;
    while (ok) {
        if (!snapshot_check(scan)) {
            ok = false;
            break;
        }
        ssize_t count = read(file, scan->block, sizeof(scan->block));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ok = snapshot_posix_error(scan, "Cannot read input file");
            break;
        }
        if (!count)
            break;
        ok = snapshot_capture(scan, &length, &hash, (size_t)count);
    }
    if (ok)
        ok = snapshot_check(scan);
    if (ok && fstat(file, &after) != 0)
        ok = snapshot_posix_error(scan, "Cannot recheck input file");
    if (ok && (!snapshot_posix_same(&before, &after) || (uint64_t)before.st_size != length))
        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT, "Input file changed during snapshot");
    if (close(file) != 0 && ok)
        ok = snapshot_posix_error(scan, "Cannot close input file");
    return ok && snapshot_add(scan, length, hash);
}

/* Takes ownership of fd even on failure. Directory traversal is relative to
 * already-open descriptors, never via a reconstructed pathname. */
static bool snapshot_posix_walk(snapshot_scan *scan, int fd, size_t depth) {
    struct stat before, after;
    bool ok = snapshot_check(scan);
    if (ok && fstat(fd, &before) != 0)
        ok = snapshot_posix_error(scan, "Cannot inspect input directory");
    if (!ok) {
        close(fd);
        return false;
    }
    DIR *directory = fdopendir(fd);
    if (!directory) {
        snapshot_posix_error(scan, "Cannot enumerate input directory");
        close(fd);
        return false;
    }
    while (ok) {
        if (!snapshot_check(scan)) {
            ok = false;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno)
                ok = snapshot_posix_error(scan, "Cannot continue input directory enumeration");
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        struct stat listed;
        if (fstatat(fd, entry->d_name, &listed, AT_SYMLINK_NOFOLLOW) != 0) {
            ok = snapshot_posix_error(scan, "Cannot inspect input directory entry");
            break;
        }
        bool is_directory = S_ISDIR(listed.st_mode);
        if (!is_directory && !S_ISREG(listed.st_mode)) {
            ok = snapshot_fail(scan, FORGE_ERR_POLICY,
                               "Input snapshot rejects links and special files");
            break;
        }
        if (is_directory && snapshot_metadata(scan, entry->d_name))
            continue;
        size_t previous = scan->relative_length;
        if (!snapshot_push(scan, entry->d_name)) {
            ok = false;
            break;
        }
        if (is_directory) {
            if (depth >= FG_INPUT_SNAPSHOT_MAX_DEPTH)
                ok =
                    snapshot_fail(scan, FORGE_ERR_LIMIT, "Input snapshot directory depth exceeded");
            else {
                int child =
                    openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (child < 0) {
                    if (errno == ELOOP || errno == ENOTDIR)
                        ok = snapshot_fail(scan, FORGE_ERR_POLICY,
                                           "Input directory changed or became a link");
                    else
                        ok = snapshot_posix_error(scan, "Cannot open input directory");
                } else {
                    struct stat opened;
                    if (fstat(child, &opened) != 0) {
                        ok = snapshot_posix_error(scan, "Cannot inspect opened input directory");
                        close(child);
                    } else if (!snapshot_posix_same(&listed, &opened)) {
                        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT,
                                           "Input directory changed before enumeration");
                        close(child);
                    } else
                        ok = snapshot_posix_walk(scan, child, depth + 1);
                }
            }
        } else
            ok = snapshot_posix_file(scan, fd, entry->d_name, &listed);
        snapshot_pop(scan, previous);
    }
    if (ok && fstat(fd, &after) != 0)
        ok = snapshot_posix_error(scan, "Cannot recheck input directory");
    if (ok && !snapshot_posix_same(&before, &after))
        ok = snapshot_fail(scan, FORGE_ERR_CONFLICT, "Input directory changed during snapshot");
    if (closedir(directory) != 0 && ok)
        ok = snapshot_posix_error(scan, "Cannot close input directory");
    return ok && snapshot_check(scan);
}

static bool snapshot_posix_root(snapshot_scan *scan, const char *root) {
    /* Resolve every supplied root component with NOFOLLOW as well. realpath()
     * would erase evidence that the root/one of its ancestors was a symlink. */
    int fd = open(root[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return snapshot_posix_error(scan, "Cannot open input snapshot workspace");
    const char *component = root;
    bool ok = true;
    while (*component && ok) {
        if (!snapshot_check(scan)) {
            ok = false;
            break;
        }
        while (*component == '/')
            component++;
        if (!*component)
            break;
        const char *end = strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : strlen(component);
        char name[FG_PATH_MAX];
        memcpy(name, component, length);
        name[length] = 0;
        struct stat info;
        if (fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            ok = snapshot_posix_error(scan, "Cannot inspect workspace component");
        else if (!S_ISDIR(info.st_mode))
            ok = snapshot_fail(scan, FORGE_ERR_POLICY,
                               "Workspace path contains a link or non-directory");
        else {
            int next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0) {
                if (errno == ELOOP || errno == ENOTDIR)
                    ok = snapshot_fail(scan, FORGE_ERR_POLICY,
                                       "Workspace component changed or became a link");
                else
                    ok = snapshot_posix_error(scan, "Cannot open workspace component");
            } else {
                struct stat opened;
                if (fstat(next, &opened) != 0) {
                    ok = snapshot_posix_error(scan, "Cannot inspect opened workspace component");
                    close(next);
                } else if (info.st_dev != opened.st_dev || info.st_ino != opened.st_ino) {
                    ok = snapshot_fail(scan, FORGE_ERR_CONFLICT,
                                       "Workspace component changed while opening");
                    close(next);
                } else {
                    if (close(fd) != 0)
                        ok = snapshot_posix_error(scan, "Cannot close workspace ancestor");
                    fd = next;
                }
            }
        }
        component += length;
    }
    if (!ok) {
        close(fd);
        return false;
    }
    return snapshot_posix_walk(scan, fd, 0);
}
#endif

static int snapshot_compare_path(const void *left, const void *right) {
    const input_file *a = left, *b = right;
    return strcmp(a->path, b->path);
}

fg_input_snapshot *fg_input_snapshot_take(const char *root, size_t max_files, uint64_t max_bytes,
                                          forge_cancel_fn cancel, void *user,
                                          uint64_t absolute_deadline, forge_error *error) {
    if (!root || !*root || !max_files || !max_bytes || max_files > SIZE_MAX / sizeof(input_file)) {
        fg_error(error, FORGE_ERR_ARGUMENT,
                 "Snapshot requires a workspace and nonzero bounded limits");
        return NULL;
    }
    if (strlen(root) >= FG_PATH_MAX) {
        fg_error(error, FORGE_ERR_LIMIT, "Input snapshot workspace path exceeds 4095 bytes");
        return NULL;
    }
    snapshot_scan *scan = calloc(1, sizeof(*scan));
    fg_input_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (!scan || !snapshot) {
        free(scan);
        free(snapshot);
        fg_error(error, FORGE_ERR_MEMORY, "Input snapshot allocation failed");
        return NULL;
    }
    scan->snapshot = snapshot;
    scan->max_files = max_files;
    scan->max_bytes = max_bytes;
    scan->cancelled = cancel;
    scan->user = user;
    scan->deadline = absolute_deadline;
    scan->error = error;
    scan->root_length = strlen(root);
    memcpy(scan->root, root, scan->root_length + 1);
    bool ok = snapshot_check(scan);
#ifdef _WIN32
    if (ok)
        ok = snapshot_windows_root(scan, root);
    for (size_t i = scan->anchor_count; i > 0; i--)
        if (!CloseHandle(scan->anchors[i - 1]) && ok)
            ok = snapshot_windows_error(scan, "Cannot close workspace ancestor");
#else
    if (ok)
        ok = snapshot_posix_root(scan, root);
#endif
    if (ok && snapshot->count > 1)
        qsort(snapshot->files, snapshot->count, sizeof(*snapshot->files), snapshot_compare_path);
    if (ok)
        ok = snapshot_check(scan);
    static const char domain[] = "forge-input-snapshot-v1";
    uint64_t hash = snapshot_hash_bytes(SNAPSHOT_HASH_OFFSET, domain, sizeof(domain) - 1);
    hash = snapshot_hash_uint(hash, (uint64_t)snapshot->count);
    hash = snapshot_hash_uint(hash, snapshot->bytes);
    for (size_t i = 0; ok && i < snapshot->count; i++) {
        ok = snapshot_check(scan);
        if (!ok)
            break;
        input_file *file = &snapshot->files[i];
        hash = snapshot_hash_uint(hash, (uint64_t)strlen(file->path));
        hash = snapshot_hash_bytes(hash, file->path, strlen(file->path));
        hash = snapshot_hash_uint(hash, file->length);
        hash = snapshot_hash_uint(hash, file->hash);
    }
    free(scan);
    if (!ok) {
        fg_input_snapshot_destroy(snapshot);
        return NULL;
    }
    snapshot->hash = hash;
    return snapshot;
}

bool fg_input_snapshot_equal(const fg_input_snapshot *left, const fg_input_snapshot *right) {
    if (!left || !right || left->count != right->count || left->bytes != right->bytes ||
        left->hash != right->hash)
        return false;
    for (size_t i = 0; i < left->count; i++) {
        const input_file *a = &left->files[i], *b = &right->files[i];
        if (a->length != b->length || a->hash != b->hash || strcmp(a->path, b->path))
            return false;
    }
    return true;
}

uint64_t fg_input_snapshot_hash(const fg_input_snapshot *snapshot) {
    return snapshot ? snapshot->hash : 0;
}

void fg_input_snapshot_destroy(fg_input_snapshot *snapshot) {
    if (snapshot) {
        for (size_t i = 0; i < snapshot->count; i++)
            free(snapshot->files[i].path);
        free(snapshot->files);
        free(snapshot);
    }
}
