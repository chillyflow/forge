#include "graph.h"
#include <ctype.h>

#define GG_NONE FG_GO_GRAPH_NONE
#define GG_MAX_MODULES FG_GO_GRAPH_MAX_MODULES
#define GG_MAX_PACKAGES FG_GO_GRAPH_MAX_PACKAGES
#define GG_MAX_EDGES FG_GO_GRAPH_MAX_EDGES
#define GG_MAX_TEXT_BYTES ((size_t)16 * 1024 * 1024)

struct fg_go_graph {
    fg_repo_snapshot *snapshot;
    forge_repo *repo;
    forge_error *error;
    uint64_t generation;
    fg_go_module *modules;
    fg_go_package *packages;
    fg_go_edge *edges;
    fg_go_reason reasons[32];
    size_t module_count, package_count, edge_count, reason_count;
    size_t module_cap, package_cap, edge_cap, text_bytes;
    bool failed, applicable;
};
typedef struct {
    const char *path;
    size_t package;
} gg_import_key;

static bool gg_stopped(fg_go_graph *g) {
    if (g->failed)
        return true;
    if (fg_repo_snapshot_stopped(g->snapshot)) {
        g->failed = true;
        return true;
    }
    return false;
}
static bool gg_fail(fg_go_graph *g, forge_status status, const char *message) {
    if (!g->failed)
        fg_error(g->error, status, "%s", message);
    g->failed = true;
    return false;
}

static bool gg_reserve(fg_go_graph *g, void **data, size_t *cap, size_t count, size_t element,
                       size_t limit) {
    if (count > limit)
        return gg_fail(g, FORGE_ERR_LIMIT, "Go import graph exceeds its documented limit");
    if (count <= *cap)
        return true;
    if (gg_stopped(g))
        return false;
    size_t next = *cap ? *cap * 2 : 16;
    next = FG_MIN(next, limit);
    void *p = realloc(*data, next * element);
    if (!p)
        return gg_fail(g, FORGE_ERR_MEMORY, "Go graph allocation failed");
    *data = p;
    *cap = next;
    return true;
}

static char *gg_copy(fg_go_graph *g, const char *s) {
    size_t length = strlen(s) + 1;
    if (length > GG_MAX_TEXT_BYTES - g->text_bytes) {
        gg_fail(g, FORGE_ERR_LIMIT, "Go graph copied text exceeds 16 MiB");
        return NULL;
    }
    char *copy = fg_strdup(s);
    if (copy)
        g->text_bytes += length;
    if (!copy)
        gg_fail(g, FORGE_ERR_MEMORY, "Go graph allocation failed");
    return copy;
}

static char *gg_adopt(fg_go_graph *g, char *text) {
    if (!text) {
        gg_fail(g, FORGE_ERR_MEMORY, "Go graph allocation failed");
        return NULL;
    }
    size_t bytes = strlen(text) + 1;
    if (bytes > GG_MAX_TEXT_BYTES - g->text_bytes) {
        free(text);
        gg_fail(g, FORGE_ERR_LIMIT, "Go graph copied text exceeds 16 MiB");
        return NULL;
    }
    g->text_bytes += bytes;
    return text;
}

static const char *gg_text(fg_go_graph *g, sqlite3_stmt *s, int column, size_t limit,
                           bool nullable) {
    if (nullable && sqlite3_column_type(s, column) == SQLITE_NULL)
        return NULL;
    if (sqlite3_column_type(s, column) != SQLITE_TEXT) {
        gg_fail(g, FORGE_ERR_PARSE, "Go graph contains a nontext indexed value");
        return NULL;
    }
    const char *text = (const char *)sqlite3_column_text(s, column);
    int bytes = sqlite3_column_bytes(s, column);
    if (!text || sqlite3_column_type(s, column) != SQLITE_TEXT || bytes < 0 ||
        memchr(text, 0, (size_t)bytes) || !fg_utf8_valid(text, (size_t)bytes)) {
        gg_fail(g, FORGE_ERR_PARSE, "Go graph contains malformed indexed UTF-8 text");
        return NULL;
    }
    if ((size_t)bytes > limit) {
        gg_fail(g, FORGE_ERR_LIMIT, "Go graph indexed text exceeds its size limit");
        return NULL;
    }
    return text;
}

