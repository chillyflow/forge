#include "graph.h"
#include "forge/validation.h"
#include <ctype.h>

#define VP_MAX_CHANGES 1024u
#define VP_MAX_COMMANDS 2048u
#define VP_MAX_PYTHON_TARGETS 1024u
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
    bool go, python, indexed;
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
    char **python_targets, **python_tests;
    const char *python_executable;
    bool *affected, *dependent;
    size_t module_count, package_count, edge_count, change_count;
    size_t python_file_count, python_syntax_count, python_test_count, python_target_count;
    vp_reason reasons[32];
    size_t reason_count, command_count;
    bool failed, applicable, verification_ready, go_applicable, python_applicable, python_pytest;
    bool python_broad_syntax;
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
        vp_fail(g, FORGE_ERR_MEMORY, "Validation allocation failed");
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
        c->python = vp_suffix(c->path, ".py");
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
        vp_fail(g, FORGE_ERR_IO, "Cannot query validation index");
    return s;
}

static bool vp_query_done(vp_graph *g, sqlite3_stmt *s, int rc) {
    sqlite3_finalize(s);
    if (vp_stopped(g))
        return false;
    return rc == SQLITE_DONE || vp_fail(g, FORGE_ERR_IO, "Cannot read validation index");
}

static bool vp_python_test_path(const char *path) {
    const char *base = vp_base(path);
    return vp_suffix(base, "_test.py") || (!strncmp(base, "test", 4) && vp_suffix(base, ".py"));
}

static bool vp_python_configuration_path(const char *path) {
    const char *base = vp_base(path);
    return !strcmp(base, "pytest.ini") || !strcmp(base, ".pytest.ini") ||
           !strcmp(base, "pyproject.toml") || !strcmp(base, "setup.cfg") ||
           !strcmp(base, "tox.ini");
}

static bool vp_python_related_test(const vp_change *change, const char *test_path) {
    if (!change->python || !change->indexed || !vp_python_test_path(test_path))
        return false;
    if (!strcmp(change->path, test_path))
        return true;
    if (vp_python_test_path(change->path))
        return false;
    const char *source = vp_base(change->path), *test = vp_base(test_path);
    size_t source_length = strlen(source), test_length = strlen(test);
    if (source_length <= 3)
        return false;
    size_t stem_length = source_length - 3;
    bool prefixed = test_length == 5 + stem_length + 3 && !strncmp(test, "test_", 5) &&
                    !memcmp(test + 5, source, stem_length) &&
                    !strcmp(test + 5 + stem_length, ".py");
    bool suffixed = test_length == stem_length + 8 && !memcmp(test, source, stem_length) &&
                    !strcmp(test + stem_length, "_test.py");
    return prefixed || suffixed;
}

static bool vp_python_target_add(vp_graph *g, const char *path) {
    if (g->python_target_count == VP_MAX_PYTHON_TARGETS)
        return vp_fail(g, FORGE_ERR_LIMIT, "Python targeted validation exceeds 1024 test files");
    char *copy = vp_copy(g, path);
    if (!copy)
        return false;
    g->python_targets[g->python_target_count++] = copy;
    return true;
}

static bool vp_python_test_add(vp_graph *g, const char *path) {
    if (g->python_test_count == VP_MAX_PYTHON_TARGETS)
        return vp_fail(g, FORGE_ERR_LIMIT, "Python validation exceeds 1024 test files");
    char *copy = vp_copy(g, path);
    if (!copy)
        return false;
    g->python_tests[g->python_test_count++] = copy;
    return true;
}

