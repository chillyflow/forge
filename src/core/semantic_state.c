#include "internal.h"
#include "input_snapshot.h"
#include "semantic_state.h"
#include <ctype.h>

#define SEM_SOURCE_LIMIT (4u * 1024u * 1024u)
#define SEM_OFFSET UINT64_C(14695981039346656037)
#define SEM_PRIME UINT64_C(1099511628211)
#define SEM_DIAGNOSTIC_LIMIT 4096u
#define SEM_FIELD_LIMIT (64u * 1024u)

typedef enum { SEM_RAW, SEM_GO, SEM_PYTHON, SEM_C, SEM_RUST, SEM_TS } sem_language;
typedef struct {
    char *path;
    uint64_t hash, units;
    bool canonical;
} sem_file;
struct fg_semantic_state {
    sem_file *files;
    size_t count, capacity, canonical_files;
    uint64_t hash;
};
typedef struct {
    fg_semantic_state *state;
    fg_buf source;
    uint64_t raw_hash, raw_length, deadline;
    size_t max_files;
    forge_cancel_fn cancelled;
    void *user;
    forge_error *error;
    bool buffering, stopped;
} sem_scan;

static uint64_t sem_bytes(uint64_t hash, const void *bytes, size_t length) {
    const unsigned char *data = bytes;
    for (size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= SEM_PRIME;
    }
    return hash;
}
static uint64_t sem_uint(uint64_t hash, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(value & 255u);
        value >>= 8;
    }
    return sem_bytes(hash, bytes, sizeof(bytes));
}
static uint64_t sem_frame(uint64_t hash, unsigned char kind, const void *bytes, size_t length) {
    hash = sem_bytes(hash, &kind, 1);
    return sem_bytes(sem_uint(hash, length), bytes, length);
}
static bool sem_check(sem_scan *scan) {
    if (scan->stopped)
        return false;
    if (scan->cancelled && scan->cancelled(scan->user)) {
        scan->stopped = true;
        fg_error(scan->error, FORGE_ERR_CANCELLED, "Semantic state scan cancelled");
        return false;
    }
    if (scan->deadline && fg_now_ms() >= scan->deadline) {
        scan->stopped = true;
        fg_error(scan->error, FORGE_ERR_LIMIT, "Semantic state scan deadline reached");
        return false;
    }
    return true;
}
static sem_language sem_language_for(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension)
        return SEM_RAW;
    if (!strcmp(extension, ".go"))
        return SEM_GO;
    if (!strcmp(extension, ".py") || !strcmp(extension, ".pyi"))
        return SEM_PYTHON;
    if (!strcmp(extension, ".rs"))
        return SEM_RUST;
    if (!strcmp(extension, ".ts") || !strcmp(extension, ".js"))
        return SEM_TS;
    static const char *const extensions[] = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
        if (!strcmp(extension, extensions[i]))
            return SEM_C;
    return SEM_RAW;
}
static bool sem_word(unsigned char c) {
    return isalnum(c) || c == '_' || c == '$' || c >= 128;
}
static bool sem_special_comment(const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    if (!n)
        return false;
    if (strchr("!@#+*", *s))
        return true;
    /* Line directives begin the comment. "same line" is ordinary prose. */
    if ((n >= 3 && !memcmp(s, "go:", 3)) || (n >= 5 && !memcmp(s, "line ", 5)))
        return true;
    static const char *const tags[] = {
        "coding",   "type:", "noqa",      "pylint",           "pyright", "mypy",     "eslint",
        "istanbul", "c8 ",   "sourceURL", "sourceMappingURL", "region",  "endregion"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++) {
        size_t length = strlen(tags[i]);
        for (size_t j = 0; j + length <= n; j++)
            if (!memcmp(s + j, tags[i], length))
                return true;
    }
    return false;
}

/* A deliberately conservative token fingerprint. It is not a language parser:
 * difficult syntax falls back to exact file bytes, never a partial token list.
 * Newlines remain explicit for Go semicolon insertion and JS/TS ASI. Python
 * indentation remains exact, including tabs; docstrings are ordinary literals.
 * C/Rust gaps remain explicit because macros can observe token spacing. */