static const char *gg_file(fg_go_graph *g, sqlite3_stmt *s, int column) {
    const char *text = gg_text(g, s, column, FG_PATH_MAX - 1, false);
    char canonical[FG_PATH_MAX];
    if (text && (!fg_relative_path(text, canonical, NULL) || strcmp(text, canonical))) {
        gg_fail(g, FORGE_ERR_PARSE, "Go graph contains a noncanonical indexed path");
        return NULL;
    }
    return text;
}

static bool gg_reason_add(fg_go_graph *g, const char *code, const char *path, const char *detail) {
    if (gg_stopped(g))
        return false;
    for (size_t i = 0; i < g->reason_count; i++) {
        if (!strcmp(g->reasons[i].code, code)) {
            /* One deterministic example per category, not a diagnostic per file. */
            if (strcmp(path, g->reasons[i].path) < 0) {
                char *copy = gg_copy(g, path);
                if (!copy)
                    return false;
                free((void *)g->reasons[i].path);
                g->reasons[i].path = copy;
            }
            return true;
        }
    }
    if (g->reason_count == sizeof(g->reasons) / sizeof(g->reasons[0]))
        return gg_fail(g, FORGE_ERR_LIMIT, "Too many validation fallback categories");
    fg_go_reason *r = &g->reasons[g->reason_count++];
    r->code = code;
    r->detail = detail;
    r->path = gg_copy(g, path);
    return r->path != NULL;
}

static const char *gg_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool gg_suffix(const char *text, const char *suffix) {
    size_t n = strlen(text), z = strlen(suffix);
    return n >= z && !strcmp(text + n - z, suffix);
}

static bool gg_below(const char *directory, const char *path) {
    if (!strcmp(directory, "."))
        return true;
    size_t n = strlen(directory);
    return !strncmp(directory, path, n) && (!path[n] || path[n] == '/');
}

static void gg_directory(const char *path, char out[FG_PATH_MAX]) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        strcpy(out, ".");
    else {
        size_t n = (size_t)(slash - path);
        memcpy(out, path, n);
        out[n] = 0;
    }
}

static int gg_module_compare(const void *a, const void *b) {
    return strcmp(((const fg_go_module *)a)->directory, ((const fg_go_module *)b)->directory);
}

static int gg_reason_compare(const void *a, const void *b) {
    return strcmp(((const fg_go_reason *)a)->code, ((const fg_go_reason *)b)->code);
}

static int gg_edge_compare(const void *a, const void *b) {
    const fg_go_edge *x = a, *y = b;
    if (x->from != y->from)
        return x->from < y->from ? -1 : 1;
    if (x->to != y->to)
        return x->to < y->to ? -1 : 1;
    return (int)x->test_only - (int)y->test_only;
}

static sqlite3_stmt *gg_query(fg_go_graph *g, const char *query) {
    if (gg_stopped(g))
        return NULL;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(g->repo->db, query, -1, &s, NULL) != SQLITE_OK)
        gg_fail(g, FORGE_ERR_IO, "Cannot query Go graph index");
    return s;
}

static bool gg_query_done(fg_go_graph *g, sqlite3_stmt *s, int rc) {
    sqlite3_finalize(s);
    if (gg_stopped(g))
        return false;
    return rc == SQLITE_DONE || gg_fail(g, FORGE_ERR_IO, "Cannot read Go graph index");
}

static bool gg_word(const char *p, const char *end, const char *word) {
    size_t n = strlen(word);
    return (size_t)(end - p) >= n && !memcmp(p, word, n) &&
           (p + n == end || isspace((unsigned char)p[n]) || p[n] == '(');
}

