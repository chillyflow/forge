#include "internal.h"
#include "core/input_snapshot.h"
#include "core/semantic_state.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

typedef struct {
    char root[FG_PATH_MAX];
    char *files[32];
    size_t count;
} fixture;
static void start(fixture *f) {
    memset(f, 0, sizeof(*f));
    char base[FG_PATH_MAX], name[96], nonce[33], canonical[FG_PATH_MAX];
#ifdef _WIN32
    DWORD length = GetTempPathA((DWORD)sizeof(base), base);
    assert(length && length < sizeof(base));
#else
    const char *temp = getenv("TMPDIR");
    snprintf(base, sizeof(base), "%s", temp && *temp ? temp : "/tmp");
#endif
    assert(fg_random_hex(nonce, 16));
    snprintf(name, sizeof(name), "forge-semantic-%s", nonce);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, NULL));
    assert(fg_workspace(f->root, canonical, NULL));
    strcpy(f->root, canonical);
}
static void write_bytes(fixture *f, const char *name, const char *text, size_t length) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    bool known = false;
    for (size_t i = 0; i < f->count; i++)
        known = known || !strcmp(f->files[i], path);
    if (!known) {
        assert(f->count < sizeof(f->files) / sizeof(*f->files));
        f->files[f->count++] = fg_strdup(path);
        assert(f->files[f->count - 1]);
    }
    assert(fg_write_file(path, text, length, NULL));
}
static void write_text(fixture *f, const char *name, const char *text) {
    write_bytes(f, name, text, strlen(text));
}
static void remove_file(fixture *f, const char *name) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, name));
    assert(remove(path) == 0);
}
static void finish(fixture *f) {
    for (size_t i = 0; i < f->count; i++) {
        remove(f->files[i]);
        free(f->files[i]);
    }
#ifdef _WIN32
    assert(RemoveDirectoryA(f->root));
#else
    assert(rmdir(f->root) == 0);
#endif
}
static fg_semantic_state *take(fixture *f) {
    forge_error error = {0};
    fg_semantic_state *state =
        fg_semantic_state_take(f->root, 32, 8u * 1024 * 1024, NULL, NULL, 0, &error);
    if (!state)
        fprintf(stderr, "semantic state: %s\n", error.message);
    assert(state);
    assert(fg_semantic_state_describe(state).complete);
    return state;
}
static void compare_source(const char *name, const char *a, const char *b, bool equal,
                           bool canonical) {
    fixture f;
    start(&f);
    write_text(&f, name, a);
    fg_semantic_state *before = take(&f);
    write_text(&f, name, b);
    fg_semantic_state *after = take(&f);
    assert(fg_semantic_state_equal(before, after) == equal);
    assert((fg_semantic_state_hash(before) == fg_semantic_state_hash(after)) == equal);
    fg_semantic_state_info info = fg_semantic_state_describe(before);
    if (info.files != 1 || info.canonical_files != (canonical ? 1u : 0u))
        fprintf(stderr, "Unexpected source mode for %s (canonical=%zu expected=%d): %s\n", name,
                info.canonical_files, (int)canonical, a);
    assert(info.files == 1 && info.canonical_files == (canonical ? 1u : 0u));
    fg_semantic_state_destroy(before);
    fg_semantic_state_destroy(after);
    finish(&f);
}
static void source_cases(void) {
    compare_source(
        "a.go", "package p\nfunc f() int { return 1 } // first\n",
        "// rephrased patch explanation\npackage p\n\nfunc f() int { return 1 } // next\n", true,
        true);
    compare_source("a.go", "package p\nfunc f() int { return 1+2 }\n",
                   "package   p\n  func f() int { return 1 + 2 }\n", true, true);
    compare_source("a.go", "package p\nvar x = 1.2\n", "package p\nvar x = 1 . 2\n", false, true);
    compare_source("a.go", "package p\nvar x = 1\n", "package p\nvar x = 2\n", false, true);
    compare_source("a.go", "package p\nvar x = \"// first\"\n", "package p\nvar x = \"// next\"\n",
                   false, true);
    compare_source("a.go", "package p\nvar x = `/*first*/\nvalue`\n",
                   "package p\nvar x = `/*next*/\nvalue`\n", false, true);
    compare_source("a.go", "package p\nfunc f() { return /* same line */ 1 }\n",
                   "package p\nfunc f() { return /* new\nline */ 1 }\n", false, true);
    compare_source("a.go", "//go:build linux\npackage p\n", "//go:build windows\npackage p\n",
                   false, false);
    compare_source("a.go", "package p\nvar x = \"unterminated\n// a\n",
                   "package p\nvar x = \"unterminated\n// b\n", false, false);
    compare_source("a.py", "def f():\n    x=1 # first\n    return x\n",
                   "# note\ndef f():\n    x = 1 # rephrased\n\n    return x\n", true, true);
    compare_source("a.py", "if flag:\n    x = 1\n    y = 2\n", "if flag:\n    x = 1\ny = 2\n",
                   false, true);
    compare_source("a.py", "if flag:\n\tx = 1\n", "if flag:\n    x = 1\n", false, true);
    compare_source("a.py", "def f():\n    \"\"\"# first\ndocstring\"\"\"\n    return 1\n",
                   "def f():\n    \"\"\"# next\ndocstring\"\"\"\n    return 1\n", false, true);
    compare_source("a.py", "x = '# first' # comment\n", "x = '# first' # changed\n", true, true);
    compare_source("a.py", "x = f'{value}' # first\n", "x = f'{value}' # next\n", false, false);
    compare_source("a.py", "# coding: ascii\nx = 1\n", "# coding: utf-8\nx = 1\n", false, false);
    compare_source("a.c", "int f() { return 1; } // first\n", "   int f() { return 1; } // next\n",
                   true, true);
    compare_source("a.c", "const char *s = \"/* first */\";\n", "const char *s = \"/* next */\";\n",
                   false, true);
    compare_source("a.c", "#define F(x) (x)\n", "#define F (x) (x)\n", false, false);
    compare_source("a.c", "int line = __LINE__; // first\n", "int line = __LINE__; // next\n",
                   false, false);
    compare_source("a.rs", "fn f() { let x = 1; } // first\n", "fn f() { let x = 1; } // next\n",
                   true, true);
    compare_source("a.rs", "/// first\nfn f() {}\n", "/// next\nfn f() {}\n", false, false);
    compare_source("a.rs", "fn f() { let x = r#\"// first\"#; }\n",
                   "fn f() { let x = r#\"// next\"#; }\n", false, false);
    compare_source("a.ts", "const x = 1; // first\n", "const x = 1; // next\n", true, true);
    compare_source("a.ts", "function f() { return\nx; }\n", "function f() { return x; }\n", false,
                   true);
    compare_source("a.ts", "const x = /a/; // first\n", "const x = /a/; // next\n", false, false);
    compare_source("a.ts", "const x = `// first`;\n", "const x = `// next`;\n", false, false);
    compare_source("config.toml", "value = 1 # first\n", "value = 1 # next\n", false, false);
}
static void workspace_cases(void) {
    fixture f;
    start(&f);
    write_text(&f, "a.go", "package p\n");
    fg_semantic_state *first = take(&f);
    write_text(&f, "b.go", "package p\n");
    fg_semantic_state *added = take(&f);
    assert(!fg_semantic_state_equal(first, added));
    remove_file(&f, "a.go");
    fg_semantic_state *renamed = take(&f);
    assert(!fg_semantic_state_equal(first, renamed));
    remove_file(&f, "b.go");
    write_text(&f, "a.go", "package p\n");
    fg_semantic_state *restored = take(&f);
    assert(fg_semantic_state_equal(first, restored));
    fg_semantic_state_destroy(first);
    fg_semantic_state_destroy(added);
    fg_semantic_state_destroy(renamed);
    fg_semantic_state_destroy(restored);
    finish(&f);
}
static bool always_cancel(void *unused) {
    (void)unused;
    return true;
}
static void incomplete_cases(void) {
    fixture f;
    start(&f);
    write_text(&f, "a.go", "package p\n");
    write_text(&f, "b.go", "package p\n");
    forge_error error = {0};
    assert(!fg_semantic_state_take(f.root, 1, 1024, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    assert(!fg_semantic_state_take(f.root, 8, 1, NULL, NULL, 0, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    assert(!fg_semantic_state_take(f.root, 8, 1024, always_cancel, NULL, 0, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    assert(!fg_semantic_state_take(f.root, 8, 1024, NULL, NULL, fg_now_ms(), &error));
    assert(error.code == FORGE_ERR_LIMIT);
    assert(!fg_semantic_state_take("not-a-semantic-workspace", 8, 1024, NULL, NULL, 0, &error));
    assert(!fg_semantic_state_equal(NULL, NULL));
    assert(!fg_semantic_state_hash(NULL));
    assert(!fg_semantic_state_describe(NULL).complete);
    finish(&f);
}
typedef struct {
    size_t chunks, finals, empty, bytes;
    char current[FG_PATH_MAX];
    bool reject;
} visitor;
static bool visit(const char *path, const void *bytes, size_t length, bool final, void *user,
                  forge_error *error) {
    visitor *v = user;
    if (v->reject) {
        fg_error(error, FORGE_ERR_POLICY, "Test observer rejected input");
        return false;
    }
    if (v->current[0])
        assert(!strcmp(v->current, path));
    else
        strcpy(v->current, path);
    if (final) {
        assert(!bytes && !length);
        if (!strcmp(path, "empty"))
            v->empty++;
        v->finals++;
        v->current[0] = 0;
    } else {
        assert(bytes && length);
        v->chunks++;
        v->bytes += length;
    }
    return true;
}
static void visitor_cases(void) {
    fixture f;
    start(&f);
    char *large = malloc(70000);
    assert(large);
    memset(large, 'x', 70000);
    write_bytes(&f, "large", large, 70000);
    write_text(&f, "empty", "");
    free(large);
    visitor v = {0};
    forge_error error = {0};
    fg_input_snapshot *observed =
        fg_input_snapshot_take_visit(f.root, 8, 100000, NULL, NULL, 0, visit, &v, &error);
    fg_input_snapshot *ordinary = fg_input_snapshot_take(f.root, 8, 100000, NULL, NULL, 0, &error);
    assert(observed && ordinary && fg_input_snapshot_equal(observed, ordinary));
    assert(v.chunks >= 2 && v.finals == 2 && v.empty == 1 && v.bytes == 70000);
    fg_input_snapshot_destroy(observed);
    fg_input_snapshot_destroy(ordinary);
    v.reject = true;
    assert(!fg_input_snapshot_take_visit(f.root, 8, 100000, NULL, NULL, 0, visit, &v, &error));
    assert(error.code == FORGE_ERR_POLICY);
    finish(&f);
}
static void diagnostic_cases(void) {
    fg_semantic_diagnostic a = {.tool = "go-test",
                                .severity = "error",
                                .path = "pkg/a.go",
                                .code = "assert-equal",
                                .symbol = "TestBalance",
                                .identity = "assert-equal:actual=1:expected=2",
                                .message = "expected 2, got 1"};
    fg_semantic_diagnostic b = a;
    b.message = "balance was 1 rather than 2";
    b.path = "pkg\\a.go";
    uint64_t first, second;
    assert(fg_semantic_diagnostic_fingerprint(&a, 1, true, &first, NULL));
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first == second);
    b.identity = "assert-equal:actual=3:expected=2";
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first != second);
    b = a;
    b.severity = "warning";
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first != second);
    a.identity = b.identity = NULL;
    a.message = "expected 2, got 1";
    b = a;
    b.message = "  expected\t2,  got 1\n";
    assert(fg_semantic_diagnostic_fingerprint(&a, 1, true, &first, NULL));
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first == second);
    b.message = "expected 2, got 3";
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first != second);
    fg_semantic_diagnostic ordered[] = {a, b}, reversed[] = {b, a};
    assert(fg_semantic_diagnostic_fingerprint(ordered, 2, true, &first, NULL));
    assert(fg_semantic_diagnostic_fingerprint(reversed, 2, true, &second, NULL));
    assert(first == second);
    assert(!fg_semantic_diagnostic_fingerprint(&a, 1, false, &second, NULL) && second == 0);
    assert(!fg_semantic_diagnostic_fingerprint(NULL, 0, true, &second, NULL));
    a.message = "expected 'a  b'";
    b = a;
    b.message = "expected 'a b'";
    assert(fg_semantic_diagnostic_fingerprint(&a, 1, true, &first, NULL));
    assert(fg_semantic_diagnostic_fingerprint(&b, 1, true, &second, NULL));
    assert(first != second);
}
int main(void) {
    source_cases();
    workspace_cases();
    incomplete_cases();
    visitor_cases();
    diagnostic_cases();
    puts("semantic repository-state evidence passed");
    return 0;
}