static bool sem_canonicalize(sem_scan *scan, sem_language language, const char *s, size_t n,
                             uint64_t *hash_out, uint64_t *units_out) {
    if (language == SEM_RAW || memchr(s, 0, n) || !fg_utf8_valid(s, n))
        return false;
    uint64_t hash = SEM_OFFSET, units = 0;
    size_t i = 0, indent_start = 0, indent_length = 0;
    bool line_start = true, newline = false, gap = false;
    const bool preserve_gap = language == SEM_C || language == SEM_RUST;
    while (i < n) {
        if (!sem_check(scan))
            return false;
        unsigned char c = (unsigned char)s[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < n && s[i + 1] == '\n')
                i++;
            i++;
            newline = units != 0;
            line_start = true;
            indent_start = i;
            indent_length = 0;
            gap = false;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (line_start)
                indent_length++;
            else
                gap = true;
            i++;
            continue;
        }
        /* Python formfeed and explicit continuation, C splicing/trigraphs and
         * directives are not safely normalized by this lexical recognizer. */
        if (c < 32 || c == '\\' || (language == SEM_C && (c == '#' || c == '?')))
            return false;
        bool python_comment = language == SEM_PYTHON && c == '#';
        bool slash_comment = language != SEM_PYTHON && c == '/' && i + 1 < n && s[i + 1] == '/';
        bool block_comment = language != SEM_PYTHON && c == '/' && i + 1 < n && s[i + 1] == '*';
        if (python_comment || slash_comment || block_comment) {
            size_t start = i + (python_comment ? 1u : 2u), end = start;
            if (language == SEM_RUST && slash_comment && start < n && s[start] == '/')
                return false;
            if (block_comment) {
                while (end + 1 < n && !(s[end] == '*' && s[end + 1] == '/')) {
                    if ((end & 4095u) == 0 && !sem_check(scan))
                        return false;
                    /* Rust supports nested block comments; preserve such files
                     * rather than guessing an outer terminator. */
                    if (s[end] == '/' && s[end + 1] == '*')
                        return false;
                    end++;
                }
                if (end + 1 >= n)
                    return false;
            } else {
                while (end < n && s[end] != '\n' && s[end] != '\r') {
                    if ((end & 4095u) == 0 && !sem_check(scan))
                        return false;
                    end++;
                }
            }
            if (sem_special_comment(s + start, end - start))
                return false;
            if (block_comment) {
                for (size_t j = start; j < end; j++)
                    if (s[j] == '\n' || s[j] == '\r') {
                        newline = units != 0;
                        line_start = true;
                        indent_start = end + 2;
                        indent_length = 0;
                    }
                i = end + 2;
            } else
                i = end;
            gap = true;
            continue;
        }
        /* JSX/TSX use raw fallback by extension. Slash may begin a regex;
         * templates and Rust lifetimes/raw strings require a real parser. */
        if ((language == SEM_TS && (c == '/' || c == '`')) || (language == SEM_RUST && c == '\''))
            return false;
        size_t start = i;
        if (c == '\'' || c == '"' || (language == SEM_GO && c == '`')) {
            bool triple =
                language == SEM_PYTHON && i + 2 < n && s[i + 1] == (char)c && s[i + 2] == (char)c;
            if (i && sem_word((unsigned char)s[i - 1])) {
                /* Prefixes include Python f-strings, C++ raw strings and Rust
                 * raw/byte strings. Keep all prefixed literals exact. */
                return false;
            }
            size_t opening = triple ? 3u : 1u;
            i += opening;
            bool closed = false;
            while (i < n) {
                if ((i & 4095u) == 0 && !sem_check(scan))
                    return false;
                if (s[i] == (char)c &&
                    (!triple || (i + 2 < n && s[i + 1] == (char)c && s[i + 2] == (char)c))) {
                    i += opening;
                    closed = true;
                    break;
                }
                if (c != '`' && s[i] == '\\') {
                    if (i + 1 >= n)
                        return false;
                    i += 2;
                } else {
                    if (!triple && c != '`' && (s[i] == '\r' || s[i] == '\n'))
                        return false;
                    i++;
                }
            }
            if (!closed)
                return false;
        } else if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            i++;
            while (i < n) {
                unsigned char next = (unsigned char)s[i];
                if (sem_word(next) || next == '.')
                    i++;
                else if ((next == '+' || next == '-') && i > start && strchr("eEpP", s[i - 1]))
                    i++;
                else
                    break;
            }
        } else if (sem_word(c)) {
            i++;
            while (i < n && sem_word((unsigned char)s[i]))
                i++;
            if (language == SEM_C && i - start == 8 && !memcmp(s + start, "__LINE__", 8))
                return false;
            if (language == SEM_RUST && i < n && s[i] == '#' && i - start <= 2)
                return false;
        } else {
            /* Maximal punctuation runs retain distinctions such as ++ versus
             * + + without claiming every possible formatting edit equivalent. */
            i++;
            while (i < n && strchr("+-*=<>!&|:^%~?", s[i]) && strchr("+-*=<>!&|:^%~?", s[start]))
                i++;
        }
        if (newline) {
            hash = sem_frame(hash, 'N', NULL, 0);
            units++;
        } else if (gap && preserve_gap && !line_start) {
            hash = sem_frame(hash, 'G', NULL, 0);
            units++;
        }
        if (line_start && language == SEM_PYTHON) {
            hash = sem_frame(hash, 'I', s + indent_start, indent_length);
            units++;
        }
        hash = sem_frame(hash, 'T', s + start, i - start);
        units++;
        newline = gap = line_start = false;
    }
    *hash_out = hash;
    *units_out = units;
    return sem_check(scan);
}