static char *gg_module_path(fg_go_graph *g, const char *text, bool *replacement) {
    char *result = NULL;
    bool invalid = false;
    while (*text) {
        if (gg_stopped(g)) {
            free(result);
            return NULL;
        }
        const char *end = strchr(text, '\n');
        if (!end)
            end = text + strlen(text);
        const char *p = text;
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (gg_word(p, end, "replace"))
            *replacement = true;
        if (gg_word(p, end, "module")) {
            p += 6;
            while (p < end && isspace((unsigned char)*p))
                p++;
            char quote = p < end && (*p == '"' || *p == '`') ? *p++ : 0;
            const char *start = p;
            while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p)))
                p++;
            size_t n = (size_t)(p - start);
            if (result || !n || n >= FG_PATH_MAX / 2 || (quote && p == end))
                invalid = true;
            if (quote && p < end)
                p++;
            while (p < end && isspace((unsigned char)*p))
                p++;
            if (p < end && (end - p < 2 || p[0] != '/' || p[1] != '/'))
                invalid = true;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)start[i];
                if (!isalnum(c) && !strchr("/.-_~", (int)c))
                    invalid = true;
            }
            if (n && (start[0] == '/' || start[n - 1] == '/'))
                invalid = true;
            if (!invalid) {
                result = malloc(n + 1);
                if (!result) {
                    gg_fail(g, FORGE_ERR_MEMORY, "Module-path allocation failed");
                    return NULL;
                }
                memcpy(result, start, n);
                result[n] = 0;
                if (strstr(result, "//") || !strcmp(result, ".") || !strcmp(result, "..") ||
                    !strncmp(result, "./", 2) || !strncmp(result, "../", 3) ||
                    strstr(result, "/./") || strstr(result, "/../") || gg_suffix(result, "/.") ||
                    gg_suffix(result, "/.."))
                    invalid = true;
            }
        }
        text = *end ? end + 1 : end;
    }
    if (invalid) {
        free(result);
        result = NULL;
    }
    return result;
}

static bool gg_module_add(fg_go_graph *g, const char *directory, char *path, bool synthetic) {
    if (!gg_reserve(g, (void **)&g->modules, &g->module_cap, g->module_count + 1,
                    sizeof(*g->modules), GG_MAX_MODULES)) {
        free(path);
        return false;
    }
    fg_go_module *m = &g->modules[g->module_count++];
    memset(m, 0, sizeof(*m));
    m->directory = gg_copy(g, directory);
    m->path = path ? gg_adopt(g, path) : NULL;
    m->synthetic = synthetic;
    return !g->failed;
}

static bool gg_load_modules(fg_go_graph *g) {
    sqlite3_stmt *s = gg_query(g, "SELECT path,content FROM chunks WHERE path='go.mod' OR "
                                  "path LIKE '%/go.mod' OR path='go.work' OR "
                                  "path LIKE '%/go.work' UNION SELECT b.path,NULL FROM "
                                  "go_module_boundaries b WHERE NOT EXISTS "
                                  "(SELECT 1 FROM chunks c WHERE c.path=b.path) ORDER BY 1");
    if (!s)
        return false;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (gg_stopped(g))
            break;
        const char *file = gg_file(g, s, 0);
        const char *text = gg_text(g, s, 1, FORGE_INDEX_MAX_FILE_BYTES, true);
        if (g->failed)
            break;
        if (!strcmp(gg_base(file), "go.work")) {
            gg_reason_add(
                g, "go_workspace", file,
                "Workspace use/replace directives and modules outside the repository "
                "are not resolved; verify every indexed module in the active Go environment.");
            continue;
        }
        char directory[FG_PATH_MAX];
        gg_directory(file, directory);
        bool replacement = false;
        char *path = text ? gg_module_path(g, text, &replacement) : NULL;
        if (!path && !g->failed)
            gg_reason_add(
                g, "unresolved_module_path", file,
                "The module directive could not be resolved; import identities are incomplete.");
        if (replacement)
            gg_reason_add(g, "module_replacements", file,
                          "Module replacements may redirect imports; local replacement aliases and "
                          "external module contents are not resolved by the syntactic graph.");
        if (g->failed) {
            free(path);
            sqlite3_finalize(s);
            return false;
        }
        if (!gg_module_add(g, directory, path, false)) {
            sqlite3_finalize(s);
            return false;
        }
    }
    return gg_query_done(g, s, rc);
}

