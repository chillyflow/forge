#include "graph.h"
#include "forge/validation.h"
#include <ctype.h>

#define VP_MAX_CHANGES 1024u
#define VP_MAX_COMMANDS 2048u
#define VP_BATCH_PACKAGES 32u
#define VP_BATCH_BYTES 12000u
#define VP_MAX_VM_STEPS UINT64_C(100000000)
#define VP_NONE FG_GO_GRAPH_NONE
#define vp_excluded fg_go_graph_excluded_path

typedef fg_go_module vp_module;
typedef fg_go_package vp_package;
typedef fg_go_edge vp_edge;
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
    fg_repo_snapshot *snapshot;
    fg_go_graph *graph;
    const vp_module *modules;
    const vp_package *packages;
    const vp_edge *edges;
    vp_change *changes;
    bool *affected, *dependent;
    size_t module_count, package_count, edge_count, change_count;
    vp_reason reasons[32];
    size_t reason_count, command_count;
    bool failed, applicable;
    yyjson_mut_doc *doc;
} vp_graph;

static bool vp_stopped(vp_graph *g) {
    if (g->failed)
        return true;
    if (g->snapshot && fg_repo_snapshot_stopped(g->snapshot)) {
        g->failed = true;
        return true;
    }
    return false;
}
static bool vp_fail(vp_graph *g, forge_status status, const char *message) {
    if (!g->failed)
        fg_error(g->error, status, "%s", message);
    g->failed = true;
    return false;
}

static char *vp_copy(vp_graph *g, const char *s) {
    char *copy = fg_strdup(s);
    if (!copy)
        vp_fail(g, FORGE_ERR_MEMORY, "Go validation allocation failed");
    return copy;
}

static bool vp_reason_add(vp_graph *g, const char *code, const char *path, const char *detail) {
    if (vp_stopped(g))
        return false;
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

static int vp_reason_compare(const void *a, const void *b) {
    return strcmp(((const vp_reason *)a)->code, ((const vp_reason *)b)->code);
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
    if (vp_stopped(g))
        return NULL;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(g->repo->db, query, -1, &s, NULL) != SQLITE_OK)
        vp_fail(g, FORGE_ERR_IO, "Cannot query Go validation index");
    return s;
}

static bool vp_query_done(vp_graph *g, sqlite3_stmt *s, int rc) {
    sqlite3_finalize(s);
    if (vp_stopped(g))
        return false;
    return rc == SQLITE_DONE || vp_fail(g, FORGE_ERR_IO, "Cannot read Go validation index");
}

static size_t vp_package_find(vp_graph *g, const char *directory, size_t *unused) {
    (void)unused;
    return fg_go_graph_find_package(g->graph, directory);
}
static size_t vp_module_for(vp_graph *g, const char *directory) {
    return fg_go_graph_module_for(g->graph, directory);
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
        if (package != VP_NONE)
            g->affected[package] = true;
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

static bool vp_configuration_changes(vp_graph *g) {
    if (!g->change_count) {
        vp_reason_add(g, "no_changed_paths", "",
                      "No changed-file set was supplied; compile, test and vet all indexed "
                      "packages before final module verification.");
        for (size_t i = 0; i < g->package_count; i++)
            g->affected[i] = true;
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
                    g->affected[j] = true;
        } else if (vp_package_find(g, directory, NULL) == VP_NONE || vp_excluded(path)) {
            vp_reason_add(g, "unassigned_changed_path", path,
                          "A changed file has no direct Go package mapping (for example a fixture "
                          "or build configuration); broad verification covers unknown consumers.");
        }
    }
    return !g->failed;
}