static bool vp_python_content_signal(vp_graph *g) {
    sqlite3_stmt *s =
        vp_query(g, "SELECT 1 FROM chunks WHERE "
                    "(((path='pyproject.toml' OR path LIKE '%/pyproject.toml' OR "
                    "path='setup.cfg' OR path LIKE '%/setup.cfg' OR path='tox.ini' OR "
                    "path LIKE '%/tox.ini') AND instr(lower(content),'pytest')>0) OR "
                    "((path GLOB 'test*.py' OR path GLOB '*/test*.py' OR "
                    "path GLOB '*_test.py') AND (instr(content,'import pytest')>0 OR "
                    "instr(content,'from pytest')>0 OR ((instr(content,'def test_')>0 OR "
                    "instr(content,'async def test_')>0) AND "
                    "instr(content,'unittest.TestCase')=0 AND "
                    "instr(content,'from unittest import TestCase')=0)))) "
                    "LIMIT 1");
    if (!s)
        return false;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW)
        g->python_pytest = true;
    else if (rc != SQLITE_DONE) {
        sqlite3_finalize(s);
        return vp_fail(g, FORGE_ERR_IO, "Cannot detect Python test configuration");
    }
    sqlite3_finalize(s);
    return !vp_stopped(g);
}

static const char *vp_python_interpreter(vp_graph *g) {
#ifdef _WIN32
    static const char *const candidates[] = {"python", "python3", "py"};
#else
    static const char *const candidates[] = {"python3", "python"};
#endif
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
        if (fg_process_executable_available(g->repo->root, g->repo->root, candidates[i]))
            return candidates[i];
    return NULL;
}

static bool vp_python_discover(vp_graph *g) {
    g->python_targets = calloc(VP_MAX_PYTHON_TARGETS, sizeof(*g->python_targets));
    g->python_tests = calloc(VP_MAX_PYTHON_TARGETS, sizeof(*g->python_tests));
    if (!g->python_targets || !g->python_tests)
        return vp_fail(g, FORGE_ERR_MEMORY, "Python validation selection allocation failed");
    sqlite3_stmt *s = vp_query(g, "SELECT path FROM files ORDER BY path");
    if (!s)
        return false;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(s, 0);
        const char *base = vp_base(path);
        if (vp_python_configuration_path(path) &&
            (!strcmp(base, "pytest.ini") || !strcmp(base, ".pytest.ini")))
            g->python_pytest = true;
        if (!vp_suffix(path, ".py"))
            continue;
        g->python_file_count++;
        if (!strcmp(base, "conftest.py"))
            g->python_pytest = true;
        if (!vp_python_test_path(path))
            continue;
        if (!vp_python_test_add(g, path)) {
            sqlite3_finalize(s);
            return false;
        }
        for (size_t i = 0; i < g->change_count; i++) {
            if (vp_python_related_test(&g->changes[i], path)) {
                if (!vp_python_target_add(g, path)) {
                    sqlite3_finalize(s);
                    return false;
                }
                break;
            }
        }
    }
    if (!vp_query_done(g, s, rc) || !vp_python_content_signal(g))
        return false;
    bool python_change = false, python_configuration_change = false;
    for (size_t i = 0; i < g->change_count; i++) {
        vp_change *change = &g->changes[i];
        const char *base = vp_base(change->path);
        bool configuration_change = vp_python_configuration_path(change->path);
        python_change |= change->python;
        python_configuration_change |= configuration_change;
        if (configuration_change && (!strcmp(base, "pytest.ini") || !strcmp(base, ".pytest.ini")))
            g->python_pytest = true;
        if (change->python && !change->indexed &&
            !vp_reason_add(
                g, "unindexed_or_deleted_python_path", change->path,
                "The changed Python file is absent from the index; it may be deleted, skipped, "
                "or newer than the index. It cannot be included in the syntax check."))
            return false;
    }
    g->python_applicable = (!g->change_count && g->python_file_count != 0) || python_change ||
                           (python_configuration_change && g->python_file_count != 0);
    for (size_t i = 0; i < g->change_count; i++) {
        if (g->changes[i].python && g->changes[i].indexed)
            g->python_syntax_count++;
    }
    if ((!g->change_count && g->python_file_count) ||
        (g->python_applicable && !g->python_syntax_count && g->python_file_count)) {
        g->python_syntax_count = g->python_file_count;
        g->python_broad_syntax = true;
    }
    if (!g->python_applicable)
        return true;
    g->python_executable = vp_python_interpreter(g);
    if (!g->python_executable &&
        !vp_reason_add(g, "python_executable_unavailable", "",
                       "Python validation is applicable, but python3/python/py was not found in "
                       "an executable PATH entry outside the workspace."))
        return false;
    if (!g->python_file_count &&
        !vp_reason_add(g, "no_indexed_python_files", "",
                       "Python changes are present, but no current indexed Python file can be "
                       "syntax checked; validation is blocked rather than treated as passing."))
        return false;
    if (python_change && g->python_test_count && !g->python_target_count &&
        !vp_reason_add(g, "python_target_mapping_unavailable", "",
                       "No related test matched the changed Python basename; broad indexed test "
                       "execution remains required."))
        return false;
    if (!g->python_test_count && !g->python_pytest &&
        !vp_reason_add(g, "python_tests_not_detected", "",
                       "No unittest-discoverable file or pytest configuration was detected; the "
                       "plan can establish syntax only, not test success."))
        return false;
    return !g->failed;
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

