#include "internal.h"
#include "forge/validation.h"
#include "repo/repo_internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

/* Fixtures are outside the checkout: git ls-files must not accidentally index
 * an enclosing repository. No Go executable, model, or GPU is used by these tests. */
typedef struct {
    char root[FG_PATH_MAX];
    char *files[256], *directories[256];
    size_t file_count, directory_count;
    forge_repo *repo;
    forge_error error;
} fixture;
static void fixture_start(fixture *f) {
    memset(f, 0, sizeof(*f));
    char base[FG_PATH_MAX], random[33], name[64];
#ifdef _WIN32
    DWORD n = GetTempPathA((DWORD)sizeof(base), base);
    assert(n && n < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
#endif
    assert(fg_random_hex(random, 16));
    snprintf(name, sizeof(name), "forge-validation-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, &f->error));
    f->repo = forge_repo_open(f->root, &f->error);
    assert(f->repo);
}
static void fixture_directory(fixture *f, const char *path) {
    for (size_t i = 0; i < f->directory_count; i++)
        if (!strcmp(f->directories[i], path))
            return;
    assert(f->directory_count < sizeof(f->directories) / sizeof(f->directories[0]));
    assert(fg_mkdir(path, &f->error));
    f->directories[f->directory_count++] = fg_strdup(path);
    assert(f->directories[f->directory_count - 1]);
}
static void fixture_write(fixture *f, const char *relative, const char *text) {
    char full[FG_PATH_MAX];
    assert(fg_path_join(full, f->root, relative));
    for (char *p = full + strlen(f->root) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            fixture_directory(f, full);
            *p = '/';
        }
    }
    bool recorded = false;
    for (size_t i = 0; i < f->file_count; i++)
        recorded |= !strcmp(f->files[i], full);
    if (!recorded) {
        assert(f->file_count < sizeof(f->files) / sizeof(f->files[0]));
        f->files[f->file_count++] = fg_strdup(full);
        assert(f->files[f->file_count - 1]);
    }
    assert(fg_write_file(full, text, strlen(text), &f->error));
}
static void fixture_remove(fixture *f, const char *relative) {
    char full[FG_PATH_MAX];
    assert(fg_path_join(full, f->root, relative));
    assert(remove(full) == 0);
}
static void fixture_index(fixture *f) {
    forge_status status = forge_repo_index(f->repo, &f->error);
    if (status != FORGE_OK)
        fprintf(stderr, "index: %s\n", f->error.message);
    assert(status == FORGE_OK);
}
static void fixture_finish(fixture *f) {
    forge_repo_close(f->repo);
    for (size_t i = 0; i < f->file_count; i++) {
        remove(f->files[i]); /* A deletion test may already have removed the file. */
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i > 0; i--) {
        assert(test_rmdir(f->directories[i - 1]) == 0);
        free(f->directories[i - 1]);
    }
    char path[FG_PATH_MAX];
    static const char *const metadata[] = {".forge/index.db-wal", ".forge/index.db-shm",
                                           ".forge/index.db"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); i++) {
        assert(fg_path_join(path, f->root, metadata[i]));
        remove(path);
    }
    assert(fg_path_join(path, f->root, ".forge"));
    assert(test_rmdir(path) == 0);
    assert(test_rmdir(f->root) == 0);
}
static char *plan_text(fixture *f, const char *const *paths, size_t count) {
    char *text = forge_repo_validation_plan(f->repo, paths, count, &f->error);
    if (!text)
        fprintf(stderr, "plan: %s\n", f->error.message);
    assert(text);
    return text;
}
static yyjson_doc *plan(fixture *f, const char *path) {
    const char *paths[] = {path};
    char *text = plan_text(f, path ? paths : NULL, path ? 1 : 0);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    assert(doc);
    forge_free(text);
    return doc;
}
static bool array_has(yyjson_val *array, const char *text) {
    size_t i, max;
    yyjson_val *value;
    yyjson_arr_foreach(array, i, max, value) {
        const char *s = yyjson_get_str(value);
        if (s && !strcmp(s, text))
            return true;
    }
    return false;
}
static yyjson_val *named(yyjson_val *array, const char *field, const char *name) {
    size_t i, max;
    yyjson_val *value;
    yyjson_arr_foreach(array, i, max, value) {
        const char *s = fg_json_str(value, field);
        if (s && !strcmp(s, name))
            return value;
    }
    return NULL;
}
static yyjson_val *stage_commands(yyjson_val *root, const char *name) {
    yyjson_val *stage = named(yyjson_obj_get(root, "stages"), "name", name);
    assert(stage);
    return yyjson_obj_get(stage, "commands");
}
static yyjson_val *command_for(yyjson_val *root, const char *stage, const char *cwd,
                               const char *target) {
    yyjson_val *array = stage_commands(root, stage);
    size_t i, max;
    yyjson_val *command;
    yyjson_arr_foreach(array, i, max, command) if (!strcmp(fg_json_str(command, "cwd"), cwd) &&
                                                   array_has(yyjson_obj_get(command, "argv"),
                                                             target)) return command;
    return NULL;
}
static yyjson_val *edge_for(yyjson_val *root, const char *from, const char *to) {
    yyjson_val *edges = yyjson_obj_get(root, "edges");
    size_t i, max;
    yyjson_val *edge;
    yyjson_arr_foreach(edges, i, max, edge) if (!strcmp(fg_json_str(edge, "from"), from) &&
                                                !strcmp(fg_json_str(edge, "to"), to)) return edge;
    return NULL;
}
static bool reason(yyjson_val *root, const char *code) {
    return named(yyjson_obj_get(root, "fallback_reasons"), "code", code) != NULL;
}
static const char *base_source = "package lib\nfunc Value() int { return 1 }\n";
static void write_graph(fixture *f) {
    fixture_write(f, "go.mod", "module example.test/root\n\ngo 1.22\n");
    fixture_write(f, "lib/base.go", base_source);
    fixture_write(f, "lib/base_test.go",
                  "package lib_test\nimport (\n _ \"example.test/root/lib\"\n \"testing\"\n)\n"
                  "func TestValue(t *testing.T) {}\n");
    fixture_write(f, "api/api.go",
                  "package api\nimport \"example.test/root/lib\"\nvar Value = lib.Value\n");
    fixture_write(
        f, "cmd/tool/main.go",
        "package main\nimport alias \"example.test/root/api\"\nfunc main() { alias.Value() }\n");
    fixture_write(f, "observer/observer.go", "package observer\n");
    fixture_write(f, "observer/observer_test.go",
                  "package observer_test\nimport (\n _ \"example.test/root/lib\"\n \"testing\"\n)\n"
                  "func TestSmoke(t *testing.T) {}\n");
    fixture_write(f, "unrelated/unused.go", "package unrelated\n");
    fixture_write(f, "nested/go.mod", "module \"other.test/nested\"\n\ngo 1.22\n");
    fixture_write(f, "nested/core/core.go", "package core\nfunc Value() int { return 2 }\n");
    fixture_write(f, "nested/client/client.go",
                  "package client\nimport \"example.test/root/lib\"\nvar Value = lib.Value\n");
    fixture_write(f, "consumer/use.go",
                  "package consumer\nimport \"other.test/nested/core\"\nvar Value = core.Value\n");
}
static void test_no_go_and_input_validation(void) {
    fixture f;
    fixture_start(&f);
    const char *valid[] = {"README.md"};
    assert(!forge_repo_validation_plan(f.repo, valid, 1, &f.error));
    assert(f.error.code == FORGE_ERR_CONFLICT);
    fixture_write(&f, "README.md", "A non-Go repository.\n");
    fixture_index(&f);
    yyjson_doc *doc = plan(&f, "README.md");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(!yyjson_get_bool(yyjson_obj_get(root, "applicable")));
    assert(yyjson_get_uint(yyjson_obj_get(root, "command_count")) == 0);
    yyjson_doc_free(doc);
    static const char *const invalid[] = {
        "../outside.go",     "C:/outside.go",   "/outside.go", "",
        "pkg/../outside.go", ".forge/index.db", "pkg/",        "pkg//x.go"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(!forge_repo_validation_plan(f.repo, &invalid[i], 1, &f.error));
        assert(f.error.code == FORGE_ERR_ARGUMENT);
    }
    assert(!forge_repo_validation_plan(f.repo, valid, 1025, &f.error));
    assert(f.error.code == FORGE_ERR_LIMIT);
    assert(!forge_repo_validation_plan(f.repo, NULL, 1, &f.error));
    assert(f.error.code == FORGE_ERR_ARGUMENT);
    fixture_finish(&f);
}
static void test_dependency_graph_and_stages(void) {
    fixture f;
    fixture_start(&f);
    write_graph(&f);
    fixture_index(&f);
    const char *paths[] = {"lib/base.go"};
    char *first = plan_text(&f, paths, 1);
    const char *duplicates[] = {"./lib/base.go", "lib\\base.go", "lib/base.go"};
    char *second = plan_text(&f, duplicates, 3);
    assert(!strcmp(first, second));
    free(second);
    uint64_t generation = forge_repo_generation(f.repo);
    fixture_index(&f);
    assert(generation == forge_repo_generation(f.repo));
    forge_repo_close(f.repo);
    f.repo = forge_repo_open(f.root, &f.error);
    assert(f.repo);
    second = plan_text(&f, paths, 1);
    assert(!strcmp(first, second));
    free(second);
    yyjson_doc *doc = yyjson_read(first, strlen(first), 0);
    free(first);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_get_bool(yyjson_obj_get(root, "applicable")));
    assert(!yyjson_get_bool(yyjson_obj_get(root, "sound")));
    yyjson_val *affected = yyjson_obj_get(root, "affected_packages");
    assert(yyjson_arr_size(affected) == 1 && array_has(affected, "lib"));
    yyjson_val *dependents = yyjson_obj_get(root, "reverse_dependents");
    assert(yyjson_arr_size(dependents) == 4);
    assert(array_has(dependents, "api") && array_has(dependents, "cmd/tool"));
    assert(array_has(dependents, "observer") && array_has(dependents, "nested/client"));
    assert(!array_has(dependents, "unrelated") && !array_has(dependents, "consumer"));
    assert(yyjson_get_bool(yyjson_obj_get(edge_for(root, "observer", "lib"), "test_only")));
    assert(!yyjson_get_bool(yyjson_obj_get(edge_for(root, "api", "lib"), "test_only")));
    assert(!reason(root, "possible_import_cycle")); /* External self-test import is legal. */
    assert(reason(root, "cross_module_import"));
    yyjson_val *package = named(yyjson_obj_get(root, "packages"), "directory", "nested/core");
    assert(!strcmp(fg_json_str(package, "import_path"), "other.test/nested/core"));
    assert(!strcmp(fg_json_str(package, "module_directory"), "nested"));
    static const char *const order[] = {"format",          "compile", "affected_tests",
                                        "dependent_tests", "vet",     "broad_tests"};
    yyjson_val *stages = yyjson_obj_get(root, "stages");
    assert(yyjson_arr_size(stages) == 6);
    for (size_t i = 0; i < 6; i++) {
        yyjson_val *stage = yyjson_arr_get(stages, i);
        assert(!strcmp(fg_json_str(stage, "name"), order[i]));
        assert(yyjson_get_bool(yyjson_obj_get(stage, "requires_previous_success")) == (i != 0));
    }
    yyjson_val *format = command_for(root, "format", ".", "./lib/base.go");
    assert(format && yyjson_get_bool(yyjson_obj_get(format, "require_empty_stdout")));
    assert(!command_for(root, "format", ".", "./nested/core/core.go"));
    yyjson_val *compile = command_for(root, "compile", ".", "./lib");
    assert(compile && array_has(yyjson_obj_get(compile, "argv"), "^$"));
    assert(array_has(yyjson_obj_get(compile, "argv"), "-vet=off"));
    assert(command_for(root, "affected_tests", ".", "./lib"));
    assert(command_for(root, "dependent_tests", ".", "./cmd/tool"));
    assert(command_for(root, "dependent_tests", "nested", "./client"));
    assert(command_for(root, "vet", ".", "./observer"));
    assert(!command_for(root, "affected_tests", ".", "./unrelated"));
    assert(yyjson_arr_size(stage_commands(root, "broad_tests")) == 2);
    assert(command_for(root, "broad_tests", ".", "./..."));
    assert(command_for(root, "broad_tests", "nested", "./..."));
    yyjson_doc_free(doc);

    doc = plan(&f, "nested/core/core.go");
    root = yyjson_doc_get_root(doc);
    assert(command_for(root, "compile", "nested", "./core"));
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "consumer"));
    assert(!array_has(yyjson_obj_get(root, "reverse_dependents"), "nested/client"));
    yyjson_doc_free(doc);

    doc = plan(&f, "observer/observer_test.go");
    root = yyjson_doc_get_root(doc);
    assert(array_has(yyjson_obj_get(root, "affected_packages"), "observer"));
    assert(!array_has(yyjson_obj_get(root, "reverse_dependents"), "lib"));
    yyjson_doc_free(doc);

    const char *unordered[] = {"nested/core/core.go", "lib/base.go"};
    const char *ordered[] = {"lib/base.go", "nested/core/core.go"};
    first = plan_text(&f, unordered, 2);
    second = plan_text(&f, ordered, 2);
    assert(!strcmp(first, second));
    free(first);
    free(second);
    char *suggestion = fg_repo_targets(f.repo, "lib/base.go", &f.error);
    assert(suggestion && strstr(suggestion, "lib") && strstr(suggestion, "4 reverse dependents"));
    assert(strlen(suggestion) < 600);
    free(suggestion);
    fixture_finish(&f);
}
static void test_deleted_package_and_reindex(void) {
    fixture f;
    fixture_start(&f);
    write_graph(&f);
    fixture_index(&f);
    uint64_t before = forge_repo_generation(f.repo);
    fixture_remove(&f, "lib/base.go");
    fixture_remove(&f, "lib/base_test.go");
    fixture_index(&f);
    assert(forge_repo_generation(f.repo) > before);
    yyjson_doc *doc = plan(&f, "lib/base.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(reason(root, "unindexed_or_deleted_package"));
    yyjson_val *package = named(yyjson_obj_get(root, "packages"), "directory", "lib");
    assert(package && !yyjson_get_bool(yyjson_obj_get(package, "present")));
    assert(yyjson_arr_size(yyjson_obj_get(root, "reverse_dependents")) == 4);
    assert(yyjson_arr_size(stage_commands(root, "format")) == 0);
    assert(yyjson_arr_size(stage_commands(root, "affected_tests")) == 0);
    assert(command_for(root, "dependent_tests", ".", "./api"));
    yyjson_doc_free(doc);
    fixture_write(&f, "lib/base.go", base_source);
    fixture_write(&f, "api/api.go", "package api\nfunc Value() int { return 0 }\n");
    fixture_index(&f);
    doc = plan(&f, "lib/base.go");
    root = yyjson_doc_get_root(doc);
    assert(!array_has(yyjson_obj_get(root, "reverse_dependents"), "api"));
    assert(!array_has(yyjson_obj_get(root, "reverse_dependents"), "cmd/tool"));
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "observer"));
    assert(!reason(root, "unindexed_or_deleted_package"));
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
static void test_conservative_fallbacks(void) {
    fixture f;
    fixture_start(&f);
    write_graph(&f);
    fixture_write(&f, "go.mod",
                  "module example.test/root\nreplace remote.test/lib => ./replacement\n");
    fixture_write(&f, "go.work", "go 1.22\nuse (\n .\n ./nested\n)\n");
    fixture_write(&f, "lib/tagged.go",
                  "//go:build special\n\npackage lib\nimport _ \"example.test/root/unrelated\"\n");
    fixture_write(&f, "lib/platform_windows.go", "package lib\n");
    fixture_write(&f, "cycle/a/a.go", "package a\nimport _ \"example.test/root/cycle/b\"\n");
    fixture_write(&f, "cycle/b/b.go", "package b\nimport _ \"example.test/root/cycle/a\"\n");
    fixture_write(&f, "broken/broken.go", "package broken\nfunc broken( {\n");
    fixture_write(&f, "missing/use.go",
                  "package missing\nimport _ \"example.test/root/not_present\"\n");
    fixture_write(&f, "ffi/ffi.go", "package ffi\nimport \"C\"\n");
    fixture_write(&f, "escape/use.go", "package escape\nimport \"example.test/root/\\x6cib\"\n");
    fixture_write(&f, "testdata/input.go", "invalid Go fixture, not a build package\n");
    fixture_index(&f);
    yyjson_doc *doc = plan(&f, "unrelated/unused.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(reason(root, "build_constraints"));
    assert(reason(root, "module_replacements"));
    assert(reason(root, "go_workspace"));
    assert(reason(root, "go_parse_error"));
    assert(reason(root, "possible_import_cycle"));
    assert(reason(root, "unresolved_local_import"));
    assert(reason(root, "unresolved_import_syntax"));
    assert(reason(root, "cgo_import"));
    /* The tagged import is included even if the host would exclude that file. */
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "lib"));
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "cmd/tool"));
    assert(!named(yyjson_obj_get(root, "packages"), "directory", "testdata"));
    yyjson_doc_free(doc);

    doc = plan(&f, "cycle/a/a.go");
    root = yyjson_doc_get_root(doc);
    assert(yyjson_arr_size(yyjson_obj_get(root, "reverse_dependents")) == 1);
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "cycle/b"));
    yyjson_doc_free(doc);

    doc = plan(&f, "go.mod");
    root = yyjson_doc_get_root(doc);
    assert(reason(root, "module_configuration_changed"));
    assert(array_has(yyjson_obj_get(root, "affected_packages"), "unrelated"));
    assert(!array_has(yyjson_obj_get(root, "affected_packages"), "nested/core"));
    assert(command_for(root, "compile", ".", "./unrelated"));
    yyjson_doc_free(doc);

    doc = plan(&f, "go.work");
    root = yyjson_doc_get_root(doc);
    assert(array_has(yyjson_obj_get(root, "affected_packages"), "nested/core"));
    yyjson_doc_free(doc);

    doc = plan(&f, "testdata/input.go");
    root = yyjson_doc_get_root(doc);
    assert(reason(root, "unassigned_changed_path"));
    assert(yyjson_arr_size(stage_commands(root, "broad_tests")) == 2);
    yyjson_doc_free(doc);

    doc = plan(&f, NULL);
    root = yyjson_doc_get_root(doc);
    assert(reason(root, "no_changed_paths"));
    assert(command_for(root, "format", ".", "./lib/base.go"));
    assert(command_for(root, "format", ".", "./nested/core/core.go"));
    assert(!command_for(root, "format", ".", "./testdata/input.go"));
    assert(command_for(root, "compile", ".", "./lib"));
    assert(command_for(root, "compile", "nested", "./core"));
    assert(command_for(root, "vet", ".", "./lib"));
    assert(command_for(root, "vet", "nested", "./core"));
    assert(yyjson_arr_size(stage_commands(root, "broad_tests")) == 2);
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
static void test_missing_and_duplicate_modules(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "root.go", "package root\n");
    fixture_write(&f, "first/go.mod", "module repeated.test/module\n");
    fixture_write(&f, "first/api.go", "package api\n");
    fixture_write(&f, "second/go.mod", "module repeated.test/module\n");
    fixture_write(&f, "second/api.go", "package api\n");
    fixture_write(&f, "consumer/go.mod", "module consumer.test/module\n");
    fixture_write(&f, "consumer/use.go", "package consumer\nimport _ \"repeated.test/module\"\n");
    fixture_write(&f, "broken/go.mod", "module\n");
    fixture_write(&f, "broken/file.go", "package broken\n");
    fixture_index(&f);
    yyjson_doc *doc = plan(&f, "first/api.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(reason(root, "missing_go_module"));
    assert(reason(root, "duplicate_module_path"));
    assert(reason(root, "unresolved_module_path"));
    assert(edge_for(root, "consumer", "first") && edge_for(root, "consumer", "second"));
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "consumer"));
    assert(command_for(root, "compile", "first", "."));
    assert(command_for(root, "broad_tests", ".", "./..."));
    assert(command_for(root, "broad_tests", "broken", "./..."));
    assert(yyjson_arr_size(stage_commands(root, "broad_tests")) == 5);
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
static void test_batching_and_edge_deduplication(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "go.mod", "module batch.test/module\n");
    fixture_write(&f, "base/base.go", "package base\n");
    for (unsigned i = 0; i < 40; i++) {
        char path[64];
        snprintf(path, sizeof(path), "consumer%02u/use.go", i);
        fixture_write(&f, path, "package consumer\nimport _ \"batch.test/module/base\"\n");
    }
    fixture_write(&f, "consumer00/extra_test.go",
                  "package consumer_test\nimport _ \"batch.test/module/base\"\n");
    fixture_index(&f);
    yyjson_doc *doc = plan(&f, "base/base.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_arr_size(yyjson_obj_get(root, "reverse_dependents")) == 40);
    assert(yyjson_arr_size(yyjson_obj_get(root, "edges")) == 40);
    assert(!yyjson_get_bool(yyjson_obj_get(edge_for(root, "consumer00", "base"), "test_only")));
    assert(yyjson_arr_size(stage_commands(root, "dependent_tests")) == 2);
    for (unsigned i = 0; i < 40; i++) {
        char target[64];
        snprintf(target, sizeof(target), "./consumer%02u", i);
        assert(command_for(root, "dependent_tests", ".", target));
    }
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
static void test_unreadable_module_boundary(void) {
    fixture f;
    fixture_start(&f);
    write_graph(&f);
    size_t length = 2u * 1024u * 1024u + 1u;
    char *oversized = malloc(length + 1);
    assert(oversized);
    memset(oversized, ' ', length);
    oversized[length] = 0;
    fixture_write(&f, "nested/go.mod", oversized);
    free(oversized);
    fixture_index(&f);
    yyjson_doc *doc = plan(&f, "nested/core/core.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(reason(root, "incomplete_go_index"));
    assert(reason(root, "unresolved_module_path"));
    yyjson_val *package = named(yyjson_obj_get(root, "packages"), "directory", "nested/core");
    assert(package && !strcmp(fg_json_str(package, "module_directory"), "nested"));
    assert(yyjson_is_null(yyjson_obj_get(package, "import_path")));
    assert(command_for(root, "compile", "nested", "./core"));
    assert(command_for(root, "broad_tests", "nested", "./..."));
    yyjson_doc_free(doc);
    fixture_write(&f, "nested/go.mod", "module other.test/nested\n");
    fixture_index(&f);
    doc = plan(&f, "nested/core/core.go");
    root = yyjson_doc_get_root(doc);
    assert(!reason(root, "incomplete_go_index"));
    assert(!reason(root, "unresolved_module_path"));
    assert(array_has(yyjson_obj_get(root, "reverse_dependents"), "consumer"));
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
static void test_delta_index_transaction_and_graph(void) {
    fixture f;
    fixture_start(&f);
    const char *paths[] = {"base/base.go", "consumer/use.go", "base\\base.go"};
    assert(forge_repo_index_paths(f.repo, paths, 1, &f.error) == FORGE_ERR_CONFLICT);
    fixture_write(&f, "go.mod", "module delta.test/module\n");
    fixture_write(&f, "base/base.go", "package base\nfunc Before() {}\n");
    fixture_write(&f, "consumer/use.go", "package consumer\nimport _ \"delta.test/module/base\"\n");
    fixture_write(&f, "unrelated.go", "package root\nfunc Untouched() {}\n");
    fixture_index(&f);
    uint64_t generation = forge_repo_generation(f.repo);
    assert(forge_repo_index_paths(f.repo, paths, 3, &f.error) == FORGE_OK);
    assert(forge_repo_generation(f.repo) == generation);
    fixture_write(&f, "base/base.go", "package base\nfunc After() {}\n");
    fixture_write(&f, "unrelated.go", "package root\nfunc NotScanned() {}\n");
    assert(forge_repo_index_paths(f.repo, paths, 3, &f.error) == FORGE_OK);
    assert(forge_repo_generation(f.repo) == generation + 1);
    char *text = forge_repo_inspect(f.repo, "Untouched", 0, &f.error);
    assert(text && strstr(text, "unrelated.go"));
    free(text);
    text = forge_repo_inspect(f.repo, "After", 0, &f.error);
    assert(text && strstr(text, "base/base.go"));
    free(text);
    yyjson_doc *doc = plan(&f, "base/base.go");
    assert(array_has(yyjson_obj_get(yyjson_doc_get_root(doc), "reverse_dependents"), "consumer"));
    yyjson_doc_free(doc);
    generation = forge_repo_generation(f.repo);
    const char *invalid[] = {"base/base.go", "missing/../escape.go"};
    fixture_write(&f, "base/base.go", "package base\nfunc MustNotCommit() {}\n");
    assert(forge_repo_index_paths(f.repo, invalid, 2, &f.error) == FORGE_ERR_POLICY);
    assert(forge_repo_generation(f.repo) == generation);
    text = forge_repo_inspect(f.repo, "After", 0, &f.error);
    assert(text && strstr(text, "base/base.go"));
    free(text);
    assert(forge_repo_index_paths(f.repo, NULL, 0, &f.error) == FORGE_OK && !f.error.code);
    assert(forge_repo_index_paths(f.repo, paths, 4097, &f.error) == FORGE_ERR_LIMIT);
    const char *directory[] = {"base"};
    assert(forge_repo_index_paths(f.repo, directory, 1, &f.error) == FORGE_ERR_ARGUMENT);
    fixture_remove(&f, "base/base.go");
    assert(forge_repo_index_paths(f.repo, paths, 3, &f.error) == FORGE_OK);
    text = forge_repo_inspect(f.repo, "After", 0, &f.error);
    assert(text && strstr(text, "No matching"));
    free(text);
    /* A later full scan still refreshes unrelated data and preserves deltas. */
    fixture_index(&f);
    text = forge_repo_inspect(f.repo, "NotScanned", 0, &f.error);
    assert(text && strstr(text, "unrelated.go"));
    free(text);
    fixture_finish(&f);
}
static void test_malformed_shared_graph(void) {
    fixture f;
    fixture_start(&f);
    fixture_write(&f, "go.mod", "module graph.test/m\n");
    fixture_write(&f, "a.go", "package p\nfunc A() {}\n");
    fixture_index(&f);
    static const char *const corruptions[] = {
        "UPDATE files SET path='x' WHERE path='a.go'", "UPDATE go_files SET is_test=4294967296",
        "UPDATE go_files SET build_constraints=-1", "UPDATE go_files SET parse_error='not a flag'",
        "UPDATE go_files SET package_name=CAST(x'ff' AS TEXT)"};
    for (size_t i = 0; i < sizeof(corruptions) / sizeof(*corruptions); i++) {
        assert(sqlite3_exec(f.repo->db, corruptions[i], NULL, NULL, NULL) == SQLITE_OK);
        char *text = forge_repo_validation_plan(f.repo, NULL, 0, &f.error);
        assert(!text && f.error.code == FORGE_ERR_PARSE);
        assert(sqlite3_get_autocommit(f.repo->db) && !f.repo->snapshot_active);
        assert(
            sqlite3_exec(
                f.repo->db,
                "UPDATE files SET path='a.go' WHERE path='x'; "
                "UPDATE go_files SET is_test=0,build_constraints=0,parse_error=0,package_name='p'",
                NULL, NULL, NULL) == SQLITE_OK);
    }
    yyjson_doc *doc = plan(&f, "a.go");
    assert(command_for(yyjson_doc_get_root(doc), "compile", ".", "."));
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_no_go_and_input_validation();
    test_dependency_graph_and_stages();
    test_deleted_package_and_reindex();
    test_conservative_fallbacks();
    test_missing_and_duplicate_modules();
    test_batching_and_edge_deduplication();
    test_unreadable_module_boundary();
    test_delta_index_transaction_and_graph();
    test_malformed_shared_graph();
    puts("Go validation planner tests passed");
    return 0;
}