static bool sem_visit(const char *path, const void *bytes, size_t length, bool final, void *user,
                      forge_error *error) {
    sem_scan *scan = user;
    if (!sem_check(scan))
        return false;
    if (!final) {
        if (scan->raw_length == 0)
            scan->buffering = sem_language_for(path) != SEM_RAW;
        scan->raw_hash = sem_bytes(scan->raw_hash, bytes, length);
        scan->raw_length += length;
        if (scan->buffering && length > SEM_SOURCE_LIMIT - scan->source.len) {
            fg_buf_clear(&scan->source);
            scan->buffering = false;
        }
        if (scan->buffering && !fg_buf_add(&scan->source, bytes, length)) {
            fg_error(error, FORGE_ERR_MEMORY, "Semantic source buffer allocation failed");
            return false;
        }
        return true;
    }
    fg_semantic_state *state = scan->state;
    if (state->count == state->capacity) {
        size_t next = state->capacity ? state->capacity * 2 : 16;
        if (next > scan->max_files)
            next = scan->max_files;
        if (next <= state->capacity || next > SIZE_MAX / sizeof(*state->files)) {
            fg_error(error, FORGE_ERR_LIMIT, "Semantic file record limit exceeded");
            return false;
        }
        sem_file *files = realloc(state->files, next * sizeof(*files));
        if (!files) {
            fg_error(error, FORGE_ERR_MEMORY, "Semantic file allocation failed");
            return false;
        }
        state->files = files;
        state->capacity = next;
    }
    sem_file file = {0};
    file.path = fg_strdup(path);
    if (!file.path) {
        fg_error(error, FORGE_ERR_MEMORY, "Semantic path allocation failed");
        return false;
    }
    file.hash = scan->raw_hash;
    file.units = scan->raw_length;
    if (scan->buffering || scan->raw_length == 0)
        file.canonical = sem_canonicalize(scan, sem_language_for(path),
                                          scan->source.data ? scan->source.data : "",
                                          scan->source.len, &file.hash, &file.units);
    if (!sem_check(scan)) {
        free(file.path);
        return false;
    }
    if (file.canonical)
        state->canonical_files++;
    state->files[state->count++] = file;
    scan->raw_hash = SEM_OFFSET;
    scan->raw_length = 0;
    scan->buffering = false;
    fg_buf_clear(&scan->source);
    return true;
}
static int sem_compare(const void *left, const void *right) {
    const sem_file *a = left, *b = right;
    return strcmp(a->path, b->path);
}

fg_semantic_state *fg_semantic_state_take(const char *root, size_t max_files, uint64_t max_bytes,
                                          forge_cancel_fn cancel, void *user,
                                          uint64_t absolute_deadline, forge_error *error) {
    if (!root || !*root || !max_files || !max_bytes || max_files > SIZE_MAX / sizeof(sem_file)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Semantic state requires bounded nonzero limits");
        return NULL;
    }
    fg_semantic_state *state = calloc(1, sizeof(*state));
    if (!state) {
        fg_error(error, FORGE_ERR_MEMORY, "Semantic state allocation failed");
        return NULL;
    }
    sem_scan scan = {.state = state,
                     .raw_hash = SEM_OFFSET,
                     .deadline = absolute_deadline,
                     .max_files = max_files,
                     .cancelled = cancel,
                     .user = user,
                     .error = error};
    fg_input_snapshot *before = fg_input_snapshot_take_visit(
        root, max_files, max_bytes, cancel, user, absolute_deadline, sem_visit, &scan, error);
    fg_input_snapshot *after = before ? fg_input_snapshot_take(root, max_files, max_bytes, cancel,
                                                               user, absolute_deadline, error)
                                      : NULL;
    bool ok = before && after;
    if (ok && !fg_input_snapshot_equal(before, after)) {
        fg_error(error, FORGE_ERR_CONFLICT, "Workspace changed during semantic state scan");
        ok = false;
    }
    fg_input_snapshot_destroy(before);
    fg_input_snapshot_destroy(after);
    fg_buf_clear(&scan.source);
    if (!ok) {
        fg_semantic_state_destroy(state);
        return NULL;
    }
    if (state->count > 1)
        qsort(state->files, state->count, sizeof(*state->files), sem_compare);
    uint64_t hash = sem_uint(sem_bytes(SEM_OFFSET, "forge-semantic-state-v1", 23), state->count);
    for (size_t i = 0; i < state->count; i++) {
        if (!sem_check(&scan)) {
            fg_semantic_state_destroy(state);
            return NULL;
        }
        const sem_file *file = &state->files[i];
        hash = sem_frame(hash, file->canonical ? 'C' : 'R', file->path, strlen(file->path));
        hash = sem_uint(sem_uint(hash, file->units), file->hash);
    }
    state->hash = hash;
    return state;
}
bool fg_semantic_state_equal(const fg_semantic_state *a, const fg_semantic_state *b) {
    if (!a || !b || a->hash != b->hash || a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; i++) {
        const sem_file *left = &a->files[i], *right = &b->files[i];
        if (left->hash != right->hash || left->units != right->units ||
            left->canonical != right->canonical || strcmp(left->path, right->path))
            return false;
    }
    return true;
}
uint64_t fg_semantic_state_hash(const fg_semantic_state *state) {
    return state ? state->hash : 0;
}
fg_semantic_state_info fg_semantic_state_describe(const fg_semantic_state *state) {
    fg_semantic_state_info info = {0};
    if (state) {
        info.files = state->count;
        info.canonical_files = state->canonical_files;
        info.raw_files = state->count - state->canonical_files;
        info.complete = true;
    }
    return info;
}
void fg_semantic_state_destroy(fg_semantic_state *state) {
    if (state) {
        for (size_t i = 0; i < state->count; i++)
            free(state->files[i].path);
        free(state->files);
        free(state);
    }
}

