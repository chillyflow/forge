#include "internal.h"
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#endif

void forge_free(void *p) {
    free(p);
}
const char *forge_status_string(forge_status s) {
    static const char *const names[] = {"ok",        "argument",  "memory",  "io",
                                        "policy",    "limit",     "model",   "parse",
                                        "cancelled", "not_found", "conflict"};
    return (unsigned)s < sizeof(names) / sizeof(*names) ? names[s] : "unknown";
}
forge_status fg_error(forge_error *e, forge_status s, const char *fmt, ...) {
    if (e) {
        e->code = s;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);
    }
    return s;
}
char *fg_strdup(const char *s) {
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
bool fg_buf_add(fg_buf *b, const char *s, size_t n) {
    if (b->failed || n > SIZE_MAX - b->len - 1) {
        b->failed = true;
        return false;
    }
    size_t need = b->len + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *p = realloc(b->data, cap);
        if (!p) {
            b->failed = true;
            return false;
        }
        b->data = p;
        b->cap = cap;
    }
    if (n)
        memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}
bool fg_buf_puts(fg_buf *b, const char *s) {
    return fg_buf_add(b, s, strlen(s));
}
bool fg_buf_printf(fg_buf *b, const char *fmt, ...) {
    va_list a, c;
    va_start(a, fmt);
    va_copy(c, a);
    int n = vsnprintf(NULL, 0, fmt, c);
    va_end(c);
    if (n < 0) {
        va_end(a);
        b->failed = true;
        return false;
    }
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) {
        va_end(a);
        b->failed = true;
        return false;
    }
    vsnprintf(tmp, (size_t)n + 1, fmt, a);
    va_end(a);
    bool ok = fg_buf_add(b, tmp, (size_t)n);
    free(tmp);
    return ok;
}
char *fg_buf_take(fg_buf *b) {
    if (b->failed) {
        fg_buf_clear(b);
        return NULL;
    }
    char *p = b->data ? b->data : fg_strdup("");
    memset(b, 0, sizeof(*b));
    return p;
}
void fg_buf_clear(fg_buf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}
uint64_t fg_hash(const void *data, size_t n) {
    const unsigned char *p = data;
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}
uint64_t fg_now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}
bool fg_random_hex(char *out, size_t bytes) {
    unsigned char raw[32];
    if (bytes > sizeof(raw))
        return false;
#ifdef _WIN32
    if (BCryptGenRandom(NULL, raw, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, raw, bytes);
    close(fd);
    if (n != (ssize_t)bytes)
        return false;
#endif
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[bytes * 2] = 0;
    return true;
}
bool fg_path_join(char out[FG_PATH_MAX], const char *a, const char *b) {
    int n = snprintf(out, FG_PATH_MAX, "%s/%s", a, b);
    return n >= 0 && n < FG_PATH_MAX;
}
bool fg_workspace(const char *path, char out[FG_PATH_MAX], forge_error *e) {
#ifdef _WIN32
    if (!path || !_fullpath(out, path, FG_PATH_MAX)) {
        fg_error(e, FORGE_ERR_IO, "Cannot resolve workspace");
        return false;
    }
    DWORD attr = GetFileAttributesA(out);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
#else
    struct stat st;
    if (!path || !realpath(path, out) || stat(out, &st) != 0 || !S_ISDIR(st.st_mode)) {
#endif
        fg_error(e, FORGE_ERR_IO, "Workspace must be an existing directory");
        return false;
    }
    size_t n = strlen(out);
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\'))
        out[--n] = 0;
    return true;
}
static bool reserved(const char *p) {
    if (!strcmp(p, ".git") || !strcmp(p, ".forge") || !strcmp(p, "..") || !strcmp(p, "."))
        return true;
    char lower[256];
    size_t n = strlen(p);
    if (!n || n >= sizeof(lower) || p[n - 1] == '.' || p[n - 1] == ' ')
        return true;
    for (size_t i = 0; i <= n; i++)
        lower[i] = (char)tolower((unsigned char)p[i]);
    if (!strcmp(lower, ".git") || !strcmp(lower, ".forge"))
        return true;
    char *dot = strchr(lower, '.');
    if (dot)
        *dot = 0;
    return !strcmp(lower, "con") || !strcmp(lower, "prn") || !strcmp(lower, "aux") ||
           !strcmp(lower, "nul") ||
           ((!strncmp(lower, "com", 3) || !strncmp(lower, "lpt", 3)) && lower[3] >= '0' &&
            lower[3] <= '9' && !lower[4]);
}
bool fg_safe_path(const char *root, const char *relative, bool missing, char out[FG_PATH_MAX],
                  forge_error *e) {
    if (!relative || !*relative || relative[0] == '/' || relative[0] == '\\' ||
        strlen(relative) >= FG_PATH_MAX / 2)
        goto deny;
    for (const unsigned char *p = (const unsigned char *)relative; *p; p++)
        if (*p < 32 || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' ||
            *p == '|')
            goto deny;
    if (strlen(root) >= FG_PATH_MAX)
        goto deny;
    strcpy(out, root);
    const char *p = relative;
    while (*p) {
        char part[256];
        size_t n = 0;
        while (*p && *p != '/' && *p != '\\') {
            if (n + 1 >= sizeof(part))
                goto deny;
            part[n++] = *p++;
        }
        part[n] = 0;
        if (reserved(part))
            goto deny;
        if (*p)
            p++;
        if (!*p &&
            (relative[strlen(relative) - 1] == '/' || relative[strlen(relative) - 1] == '\\'))
            goto deny;
        size_t len = strlen(out);
        if (len + n + 2 > FG_PATH_MAX)
            goto deny;
        out[len] = '/';
        memcpy(out + len + 1, part, n + 1);
#ifdef _WIN32
        DWORD attr = GetFileAttributesA(out);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            if (!*p && missing && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND))
                return true;
            fg_error(e, FORGE_ERR_NOT_FOUND, "Path does not exist: %s", relative);
            return false;
        }
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
            goto deny;
        if (*p && !(attr & FILE_ATTRIBUTE_DIRECTORY))
            goto deny;
#else
        struct stat st;
        if (lstat(out, &st) != 0) {
            if (!*p && missing && errno == ENOENT)
                return true;
            fg_error(e, FORGE_ERR_NOT_FOUND, "Path does not exist: %s", relative);
            return false;
        }
        if (S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)))
            goto deny;
        if (*p && !S_ISDIR(st.st_mode))
            goto deny;
        if (missing && !*p && st.st_nlink > 1)
            goto deny;