static bool vp_python_syntax_file(vp_graph *g, yyjson_mut_val *commands, vp_format_batch *batch,
                                  const char *file, const char *reason) {
    static const char script[] = "import pathlib,sys;"
                                 "all(compile(pathlib.Path(p).read_bytes(),p,'exec') is not None "
                                 "for p in sys.argv[1:])";
    const char *prefix[] = {g->python_executable, "-B", "-c", script};
    size_t n = strlen(file) + 3;
    if (!batch->argv || batch->count == VP_BATCH_PACKAGES || batch->bytes + n > VP_BATCH_BYTES) {
        batch->argv = vp_command(g, commands, ".", prefix, 4, false, reason);
        batch->count = batch->bytes = 0;
    }
    char path[FG_PATH_MAX];
    snprintf(path, sizeof(path), "./%s", file);
    vp_append_string(g, batch->argv, path);
    batch->count++;
    batch->bytes += n;
    return !g->failed;
}

static bool vp_python_syntax_commands(vp_graph *g, yyjson_mut_val *commands) {
    vp_format_batch batch = {0};
    if (g->python_broad_syntax) {
        sqlite3_stmt *s =
            vp_query(g, "SELECT path FROM files WHERE path GLOB '*.py' ORDER BY path");
        if (!s)
            return false;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            const char *file = (const char *)sqlite3_column_text(s, 0);
            if (!vp_python_syntax_file(g, commands, &batch, file,
                                       g->change_count
                                           ? "indexed_python_files_for_unmapped_changes"
                                           : "indexed_python_files_for_unknown_changes")) {
                sqlite3_finalize(s);
                return false;
            }
        }
        return vp_query_done(g, s, rc);
    }
    for (size_t i = 0; i < g->change_count; i++) {
        vp_change *change = &g->changes[i];
        if (change->python && change->indexed &&
            !vp_python_syntax_file(g, commands, &batch, change->path,
                                   "changed_python_files_syntax"))
            return false;
    }
    return true;
}

static bool vp_python_file_commands(vp_graph *g, yyjson_mut_val *commands, char *const *files,
                                    size_t file_count, bool use_pytest, const char *reason) {
    static const char unittest_script[] =
        "import sys,unittest;"
        "p=unittest.main(module=None,argv=['unittest','-v',*sys.argv[1:]],exit=False);"
        "sys.exit(not p.result.wasSuccessful() or p.result.testsRun==0)";
    const char *pytest_argv[] = {g->python_executable, "-B", "-m", "pytest", "-q", "-p",
                                 "no:cacheprovider"};
    const char *unittest_argv[] = {g->python_executable, "-B", "-c", unittest_script};
    const char *const *prefix = use_pytest ? pytest_argv : unittest_argv;
    size_t prefix_count = use_pytest ? 7 : 4;
    vp_format_batch batch = {0};
    for (size_t i = 0; i < file_count; i++) {
        const char *file = files[i];
        size_t n = strlen(file) + 3;
        if (!batch.argv || batch.count == VP_BATCH_PACKAGES || batch.bytes + n > VP_BATCH_BYTES) {
            batch.argv = vp_command(g, commands, ".", prefix, prefix_count, false, reason);
            batch.count = batch.bytes = 0;
        }
        char path[FG_PATH_MAX];
        snprintf(path, sizeof(path), "./%s", file);
        vp_append_string(g, batch.argv, path);
        batch.count++;
        batch.bytes += n;
        if (g->failed)
            return false;
    }
    return true;
}