static size_t gg_package_find(fg_go_graph *g, const char *directory, size_t *position) {
    size_t lo = 0, hi = g->package_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(g->packages[mid].directory, directory);
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (position)
        *position = lo;
    return lo < g->package_count && !strcmp(g->packages[lo].directory, directory) ? lo : GG_NONE;
}

static size_t gg_package_add(fg_go_graph *g, const char *directory) {
    size_t position = 0, found = gg_package_find(g, directory, &position);
    if (found != GG_NONE)
        return found;
    if (!gg_reserve(g, (void **)&g->packages, &g->package_cap, g->package_count + 1,
                    sizeof(*g->packages), GG_MAX_PACKAGES))
        return GG_NONE;
    memmove(g->packages + position + 1, g->packages + position,
            (g->package_count - position) * sizeof(*g->packages));
    fg_go_package *p = &g->packages[position];
    memset(p, 0, sizeof(*p));
    p->directory = gg_copy(g, directory);
    p->module = p->from_head = p->to_head = GG_NONE;
    g->package_count++;
    return g->failed ? GG_NONE : position;
}

bool fg_go_graph_excluded_path(const char *path) {
    if (!path)
        return true;
    while (*path) {
        const char *end = strchr(path, '/');
        if (!end)
            return path[0] == '.' || path[0] == '_';
        size_t n = (size_t)(end - path);
        if (path[0] == '.' || path[0] == '_' || (n == 6 && !memcmp(path, "vendor", 6)) ||
            (n == 8 && !memcmp(path, "testdata", 8)))
            return true;
        path = end + 1;
    }
    return false;
}

static bool gg_platform_filename(const char *path) {
    /* Unknown future suffixes remain covered by the documented environment limitation. */
    static const char *const suffixes[] = {
        "aix",    "android",  "darwin", "dragonfly", "freebsd", "hurd",    "illumos", "ios",
        "js",     "linux",    "netbsd", "openbsd",   "plan9",   "solaris", "wasip1",  "windows",
        "zos",    "386",      "amd64",  "arm",       "arm64",   "loong64", "mips",    "mipsle",
        "mips64", "mips64le", "ppc64",  "ppc64le",   "riscv64", "s390x",   "wasm"};
    char name[FG_PATH_MAX];
    size_t n = strlen(gg_base(path));
    if (n < 3 || !gg_suffix(path, ".go"))
        return false;
    if (n >= sizeof(name))
        return true;
    memcpy(name, gg_base(path), n + 1);
    n -= gg_suffix(name, "_test.go") ? 8u : 3u;
    name[n] = 0;
    const char *suffix = strrchr(name, '_');
    if (!suffix)
        return false;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (!strcmp(suffix + 1, suffixes[i]))
            return true;
    return false;
}