#endif
    }
    return true;
deny:
    fg_error(e, FORGE_ERR_POLICY,
             "Unsafe workspace path (absolute, traversal, protected, link, or special file)");
    return false;
}
char *fg_read_file(const char *path, size_t cap, size_t *length, forge_error *e) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fg_error(e, FORGE_ERR_IO, "Cannot read %s", path);
        return NULL;
    }
    fg_buf b = {0};
    char block[8192];
    size_t n;
    while ((n = fread(block, 1, sizeof(block), f)) > 0) {
        if (n > cap - b.len) {
            fclose(f);
            fg_buf_clear(&b);
            fg_error(e, FORGE_ERR_LIMIT, "File exceeds %zu bytes", cap);
            return NULL;
        }
        if (!fg_buf_add(&b, block, n)) {
            fclose(f);
            fg_buf_clear(&b);
            fg_error(e, FORGE_ERR_MEMORY, "Read allocation failed");
            return NULL;
        }
    }
    if (ferror(f)) {
        fclose(f);
        fg_buf_clear(&b);
        fg_error(e, FORGE_ERR_IO, "File read failed");
        return NULL;
    }
    fclose(f);
    if (length)
        *length = b.len;
    return fg_buf_take(&b);
}
bool fg_write_file(const char *path, const char *data, size_t n, forge_error *e) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fg_error(e, FORGE_ERR_IO, "Cannot write %s", path);
        return false;
    }
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        fg_error(e, FORGE_ERR_IO, "Write failed for %s", path);
    return ok;
}
bool fg_mkdir(const char *path, forge_error *e) {
#ifdef _WIN32
    if (_mkdir(path) == 0)
        return true;
    DWORD a = GetFileAttributesA(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
        !(a & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#else
    if (mkdir(path, 0700) == 0)
        return true;
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return true;
#endif
    fg_error(e, FORGE_ERR_IO, "Cannot create safe directory %s", path);
    return false;
}
bool fg_regular_target(const char *path, forge_error *e) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return true;
    } else if (!(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        bool ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &info) &&
                  info.nNumberOfLinks == 1;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (ok)
            return true;
    }
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT)
            return true;
    } else if (S_ISREG(st.st_mode) && st.st_nlink == 1)
        return true;