static bool sem_field(uint64_t *hash, const char *field, bool path, bool normalize,
                      forge_error *error) {
    if (!field)
        field = "";
    size_t length = 0;
    while (length <= SEM_FIELD_LIMIT && field[length])
        length++;
    if (length > SEM_FIELD_LIMIT || !fg_utf8_valid(field, length)) {
        fg_error(error, FORGE_ERR_LIMIT, "Diagnostic field is invalid or exceeds 64 KiB");
        return false;
    }
    /* Quotes may contain whitespace-sensitive values (expected strings, paths,
     * symbols). Keep such messages exact rather than rephrase their payload. */
    if (memchr(field, '\'', length) || memchr(field, '"', length))
        normalize = false;
    uint64_t normalized = SEM_OFFSET, count = 0;
    bool pending = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)field[i];
        if (normalize && isspace(c)) {
            pending = count != 0;
            continue;
        }
        if (pending) {
            normalized = sem_bytes(normalized, " ", 1);
            count++;
            pending = false;
        }
        if (path && c == '\\')
            c = '/';
        normalized = sem_bytes(normalized, &c, 1);
        count++;
    }
    *hash = sem_uint(sem_uint(*hash, count), normalized);
    return true;
}
static int sem_hash_compare(const void *a, const void *b) {
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return left < right ? -1 : left > right;
}
bool fg_semantic_diagnostic_fingerprint(const fg_semantic_diagnostic *diagnostics, size_t count,
                                        bool complete, uint64_t *hash, forge_error *error) {
    if (hash)
        *hash = 0;
    if (!hash || !complete || !count || !diagnostics || count > SEM_DIAGNOSTIC_LIMIT) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Diagnostic evidence must be complete and nonempty");
        return false;
    }
    uint64_t *entries = malloc(count * sizeof(*entries));
    if (!entries) {
        fg_error(error, FORGE_ERR_MEMORY, "Diagnostic fingerprint allocation failed");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        const fg_semantic_diagnostic *d = &diagnostics[i];
        uint64_t h = SEM_OFFSET;
        ok = sem_field(&h, d->tool, false, false, error) &&
             sem_field(&h, d->severity, false, false, error) &&
             sem_field(&h, d->path, true, false, error) &&
             sem_field(&h, d->code, false, false, error) &&
             sem_field(&h, d->symbol, false, false, error) &&
             sem_field(&h, d->identity, false, false, error);
        bool identity = d->identity && *d->identity;
        if (ok && !identity)
            ok = sem_field(&h, d->message, false, true, error);
        if (ok && !identity && (!d->message || !*d->message)) {
            fg_error(error, FORGE_ERR_ARGUMENT, "Diagnostic requires a message or stable identity");
            ok = false;
        }
        entries[i] = sem_uint(h, identity);
    }
    if (ok) {
        qsort(entries, count, sizeof(*entries), sem_hash_compare);
        uint64_t h = sem_uint(sem_bytes(SEM_OFFSET, "forge-semantic-diagnostic-v1", 28), count);
        for (size_t i = 0; i < count; i++)
            h = sem_uint(h, entries[i]);
        *hash = h;
    }
    free(entries);
    return ok;
}