static bool gg_load_packages(fg_go_graph *g) {
    sqlite3_stmt *s = gg_query(g, "SELECT f.path,g.package_name,g.is_test,g.build_constraints,"
                                  "g.parse_error FROM files f LEFT JOIN go_files g ON "
                                  "g.file_id=f.id WHERE f.language='go' ORDER BY f.path");
    if (!s)
        return false;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (gg_stopped(g))
            break;
        const char *file = gg_file(g, s, 0);
        if (g->failed)
            break;
        if (fg_go_graph_excluded_path(file))
            continue;
        if (!gg_suffix(file, ".go")) {
            gg_fail(g, FORGE_ERR_PARSE, "Indexed Go source has a non-Go filename");
            break;
        }
        for (int col = 2; col <= 4; col++)
            if (sqlite3_column_type(s, col) != SQLITE_NULL &&
                (sqlite3_column_type(s, col) != SQLITE_INTEGER ||
                 sqlite3_column_int64(s, col) < 0 || sqlite3_column_int64(s, col) > 1)) {
                gg_fail(g, FORGE_ERR_PARSE, "Go graph contains invalid indexed file flags");
                break;
            }
        if (g->failed)
            break;
        char directory[FG_PATH_MAX];
        gg_directory(file, directory);
        size_t index = gg_package_add(g, directory);
        if (index == GG_NONE) {
            sqlite3_finalize(s);
            return false;
        }
        fg_go_package *p = &g->packages[index];
        p->present = true;
        bool test = sqlite3_column_int(s, 2) != 0;
        p->tests |= test;
        const char *name = gg_text(g, s, 1, FORGE_INDEX_MAX_FILE_BYTES, true);
        if (g->failed)
            break;
        if (!test && name) {
            if (p->name && strcmp(p->name, name))
                gg_reason_add(g, "conflicting_package_names", directory,
                              "Files in one directory declare different package names; build "
                              "selection and type correctness require the Go toolchain.");
            if (!p->name)
                p->name = gg_copy(g, name);
        }
        if (sqlite3_column_type(s, 1) == SQLITE_NULL || sqlite3_column_int(s, 4))
            gg_reason_add(g, "go_parse_error", file,
                          "Missing or incomplete Go syntax metadata can omit imports; broad "
                          "verification is required.");
        if (sqlite3_column_int(s, 3) || gg_platform_filename(file))
            gg_reason_add(
                g, "build_constraints", file,
                "The graph unions imports across build constraints and platform suffixes; "
                "commands validate only the active GOOS/GOARCH/tags configuration.");
        if (g->failed) {
            sqlite3_finalize(s);
            return false;
        }
    }
    return gg_query_done(g, s, rc);
}

static size_t gg_module_for(const fg_go_graph *g, const char *directory) {
    size_t found = GG_NONE, length = 0;
    for (size_t i = 0; i < g->module_count; i++) {
        size_t n = !strcmp(g->modules[i].directory, ".") ? 0 : strlen(g->modules[i].directory);
        if (gg_below(g->modules[i].directory, directory) && (found == GG_NONE || n > length)) {
            found = i;
            length = n;
        }
    }
    return found;
}

static bool gg_assign_modules(fg_go_graph *g) {
    for (size_t i = 0; i < g->package_count; i++) {
        if (gg_stopped(g))
            return false;
        if (gg_module_for(g, g->packages[i].directory) == GG_NONE) {
            if (!gg_module_add(g, ".", NULL, true))
                return false;
            gg_reason_add(
                g, "missing_go_module", g->packages[i].directory,
                "Go source has no containing indexed go.mod; GOPATH and modules outside "
                "the workspace are not resolved. Root verification may require environment setup.");
            break;
        }
    }
    if (g->module_count > 1)
        qsort(g->modules, g->module_count, sizeof(*g->modules), gg_module_compare);
    for (size_t i = 0; i < g->package_count; i++) {
        if (gg_stopped(g))
            return false;
        fg_go_package *p = &g->packages[i];
        p->module = gg_module_for(g, p->directory);
        if (p->module == GG_NONE)
            return gg_fail(g, FORGE_ERR_PARSE, "Cannot assign Go package to a module boundary");
        fg_go_module *m = &g->modules[p->module];
        if (m->path) {
            const char *relative = !strcmp(m->directory, p->directory) ? ""
                                   : !strcmp(m->directory, ".")
                                       ? p->directory
                                       : p->directory + strlen(m->directory) + 1;
            fg_buf b = {0};
            if (!fg_buf_puts(&b, m->path) || (*relative && !fg_buf_printf(&b, "/%s", relative))) {
                fg_buf_clear(&b);
                return gg_fail(g, FORGE_ERR_MEMORY, "Go package import path allocation failed");
            }
            p->path = gg_adopt(g, fg_buf_take(&b));
            if (!p->path)
                return false;
        }
    }
    for (size_t i = 0; i < g->module_count; i++)
        for (size_t j = i + 1; j < g->module_count; j++)
            if (g->modules[i].path && g->modules[j].path &&
                !strcmp(g->modules[i].path, g->modules[j].path))
                gg_reason_add(g, "duplicate_module_path", g->modules[i].directory,
                              "Multiple module roots declare the same import path; retain all "
                              "matching dependency edges and verify each module separately.");
    return !g->failed;
}

