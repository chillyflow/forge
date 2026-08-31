#include "repo/repo_internal.h"
#include "core/digest.h"
#include "forge/summary.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

/* Real SQLite, Tree-sitter and shared graph. Literal text and explicit model
 * seams test storage/generation control, never actual model quality. */
typedef struct {
    char root[FG_PATH_MAX];
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
    snprintf(name, sizeof(name), "forge-summary-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, &f->error));
    char canonical[FG_PATH_MAX];
    assert(fg_workspace(f->root, canonical, &f->error));
    strcpy(f->root, canonical);
    f->repo = forge_repo_open(f->root, &f->error);
    assert(f->repo);
}
static void erase(const char *root, const char *path) {
    size_t length = strlen(root);
    assert(!strncmp(root, path, length) && (!path[length] || path[length] == '/'));
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    assert(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT));
    char pattern[FG_PATH_MAX];
    assert(fg_path_join(pattern, path, "*"));
    WIN32_FIND_DATAA entry;
    HANDLE iterator = FindFirstFileA(pattern, &entry);
    assert(iterator != INVALID_HANDLE_VALUE);
    do {
        if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, ".."))
            continue;
        char child[FG_PATH_MAX];
        assert(fg_path_join(child, path, entry.cFileName));
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            erase(root, child);
        else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            assert(RemoveDirectoryA(child));
        else
            assert(DeleteFileA(child));
    } while (FindNextFileA(iterator, &entry));
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    FindClose(iterator);
#else
    DIR *dir = opendir(path);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[FG_PATH_MAX];
        struct stat info;
        assert(fg_path_join(child, path, entry->d_name));
        assert(lstat(child, &info) == 0);
        if (S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode))
            erase(root, child);
        else
            assert(unlink(child) == 0);
    }
    closedir(dir);
