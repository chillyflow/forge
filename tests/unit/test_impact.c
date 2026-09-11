#include "internal.h"
#include "forge/validation.h"
#include "repo/impact.h"
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

typedef struct {
    char root[FG_PATH_MAX];
    char *files[64], *directories[64];
    size_t file_count, directory_count;
    forge_repo *repo;
    forge_error error;
} fixture;
static void start(fixture *f) {
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
    snprintf(name, sizeof(name), "forge-impact-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, &f->error));
    /* A real complete git enumeration is required for the non-fallback case.
     * Empty templates avoid platform/user hook files in this temporary repo. */
    const char *argv[] = {"git", "init", "-q", "--template=", NULL};
    fg_process_result process = {0};
    assert(fg_process(f->root, argv, 10000, 4096, NULL, NULL, &process, &f->error) == FORGE_OK);
    assert(process.exit_code == 0);
    fg_process_free(&process);
    f->repo = forge_repo_open(f->root, &f->error);
    assert(f->repo);
}
static void write_file(fixture *f, const char *relative, const char *text) {
    char full[FG_PATH_MAX];
    assert(fg_path_join(full, f->root, relative));
    for (char *p = full + strlen(f->root) + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        bool found = false;
        for (size_t i = 0; i < f->directory_count; i++)
            found |= !strcmp(f->directories[i], full);
        if (!found) {
            assert(f->directory_count < 64);
            assert(fg_mkdir(full, &f->error));
            f->directories[f->directory_count++] = fg_strdup(full);
        }
        *p = '/';
    }
    bool found = false;
    for (size_t i = 0; i < f->file_count; i++)
        found |= !strcmp(f->files[i], full);
    if (!found) {
        assert(f->file_count < 64);
        f->files[f->file_count++] = fg_strdup(full);
    }
    assert(fg_write_file(full, text, strlen(text), &f->error));
}
static void index_files(fixture *f) {
    assert(forge_repo_index(f->repo, &f->error) == FORGE_OK);
}
static fg_impact_snapshot *capture(fixture *f) {
    index_files(f);
    fg_impact_snapshot *s = fg_impact_snapshot_take(f->repo, 0, NULL, NULL, &f->error);
    if (!s)
        fprintf(stderr, "capture: %s\n", f->error.message);
    assert(s);
    return s;
}
static yyjson_doc *analyze(fixture *f, fg_impact_snapshot *s, bool plan) {
    index_files(f);
    char *text = plan ? fg_repo_validation_plan_impact(f->repo, s, 0, NULL, NULL, &f->error)
                      : fg_impact_analyze(f->repo, s, 0, NULL, NULL, &f->error);
    if (!text)
        fprintf(stderr, "analysis: %s\n", f->error.message);
    assert(text);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(text);
    assert(doc);
    return doc;
}
static void finish(fixture *f) {
    forge_repo_close(f->repo);
    for (size_t i = 0; i < f->file_count; i++) {
        remove(f->files[i]);
        free(f->files[i]);
    }
    for (size_t i = f->directory_count; i; i--) {
        assert(test_rmdir(f->directories[i - 1]) == 0);
        free(f->directories[i - 1]);
    }
    const char *metadata[] = {".forge/index.db-wal", ".forge/index.db-shm", ".forge/index.db"};
    char path[FG_PATH_MAX];
    for (size_t i = 0; i < 3; i++) {
        assert(fg_path_join(path, f->root, metadata[i]));
        remove(path);
    }
    assert(fg_path_join(path, f->root, ".forge"));
    assert(test_rmdir(path) == 0);
    const char *git_files[] = {".git/HEAD", ".git/config", ".git/description"};
    for (size_t i = 0; i < sizeof(git_files) / sizeof(git_files[0]); i++) {
        assert(fg_path_join(path, f->root, git_files[i]));
        remove(path);
    }
    const char *git_dirs[] = {
        ".git/refs/heads",   ".git/refs/tags", ".git/refs", ".git/objects/info",
        ".git/objects/pack", ".git/objects",   ".git"};
    for (size_t i = 0; i < sizeof(git_dirs) / sizeof(git_dirs[0]); i++) {
        assert(fg_path_join(path, f->root, git_dirs[i]));
        assert(test_rmdir(path) == 0);
    }
    assert(test_rmdir(f->root) == 0);
}
static yyjson_val *named(yyjson_val *array, const char *key, const char *name) {
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(array, i, n, v) {
        const char *s = fg_json_str(v, key);
        if (s && !strcmp(s, name))
            return v;
    }
    return NULL;
}
static yyjson_val *stage(yyjson_val *root, const char *name) {
    yyjson_val *v = named(yyjson_obj_get(root, "stages"), "name", name);
    assert(v);
    return v;
}
static bool has_arg(yyjson_val *commands, const char *arg) {
    size_t i, n;
    yyjson_val *v;
    yyjson_arr_foreach(commands, i, n, v) {
        yyjson_val *args = yyjson_obj_get(v, "argv");
        for (size_t j = 0; j < yyjson_arr_size(args); j++)
            if (!strcmp(yyjson_get_str(yyjson_arr_get(args, j)), arg))
                return true;
    }
    return false;
}
static bool reason(yyjson_val *root, const char *code) {
    return named(yyjson_obj_get(root, "fallback_reasons"), "code", code) != NULL;
}
static void go_fixture(fixture *f) {
    write_file(f, "go.mod", "module example.test/app\n\ngo 1.22\n");
    write_file(f, "lib/value.go", "package lib\nfunc Value() int { return 1 }\n");
    write_file(f, "lib/wrap.go", "package lib\nfunc Wrap() int { return Value() }\n");
    write_file(f, "lib/value_test.go",
               "package lib\nimport \"testing\"\n"
               "func TestValue(t *testing.T) { if Wrap()!=2 { t.Fatal(\"bad\") } }\n"
               "func TestUnrelated(t *testing.T) {}\n");
    write_file(
        f, "client/client.go",
        "package client\nimport \"example.test/app/lib\"\nfunc Read() int { return lib.Wrap() }\n");
    write_file(f, "client/client_test.go",
               "package client\nimport \"testing\"\nfunc TestClient(t *testing.T) { Read() }\n");
    write_file(f, "unrelated/other.go", "package unrelated\nfunc Other() int { return 3 }\n");
    write_file(f, "unrelated/other_test.go",
               "package unrelated\nimport \"testing\"\nfunc TestOther(t *testing.T) { Other() }\n");
}
static void test_go_callers_and_reverse_tests(void) {
    fixture f;
    start(&f);
    go_fixture(&f);
    fg_impact_snapshot *baseline = capture(&f);
    write_file(&f, "lib/value.go", "package lib\nfunc Value() int { return 2 }\n");
    yyjson_doc *doc = analyze(&f, baseline, true);
    yyjson_val *root = yyjson_doc_get_root(doc),
               *impact = yyjson_obj_get(root, "structural_impact");
    assert(!yyjson_get_bool(yyjson_obj_get(impact, "fallback")));
    assert(!yyjson_get_bool(yyjson_obj_get(impact, "sound")));
    assert(yyjson_get_bool(yyjson_obj_get(root, "broad_verification_required")));
    assert(named(yyjson_obj_get(impact, "changed_symbols"), "name", "Value"));
    assert(named(yyjson_obj_get(impact, "caller_candidates"), "name", "Wrap"));
    assert(named(yyjson_obj_get(impact, "caller_candidates"), "name", "TestValue"));
    yyjson_val *tests = yyjson_obj_get(impact, "targeted_tests");
    assert(named(tests, "name", "TestValue"));
    assert(named(tests, "name", "TestClient"));
    assert(!named(tests, "name", "TestOther"));
    assert(!named(tests, "name", "TestUnrelated"));
    yyjson_val *affected = yyjson_obj_get(stage(root, "affected_tests"), "commands");
    yyjson_val *dependent = yyjson_obj_get(stage(root, "dependent_tests"), "commands");
    assert(has_arg(affected, "^(TestValue)$"));
    assert(has_arg(affected, "./lib"));
    assert(has_arg(dependent, "^(TestClient)$"));
    assert(has_arg(dependent, "./client"));
    assert(!has_arg(affected, "./unrelated") && !has_arg(dependent, "./unrelated"));
    const char *paths[] = {"lib/value.go"};
    char *ordinary = forge_repo_validation_plan(f.repo, paths, 1, &f.error);
    assert(ordinary);
    yyjson_doc *ordinary_doc = yyjson_read(ordinary, strlen(ordinary), 0);
    char *broad = yyjson_val_write(stage(root, "broad_tests"), 0, NULL);
    char *control_broad =
        yyjson_val_write(stage(yyjson_doc_get_root(ordinary_doc), "broad_tests"), 0, NULL);
    assert(!strcmp(broad, control_broad));
    free(broad);
    free(control_broad);
    free(ordinary);
    yyjson_doc_free(ordinary_doc);
    yyjson_doc_free(doc);
    fg_impact_snapshot_destroy(baseline);
    finish(&f);
}
static void test_renamed_deleted_and_external_changes(void) {
    fixture f;
    start(&f);
    go_fixture(&f);
    fg_impact_snapshot *baseline = capture(&f);
    write_file(&f, "lib/value.go", "package lib\nfunc Renamed() int { return 2 }\n");
    yyjson_doc *doc = analyze(&f, baseline, true);
    yyjson_val *impact = yyjson_obj_get(yyjson_doc_get_root(doc), "structural_impact");
    assert(reason(impact, "removed_or_renamed_symbol"));
    yyjson_val *removed = named(yyjson_obj_get(impact, "changed_symbols"), "name", "Value");
    assert(!strcmp(fg_json_str(removed, "change"), "removed"));
    assert(named(yyjson_obj_get(impact, "caller_candidates"), "resolution",
                 "possible_broken_reference_to_removed_name"));
    yyjson_doc_free(doc);
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f.root, "lib/value.go"));
    assert(remove(path) == 0);
    doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "deleted_file"));
    yyjson_doc_free(doc);
    write_file(&f, "lib/value.go",
               "package lib\nimport \"fmt\"\nfunc Value() int { fmt.Println(2); return 2 }\n");
    doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "outside_declaration_change"));
    yyjson_doc_free(doc);
    write_file(&f, "lib/value.go", "package lib\nfunc Value() int { return 2 }\n");
    write_file(&f, "README.md", "changed input\n");
    doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "unsupported_change"));
    yyjson_doc_free(doc);
    fg_impact_snapshot_destroy(baseline);
    finish(&f);
}
static void test_python_candidates_and_dynamic_fallback(void) {
    fixture f;
    start(&f);
    write_file(&f, "calc.py", "def calculate():\n    return 1\n");
    write_file(&f, "caller.py",
               "from calc import calculate\ndef wrapped():\n    return calculate()\n");
    write_file(&f, "test_feature.py",
               "from caller import wrapped\nimport unittest\nclass Case(unittest.TestCase):\n"
               "    def test_feature(self):\n        self.assertEqual(wrapped(), 2)\n");
    write_file(&f, "dynamic.py", "def dynamic(obj, name):\n    return getattr(obj, name)\n");
    fg_impact_snapshot *baseline = capture(&f);
    write_file(&f, "calc.py", "def calculate():\n    return 2\n");
    yyjson_doc *doc = analyze(&f, baseline, true);
    yyjson_val *root = yyjson_doc_get_root(doc),
               *impact = yyjson_obj_get(root, "structural_impact");
    assert(reason(impact, "python_structure_unresolved"));
    assert(reason(impact, "python_dynamic_behavior"));
    assert(named(yyjson_obj_get(impact, "changed_symbols"), "name", "calculate"));
    assert(named(yyjson_obj_get(impact, "caller_candidates"), "name", "wrapped"));
    assert(named(yyjson_obj_get(impact, "targeted_tests"), "path", "test_feature.py"));
    assert(yyjson_get_bool(yyjson_obj_get(root, "broad_verification_required")));
    assert(yyjson_arr_size(yyjson_obj_get(stage(root, "broad_tests"), "commands")) > 0);
    yyjson_doc_free(doc);
    fg_impact_snapshot_destroy(baseline);
    finish(&f);
}
static void test_ambiguous_and_dynamic_go(void) {
    fixture f;
    start(&f);
    go_fixture(&f);
    fg_impact_snapshot *baseline = capture(&f);
    write_file(&f, "lib/value.go", "package lib\nfunc Value() int { return 2 }\n");
    write_file(&f, "lib/duplicate.go", "package lib\nfunc Value() int { return 3 }\n");
    yyjson_doc *doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "ambiguous_symbol"));
    yyjson_doc_free(doc);
    write_file(
        &f, "lib/duplicate.go",
        "package lib\nimport \"reflect\"\nfunc Type() interface{} { return reflect.TypeOf(2) }\n");
    doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "go_dynamic_or_generated_behavior"));
    yyjson_doc_free(doc);
    fg_impact_snapshot_destroy(baseline);
    finish(&f);
}
static bool cancelled(void *unused) {
    (void)unused;
    return true;
}
static void test_disabled_and_bounds(void) {
    fixture f;
    start(&f);
    go_fixture(&f);
    fg_impact_snapshot *baseline = capture(&f);
    char *ordinary = forge_repo_validation_plan(f.repo, NULL, 0, &f.error);
    char *disabled = fg_repo_validation_plan_impact(f.repo, NULL, 0, NULL, NULL, &f.error);
    assert(ordinary && disabled && !strcmp(ordinary, disabled));
    free(ordinary);
    free(disabled);
    yyjson_doc *doc = analyze(&f, baseline, false);
    assert(reason(yyjson_doc_get_root(doc), "no_indexed_changes"));
    yyjson_doc_free(doc);
    assert(!fg_impact_snapshot_take(f.repo, 0, cancelled, NULL, &f.error));
    assert(f.error.code == FORGE_ERR_CANCELLED);
    assert(!fg_impact_analyze(f.repo, baseline, 1, NULL, NULL, &f.error));
    assert(f.error.code == FORGE_ERR_LIMIT);
    assert(!fg_impact_analyze(f.repo, NULL, 0, NULL, NULL, &f.error));
    assert(f.error.code == FORGE_ERR_ARGUMENT);
    fg_impact_snapshot_destroy(baseline);
    finish(&f);
}
int main(void) {
    test_go_callers_and_reverse_tests();
    test_renamed_deleted_and_external_changes();
    test_python_candidates_and_dynamic_fallback();
    test_ambiguous_and_dynamic_go();
    test_disabled_and_bounds();
    puts("structural impact tests passed");
    return 0;
}