static int gg_import_compare(const void *a, const void *b) {
    const gg_import_key *x = a, *y = b;
    int cmp = strcmp(x->path, y->path);
    if (cmp)
        return cmp;
    return x->package == y->package ? 0 : x->package < y->package ? -1 : 1;
}

static bool gg_edge_add(fg_go_graph *g, size_t from, size_t to, bool test) {
    if (!gg_reserve(g, (void **)&g->edges, &g->edge_cap, g->edge_count + 1, sizeof(*g->edges),
                    GG_MAX_EDGES))
        return false;
    fg_go_edge *e = &g->edges[g->edge_count++];
    e->from = from;
    e->to = to;
    e->test_only = test;
    e->next_from = e->next_to = GG_NONE;
    return true;
}

static bool gg_load_edges(fg_go_graph *g) {
    gg_import_key *keys = calloc(g->package_count ? g->package_count : 1, sizeof(*keys));
    if (!keys)
        return gg_fail(g, FORGE_ERR_MEMORY, "Import lookup allocation failed");
    size_t count = 0;
    for (size_t i = 0; i < g->package_count; i++) {
        if (g->packages[i].path) {
            keys[count].path = g->packages[i].path;
            keys[count++].package = i;
        }
    }
    qsort(keys, count, sizeof(*keys), gg_import_compare);
    sqlite3_stmt *s = gg_query(g, "SELECT f.path,i.path,g.is_test FROM imports i JOIN files f ON "
                                  "f.id=i.file_id LEFT JOIN go_files g ON g.file_id=f.id "
                                  "ORDER BY f.path,i.path");
    if (!s) {
        free(keys);
        return false;
    }
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (gg_stopped(g))
            break;
        const char *file = gg_file(g, s, 0);
        const char *import = gg_text(g, s, 1, FORGE_INDEX_MAX_FILE_BYTES, false);
        if (g->failed)
            break;
        if (fg_go_graph_excluded_path(file))
            continue;
        char directory[FG_PATH_MAX];
        gg_directory(file, directory);
        size_t from = gg_package_find(g, directory, NULL);
        if (from == GG_NONE)
            continue;
        if (!strcmp(import, "C")) {
            gg_reason_add(g, "cgo_import", file,
                          "Cgo dependencies, external headers, libraries, and toolchains are not "
                          "represented in the Go import graph.");
            continue;
        }
        if (strchr(import, '\\') || import[0] == '.') {
            gg_reason_add(g, "unresolved_import_syntax", file,
                          "Escaped or relative import paths are not resolved; broad verification "
                          "is required.");
            continue;
        }
        size_t lo = 0, hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (strcmp(keys[mid].path, import) < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        bool matched = false;
        for (size_t i = lo; i < count && !strcmp(keys[i].path, import); i++) {
            matched = true;
            if (!gg_edge_add(g, from, keys[i].package, sqlite3_column_int(s, 2) != 0))
                break;
            if (g->packages[from].module != g->packages[keys[i].package].module)
                gg_reason_add(g, "cross_module_import", file,
                              "An import matches another local module by its declared path; actual "
                              "version/workspace/replacement selection is delegated to Go.");
        }
        if (!matched) {
            for (size_t i = 0; i < g->module_count; i++) {
                if (g->modules[i].path && gg_below(g->modules[i].path, import)) {
                    gg_reason_add(g, "unresolved_local_import", file,
                                  "An import under an indexed module path has no indexed package; "
                                  "it may be missing, generated, excluded, or replaced.");
                    break;
                }
            }
        }
        if (g->failed) {
            sqlite3_finalize(s);
            free(keys);
            return false;
        }
    }
    free(keys);
    if (!gg_query_done(g, s, rc))
        return false;
    if (g->edge_count > 1)
        qsort(g->edges, g->edge_count, sizeof(*g->edges), gg_edge_compare);
    size_t used = 0;
    for (size_t i = 0; i < g->edge_count; i++) {
        if (used && g->edges[used - 1].from == g->edges[i].from &&
            g->edges[used - 1].to == g->edges[i].to)
            g->edges[used - 1].test_only &= g->edges[i].test_only;
        else
            g->edges[used++] = g->edges[i];
    }
    g->edge_count = used;
    return true;
}