static bool vp_reachability(vp_graph *g) {
    size_t n = g->package_count ? g->package_count : 1;
    size_t *queue = malloc(n * sizeof(*queue));
    if (!queue)
        return vp_fail(g, FORGE_ERR_MEMORY, "Dependency traversal allocation failed");
    size_t head = 0, tail = 0;
    for (size_t i = 0; i < g->package_count; i++)
        if (g->affected[i])
            queue[tail++] = i;
    while (head < tail) {
        if (vp_stopped(g))
            break;
        size_t node = queue[head++];
        for (size_t edge = g->packages[node].to_head; edge != VP_NONE;
             edge = g->edges[edge].next_to) {
            size_t from = g->edges[edge].from;
            if (!g->affected[from] && !g->dependent[from]) {
                g->dependent[from] = true;
                queue[tail++] = from;
            }
        }
    }
    free(queue);
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
            const vp_package *p = &g->packages[j];
            bool selected = stage == 3   ? g->dependent[j]
                            : stage == 4 ? g->affected[j] || g->dependent[j]
                                         : g->affected[j];
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
    vp_number(g, root, "generation", g->snapshot->generation);
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
        const vp_package *p = &g->packages[i];
        yyjson_mut_val *value = vp_object(g);
        vp_string(g, value, "directory", p->directory);
        vp_string(g, value, "import_path", p->path);
        vp_string(g, value, "module_directory", g->modules[p->module].directory);
        vp_bool(g, value, "present", p->present);
        vp_bool(g, value, "has_tests", p->tests);
        vp_bool(g, value, "affected", g->affected[i]);
        vp_bool(g, value, "dependent", g->dependent[i]);
        vp_append(g, packages, value);
        if (g->affected[i])
            vp_append_string(g, affected, p->directory);
        if (g->dependent[i])
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
    for (size_t i = 0; i < g->change_count; i++)
        free(g->changes[i].path);
    for (size_t i = 0; i < g->reason_count; i++)
        free(g->reasons[i].path);
    free(g->changes);
    free(g->affected);
    free(g->dependent);
    fg_go_graph_destroy(g->graph);
    if (g->doc)
        yyjson_mut_doc_free(g->doc);
}
char *forge_repo_validation_plan(forge_repo *repo, const char *const *paths, size_t count,
                                 forge_error *error) {
    forge_error local = {0};
    if (!error)
        error = &local;
    memset(error, 0, sizeof(*error));
    if (!repo || (count && !paths)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Repository and changed-path list are required");
        return NULL;
    }
    if (count > VP_MAX_CHANGES) {
        fg_error(error, FORGE_ERR_LIMIT, "Validation accepts at most 1024 changed paths");
        return NULL;
    }
    vp_graph g = {0};
    g.repo = repo;
    g.error = error;
    char *out = NULL;
    fg_repo_snapshot snapshot = {0};
    if (!vp_changes(&g, paths, count))
        goto done;
    if (fg_repo_snapshot_begin(repo, &snapshot, false, 0, NULL, NULL, VP_MAX_VM_STEPS, error) !=
        FORGE_OK)
        goto done;
    g.snapshot = &snapshot;
    const char *extra[VP_MAX_CHANGES];
    for (size_t i = 0; i < g.change_count; i++)
        extra[i] = g.changes[i].path;
    g.graph = fg_go_graph_load(&snapshot, extra, g.change_count, error);
    if (!g.graph)
        goto done;
    g.modules = fg_go_graph_modules(g.graph, &g.module_count);
    g.packages = fg_go_graph_packages(g.graph, &g.package_count);
    g.edges = fg_go_graph_edges(g.graph, &g.edge_count);
    g.applicable = fg_go_graph_applicable(g.graph);
    size_t slots = g.package_count ? g.package_count : 1;
    g.affected = calloc(slots, sizeof(*g.affected));
    g.dependent = calloc(slots, sizeof(*g.dependent));
    if (!g.affected || !g.dependent) {
        vp_fail(&g, FORGE_ERR_MEMORY, "Validation selection allocation failed");
        goto done;
    }
    size_t reason_count = 0;
    const fg_go_reason *reasons = fg_go_graph_reasons(g.graph, &reason_count);
    for (size_t i = 0; i < reason_count; i++)
        if (!vp_reason_add(&g, reasons[i].code, reasons[i].path, reasons[i].detail))
            goto done;
    if (!vp_mark_changes(&g))
        goto done;
    if (g.applicable && (!vp_configuration_changes(&g) || !vp_reachability(&g)))
        goto done;
    out = vp_serialize(&g);
done:
    if (snapshot.internal && fg_repo_snapshot_end(&snapshot, out != NULL, error) != FORGE_OK) {
        free(out);
        out = NULL;
    }
    vp_free(&g);
    return out;
}