static bool vp_python_target_commands(vp_graph *g, yyjson_mut_val *commands) {
    return vp_python_file_commands(
        g, commands, g->python_targets, g->python_target_count, g->python_pytest,
        g->python_pytest ? "related_pytest_files" : "related_unittest_files");
}

static bool vp_python_broad_command(vp_graph *g, yyjson_mut_val *commands) {
    if (!g->python_test_count && !g->python_pytest)
        return true;
    if (g->python_pytest) {
        const char *argv[] = {g->python_executable, "-B", "-m", "pytest", "-q", "-p",
                              "no:cacheprovider"};
        vp_command(g, commands, ".", argv, 7, false, "final_python_pytest_discovery");
    } else
        vp_python_file_commands(g, commands, g->python_tests, g->python_test_count, false,
                                "final_python_unittest_files");
    return !g->failed;
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
        if (!g->verification_ready)
            continue;
        if (g->go_applicable) {
            if (i == 0)
                vp_format_commands(g, commands);
            else if (i < 5)
                vp_package_commands(g, commands, i);
            else
                for (size_t j = 0; j < g->module_count; j++)
                    vp_command(g, commands, g->modules[j].directory, broad, 5, false,
                               g->reason_count ? "conservative_fallback_and_final_verification"
                                               : "final_module_verification");
        }
        if (g->python_applicable) {
            if (i == 1)
                vp_python_syntax_commands(g, commands);
            else if (i == 2)
                vp_python_target_commands(g, commands);
            else if (i == 5)
                vp_python_broad_command(g, commands);
        }
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
    const char *verification_status = !g->applicable          ? "not_applicable"
                                      : g->verification_ready ? "planned"
                                                              : "blocked";
    vp_number(g, root, "schema_version", 2);
    vp_number(g, root, "generation", g->snapshot->generation);
    vp_string(g, root, "language", g->go_applicable || !g->python_applicable ? "go" : "python");
    vp_string(g, root, "status", verification_status);
    vp_string(g, root, "verification_status", verification_status);
    vp_string(g, root, "graph_kind", g->go_applicable ? "syntactic_package_imports" : "none");
    vp_bool(g, root, "sound", false);
    vp_bool(g, root, "applicable", g->applicable);
    vp_bool(g, root, "verification_available", g->verification_ready);
    vp_bool(g, root, "broad_verification_required", g->applicable);
    yyjson_mut_val *changed = vp_array(g), *languages = vp_array(g), *missing_tools = vp_array(g),
                   *modules = vp_array(g), *packages = vp_array(g), *edges = vp_array(g),
                   *affected = vp_array(g), *dependents = vp_array(g), *reasons = vp_array(g),
                   *limitations = vp_array(g), *stages = vp_array(g), *python = vp_object(g),
                   *python_targets = vp_array(g);
    vp_value(g, root, "changed_paths", changed);
    vp_value(g, root, "languages", languages);
    vp_value(g, root, "missing_tools", missing_tools);
    vp_value(g, root, "modules", modules);
    vp_value(g, root, "packages", packages);
    vp_value(g, root, "edges", edges);
    vp_value(g, root, "affected_packages", affected);
    vp_value(g, root, "reverse_dependents", dependents);
    vp_value(g, root, "fallback_reasons", reasons);
    vp_value(g, root, "limitations", limitations);
    vp_value(g, root, "python", python);
    vp_value(g, root, "stages", stages);
    if (g->go_applicable)
        vp_append_string(g, languages, "go");
    if (g->python_applicable)
        vp_append_string(g, languages, "python");
    if (g->python_applicable && !g->python_executable)
        vp_append_string(g, missing_tools, "python");
    vp_bool(g, python, "applicable", g->python_applicable);
    vp_bool(g, python, "executable_available", g->python_executable != NULL);
    vp_string(g, python, "executable", g->python_executable ? g->python_executable : "");
    vp_string(g, python, "selection_kind", "changed_files_and_filename_related_tests");
    vp_string(g, python, "syntax_scope", g->python_broad_syntax ? "all_indexed" : "changed");
    vp_number(g, python, "source_file_count", (uint64_t)g->python_file_count);
    vp_number(g, python, "syntax_file_count", (uint64_t)g->python_syntax_count);
    vp_number(g, python, "test_file_count", (uint64_t)g->python_test_count);
    vp_number(g, python, "targeted_test_count", (uint64_t)g->python_target_count);
    vp_bool(g, python, "syntax_scheduled", g->verification_ready && g->python_syntax_count != 0);
    vp_bool(g, python, "tests_applicable", g->python_test_count != 0 || g->python_pytest);
    vp_bool(g, python, "tests_scheduled",
            g->verification_ready && (g->python_test_count != 0 || g->python_pytest));
    vp_string(g, python, "test_runner",
              !g->python_test_count && !g->python_pytest ? "none"
              : g->python_pytest                         ? "pytest"
                                                         : "unittest");
    vp_bool(g, python, "bytecode_writes_disabled", true);
    vp_bool(g, python, "pytest_cache_disabled", g->python_pytest);
    vp_value(g, python, "targeted_test_files", python_targets);
    for (size_t i = 0; i < g->python_target_count; i++)
        vp_append_string(g, python_targets, g->python_targets[i]);
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
    if (g->go_applicable) {
        vp_append_string(
            g, limitations,
            "Package imports are syntactic, not type-resolved references or test coverage.");
        vp_append_string(g, limitations,
                         "Imports from production and test files are unioned; test-only changes "
                         "also include reverse dependents conservatively.");
        vp_append_string(
            g, limitations,
            "Only indexed workspace files and module directives are resolved; generated code, "
            "embed inputs, replacement aliases, vendored/external modules and runtime "
            "dependencies may escape the graph.");
        vp_append_string(
            g, limitations,
            "Broad tests cover each indexed module only in the active Go environment; other tags, "
            "platforms, toolchains, integration services and modules outside the workspace need "
            "separate validation.");
        vp_append_string(
            g, limitations,
            "The compile check uses go test -run ^$ and may execute package initialization and "
            "TestMain. All Go checks require process permission and may access caches or the "
            "network.");
    }
    if (g->python_applicable) {
        vp_append_string(
            g, limitations,
            "Python has no structural dependency graph; related tests are selected only by "
            "test_<module>.py and <module>_test.py basenames, then broad verification is "
            "required.");
        vp_append_string(
            g, limitations,
            "pytest selection uses indexed configuration, conftest files, pytest imports and "
            "filename conventions; undeclared plugins, environments and services need separate "
            "validation.");
        vp_append_string(
            g, limitations,
            "Python syntax checks compile source without importing code. Python runs use -B and "
            "pytest disables its cache provider, but project tests may still mutate inputs and "
            "must be rejected if the validation snapshot changes.");
        vp_append_string(
            g, limitations,
            "Unittest file paths use the standard loader's path-to-module conversion; a local "
            "test namespace that collides with an installed regular package may need pytest or "
            "explicit project configuration.");
    }
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
    for (size_t i = 0; i < g->python_target_count; i++)
        free(g->python_targets[i]);
    for (size_t i = 0; i < g->python_test_count; i++)
        free(g->python_tests[i]);
    for (size_t i = 0; i < g->reason_count; i++)
        free(g->reasons[i].path);
    free(g->changes);
    free(g->python_targets);
    free(g->python_tests);
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
    g.go_applicable = fg_go_graph_applicable(g.graph);
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
    if (!vp_python_discover(&g))
        goto done;
    g.applicable = g.go_applicable || g.python_applicable;
    g.verification_ready =
        g.applicable &&
        (!g.python_applicable || (g.python_executable != NULL && g.python_file_count != 0 &&
                                  (g.python_test_count != 0 || g.python_pytest)));
    if (g.go_applicable && (!vp_configuration_changes(&g) || !vp_reachability(&g)))
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