static bool gg_extra_packages(fg_go_graph *g, const char *const *paths, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (gg_stopped(g))
            return false;
        char canonical[FG_PATH_MAX], directory[FG_PATH_MAX];
        if (!paths[i] || !fg_utf8_valid(paths[i], strlen(paths[i])) ||
            !fg_relative_path(paths[i], canonical, NULL) || strcmp(paths[i], canonical))
            return gg_fail(g, FORGE_ERR_ARGUMENT, "Go graph extra paths must be canonical files");
        if (!gg_suffix(canonical, ".go") || fg_go_graph_excluded_path(canonical))
            continue;
        gg_directory(canonical, directory);
        if (gg_package_find(g, directory, NULL) != GG_NONE)
            continue;
        if (gg_package_add(g, directory) == GG_NONE ||
            !gg_reason_add(g, "unindexed_or_deleted_package", canonical,
                           "This Go package has no indexed source; retain incoming imports and "
                           "validate dependents plus the full module suite."))
            return false;
    }
    return true;
}

static bool gg_adjacency(fg_go_graph *g) {
    size_t n = g->package_count ? g->package_count : 1;
    size_t *queue = malloc(n * sizeof(*queue));
    size_t *indegree = calloc(n, sizeof(*indegree));
    if (!queue || !indegree) {
        free(queue);
        free(indegree);
        return gg_fail(g, FORGE_ERR_MEMORY, "Dependency traversal allocation failed");
    }
    for (size_t i = g->edge_count; i > 0; i--) {
        fg_go_edge *edge = &g->edges[i - 1];
        edge->next_from = g->packages[edge->from].from_head;
        edge->next_to = g->packages[edge->to].to_head;
        g->packages[edge->from].from_head = i - 1;
        g->packages[edge->to].to_head = i - 1;
        if (edge->from != edge->to || !edge->test_only)
            indegree[edge->to]++;
    }
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < g->package_count; i++)
        if (!indegree[i])
            queue[tail++] = i;
    while (head < tail) {
        if (gg_stopped(g))
            break;
        size_t node = queue[head++];
        for (size_t i = g->packages[node].from_head; i != GG_NONE; i = g->edges[i].next_from) {
            const fg_go_edge *edge = &g->edges[i];
            if (edge->from == edge->to && edge->test_only)
                continue;
            if (!--indegree[edge->to])
                queue[tail++] = edge->to;
        }
    }
    if (!g->failed && tail != g->package_count) {
        for (size_t i = 0; i < g->package_count; i++) {
            if (indegree[i]) {
                gg_reason_add(
                    g, "possible_import_cycle", g->packages[i].directory,
                    "The union import graph contains a cycle. Test packages or mutually "
                    "exclusive build constraints may explain it; let Go determine validity.");
                break;
            }
        }
    }
    free(queue);
    free(indegree);
    return !gg_stopped(g);
}

