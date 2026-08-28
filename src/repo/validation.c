#include "repo_internal.h"
#include "forge/validation.h"
#include <ctype.h>

#define VP_MAX_MODULES 256u
#define VP_MAX_PACKAGES 4096u
#define VP_MAX_EDGES 65536u
#define VP_MAX_CHANGES 1024u
#define VP_MAX_COMMANDS 2048u
#define VP_BATCH_PACKAGES 32u
#define VP_BATCH_BYTES 12000u
#define VP_NONE SIZE_MAX

typedef struct {
    char *directory, *path;
    bool synthetic;
} vp_module;
typedef struct {
    char *directory, *path, *name;
    size_t module;
    bool present, tests, affected, dependent;
} vp_package;
typedef struct {
    size_t from, to, next_from, next_to;
    bool test_only;
} vp_edge;
typedef struct {
    char *path;
    bool go, indexed;
} vp_change;
typedef struct {
    const char *code, *detail;
    char *path;
} vp_reason;
typedef struct {
    forge_repo *repo;
    forge_error *error;
    vp_module *modules;
    vp_package *packages;
    vp_edge *edges;
    vp_change *changes;
    size_t module_count, package_count, edge_count, change_count;
    size_t module_cap, package_cap, edge_cap;
    vp_reason reasons[32];
    size_t reason_count, command_count;
    size_t *from_head, *to_head;
    bool failed, applicable;
    yyjson_mut_doc *doc;
} vp_graph;