#endif
    assert(test_rmdir(path) == 0);
}
static void finish(fixture *f) {
    forge_repo_close(f->repo);
    const char *base = strrchr(f->root, '/');
    const char *backslash = strrchr(f->root, '\\');
    if (backslash && (!base || backslash > base))
        base = backslash;
    assert(base && !strncmp(base + 1, "forge-summary-", 14) && strlen(base + 1) == 46);
    char canonical[FG_PATH_MAX];
    assert(fg_workspace(f->root, canonical, NULL) && !strcmp(canonical, f->root));
    erase(f->root, f->root);
}
static void write_source(fixture *f, const char *relative, const char *source) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    for (char *p = path + strlen(f->root) + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            assert(fg_mkdir(path, &f->error));
            *p = '/';
        }
    assert(fg_write_file(path, source, strlen(source), &f->error));
}
static void remove_source(fixture *f, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    assert(remove(path) == 0);
}
static void index_repo(fixture *f) {
    forge_status status = forge_repo_index(f->repo, &f->error);
    if (status != FORGE_OK)
        fprintf(stderr, "index: %s\n", f->error.message);
    assert(status == FORGE_OK);
}
static void db(fixture *f, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(f->repo->db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        fprintf(stderr, "database: %s\n", error ? error : "unknown");
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}
static uint64_t scalar(fixture *f, const char *sql) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(f->repo->db, sql, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    uint64_t value = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return value;
}
static forge_summary_input *prepare(fixture *f, forge_summary_scope scope, const char *path,
                                    const char *symbol, const forge_summary_options *options) {
    forge_summary_target target = {0};
    target.scope = scope;
    target.path = path;
    target.symbol = symbol;
    forge_summary_input *input = forge_repo_summary_prepare(f->repo, &target, options, &f->error);
    if (!input)
        fprintf(stderr, "prepare %s: %s\n", path ? path : ".", f->error.message);
    assert(input && !f->error.code);
    return input;
}
static forge_summary_view view(forge_summary_input *input) {
    forge_summary_view v;
    assert(forge_summary_input_get(input, &v));
    return v;
}
static forge_summary_store_result store(fixture *f, forge_summary_input *input, const char *text) {
    forge_summary_store_result result;
    forge_status status =
        forge_repo_summary_store(f->repo, input, text, strlen(text), &result, &f->error);
    if (status != FORGE_OK)
        fprintf(stderr, "store: %s\n", f->error.message);
    assert(status == FORGE_OK && !f->error.code);
    return result;
}
static void sources(fixture *f) {
    write_source(f, "go.mod", "module example.test/m\n\ngo 1.22\n");
    write_source(f, "p/a.go",
                 "package p\nimport \"example.test/m/q\"\nfunc Alpha() int { return q.Value() }\n");
    write_source(
        f, "p/a_test.go",
        "package p_test\nimport \"example.test/m/p\"\nfunc ExampleAlpha() { p.Alpha() }\n");
    write_source(f, "q/q.go", "package q\nfunc Value() int { return 3 }\n");
    write_source(f, "README.md", "# Example fixture\n");
    index_repo(f);
}
static void test_sha256(void) {
    static const struct {
        const char *text, *hash;
    } vectors[] = {{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
                   {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
                   {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"}};
    char digest[65];
    for (size_t i = 0; i < sizeof(vectors) / sizeof(*vectors); i++) {
        assert(fg_sha256_hex(vectors[i].text, strlen(vectors[i].text), digest));
        assert(!strcmp(digest, vectors[i].hash));
        fg_sha256 state;
        fg_sha256_init(&state);
        for (const char *p = vectors[i].text; *p; p++)
            assert(fg_sha256_update(&state, p, 1));
        assert(fg_sha256_finish_hex(&state, digest));
        assert(!strcmp(digest, vectors[i].hash));
        assert(!fg_sha256_update(&state, "x", 1));
    }
    fg_sha256 state;
    fg_sha256_init(&state);
    char block[1000];
    memset(block, 'a', sizeof(block));
    for (size_t i = 0; i < 1000; i++)
        assert(fg_sha256_update(&state, block, sizeof(block)));
    assert(fg_sha256_finish_hex(&state, digest));
    assert(!strcmp(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    fg_sha256_init(&state);
    state.bytes = UINT64_MAX / 8;
    assert(!fg_sha256_update(&state, "a", 1));
    assert(!fg_sha256_finish_hex(&state, digest));
    assert(!fg_sha256_hex(NULL, 1, digest));
    assert(fg_sha256_hex(NULL, 0, digest) && fg_sha256_valid_hex(digest));
    assert(!fg_sha256_valid_hex(NULL) && !fg_sha256_valid_hex("abc"));
    fg_sha256_init(&state);
    assert(fg_sha256_field(&state, "ab", 2) && fg_sha256_field(&state, "c", 1));
    assert(fg_sha256_finish_hex(&state, digest));
    char other[65];
    fg_sha256_init(&state);
    assert(fg_sha256_field(&state, "a", 1) && fg_sha256_field(&state, "bc", 2));
    assert(fg_sha256_finish_hex(&state, other) && strcmp(digest, other));
}
static void test_scopes_and_cache(void) {
    fixture f;
    start(&f);
    sources(&f);
    uint64_t generation = forge_repo_generation(f.repo);
    static const forge_summary_scope scopes[] = {FORGE_SUMMARY_REPOSITORY, FORGE_SUMMARY_MODULE,
                                                 FORGE_SUMMARY_PACKAGE, FORGE_SUMMARY_FILE,
                                                 FORGE_SUMMARY_SYMBOL};
    const char *paths[] = {".", ".", "p", "p/a.go", "p/a.go"};
    for (size_t i = 0; i < sizeof(scopes) / sizeof(*scopes); i++) {
        const char *symbol = scopes[i] == FORGE_SUMMARY_SYMBOL ? "Alpha" : NULL;
        forge_summary_input *first = prepare(&f, scopes[i], paths[i], symbol, NULL);
        forge_summary_input *same = prepare(&f, scopes[i], paths[i], symbol, NULL);
        forge_summary_view a = view(first), b = view(same);
        assert(a.cache_status == FORGE_SUMMARY_MISS && !a.text && a.dependencies);
        assert(!strcmp(a.cache_key, b.cache_key) && !strcmp(a.prompt, b.prompt));
        assert(a.generation == generation && !a.tokens_known);
        bool full_source = scopes[i] >= FORGE_SUMMARY_FILE;
        assert((strstr(a.prompt, "Selected evidence mode: full source.") != NULL) == full_source);
        assert((strstr(a.prompt, "source bodies and non-Go content are omitted.") != NULL) ==
               !full_source);
        assert(fg_sha256_valid_hex(a.recipe_hash) && fg_sha256_valid_hex(a.dependency_hash));
        const char *text = "Fixture summary: caf\xc3\xa9 \xf0\x9f\x98\x80";
        assert(!store(&f, first, text).reused);
        assert(store(&f, same, "Different second writer").reused);
        assert(forge_repo_generation(f.repo) == generation);
        assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == generation);
        forge_summary_input *hit = prepare(&f, scopes[i], paths[i], symbol, NULL);
        forge_summary_view h = view(hit);
        assert(h.cache_status == FORGE_SUMMARY_HIT && !strcmp(h.text, text));
        assert(h.created_generation == generation && h.validated_generation == generation);
        char *json = forge_summary_input_json(hit, &f.error);
        assert(json);
        yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
        assert(doc && !strcmp(fg_json_str(yyjson_doc_get_root(doc), "text"), text));
        yyjson_doc *manifest = yyjson_read(h.manifest_json, strlen(h.manifest_json), 0);
        assert(manifest);
        assert(yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(manifest), "dependencies")) ==
               h.dependencies);
        yyjson_doc_free(manifest);
        yyjson_doc_free(doc);
        free(json);
        forge_summary_input_destroy(first);
        forge_summary_input_destroy(same);
        forge_summary_input_destroy(hit);
    }
    assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 5);
    forge_summary_options options = forge_default_summary_options();
    options.recipe_id = "other-template-and-profile";
    forge_summary_input *different = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
    assert(view(different).cache_status == FORGE_SUMMARY_MISS);
    forge_summary_input_destroy(different);
    options = forge_default_summary_options();
    options.evidence = FORGE_SUMMARY_FULL_SOURCE;
    different = prepare(&f, FORGE_SUMMARY_PACKAGE, "p", NULL, &options);
    assert(view(different).cache_status == FORGE_SUMMARY_MISS);
    assert(strstr(view(different).prompt, "return q.Value()"));
    assert(strstr(view(different).prompt, "Selected evidence mode: full source."));
    forge_summary_input_destroy(different);
    finish(&f);
}
static void assert_idle(fixture *f) {
    assert(sqlite3_get_autocommit(f->repo->db));
    assert(!f->repo->snapshot_active && !f->repo->index_scope_active);
}
static void expect_prepare_error(fixture *f, forge_summary_scope scope, const char *path,
                                 const char *symbol, const forge_summary_options *options,
                                 forge_status expected) {
    forge_summary_target target = {0};
    target.scope = scope;
    target.path = path;
    target.symbol = symbol;
    forge_summary_input *input = forge_repo_summary_prepare(f->repo, &target, options, &f->error);
    if (input || f->error.code != expected)
        fprintf(stderr, "expected prepare error %d, got %d: %s\n", expected, f->error.code,
                f->error.message);
    assert(!input && f->error.code == expected);
    assert_idle(f);
}
static void expect_store_error(fixture *f, forge_summary_input *input, const char *text,
                               size_t length, forge_status expected) {
    forge_summary_store_result result;
    memset(&result, 0xff, sizeof(result));
    forge_status status =
        forge_repo_summary_store(f->repo, input, text, length, &result, &f->error);
    if (status != expected)
        fprintf(stderr, "expected store error %d, got %d: %s\n", expected, status,
                f->error.message);
    assert(status == expected && f->error.code == expected);
    assert(!result.generation && !result.evicted_entries && !result.reused &&
           !result.repaired_corruption);
    assert_idle(f);
}
static void test_dependency_invalidation(void) {
    fixture f;
    start(&f);
    sources(&f);
    forge_summary_input *file = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    forge_summary_input *symbol = prepare(&f, FORGE_SUMMARY_SYMBOL, "p/a.go", "Alpha", NULL);
    store(&f, file, "file evidence");
    store(&f, symbol, "declaration evidence");
    write_source(&f, "q/q.go", "package q\nfunc Value() int { return 999 }\n");
    index_repo(&f);
    forge_summary_input *next = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    assert(view(next).cache_status == FORGE_SUMMARY_HIT);
    assert(!strcmp(view(file).cache_key, view(next).cache_key));
    assert(view(next).generation > view(file).generation);
    forge_summary_store_result result = store(&f, file, "discarded second writer");
    assert(result.reused && result.generation == view(next).generation);
    forge_summary_input_destroy(next);

    /* Only indexed evidence participates, never an implicit read of live files. */
    const char *with_sibling = "package p\nimport \"example.test/m/q\"\n"
                               "func Alpha() int { return q.Value() }\n"
                               "func Beta() int { return 7 }\n";
    write_source(&f, "p/a.go", with_sibling);
    next = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    assert(view(next).cache_status == FORGE_SUMMARY_HIT && !strstr(view(next).prompt, "Beta"));
    forge_summary_input_destroy(next);
    index_repo(&f);
    next = prepare(&f, FORGE_SUMMARY_SYMBOL, "p/a.go", "Alpha", NULL);
    assert(view(next).cache_status == FORGE_SUMMARY_HIT);
    assert(!strcmp(view(symbol).cache_key, view(next).cache_key));
    assert(!strstr(view(next).prompt, "Beta"));
    forge_summary_input_destroy(next);
    next = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    assert(view(next).cache_status == FORGE_SUMMARY_MISS && strstr(view(next).prompt, "Beta"));
    assert(strcmp(view(next).cache_key, view(file).cache_key));
    expect_store_error(&f, file, "stale", 5, FORGE_ERR_CONFLICT);
    forge_summary_input_destroy(next);

    write_source(&f, "p/a.go",
                 "package p\nimport \"example.test/m/q\"\n"
                 "func Alpha() int { return 2 * q.Value() }\n");
    index_repo(&f);
    next = prepare(&f, FORGE_SUMMARY_SYMBOL, "p/a.go", "Alpha", NULL);
    assert(view(next).cache_status == FORGE_SUMMARY_MISS);
    assert(strcmp(view(next).cache_key, view(symbol).cache_key));
    expect_store_error(&f, symbol, "stale", 5, FORGE_ERR_CONFLICT);
    store(&f, next, "changed declaration");
    remove_source(&f, "p/a.go");
    index_repo(&f);
    expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, FORGE_ERR_NOT_FOUND);
    expect_store_error(&f, next, "deleted", 7, FORGE_ERR_CONFLICT);
    forge_summary_input_destroy(next);
    forge_summary_input_destroy(file);
    forge_summary_input_destroy(symbol);
    finish(&f);
}
static void test_membership_and_recipe_keys(void) {
    fixture f;
    start(&f);
    sources(&f);
    forge_summary_input *package = prepare(&f, FORGE_SUMMARY_PACKAGE, "p", NULL, NULL);
    store(&f, package, "package before addition");
    write_source(&f, "p/b.go", "package p\nfunc Beta() {}\n");
    index_repo(&f);
    forge_summary_input *added = prepare(&f, FORGE_SUMMARY_PACKAGE, "p", NULL, NULL);
    assert(view(added).cache_status == FORGE_SUMMARY_MISS && strstr(view(added).prompt, "p/b.go"));
    assert(strcmp(view(package).cache_key, view(added).cache_key));
    expect_store_error(&f, package, "stale", 5, FORGE_ERR_CONFLICT);
    remove_source(&f, "p/b.go");
    index_repo(&f);
    forge_summary_input *restored = prepare(&f, FORGE_SUMMARY_PACKAGE, "p", NULL, NULL);
    assert(view(restored).cache_status == FORGE_SUMMARY_HIT);
    assert(!strcmp(view(package).cache_key, view(restored).cache_key));
    forge_summary_input_destroy(restored);
    forge_summary_input_destroy(added);
    forge_summary_input_destroy(package);
    forge_summary_input *file = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    store(&f, file, "old module metadata");
    write_source(&f, "go.mod", "module example.test/m\n\ngo 1.23\n");
    index_repo(&f);
    added = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    assert(strcmp(view(file).cache_key, view(added).cache_key));
    expect_store_error(&f, file, "stale", 5, FORGE_ERR_CONFLICT);
    forge_summary_input_destroy(file);
    file = added;
    for (size_t i = 0; i < 3; i++) {
        forge_summary_options options = forge_default_summary_options();
        if (!i)
            options.recipe_id = "new recipe";
        else if (i == 1)
            options.producer_id = "new model and profile";
        else
            options.instructions = "Different summary instructions.";
        added = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
        assert(strcmp(view(file).recipe_hash, view(added).recipe_hash));
        assert(strcmp(view(file).cache_key, view(added).cache_key));
        forge_summary_input_destroy(added);
    }
    forge_summary_input_destroy(file);
    finish(&f);
}
static void test_corruption_and_publication_rollback(void) {
    fixture f;
    start(&f);
    sources(&f);
    forge_summary_input *input = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    store(&f, input, "verified fixture summary");
    static const char *const corruptions[] = {
        "UPDATE summary_cache SET content='tampered'",
        "UPDATE summary_cache SET manifest='{}'",
        "UPDATE summary_cache SET content_hash='bad'",
        "UPDATE summary_cache SET recipe_hash='bad'",
        "UPDATE summary_cache SET dependency_hash='bad'",
        "UPDATE summary_cache SET version=2",
        "UPDATE summary_cache SET version=4294967297",
        "UPDATE summary_cache SET validated_generation=9223372036854775807",
        "UPDATE summary_cache SET created_generation=-1",
        "UPDATE summary_cache SET content=CAST(x'610062' AS TEXT)",
        "UPDATE summary_cache SET content=CAST(x'ff' AS TEXT)"};
    for (size_t i = 0; i < sizeof(corruptions) / sizeof(*corruptions); i++) {
        db(&f, corruptions[i]);
        forge_summary_input *bad = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
        if (view(bad).cache_status != FORGE_SUMMARY_CORRUPT)
            fprintf(stderr, "unrejected corruption: %s\n", corruptions[i]);
        assert(view(bad).cache_status == FORGE_SUMMARY_CORRUPT && !view(bad).text);
        forge_summary_store_result result = store(&f, bad, "repaired fixture summary");
        assert(result.repaired_corruption && !result.reused);
        forge_summary_input_destroy(bad);
    }
    db(&f, "DELETE FROM summary_cache");
    uint64_t generation = forge_repo_generation(f.repo);
    static const char *const triggers[] = {
        "CREATE TRIGGER break_summary BEFORE INSERT ON summary_cache BEGIN SELECT "
        "RAISE(ABORT,'fixture'); END",
        "CREATE TRIGGER break_summary AFTER INSERT ON summary_cache BEGIN UPDATE summary_cache SET "
        "content='corrupt'; END",
        "CREATE TRIGGER break_summary AFTER INSERT ON summary_cache BEGIN UPDATE meta SET "
        "value=value+1 WHERE key='generation'; END",
        "CREATE TRIGGER break_summary AFTER INSERT ON summary_cache BEGIN UPDATE chunks SET "
        "content='corrupted indexed source'; END",
        "CREATE TRIGGER break_summary AFTER INSERT ON summary_cache BEGIN DELETE FROM symbols; "
        "END"};
    for (size_t i = 0; i < sizeof(triggers) / sizeof(*triggers); i++) {
        db(&f, triggers[i]);
        expect_store_error(&f, input, "new summary", 11, i ? FORGE_ERR_POLICY : FORGE_ERR_IO);
        assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 0);
        assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == generation);
        db(&f, "DROP TRIGGER break_summary");
    }
    assert(!store(&f, input, "after rollback").reused);
    db(&f, "UPDATE file_digests SET sha256='bad' WHERE file_id=(SELECT id FROM files WHERE "
           "path='p/a.go')");
    expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, FORGE_ERR_PARSE);
    forge_summary_input_destroy(input);
    finish(&f);
}
typedef struct {
    size_t calls, after;
} cancel_state;
static bool cancel_at(void *userdata) {
    cancel_state *state = userdata;
    return ++state->calls >= state->after;
}
static size_t count_bytes(const char *text, void *userdata) {
    (void)userdata;
    return strlen(text);
}
static void test_limits_interruptions_and_eviction(void) {
    fixture f;
    start(&f);
    sources(&f);
    forge_summary_options options;
    for (size_t i = 0; i < 6; i++) {
        options = forge_default_summary_options();
        if (!i)
            options.max_input_bytes = 1;
        else if (i == 1)
            options.max_manifest_bytes = 1;
        else if (i == 2)
            options.max_source_bytes = 1;
        else if (i == 3)
            options.max_dependencies = 1;
        else if (i == 4)
            options.max_vm_steps = 1;
        else
            options.deadline_ms = fg_now_ms() - 1;
        expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options, FORGE_ERR_LIMIT);
    }
    options = forge_default_summary_options();
    options.max_input_tokens = 1;
    expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options, FORGE_ERR_ARGUMENT);
    options.count_tokens = count_bytes;
    expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options, FORGE_ERR_LIMIT);
    options.max_input_tokens = 65536;
    options.max_summary_tokens = 5;
    forge_summary_input *input = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
    assert(view(input).tokens_known && view(input).input_tokens == view(input).input_bytes);
    expect_store_error(&f, input, "too long", 8, FORGE_ERR_LIMIT);
    expect_store_error(&f, input, "a\0b", 3, FORGE_ERR_PARSE);
    expect_store_error(&f, input, "\xff", 1, FORGE_ERR_PARSE);
    expect_store_error(&f, input, "", 0, FORGE_ERR_ARGUMENT);
    char untrusted_length = 'x';
    expect_store_error(&f, input, &untrusted_length, FORGE_SUMMARY_MAX_TEXT_BYTES + 1,
                       FORGE_ERR_LIMIT);
    store(&f, input, "five");
    forge_summary_input_destroy(input);
    db(&f, "DELETE FROM summary_cache");
    options = forge_default_summary_options();
    cancel_state cancel = {0, 1};
    options.cancelled = cancel_at;
    options.userdata = &cancel;
    expect_prepare_error(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options, FORGE_ERR_CANCELLED);
    cancel = (cancel_state){0, SIZE_MAX};
    input = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
    cancel = (cancel_state){0, 1};
    expect_store_error(&f, input, "cancelled", 9, FORGE_ERR_CANCELLED);
    assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 0);
    forge_summary_input_destroy(input);
    for (size_t i = 1; i < 8; i++) {
        cancel = (cancel_state){0, i * 4};
        expect_prepare_error(&f, FORGE_SUMMARY_REPOSITORY, ".", NULL, &options,
                             FORGE_ERR_CANCELLED);
    }
    options = forge_default_summary_options();
    options.max_cache_entries = 2;
    const char *paths[] = {"p/a.go", "p/a_test.go", "q/q.go"};
    for (size_t i = 0; i < 3; i++) {
        input = prepare(&f, FORGE_SUMMARY_FILE, paths[i], NULL, &options);
        forge_summary_store_result result = store(&f, input, "bounded cache");
        assert(result.evicted_entries == (i == 2 ? 1 : 0));
        forge_summary_input_destroy(input);
    }
    input = prepare(&f, FORGE_SUMMARY_FILE, paths[0], NULL, NULL);
    assert(view(input).cache_status == FORGE_SUMMARY_MISS);
    forge_summary_input_destroy(input);
    input = prepare(&f, FORGE_SUMMARY_FILE, paths[1], NULL, NULL);
    assert(view(input).cache_status == FORGE_SUMMARY_HIT);
    forge_summary_input_destroy(input);
    options.max_cache_bytes = 1;
    input = prepare(&f, FORGE_SUMMARY_FILE, paths[0], NULL, &options);
    expect_store_error(&f, input, "cannot fit", 10, FORGE_ERR_LIMIT);
    assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 2);
    forge_summary_input_destroy(input);
    finish(&f);
}
typedef struct {
    forge_repo *other;
    bool applied;
} concurrent_index;
static size_t index_during_count(const char *text, void *userdata) {
    concurrent_index *state = userdata;
    if (!state->applied) {
        forge_error error = {0};
        const char *paths[] = {"p/a.go"};
        assert(forge_repo_index_paths(state->other, paths, 1, &error) == FORGE_OK);
        state->applied = true;
    }
    return strlen(text);
}
static int reject_summary_commit(void *userdata) {
    (*(size_t *)userdata)++;
    return 1;
}
static void test_snapshot_isolation_and_rejection(void) {
    fixture f;
    start(&f);
    sources(&f);
    uint64_t generation = forge_repo_generation(f.repo);
    forge_repo *other = forge_repo_open(f.root, &f.error);
    assert(other);
    write_source(&f, "p/a.go", "package p\nfunc NewIndexedVersion() {}\n");
    concurrent_index concurrent = {other, false};
    forge_summary_options options = forge_default_summary_options();
    options.count_tokens = index_during_count;
    options.count_userdata = &concurrent;
    forge_summary_input *old = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
    assert(concurrent.applied && view(old).generation == generation);
    assert(strstr(view(old).prompt, "Alpha") && !strstr(view(old).prompt, "NewIndexedVersion"));
    expect_store_error(&f, old, "stale indexed snapshot", 22, FORGE_ERR_CONFLICT);
    forge_summary_input_destroy(old);
    forge_summary_input *current = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL);
    assert(view(current).generation == generation + 1);
    assert(strstr(view(current).prompt, "NewIndexedVersion"));
    size_t commits = 0;
    sqlite3_commit_hook(f.repo->db, reject_summary_commit, &commits);
    expect_store_error(&f, current, "rejected", 8, FORGE_ERR_IO);
    sqlite3_commit_hook(f.repo->db, NULL, NULL);
    assert(commits == 1 && scalar(&f, "SELECT count(*) FROM summary_cache") == 0);
    store(&f, current, "accepted after rejection");
    forge_summary_input_destroy(current);
    options = forge_default_summary_options();
    /* Preparation shares this per-operation option. Allow it to finish on
     * coarse Windows clocks/loaded runners before testing the blocked writer. */
    options.timeout_ms = 500;
    current = prepare(&f, FORGE_SUMMARY_FILE, "p/a.go", NULL, &options);
    assert(sqlite3_exec(other->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    expect_store_error(&f, current, "blocked writer", 14, FORGE_ERR_LIMIT);
    assert(sqlite3_exec(other->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    forge_summary_input_destroy(current);

    forge_error outer_error = {0};
    fg_repo_snapshot snapshot = {0};
    assert(fg_repo_snapshot_begin(f.repo, &snapshot, false, 0, NULL, NULL, 1000000, &outer_error) ==
           FORGE_OK);
    forge_summary_target target = {0};
    target.scope = FORGE_SUMMARY_FILE;
    target.path = "p/a.go";
    assert(!forge_repo_summary_prepare(f.repo, &target, NULL, &f.error));
    assert(f.error.code == FORGE_ERR_CONFLICT && f.repo->snapshot_active);
    assert(forge_repo_index(f.repo, &f.error) == FORGE_ERR_CONFLICT);
    assert(fg_repo_snapshot_end(&snapshot, true, &outer_error) == FORGE_OK);
    assert_idle(&f);
    forge_repo_close(other);
    finish(&f);
}
typedef struct {
    fixture *fixture;
    forge_repo *writer;
    const forge_summary_input *competitor;
    size_t calls, counts;
    const char *text;
    size_t text_length;
    int mutation; /* 1: dependency, 2: unrelated, 3: delete dependency. */
    bool cancel, cancel_after, fail, exhaust, reenter, bad_counter, wait_deadline;
} model_fixture;
static bool generation_cancelled(void *userdata) {
    return ((model_fixture *)userdata)->cancel;
}
static size_t generation_count(forge_model *model, const char *text) {
    model_fixture *state = model->backend;
    state->counts++;
    assert(model->operation_active);
    if (state->reenter) {
        forge_metrics metrics;
        forge_error error = {0};
        assert(forge_complete(model, "reentry", 1, NULL, NULL, &metrics, &error) ==
               FORGE_ERR_CONFLICT);
        assert(model->operation_active);
        state->reenter = false;
    }
    return state->bad_counter ? SIZE_MAX : (strlen(text) + 3) / 4;
}
static forge_status generate_fixture(forge_model *model, const char *prompt, const char *grammar,
                                     const fg_decode_policy *policy, size_t max_tokens,
                                     forge_token_fn token, void *userdata, char **out,
                                     forge_metrics *metrics, forge_cancel_fn cancel,
                                     void *cancel_data, uint64_t deadline, forge_error *error) {
    model_fixture *state = model->backend;
    fixture *f = state->fixture;
    assert(model->operation_active && !model->cache_request && !grammar && !policy);
    assert(!f->repo->snapshot_active && sqlite3_get_autocommit(f->repo->db));
    assert(strstr(prompt, "Evidence:") && token);
    state->calls++;
    metrics->simulated = true;
    metrics->prompt_tokens = generation_count(model, prompt);
    metrics->prefill_tokens = metrics->prompt_tokens;
    metrics->generated_tokens = state->exhaust ? max_tokens : 3;
    if (state->competitor) {
        forge_summary_store_result result;
        assert(forge_repo_summary_store(f->repo, state->competitor, "competing winner", 16, &result,
                                        error) == FORGE_OK);
        assert(!result.reused);
    }
    if (state->mutation) {
        const char *path = state->mutation == 2 ? "q/q.go" : "p/a.go";
        if (state->mutation == 3)
            remove_source(f, path);
        else
            write_source(f, path,
                         state->mutation == 2 ? "package q\nfunc Unrelated() {}\n"
                                              : "package p\nfunc Changed() {}\n");
        assert(forge_repo_index_paths(state->writer, &path, 1, error) == FORGE_OK);
    }
    if (state->wait_deadline) {
        assert(deadline && deadline >= fg_now_ms());
        while (fg_now_ms() < deadline) {
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
#endif
        }
    }
    if (state->fail)
        return fg_error(error, FORGE_ERR_MODEL, "explicit model fixture failure");
    size_t length = state->text_length ? state->text_length : strlen(state->text);
    /* Split every byte, including inside UTF-8 characters. */
    for (size_t i = 0; i < length; i++)
        if (!token(state->text + i, 1, userdata))
            return fg_error(error, FORGE_ERR_CANCELLED, "fixture token rejected");
    *out = malloc(length + 1);
    assert(*out);
    memcpy(*out, state->text, length);
    (*out)[length] = 0;
    if (state->cancel_after)
        state->cancel = true;
    (void)cancel;
    (void)cancel_data;
    return FORGE_OK;
}
static forge_model generation_model(model_fixture *state) {
    forge_model model = {0};
    model.config = forge_default_model_config();
    model.backend = state;
    model.count = generation_count;
    model.generate = generate_fixture;
    return model;
}
static forge_summary_options generation_options(model_fixture *state) {
    forge_summary_options options = forge_default_summary_options();
    options.producer_id = "explicit simulated summary producer v1";
    options.timeout_ms = 0;
    options.cancelled = generation_cancelled;
    options.userdata = state;
    return options;
}
static forge_summary_input *generate(fixture *f, forge_model *model,
                                     const forge_summary_target *target,
                                     const forge_summary_options *options,
                                     forge_summary_generation_stats *stats, forge_status expected) {
    forge_summary_input *input =
        forge_repo_summary_generate(f->repo, model, target, options, 256, stats, &f->error);
    if (f->error.code != expected)
        fprintf(stderr, "generate: wanted %d, got %d: %s\n", expected, f->error.code,
                f->error.message);
    assert(f->error.code == expected && (input != NULL) == (expected == FORGE_OK));
    assert(!model->operation_active);
    assert_idle(f);
    return input;
}
static void test_model_generation_and_reuse(void) {
    fixture f;
    start(&f);
    sources(&f);
    model_fixture state = {0};
    state.fixture = &f;
    state.text = "model fixture caf\xc3\xa9";
    state.reenter = true;
    forge_model model = generation_model(&state);
    forge_summary_options options = generation_options(&state);
    forge_summary_generation_stats stats;
    const forge_summary_target targets[] = {
        {FORGE_SUMMARY_REPOSITORY, ".", NULL, NULL, false, 0},
        {FORGE_SUMMARY_MODULE, ".", NULL, NULL, false, 0},
        {FORGE_SUMMARY_PACKAGE, "p", NULL, NULL, false, 0},
        {FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, false, 0},
        {FORGE_SUMMARY_SYMBOL, "p/a.go", "Alpha", NULL, false, 0}};
    for (size_t i = 0; i < sizeof(targets) / sizeof(*targets); i++) {
        forge_summary_input *first = generate(&f, &model, &targets[i], &options, &stats, FORGE_OK);
        assert(stats.prepared && !stats.cache_hit && stats.model_calls == 1 && stats.published);
        assert(stats.initial_cache_status == FORGE_SUMMARY_MISS && stats.inference.simulated);
        assert(view(first).cache_status == FORGE_SUMMARY_HIT &&
               !strcmp(view(first).text, state.text));
        size_t calls = state.calls;
        forge_summary_input *hit = generate(&f, &model, &targets[i], &options, &stats, FORGE_OK);
        assert(stats.cache_hit && !stats.model_calls && !stats.published && state.calls == calls);
        assert(!stats.inference.prompt_tokens && !stats.inference.generated_tokens);
        assert(!strcmp(view(first).cache_key, view(hit).cache_key));
        assert(view(hit).tokens_known && view(hit).input_tokens == (view(hit).input_bytes + 3) / 4);
        forge_summary_input_destroy(first);
        forge_summary_input_destroy(hit);
    }
    forge_summary_target target = targets[3];
    forge_summary_input *saved = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    uint64_t generation = view(saved).generation;
    write_source(&f, "q/q.go", "package q\nfunc Another() {}\n");
    index_repo(&f);
    forge_summary_input *hit = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    assert(stats.cache_hit && view(hit).generation == generation + 1);
    assert(!strcmp(view(saved).cache_key, view(hit).cache_key));
    forge_summary_input_destroy(hit);
    db(&f, "UPDATE summary_cache SET content='corrupted'");
    hit = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    assert(stats.initial_cache_status == FORGE_SUMMARY_CORRUPT && stats.store.repaired_corruption);
    forge_summary_input_destroy(hit);
    db(&f, "DELETE FROM summary_cache");
    state.competitor = saved;
    hit = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    assert(stats.model_calls == 1 && stats.store.reused && stats.published);
    assert(!strcmp(view(hit).text, "competing winner"));
    forge_summary_input_destroy(hit);
    state.competitor = NULL;
    model.config.seed++;
    hit = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    assert(stats.model_calls == 1 && strcmp(view(hit).cache_key, view(saved).cache_key));
    forge_summary_input_destroy(hit);
    forge_summary_input_destroy(saved);
    finish(&f);
}
static void test_generation_dependency_races(void) {
    for (int mutation = 1; mutation <= 3; mutation++) {
        fixture f;
        start(&f);
        sources(&f);
        model_fixture state = {0};
        state.fixture = &f;
        state.text = "generated from the earlier input";
        state.mutation = mutation;
        state.writer = forge_repo_open(f.root, &f.error);
        assert(state.writer);
        forge_model model = generation_model(&state);
        forge_summary_options options = generation_options(&state);
        forge_summary_target target = {FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, false, 0};
        forge_summary_generation_stats stats;
        uint64_t generation = forge_repo_generation(f.repo);
        forge_summary_input *input = generate(&f, &model, &target, &options, &stats,
                                              mutation == 2 ? FORGE_OK : FORGE_ERR_CONFLICT);
        assert(stats.model_calls == 1 && stats.inference.generated_tokens == 3);
        assert(stats.published == (mutation == 2));
        assert(scalar(&f, "SELECT count(*) FROM summary_cache") == (mutation == 2 ? 1u : 0u));
        if (input)
            assert(view(input).generation == generation + 1);
        forge_summary_input_destroy(input);
        forge_repo_close(state.writer);
        finish(&f);
    }
}
static void test_generation_failures(void) {
    fixture f;
    start(&f);
    sources(&f);
    for (size_t test = 0; test < 15; test++) {
        model_fixture state = {0};
        state.fixture = &f;
        state.text = "generated fixture";
        forge_model model = generation_model(&state);
        forge_summary_options options = generation_options(&state);
        forge_summary_target target = {FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, false, 0};
        forge_summary_generation_stats stats;
        forge_status expected = FORGE_ERR_LIMIT;
        if (test == 0) {
            options.producer_id = "caller";
            expected = FORGE_ERR_ARGUMENT;
        } else if (test == 1) {
            options.count_tokens = count_bytes;
            expected = FORGE_ERR_ARGUMENT;
        } else if (test == 2)
            options.max_input_tokens = 1;
        else if (test == 3) {
            state.bad_counter = true;
            expected = FORGE_ERR_LIMIT;
        } else if (test == 4)
            options.max_summary_bytes = 4;
        else if (test == 5)
            options.max_summary_tokens = 1;
        else if (test == 6) {
            state.text = "a\0b";
            state.text_length = 3;
            expected = FORGE_ERR_PARSE;
        } else if (test == 7) {
            state.text = "\xff";
            expected = FORGE_ERR_PARSE;
        } else if (test == 8) {
            state.text = "";
            expected = FORGE_ERR_PARSE;
        } else if (test == 9) {
            state.fail = true;
            expected = FORGE_ERR_MODEL;
        } else if (test == 10) {
            state.cancel = true;
            expected = FORGE_ERR_CANCELLED;
        } else if (test == 11) {
            state.cancel_after = true;
            expected = FORGE_ERR_CANCELLED;
        } else if (test == 12)
            state.exhaust = true;
        else if (test == 13) {
            options.timeout_ms = 1000;
            state.wait_deadline = true;
        } else
            options.max_cache_bytes = 1;
        assert(!generate(&f, &model, &target, &options, &stats, expected));
        assert(!stats.cache_hit && !stats.published && stats.model_calls == state.calls);
        if (state.calls)
            assert(stats.inference.generated_tokens > 0 && stats.inference.simulated);
        assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 0);
    }
    model_fixture state = {0};
    state.fixture = &f;
    state.text = "must not survive rejected publication";
    forge_model model = generation_model(&state);
    forge_summary_options options = generation_options(&state);
    forge_summary_target target = {FORGE_SUMMARY_FILE, "p/a.go", NULL, NULL, false, 0};
    forge_summary_generation_stats stats;
    model.operation_active = true;
    assert(!forge_repo_summary_generate(f.repo, &model, &target, &options, 256, &stats, &f.error));
    assert(f.error.code == FORGE_ERR_CONFLICT && model.operation_active && !state.calls);
    model.operation_active = false;
    size_t rejected = 0;
    sqlite3_commit_hook(f.repo->db, reject_summary_commit, &rejected);
    assert(!generate(&f, &model, &target, &options, &stats, FORGE_ERR_IO));
    sqlite3_commit_hook(f.repo->db, NULL, NULL);
    assert(rejected == 1 && stats.model_calls == 1 && !stats.published);
    assert(scalar(&f, "SELECT count(*) FROM summary_cache") == 0);
    forge_summary_input *retry = generate(&f, &model, &target, &options, &stats, FORGE_OK);
    assert(stats.published && !stats.cache_hit);
    forge_summary_input_destroy(retry);
    finish(&f);
}
int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_sha256();
    test_scopes_and_cache();
    test_dependency_invalidation();
    test_membership_and_recipe_keys();
    test_corruption_and_publication_rollback();
    test_limits_interruptions_and_eviction();
    test_snapshot_isolation_and_rejection();
    test_model_generation_and_reuse();
    test_generation_dependency_races();
    test_generation_failures();
    puts("Summary and digest tests passed");
    return 0;
}