fg_go_graph *fg_go_graph_load(fg_repo_snapshot *snapshot, const char *const *extra_files,
                              size_t count, forge_error *error) {
    if (!snapshot || !snapshot->internal || !snapshot->repo || (count && !extra_files)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "An active indexed snapshot is required for the graph");
        return NULL;
    }
    if (count > FG_GO_GRAPH_MAX_EXTRA_FILES) {
        fg_error(error, FORGE_ERR_LIMIT, "Go graph accepts at most 1024 extra file paths");
        return NULL;
    }
    fg_go_graph *g = calloc(1, sizeof(*g));
    if (!g) {
        fg_error(error, FORGE_ERR_MEMORY, "Go graph allocation failed");
        return NULL;
    }
    g->repo = snapshot->repo;
    g->snapshot = snapshot;
    g->error = error;
    g->generation = snapshot->generation;
    sqlite3_stmt *scan = gg_query(g, "SELECT value FROM meta WHERE key='scan'");
    if (!scan)
        goto fail;
    int scan_status = sqlite3_step(scan);
    bool indexed = scan_status == SQLITE_ROW && sqlite3_column_int64(scan, 0) > 0;
    sqlite3_finalize(scan);
    if (gg_stopped(g))
        goto fail;
    if (!indexed) {
        gg_fail(g, scan_status == SQLITE_ROW ? FORGE_ERR_CONFLICT : FORGE_ERR_IO,
                "Index the repository before requesting a Go graph");
        goto fail;
    }
    if (!gg_load_modules(g) || !gg_load_packages(g) || !gg_extra_packages(g, extra_files, count) ||
        !gg_assign_modules(g))
        goto fail;
    if (!g->module_count && !g->package_count && snapshot->go_index_incomplete &&
        !gg_module_add(g, ".", NULL, true))
        goto fail;
    g->applicable = g->module_count != 0 || g->package_count != 0;
    if (g->applicable) {
        if (snapshot->go_index_incomplete)
            gg_reason_add(
                g, "incomplete_go_index", "",
                "Some Go source or module metadata was unreadable, unsafe, binary, or too "
                "large to index; the graph cannot justify narrow verification alone.");
        if (snapshot->filesystem_scan)
            gg_reason_add(
                g, "filesystem_scan", "",
                "Git enumeration was unavailable; the fallback walker excludes hidden, "
                "vendor, build, and other generated directories, which may omit Go inputs.");
        if (g->failed || !gg_load_edges(g) || !gg_adjacency(g))
            goto fail;
    }
    if (gg_stopped(g))
        goto fail;
    if (g->reason_count > 1)
        qsort(g->reasons, g->reason_count, sizeof(*g->reasons), gg_reason_compare);
    g->repo = NULL;
    g->snapshot = NULL;
    g->error = NULL;
    return g;
fail:
    fg_go_graph_destroy(g);
    return NULL;
}

void fg_go_graph_destroy(fg_go_graph *g) {
    if (!g)
        return;
    for (size_t i = 0; i < g->module_count; i++) {
        free((void *)g->modules[i].directory);
        free((void *)g->modules[i].path);
    }
    for (size_t i = 0; i < g->package_count; i++) {
        free((void *)g->packages[i].directory);
        free((void *)g->packages[i].path);
        free((void *)g->packages[i].name);
    }
    for (size_t i = 0; i < g->reason_count; i++)
        free((void *)g->reasons[i].path);
    free(g->modules);
    free(g->packages);
    free(g->edges);
    free(g);
}

uint64_t fg_go_graph_generation(const fg_go_graph *g) {
    return g ? g->generation : 0;
}
bool fg_go_graph_applicable(const fg_go_graph *g) {
    return g && g->applicable;
}
const fg_go_module *fg_go_graph_modules(const fg_go_graph *g, size_t *count) {
    if (count)
        *count = g ? g->module_count : 0;
    return g ? g->modules : NULL;
}
const fg_go_package *fg_go_graph_packages(const fg_go_graph *g, size_t *count) {
    if (count)
        *count = g ? g->package_count : 0;
    return g ? g->packages : NULL;
}
const fg_go_edge *fg_go_graph_edges(const fg_go_graph *g, size_t *count) {
    if (count)
        *count = g ? g->edge_count : 0;
    return g ? g->edges : NULL;
}
const fg_go_reason *fg_go_graph_reasons(const fg_go_graph *g, size_t *count) {
    if (count)
        *count = g ? g->reason_count : 0;
    return g ? g->reasons : NULL;
}
size_t fg_go_graph_find_package(const fg_go_graph *g, const char *directory) {
    if (!g || !directory)
        return GG_NONE;
    size_t lo = 0, hi = g->package_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(g->packages[mid].directory, directory) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g->package_count && !strcmp(g->packages[lo].directory, directory) ? lo : GG_NONE;
}
size_t fg_go_graph_module_for(const fg_go_graph *g, const char *directory) {
    return g && directory ? gg_module_for(g, directory) : GG_NONE;
}