static bool vp_fail(vp_graph *g, forge_status status, const char *message) {
    if (!g->failed)
        fg_error(g->error, status, "%s", message);
    g->failed = true;
    return false;
}
static bool vp_reserve(vp_graph *g, void **data, size_t *cap, size_t count, size_t element,
                       size_t limit) {
    if (count > limit)
        return vp_fail(g, FORGE_ERR_LIMIT, "Go validation graph exceeds its documented limit");
    if (count <= *cap)
        return true;
    size_t next = *cap ? *cap * 2 : 16;
    next = FG_MIN(next, limit);
    void *p = realloc(*data, next * element);
    if (!p)
        return vp_fail(g, FORGE_ERR_MEMORY, "Go validation allocation failed");
    *data = p;
    *cap = next;
    return true;
}
static char *vp_copy(vp_graph *g, const char *s) {
    char *copy = fg_strdup(s);
    if (!copy)
        vp_fail(g, FORGE_ERR_MEMORY, "Go validation allocation failed");
    return copy;
}
static bool vp_reason_add(vp_graph *g, const char *code, const char *path, const char *detail) {
    for (size_t i = 0; i < g->reason_count; i++) {
        if (!strcmp(g->reasons[i].code, code)) {
            /* One deterministic example per category, not a diagnostic per file. */
            if (strcmp(path, g->reasons[i].path) < 0) {
                char *copy = vp_copy(g, path);
                if (!copy)
                    return false;
                free(g->reasons[i].path);
                g->reasons[i].path = copy;
            }
            return true;
        }
    }
    if (g->reason_count == sizeof(g->reasons) / sizeof(g->reasons[0]))
        return vp_fail(g, FORGE_ERR_LIMIT, "Too many validation fallback categories");
    vp_reason *r = &g->reasons[g->reason_count++];
    r->code = code;
    r->detail = detail;
    r->path = vp_copy(g, path);
    return r->path != NULL;
}
static const char *vp_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}
static bool vp_suffix(const char *text, const char *suffix) {
    size_t n = strlen(text), z = strlen(suffix);
    return n >= z && !strcmp(text + n - z, suffix);
}
static bool vp_below(const char *directory, const char *path) {
    if (!strcmp(directory, "."))
        return true;
    size_t n = strlen(directory);
    return !strncmp(directory, path, n) && (!path[n] || path[n] == '/');
}
static void vp_directory(const char *path, char out[FG_PATH_MAX]) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        strcpy(out, ".");
    else {
        size_t n = (size_t)(slash - path);
        memcpy(out, path, n);
        out[n] = 0;
    }
}
static char *vp_normalize(vp_graph *g, const char *path) {
    if (!path || !*path || strlen(path) >= FG_PATH_MAX / 2 || path[0] == '/' || path[0] == '\\') {
        vp_fail(g, FORGE_ERR_ARGUMENT, "Changed paths must be workspace-relative file paths");
        return NULL;
    }
    char result[FG_PATH_MAX];
    size_t n = 0;
    while (*path) {
        const char *start = path;
        while (*path && *path != '/' && *path != '\\') {
            unsigned char c = (unsigned char)*path;
            if (c < 32 || strchr(":*?\"<>|", (int)c)) {
                vp_fail(g, FORGE_ERR_ARGUMENT, "Unsafe character in changed path");
                return NULL;
            }
            path++;
        }
        size_t len = (size_t)(path - start);
        if (!len || (len == 2 && !memcmp(start, "..", 2))) {
            vp_fail(g, FORGE_ERR_ARGUMENT,
                    "Changed paths cannot contain traversal or empty components");
            return NULL;
        }
        if (!(len == 1 && *start == '.')) {
            if ((len == 4 && tolower((unsigned char)start[0]) == '.' &&
                 tolower((unsigned char)start[1]) == 'g' &&
                 tolower((unsigned char)start[2]) == 'i' &&
                 tolower((unsigned char)start[3]) == 't') ||
                (len == 6 && start[0] == '.' && tolower((unsigned char)start[1]) == 'f' &&
                 tolower((unsigned char)start[2]) == 'o' &&
                 tolower((unsigned char)start[3]) == 'r' &&
                 tolower((unsigned char)start[4]) == 'g' &&
                 tolower((unsigned char)start[5]) == 'e')) {
                vp_fail(g, FORGE_ERR_ARGUMENT, "Changed path names protected repository metadata");
                return NULL;
            }
            if (n)
                result[n++] = '/';
            memcpy(result + n, start, len);
            n += len;
        }
        if (*path && !*++path) {
            vp_fail(g, FORGE_ERR_ARGUMENT, "Changed paths must name files, not directories");
            return NULL;
        }
    }
    if (!n) {
        vp_fail(g, FORGE_ERR_ARGUMENT, "Changed paths must name files, not the workspace");
        return NULL;
    }
    result[n] = 0;
    /* Missing parent directories are valid for deletions. Lexical validation
     * above still rejects traversal when fg_safe_path cannot inspect a parent. */
    char full[FG_PATH_MAX];
    forge_error e = {0};
    if (!fg_safe_path(g->repo->root, result, true, full, &e) && e.code != FORGE_ERR_NOT_FOUND) {
        vp_fail(g, e.code, e.message);
        return NULL;
    }
    return vp_copy(g, result);
}
static int vp_change_compare(const void *a, const void *b) {
    return strcmp(((const vp_change *)a)->path, ((const vp_change *)b)->path);
}
static int vp_module_compare(const void *a, const void *b) {
    return strcmp(((const vp_module *)a)->directory, ((const vp_module *)b)->directory);
}
static int vp_reason_compare(const void *a, const void *b) {
    return strcmp(((const vp_reason *)a)->code, ((const vp_reason *)b)->code);
}
static int vp_edge_compare(const void *a, const void *b) {
    const vp_edge *x = a, *y = b;
    if (x->from != y->from)
        return x->from < y->from ? -1 : 1;
    if (x->to != y->to)
        return x->to < y->to ? -1 : 1;
    return (int)x->test_only - (int)y->test_only;
}
static bool vp_changes(vp_graph *g, const char *const *paths, size_t count) {
    g->changes = calloc(count ? count : 1, sizeof(*g->changes));
    if (!g->changes)
        return vp_fail(g, FORGE_ERR_MEMORY, "Changed-path allocation failed");
    for (size_t i = 0; i < count; i++) {
        vp_change *c = &g->changes[g->change_count];
        c->path = vp_normalize(g, paths[i]);
        if (!c->path)
            return false;
        c->go = vp_suffix(c->path, ".go");
        g->change_count++;
    }
    qsort(g->changes, g->change_count, sizeof(*g->changes), vp_change_compare);
    size_t used = 0;
    for (size_t i = 0; i < g->change_count; i++) {
        if (used && !strcmp(g->changes[used - 1].path, g->changes[i].path))
            free(g->changes[i].path);
        else
            g->changes[used++] = g->changes[i];
    }
    g->change_count = used;
    return true;
}
static sqlite3_stmt *vp_query(vp_graph *g, const char *query) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(g->repo->db, query, -1, &s, NULL) != SQLITE_OK)
        vp_fail(g, FORGE_ERR_IO, "Cannot query Go validation index");
    return s;
}
static bool vp_query_done(vp_graph *g, sqlite3_stmt *s, int rc) {
    sqlite3_finalize(s);
    return rc == SQLITE_DONE || vp_fail(g, FORGE_ERR_IO, "Cannot read Go validation index");
}
static bool vp_word(const char *p, const char *end, const char *word) {
    size_t n = strlen(word);
    return (size_t)(end - p) >= n && !memcmp(p, word, n) &&
           (p + n == end || isspace((unsigned char)p[n]) || p[n] == '(');
}
static char *vp_module_path(vp_graph *g, const char *text, bool *replacement) {
    char *result = NULL;
    bool invalid = false;
    while (*text) {
        const char *end = strchr(text, '\n');
        if (!end)
            end = text + strlen(text);
        const char *p = text;
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (vp_word(p, end, "replace"))
            *replacement = true;
        if (vp_word(p, end, "module")) {
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
                    vp_fail(g, FORGE_ERR_MEMORY, "Module-path allocation failed");
                    return NULL;
                }
                memcpy(result, start, n);
                result[n] = 0;
                if (strstr(result, "//") || !strcmp(result, ".") || !strcmp(result, "..") ||
                    !strncmp(result, "./", 2) || !strncmp(result, "../", 3) ||
                    strstr(result, "/./") || strstr(result, "/../") || vp_suffix(result, "/.") ||
                    vp_suffix(result, "/.."))
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
static bool vp_module_add(vp_graph *g, const char *directory, char *path, bool synthetic) {
    if (!vp_reserve(g, (void **)&g->modules, &g->module_cap, g->module_count + 1,
                    sizeof(*g->modules), VP_MAX_MODULES)) {
        free(path);
        return false;
    }
    vp_module *m = &g->modules[g->module_count++];
    memset(m, 0, sizeof(*m));
    m->directory = vp_copy(g, directory);
    m->path = path;
    m->synthetic = synthetic;
    return !g->failed;
}
static bool vp_load_modules(vp_graph *g) {
    sqlite3_stmt *s = vp_query(g, "SELECT path,content FROM chunks WHERE path='go.mod' OR "
                                  "path LIKE '%/go.mod' OR path='go.work' OR "
                                  "path LIKE '%/go.work' UNION SELECT b.path,NULL FROM "
                                  "go_module_boundaries b WHERE NOT EXISTS "
                                  "(SELECT 1 FROM chunks c WHERE c.path=b.path) ORDER BY 1");
    if (!s)
        return false;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *file = (const char *)sqlite3_column_text(s, 0);
        const char *text = (const char *)sqlite3_column_text(s, 1);
        if (!strcmp(vp_base(file), "go.work")) {
            vp_reason_add(
                g, "go_workspace", file,
                "Workspace use/replace directives and modules outside the repository "
                "are not resolved; verify every indexed module in the active Go environment.");
            continue;
        }
        char directory[FG_PATH_MAX];
        vp_directory(file, directory);
        bool replacement = false;
        char *path = text ? vp_module_path(g, text, &replacement) : NULL;
        if (!path && !g->failed)
            vp_reason_add(
                g, "unresolved_module_path", file,
                "The module directive could not be resolved; import identities are incomplete.");
        if (replacement)
            vp_reason_add(g, "module_replacements", file,
                          "Module replacements may redirect imports; local replacement aliases and "
                          "external module contents are not resolved by the syntactic graph.");
        if (g->failed) {
            free(path);
            sqlite3_finalize(s);
            return false;
        }
        if (!vp_module_add(g, directory, path, false)) {
            sqlite3_finalize(s);
            return false;
        }
    }
    return vp_query_done(g, s, rc);
}
static size_t vp_package_find(vp_graph *g, const char *directory, size_t *position) {
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
    return lo < g->package_count && !strcmp(g->packages[lo].directory, directory) ? lo : VP_NONE;
}
static size_t vp_package_add(vp_graph *g, const char *directory) {
    size_t position = 0, found = vp_package_find(g, directory, &position);
    if (found != VP_NONE)
        return found;
    if (!vp_reserve(g, (void **)&g->packages, &g->package_cap, g->package_count + 1,
                    sizeof(*g->packages), VP_MAX_PACKAGES))
        return VP_NONE;
    memmove(g->packages + position + 1, g->packages + position,
            (g->package_count - position) * sizeof(*g->packages));
    vp_package *p = &g->packages[position];
    memset(p, 0, sizeof(*p));
    p->directory = vp_copy(g, directory);
    p->module = VP_NONE;
    g->package_count++;
    return g->failed ? VP_NONE : position;
}
static bool vp_excluded(const char *path) {
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
static bool vp_platform_filename(const char *path) {
    /* Unknown future suffixes remain covered by the documented environment limitation. */
    static const char *const suffixes[] = {
        "aix",    "android",  "darwin", "dragonfly", "freebsd", "hurd",    "illumos", "ios",
        "js",     "linux",    "netbsd", "openbsd",   "plan9",   "solaris", "wasip1",  "windows",
        "zos",    "386",      "amd64",  "arm",       "arm64",   "loong64", "mips",    "mipsle",
        "mips64", "mips64le", "ppc64",  "ppc64le",   "riscv64", "s390x",   "wasm"};
    char name[FG_PATH_MAX];
    size_t n = strlen(vp_base(path));
    if (n >= sizeof(name))
        return true;
    memcpy(name, vp_base(path), n + 1);
    n -= vp_suffix(name, "_test.go") ? 8u : 3u;
    name[n] = 0;
    const char *suffix = strrchr(name, '_');
    if (!suffix)
        return false;
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        if (!strcmp(suffix + 1, suffixes[i]))
            return true;
    return false;
}
static bool vp_load_packages(vp_graph *g) {
    sqlite3_stmt *s = vp_query(g, "SELECT f.path,g.package_name,g.is_test,g.build_constraints,"
                                  "g.parse_error FROM files f LEFT JOIN go_files g ON "
                                  "g.file_id=f.id WHERE f.language='go' ORDER BY f.path");
    if (!s)
        return false;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *file = (const char *)sqlite3_column_text(s, 0);
        if (vp_excluded(file))
            continue;
        char directory[FG_PATH_MAX];
        vp_directory(file, directory);
        size_t index = vp_package_add(g, directory);
        if (index == VP_NONE) {
            sqlite3_finalize(s);
            return false;
        }
        vp_package *p = &g->packages[index];
        p->present = true;
        bool test = sqlite3_column_int(s, 2) != 0;
        p->tests |= test;
        const char *name = (const char *)sqlite3_column_text(s, 1);
        if (!test && name) {
            if (p->name && strcmp(p->name, name))
                vp_reason_add(g, "conflicting_package_names", directory,
                              "Files in one directory declare different package names; build "
                              "selection and type correctness require the Go toolchain.");
            if (!p->name)
                p->name = vp_copy(g, name);
        }
        if (sqlite3_column_type(s, 1) == SQLITE_NULL || sqlite3_column_int(s, 4))
            vp_reason_add(g, "go_parse_error", file,
                          "Missing or incomplete Go syntax metadata can omit imports; broad "
                          "verification is required.");
        if (sqlite3_column_int(s, 3) || vp_platform_filename(file))
            vp_reason_add(
                g, "build_constraints", file,
                "The graph unions imports across build constraints and platform suffixes; "
                "commands validate only the active GOOS/GOARCH/tags configuration.");
        if (g->failed) {
            sqlite3_finalize(s);
            return false;
        }
    }
    return vp_query_done(g, s, rc);
}
static bool vp_mark_changes(vp_graph *g) {
    sqlite3_stmt *s = vp_query(g, "SELECT language FROM files WHERE path=?");
    if (!s)
        return false;
    for (size_t i = 0; i < g->change_count; i++) {
        vp_change *c = &g->changes[i];
        sqlite3_bind_text(s, 1, c->path, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        c->indexed = rc == SQLITE_ROW;
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sqlite3_finalize(s);
            return vp_fail(g, FORGE_ERR_IO, "Cannot resolve changed path in the index");
        }
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
        char directory[FG_PATH_MAX];
        vp_directory(c->path, directory);
        size_t package = vp_package_find(g, directory, NULL);
        if (c->go && !vp_excluded(c->path) && package == VP_NONE) {
            /* A tombstone preserves incoming edges for an entirely deleted package. */
            package = vp_package_add(g, directory);
            vp_reason_add(g, "unindexed_or_deleted_package", c->path,
                          "This Go package has no indexed source; retain incoming imports and "
                          "validate dependents plus the full module suite.");
        }
        if (package != VP_NONE)
            g->packages[package].affected = true;
        if (c->go && !c->indexed)
            vp_reason_add(
                g, "unindexed_or_deleted_path", c->path,
                "The changed Go file is absent from the index; it may be deleted, skipped, "
                "or newer than the index. It cannot be included in the formatting check.");
        if (g->failed) {
            sqlite3_finalize(s);
            return false;
        }
    }
    sqlite3_finalize(s);
    return true;
}
static size_t vp_module_for(vp_graph *g, const char *directory) {
    size_t found = VP_NONE, length = 0;
    for (size_t i = 0; i < g->module_count; i++) {
        size_t n = !strcmp(g->modules[i].directory, ".") ? 0 : strlen(g->modules[i].directory);
        if (vp_below(g->modules[i].directory, directory) && (found == VP_NONE || n > length)) {
            found = i;
            length = n;
        }
    }
    return found;
}
static bool vp_assign_modules(vp_graph *g) {
    for (size_t i = 0; i < g->package_count; i++) {
        if (vp_module_for(g, g->packages[i].directory) == VP_NONE) {
            if (!vp_module_add(g, ".", NULL, true))
                return false;
            vp_reason_add(
                g, "missing_go_module", g->packages[i].directory,
                "Go source has no containing indexed go.mod; GOPATH and modules outside "
                "the workspace are not resolved. Root verification may require environment setup.");
            break;
        }
    }
    if (g->module_count > 1)
        qsort(g->modules, g->module_count, sizeof(*g->modules), vp_module_compare);
    for (size_t i = 0; i < g->package_count; i++) {
        vp_package *p = &g->packages[i];
        p->module = vp_module_for(g, p->directory);
        if (p->module == VP_NONE)
            return vp_fail(g, FORGE_ERR_PARSE, "Cannot assign Go package to a module boundary");
        vp_module *m = &g->modules[p->module];
        if (m->path) {
            const char *relative = !strcmp(m->directory, p->directory) ? ""
                                   : !strcmp(m->directory, ".")
                                       ? p->directory
                                       : p->directory + strlen(m->directory) + 1;
            fg_buf b = {0};
            fg_buf_puts(&b, m->path);
            if (*relative)
                fg_buf_printf(&b, "/%s", relative);
            p->path = fg_buf_take(&b);
            if (!p->path)
                return vp_fail(g, FORGE_ERR_MEMORY, "Package import-path allocation failed");
        }
    }
    for (size_t i = 0; i < g->module_count; i++)
        for (size_t j = i + 1; j < g->module_count; j++)
            if (g->modules[i].path && g->modules[j].path &&
                !strcmp(g->modules[i].path, g->modules[j].path))
                vp_reason_add(g, "duplicate_module_path", g->modules[i].directory,
                              "Multiple module roots declare the same import path; retain all "
                              "matching dependency edges and verify each module separately.");
    return !g->failed;
}
static bool vp_configuration_changes(vp_graph *g) {
    if (!g->change_count) {
        vp_reason_add(g, "no_changed_paths", "",
                      "No changed-file set was supplied; compile, test and vet all indexed "
                      "packages before final module verification.");
        for (size_t i = 0; i < g->package_count; i++)
            g->packages[i].affected = true;
    }
    for (size_t i = 0; i < g->change_count; i++) {
        const char *path = g->changes[i].path, *base = vp_base(path);
        bool workspace = !strcmp(base, "go.work") || !strcmp(base, "go.work.sum");
        bool module = !strcmp(base, "go.mod") || !strcmp(base, "go.sum");
        char directory[FG_PATH_MAX];
        vp_directory(path, directory);
        if (workspace || module) {
            vp_reason_add(
                g, "module_configuration_changed", path,
                "Module or workspace configuration can change dependency resolution for "
                "all packages in its scope; broaden compile, tests, and final verification.");
            size_t owner = vp_module_for(g, directory);
            for (size_t j = 0; j < g->package_count; j++)
                if (workspace || g->packages[j].module == owner)
                    g->packages[j].affected = true;
        } else if (vp_package_find(g, directory, NULL) == VP_NONE || vp_excluded(path)) {
            vp_reason_add(g, "unassigned_changed_path", path,
                          "A changed file has no direct Go package mapping (for example a fixture "
                          "or build configuration); broad verification covers unknown consumers.");
        }
    }
    return !g->failed;
}
typedef struct {
    const char *path;
    size_t package;
} vp_import_key;
static int vp_import_compare(const void *a, const void *b) {
    const vp_import_key *x = a, *y = b;
    int cmp = strcmp(x->path, y->path);
    if (cmp)
        return cmp;
    return x->package == y->package ? 0 : x->package < y->package ? -1 : 1;
}
static bool vp_edge_add(vp_graph *g, size_t from, size_t to, bool test) {
    if (!vp_reserve(g, (void **)&g->edges, &g->edge_cap, g->edge_count + 1, sizeof(*g->edges),
                    VP_MAX_EDGES))
        return false;
    vp_edge *e = &g->edges[g->edge_count++];
    e->from = from;
    e->to = to;
    e->test_only = test;
    e->next_from = e->next_to = VP_NONE;
    return true;
}
static bool vp_load_edges(vp_graph *g) {
    vp_import_key *keys = calloc(g->package_count ? g->package_count : 1, sizeof(*keys));
    if (!keys)
        return vp_fail(g, FORGE_ERR_MEMORY, "Import lookup allocation failed");
    size_t count = 0;
    for (size_t i = 0; i < g->package_count; i++) {
        if (g->packages[i].path) {
            keys[count].path = g->packages[i].path;
            keys[count++].package = i;
        }
    }
    qsort(keys, count, sizeof(*keys), vp_import_compare);
    sqlite3_stmt *s = vp_query(g, "SELECT f.path,i.path,g.is_test FROM imports i JOIN files f ON "
                                  "f.id=i.file_id LEFT JOIN go_files g ON g.file_id=f.id "
                                  "ORDER BY f.path,i.path");
    if (!s) {
        free(keys);
        return false;
    }
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *file = (const char *)sqlite3_column_text(s, 0);
        const char *import = (const char *)sqlite3_column_text(s, 1);
        if (vp_excluded(file))
            continue;
        char directory[FG_PATH_MAX];
        vp_directory(file, directory);
        size_t from = vp_package_find(g, directory, NULL);
        if (from == VP_NONE)
            continue;
        if (!strcmp(import, "C")) {
            vp_reason_add(g, "cgo_import", file,
                          "Cgo dependencies, external headers, libraries, and toolchains are not "
                          "represented in the Go import graph.");
            continue;
        }
        if (strchr(import, '\\') || import[0] == '.') {
            vp_reason_add(g, "unresolved_import_syntax", file,
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
            if (!vp_edge_add(g, from, keys[i].package, sqlite3_column_int(s, 2) != 0))
                break;
            if (g->packages[from].module != g->packages[keys[i].package].module)
                vp_reason_add(g, "cross_module_import", file,
                              "An import matches another local module by its declared path; actual "
                              "version/workspace/replacement selection is delegated to Go.");
        }
        if (!matched) {
            for (size_t i = 0; i < g->module_count; i++) {
                if (g->modules[i].path && vp_below(g->modules[i].path, import)) {
                    vp_reason_add(g, "unresolved_local_import", file,
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
    if (!vp_query_done(g, s, rc))
        return false;
    if (g->edge_count > 1)
        qsort(g->edges, g->edge_count, sizeof(*g->edges), vp_edge_compare);
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
static bool vp_reachability(vp_graph *g) {
    size_t n = g->package_count ? g->package_count : 1;
    g->from_head = malloc(n * sizeof(*g->from_head));
    g->to_head = malloc(n * sizeof(*g->to_head));
    size_t *queue = malloc(n * sizeof(*queue)), *indegree = calloc(n, sizeof(*indegree));
    if (!g->from_head || !g->to_head || !queue || !indegree) {
        free(queue);
        free(indegree);
        return vp_fail(g, FORGE_ERR_MEMORY, "Dependency traversal allocation failed");
    }
    for (size_t i = 0; i < n; i++)
        g->from_head[i] = g->to_head[i] = VP_NONE;
    for (size_t i = 0; i < g->edge_count; i++) {
        vp_edge *e = &g->edges[i];
        e->next_from = g->from_head[e->from];
        e->next_to = g->to_head[e->to];
        g->from_head[e->from] = g->to_head[e->to] = i;
        if (e->from != e->to || !e->test_only)
            indegree[e->to]++;
    }
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < g->package_count; i++)
        if (g->packages[i].affected)
            queue[tail++] = i;
    while (head < tail) {
        size_t node = queue[head++];
        for (size_t edge = g->to_head[node]; edge != VP_NONE; edge = g->edges[edge].next_to) {
            size_t from = g->edges[edge].from;
            if (!g->packages[from].affected && !g->packages[from].dependent) {
                g->packages[from].dependent = true;
                queue[tail++] = from;
            }
        }
    }
    head = tail = 0;
    for (size_t i = 0; i < g->package_count; i++)
        if (!indegree[i])
            queue[tail++] = i;
    while (head < tail) {
        size_t node = queue[head++];
        for (size_t edge = g->from_head[node]; edge != VP_NONE; edge = g->edges[edge].next_from) {
            vp_edge *e = &g->edges[edge];
            if (e->from == e->to && e->test_only)
                continue; /* External tests may legally import their own package. */
            if (!--indegree[e->to])
                queue[tail++] = e->to;
        }
    }
    if (tail != g->package_count) {
        for (size_t i = 0; i < g->package_count; i++) {
            if (indegree[i]) {
                vp_reason_add(
                    g, "possible_import_cycle", g->packages[i].directory,
                    "The union import graph contains a cycle. Test packages or mutually "
                    "exclusive build constraints may explain it; let Go determine validity.");
                break;
            }
        }
    }
    free(queue);
    free(indegree);
    return !g->failed;
}

/* JSON helpers use copied strings: the serialized plan never borrows graph memory. */
static yyjson_mut_val *vp_object(vp_graph *g) {
    yyjson_mut_val *v = yyjson_mut_obj(g->doc);
    if (!v)
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
    return v;
}
static yyjson_mut_val *vp_array(vp_graph *g) {
    yyjson_mut_val *v = yyjson_mut_arr(g->doc);
    if (!v)
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
    return v;
}
static void vp_value(vp_graph *g, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value) {
    if (!yyjson_mut_obj_add_val(g->doc, obj, key, value))
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static void vp_string(vp_graph *g, yyjson_mut_val *obj, const char *key, const char *text) {
    bool ok = text ? yyjson_mut_obj_add_strcpy(g->doc, obj, key, text)
                   : yyjson_mut_obj_add_null(g->doc, obj, key);
    if (!ok)
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static void vp_bool(vp_graph *g, yyjson_mut_val *obj, const char *key, bool value) {
    if (!yyjson_mut_obj_add_bool(g->doc, obj, key, value))
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static void vp_number(vp_graph *g, yyjson_mut_val *obj, const char *key, uint64_t value) {
    if (!yyjson_mut_obj_add_uint(g->doc, obj, key, value))
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static void vp_append(vp_graph *g, yyjson_mut_val *array, yyjson_mut_val *value) {
    if (!yyjson_mut_arr_append(array, value))
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static void vp_append_string(vp_graph *g, yyjson_mut_val *array, const char *text) {
    if (!yyjson_mut_arr_add_strcpy(g->doc, array, text))
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
}
static yyjson_mut_val *vp_command(vp_graph *g, yyjson_mut_val *commands, const char *cwd,
                                  const char *const *prefix, size_t prefix_count, bool empty_stdout,
                                  const char *reason) {
    if (++g->command_count > VP_MAX_COMMANDS) {
        vp_fail(g, FORGE_ERR_LIMIT, "Validation plan exceeds 2048 commands; no plan was returned");
        return NULL;
    }
    yyjson_mut_val *command = vp_object(g), *argv = vp_array(g);
    vp_string(g, command, "cwd", cwd);
    vp_value(g, command, "argv", argv);
    vp_bool(g, command, "require_empty_stdout", empty_stdout);
    vp_string(g, command, "reason", reason);
    vp_append(g, commands, command);
    for (size_t i = 0; i < prefix_count; i++)
        vp_append_string(g, argv, prefix[i]);
    return argv;
}
typedef struct {
    yyjson_mut_val *argv;
    size_t count, bytes;
} vp_format_batch;
static bool vp_format_file(vp_graph *g, yyjson_mut_val *commands, vp_format_batch *batch,
                           const char *file, const char *reason) {
    static const char *const prefix[] = {"gofmt", "-l"};
    size_t n = strlen(file) + 3;
    if (!batch->argv || batch->count == VP_BATCH_PACKAGES || batch->bytes + n > VP_BATCH_BYTES) {
        batch->argv = vp_command(g, commands, ".", prefix, 2, true, reason);
        batch->count = batch->bytes = 0;
    }
    char path[FG_PATH_MAX];
    snprintf(path, sizeof(path), "./%s", file);
    vp_append_string(g, batch->argv, path);
    batch->count++;
    batch->bytes += n;
    return !g->failed;
}
static bool vp_format_commands(vp_graph *g, yyjson_mut_val *commands) {
    vp_format_batch batch = {0};
    if (!g->change_count) {
        /* Unknown command-origin edits and explicit full verification still
         * need a formatting gate. A narrow path list is not available here. */
        sqlite3_stmt *s = vp_query(g, "SELECT path FROM files WHERE language='go' ORDER BY path");
        if (!s)
            return false;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const char *file = (const char *)sqlite3_column_text(s, 0);
            if (!vp_excluded(file) && !vp_format_file(g, commands, &batch, file,
                                                      "indexed_go_files_for_unknown_changes")) {
                sqlite3_finalize(s);
                return false;
            }
        }
        return vp_query_done(g, s, rc);
    }
    for (size_t i = 0; i < g->change_count; i++) {
        vp_change *c = &g->changes[i];
        if (!c->go || !c->indexed)
            continue;
        if (!vp_format_file(g, commands, &batch, c->path, "changed_go_files"))
            return false;
    }
    return true;
}
static bool vp_package_commands(vp_graph *g, yyjson_mut_val *commands, unsigned stage) {
    static const char *const compile[] = {"go",       "test", "-json", "-count=1",
                                          "-vet=off", "-run", "^$"};
    static const char *const test[] = {"go", "test", "-json", "-count=1", "-vet=off"};
    static const char *const vet[] = {"go", "vet"};
    const char *const *prefix = stage == 1 ? compile : stage == 4 ? vet : test;
    size_t prefix_count = stage == 1 ? 7 : stage == 4 ? 2 : 5;
    const char *reason = stage == 3   ? "transitive_reverse_imports"
                         : stage == 4 ? "affected_and_dependent_packages"
                                      : "directly_affected_packages";
    for (size_t i = 0; i < g->module_count; i++) {
        yyjson_mut_val *argv = NULL;
        size_t count = 0, bytes = 0;
        for (size_t j = 0; j < g->package_count; j++) {
            vp_package *p = &g->packages[j];
            bool selected = stage == 3   ? p->dependent
                            : stage == 4 ? p->affected || p->dependent
                                         : p->affected;
            if (!selected || !p->present || p->module != i)
                continue;
            const char *directory = g->modules[i].directory;
            const char *relative = !strcmp(directory, p->directory) ? "."
                                   : !strcmp(directory, ".") ? p->directory
                                                             : p->directory + strlen(directory) + 1;
            char target[FG_PATH_MAX];
            if (!strcmp(relative, "."))
                strcpy(target, ".");
            else
                snprintf(target, sizeof(target), "./%s", relative);
            size_t n = strlen(target) + 1;
            if (!argv || count == VP_BATCH_PACKAGES || bytes + n > VP_BATCH_BYTES) {
                argv = vp_command(g, commands, directory, prefix, prefix_count, false, reason);
                count = bytes = 0;
            }
            vp_append_string(g, argv, target);
            count++;
            bytes += n;
            if (g->failed)
                return false;
        }
    }
    return true;
}
static void vp_stages(vp_graph *g, yyjson_mut_val *array) {
    static const char *const names[] = {"format",          "compile", "affected_tests",
                                        "dependent_tests", "vet",     "broad_tests"};
    static const char *const broad[] = {"go", "test", "-json", "-count=1", "./..."};
    for (unsigned i = 0; i < 6; i++) {
        yyjson_mut_val *stage = vp_object(g), *commands = vp_array(g);
        vp_string(g, stage, "name", names[i]);
        vp_bool(g, stage, "requires_previous_success", i != 0);
        vp_value(g, stage, "commands", commands);
        vp_append(g, array, stage);
        if (!g->applicable)
            continue;
        if (i == 0)
            vp_format_commands(g, commands);
        else if (i < 5)
            vp_package_commands(g, commands, i);
        else
            for (size_t j = 0; j < g->module_count; j++)
                vp_command(g, commands, g->modules[j].directory, broad, 5, false,
                           g->reason_count ? "conservative_fallback_and_final_verification"
                                           : "final_module_verification");
        if (g->failed)
            return;
    }
}
static char *vp_serialize(vp_graph *g) {
    g->doc = yyjson_mut_doc_new(NULL);
    if (!g->doc) {
        vp_fail(g, FORGE_ERR_MEMORY, "Validation JSON allocation failed");
        return NULL;
    }
    yyjson_mut_val *root = vp_object(g);
    yyjson_mut_doc_set_root(g->doc, root);
    vp_number(g, root, "schema_version", 1);
    vp_number(g, root, "generation", g->repo->generation);
    vp_string(g, root, "language", "go");
    vp_string(g, root, "status", "planned");
    vp_string(g, root, "graph_kind", "syntactic_package_imports");
    vp_bool(g, root, "sound", false);
    vp_bool(g, root, "applicable", g->applicable);
    vp_bool(g, root, "broad_verification_required", g->applicable);
    yyjson_mut_val *changed = vp_array(g), *modules = vp_array(g), *packages = vp_array(g),
                   *edges = vp_array(g), *affected = vp_array(g), *dependents = vp_array(g),
                   *reasons = vp_array(g), *limitations = vp_array(g), *stages = vp_array(g);
    vp_value(g, root, "changed_paths", changed);
    vp_value(g, root, "modules", modules);
    vp_value(g, root, "packages", packages);
    vp_value(g, root, "edges", edges);
    vp_value(g, root, "affected_packages", affected);
    vp_value(g, root, "reverse_dependents", dependents);
    vp_value(g, root, "fallback_reasons", reasons);
    vp_value(g, root, "limitations", limitations);
    vp_value(g, root, "stages", stages);
    for (size_t i = 0; i < g->change_count; i++)
        vp_append_string(g, changed, g->changes[i].path);
    for (size_t i = 0; i < g->module_count; i++) {
        yyjson_mut_val *m = vp_object(g);
        vp_string(g, m, "directory", g->modules[i].directory);
        vp_string(g, m, "module_path", g->modules[i].path);
        vp_bool(g, m, "synthetic", g->modules[i].synthetic);
        vp_append(g, modules, m);
    }
    for (size_t i = 0; i < g->package_count; i++) {
        vp_package *p = &g->packages[i];
        yyjson_mut_val *value = vp_object(g);
        vp_string(g, value, "directory", p->directory);
        vp_string(g, value, "import_path", p->path);
        vp_string(g, value, "module_directory", g->modules[p->module].directory);
        vp_bool(g, value, "present", p->present);
        vp_bool(g, value, "has_tests", p->tests);
        vp_bool(g, value, "affected", p->affected);
        vp_bool(g, value, "dependent", p->dependent);
        vp_append(g, packages, value);
        if (p->affected)
            vp_append_string(g, affected, p->directory);
        if (p->dependent)
            vp_append_string(g, dependents, p->directory);
    }
    for (size_t i = 0; i < g->edge_count; i++) {
        yyjson_mut_val *edge = vp_object(g);
        vp_string(g, edge, "from", g->packages[g->edges[i].from].directory);
        vp_string(g, edge, "to", g->packages[g->edges[i].to].directory);
        vp_bool(g, edge, "test_only", g->edges[i].test_only);
        vp_append(g, edges, edge);
    }
    qsort(g->reasons, g->reason_count, sizeof(*g->reasons), vp_reason_compare);
    for (size_t i = 0; i < g->reason_count; i++) {
        yyjson_mut_val *reason = vp_object(g);
        vp_string(g, reason, "code", g->reasons[i].code);
        vp_string(g, reason, "path", g->reasons[i].path);
        vp_string(g, reason, "detail", g->reasons[i].detail);
        vp_append(g, reasons, reason);
    }
    vp_append_string(
        g, limitations,
        "Package imports are syntactic, not type-resolved references or test coverage.");
    vp_append_string(g, limitations,
                     "Imports from production and test files are unioned; test-only changes also "
                     "include reverse dependents conservatively.");
    vp_append_string(g, limitations,
                     "Only indexed workspace files and module directives are resolved; generated "
                     "code, embed inputs, replacement aliases, vendored/external modules and "
                     "runtime dependencies may escape the graph.");
    vp_append_string(g, limitations,
                     "Broad tests cover each indexed module only in the active Go environment; "
                     "other tags, platforms, toolchains, integration services and modules outside "
                     "the workspace need separate validation.");
    vp_append_string(
        g, limitations,
        "The compile check uses go test -run ^$ and may execute package initialization and "
        "TestMain. All Go checks require process permission and may access caches or the network.");
    vp_stages(g, stages);
    vp_number(g, root, "command_count", (uint64_t)g->command_count);
    if (g->failed)
        return NULL;
    size_t length = 0;
    char *out = yyjson_mut_write(g->doc, 0, &length);
    if (!out)
        vp_fail(g, FORGE_ERR_MEMORY, "Cannot serialize validation plan");
    else if (length > FG_MAX_JSON) {
        free(out);
        out = NULL;
        vp_fail(g, FORGE_ERR_LIMIT, "Validation plan exceeds the 16 MiB JSON limit");
    }
    return out;
}
static void vp_free(vp_graph *g) {
    for (size_t i = 0; i < g->module_count; i++) {
        free(g->modules[i].directory);
        free(g->modules[i].path);
    }
    for (size_t i = 0; i < g->package_count; i++) {
        free(g->packages[i].directory);
        free(g->packages[i].path);
        free(g->packages[i].name);
    }
    for (size_t i = 0; i < g->change_count; i++)
        free(g->changes[i].path);
    for (size_t i = 0; i < g->reason_count; i++)
        free(g->reasons[i].path);
    free(g->modules);
    free(g->packages);
    free(g->edges);
    free(g->changes);
    free(g->from_head);
    free(g->to_head);
    if (g->doc)
        yyjson_mut_doc_free(g->doc);
}
char *forge_repo_validation_plan(forge_repo *repo, const char *const *paths, size_t count,
                                 forge_error *error) {
    if (error) {
        error->code = FORGE_OK;
        error->message[0] = 0;
    }
    if (!repo || (count && !paths)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Repository and changed-path list are required");
        return NULL;
    }
    if (count > VP_MAX_CHANGES) {
        fg_error(error, FORGE_ERR_LIMIT, "Validation accepts at most 1024 changed paths");
        return NULL;
    }
    if (!repo->scan) {
        fg_error(error, FORGE_ERR_CONFLICT,
                 "Index the repository before requesting a validation plan");
        return NULL;
    }
    vp_graph g = {0};
    g.repo = repo;
    g.error = error;
    char *out = NULL;
    if (!vp_changes(&g, paths, count) || !vp_load_modules(&g) || !vp_load_packages(&g) ||
        !vp_mark_changes(&g) || !vp_assign_modules(&g))
        goto done;
    if (!g.module_count && !g.package_count && repo->go_index_incomplete &&
        !vp_module_add(&g, ".", NULL, true))
        goto done;
    g.applicable = g.module_count != 0 || g.package_count != 0;
    if (g.applicable) {
        if (repo->go_index_incomplete)
            vp_reason_add(
                &g, "incomplete_go_index", "",
                "Some Go source or module metadata was unreadable, unsafe, binary, or too "
                "large to index; the graph cannot justify narrow verification alone.");
        if (repo->filesystem_scan)
            vp_reason_add(
                &g, "filesystem_scan", "",
                "Git enumeration was unavailable; the fallback walker excludes hidden, "
                "vendor, build, and other generated directories, which may omit Go inputs.");
        if (!vp_configuration_changes(&g) || !vp_load_edges(&g) || !vp_reachability(&g))
            goto done;
    }
    out = vp_serialize(&g);
done:
    vp_free(&g);
    return out;
}