#endif
    fg_error(e, FORGE_ERR_POLICY, "Refusing linked or special metadata file: %s", path);
    return false;
}
static bool skip_dir(const char *s) {
    return !strcmp(s, ".") || !strcmp(s, "..") || s[0] == '.' || !strcmp(s, "node_modules") ||
           !strcmp(s, "vendor") || !strcmp(s, "target") || !strncmp(s, "build", 5) ||
           !strcmp(s, "dist") || !strcmp(s, "__pycache__");
}
bool fg_walk(const char *root, const char *rel, fg_walk_fn cb, void *user, forge_error *e) {
    char dir[FG_PATH_MAX];
    if (*rel) {
        if (!fg_path_join(dir, root, rel))
            return false;
    } else
        strcpy(dir, root);
#ifdef _WIN32
    char pattern[FG_PATH_MAX];
    if (!fg_path_join(pattern, dir, "*"))
        return false;
    WIN32_FIND_DATAA info;
    HANDLE h = FindFirstFileA(pattern, &info);
    if (h == INVALID_HANDLE_VALUE)
        return true;
    bool ok = true;
    do {
        const char *name = info.cFileName;
        if (skip_dir(name) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        char next[FG_PATH_MAX];
        if (*rel) {
            if (!fg_path_join(next, rel, name)) {
                ok = false;
                break;
            }
        } else
            strcpy(next, name);
        if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = fg_walk(root, next, cb, user, e);
        else
            ok = cb(next, user);
        if (!ok)
            break;
    } while (FindNextFileA(h, &info));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(dir);
    if (!d) {
        fg_error(e, FORGE_ERR_IO, "Cannot enumerate %s", dir);
        return false;
    }
    struct dirent *ent;
    bool ok = true;
    while ((ent = readdir(d))) {
        if (skip_dir(ent->d_name))
            continue;
        char next[FG_PATH_MAX], full[FG_PATH_MAX];
        if (*rel) {
            if (!fg_path_join(next, rel, ent->d_name)) {
                ok = false;
                break;
            }
        } else
            strcpy(next, ent->d_name);
        if (!fg_path_join(full, root, next)) {
            ok = false;
            break;
        }
        struct stat st;
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode))
            continue;
        if (S_ISDIR(st.st_mode))
            ok = fg_walk(root, next, cb, user, e);
        else if (S_ISREG(st.st_mode))
            ok = cb(next, user);
        if (!ok)
            break;
    }
    closedir(d);
    return ok;
#endif
}
char *fg_json_string(const char *s) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_doc_set_root(d, yyjson_mut_str(d, s ? s : ""));
    char *out = yyjson_mut_write(d, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
    yyjson_mut_doc_free(d);
    return out;
}
const char *fg_json_str(yyjson_val *obj, const char *key) {
    return yyjson_get_str(yyjson_obj_get(obj, key));
}
bool fg_json_uint(yyjson_val *obj, const char *key, size_t *out, size_t fallback) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    if (!v) {
        *out = fallback;
        return true;
    }
    if (!yyjson_is_uint(v) || yyjson_get_uint(v) > SIZE_MAX)
        return false;
    *out = (size_t)yyjson_get_uint(v);
    return true;
}
