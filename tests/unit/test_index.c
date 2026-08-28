#include "repo/repo_internal.h"
#include "forge/index.h"
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

/* Isolated temporary workspaces; no Go executable, model, or GPU is required.
 * The internal header is used for real Tree-sitter comparisons and SQLite
 * fault injection, not to replace either dependency with a mock. */
typedef struct {
    char root[FG_PATH_MAX];
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
    snprintf(name, sizeof(name), "forge-index-%s", random);
    assert(fg_path_join(f->root, base, name));
    assert(fg_mkdir(f->root, &f->error));
    f->repo = forge_repo_open(f->root, &f->error);
    assert(f->repo);
}
static void erase_directory(const char *path) {
#ifdef _WIN32
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
            erase_directory(child);
        else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            assert(RemoveDirectoryA(child));
        else {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                assert(
                    SetFileAttributesA(child, entry.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY));
            assert(DeleteFileA(child));
        }
    } while (FindNextFileA(iterator, &entry));
    assert(GetLastError() == ERROR_NO_MORE_FILES);
    FindClose(iterator);
#else
    DIR *directory = opendir(path);
    assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[FG_PATH_MAX];
        struct stat info;
        assert(fg_path_join(child, path, entry->d_name));
        assert(lstat(child, &info) == 0);
        if (S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode))
            erase_directory(child);
        else
            assert(unlink(child) == 0);
    }
    closedir(directory);
#endif
    assert(test_rmdir(path) == 0);
}
static void fixture_finish(fixture *f) {
    forge_repo_close(f->repo);
    const char *base = strrchr(f->root, '/');
    assert(base && !strncmp(base + 1, "forge-index-", 12));
    erase_directory(f->root);
}
static void write_bytes(fixture *f, const char *relative, const char *text, size_t size) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    for (char *p = path + strlen(f->root) + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            assert(fg_mkdir(path, &f->error));
            *p = '/';
        }
    assert(fg_write_file(path, text, size, &f->error));
}
static void write_source(fixture *f, const char *relative, const char *text) {
    write_bytes(f, relative, text, strlen(text));
}
static void remove_source(fixture *f, const char *relative) {
    char path[FG_PATH_MAX];
    assert(fg_path_join(path, f->root, relative));
    assert(remove(path) == 0);
}
static void full_index(fixture *f) {
    forge_status status = forge_repo_index(f->repo, &f->error);
    if (status != FORGE_OK)
        fprintf(stderr, "full index: %s\n", f->error.message);
    assert(status == FORGE_OK && !f->error.code);
}
static void delta_index(fixture *f, const char *path) {
    const char *paths[] = {path};
    forge_status status = forge_repo_index_paths(f->repo, paths, 1, &f->error);
    if (status != FORGE_OK)
        fprintf(stderr, "delta index: %s\n", f->error.message);
    assert(status == FORGE_OK && !f->error.code);
}
static forge_index_stats stats(fixture *f) {
    forge_index_stats value;
    assert(forge_repo_get_index_stats(f->repo, &value));
    return value;
}
static void database(fixture *f, const char *query) {
    char *error = NULL;
    int rc = sqlite3_exec(f->repo->db, query, NULL, NULL, &error);
    if (rc != SQLITE_OK)
        fprintf(stderr, "test database: %s\n", error ? error : "unknown");
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}
static uint64_t scalar(fixture *f, const char *query) {
    sqlite3_stmt *s = NULL;
    assert(sqlite3_prepare_v2(f->repo->db, query, -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    uint64_t value = (uint64_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return value;
}
static char *describe(fixture *f, const char *path) {
    char *text = forge_repo_index_describe(f->repo, path, &f->error);
    if (!text)
        fprintf(stderr, "describe: %s\n", f->error.message);
    assert(text && !f->error.code);
    return text;
}
static yyjson_doc *description(fixture *f, const char *path) {
    char *text = describe(f, path);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(text);
    assert(doc);
    return doc;
}
static const char *symbol_hash(yyjson_doc *doc, const char *name) {
    yyjson_val *symbols = yyjson_obj_get(yyjson_doc_get_root(doc), "symbols");
    for (size_t i = 0; i < yyjson_arr_size(symbols); i++) {
        yyjson_val *symbol = yyjson_arr_get(symbols, i);
        if (!strcmp(fg_json_str(symbol, "name"), name))
            return fg_json_str(symbol, "source_hash");
    }
    return NULL;
}
static fg_repo_tree *cached_tree(fixture *f, const char *path) {
    for (fg_repo_tree *entry = f->repo->trees; entry; entry = entry->next)
        if (!strcmp(entry->path, path))
            return entry;
    return NULL;
}
static void compare_descriptions(fixture *a, fixture *b, const char *path) {
    yyjson_doc *ad = description(a, path), *bd = description(b, path);
    const char *fields[] = {"source_hash",  "ast_hash",          "symbol_hash",
                            "source_bytes", "ast_nodes",         "symbol_count",
                            "parse_error",  "metadata_complete", "symbols"};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        char *av = yyjson_val_write(yyjson_obj_get(yyjson_doc_get_root(ad), fields[i]), 0, NULL);
        char *bv = yyjson_val_write(yyjson_obj_get(yyjson_doc_get_root(bd), fields[i]), 0, NULL);
        assert(av && bv);
        if (strcmp(av, bv))
            fprintf(stderr, "cold/incremental mismatch in %s\n", fields[i]);
        assert(!strcmp(av, bv));
        free(av);
        free(bv);
    }
    yyjson_doc_free(ad);
    yyjson_doc_free(bd);
    fg_repo_tree *at = cached_tree(a, path), *bt = cached_tree(b, path);
    assert(at && bt);
    char *an = ts_node_string(ts_tree_root_node(at->tree));
    char *bn = ts_node_string(ts_tree_root_node(bt->tree));
    assert(an && bn && !strcmp(an, bn));
    assert(at->nodes == bt->nodes && at->bytes == bt->bytes);
    free(an);
    free(bn);
}
static void test_edit_parity(void) {
    static const char *const cases[] = {
        "package sample\r\nimport \"fmt\"\r\nfunc Caf\xc3\xa9() string { return "
        "\"\xf0\x9f\x98\x80\" }\r\nvar A, B = 1, 2\r\n",
        "package sample\r\nimport \"fmt\"\r\nfunc Caf\xc3\xaa() string { return "
        "\"\xf0\x9f\x98\x80\" }\r\nvar A, B = 1, 2\r\n",
        "package sample\r\nimport \"fmt\"\r\nfunc Caf\xc4\xaa() string { return "
        "\"\xf0\x9f\x98\x80\" }\r\nvar A, B = 1, 2\r\n",
        "package sample\r\nimport \"fmt\"\r\nfunc Caf\xc4\xaa() string { return "
        "\"\xf0\x9f\x8d\x95\" }\r\nvar A, B = 1, 2\r\n",
        "// \xc3\xa9\npackage sample\n\nimport \"fmt\"\nfunc Caf\xc4\xaa() string {\n return "
        "\"\xf0\x9f\x8d\x95\"\n}\nvar A, B = 1, 2\n",
        "package changed\n\nimport (\"fmt\"; _ \"net/http\")\nfunc Caf\xc4\xaa() string {\n return "
        "\"new\"\n}\nvar A, B = 3, 4\n",
        "package changed\nvar A, B = 3, 4\n",
        "package changed\nfunc Broken( {\n",
        "",
        "package final\nfunc Recreated() {}\n"};
    fixture warm, cold;
    fixture_start(&warm);
    fixture_start(&cold);
    forge_index_limits disabled = {0};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        write_source(&warm, "edit.go", cases[i]);
        write_source(&cold, "edit.go", cases[i]);
        /* Clear only the cold fixture's AST cache; the parser always receives NULL. */
        assert(forge_repo_set_index_limits(cold.repo, &disabled, &cold.error) == FORGE_OK);
        assert(forge_repo_set_index_limits(cold.repo, NULL, &cold.error) == FORGE_OK);
        if (!i)
            full_index(&warm);
        else
            delta_index(&warm, "edit.go");
        full_index(&cold);
        compare_descriptions(&warm, &cold, "edit.go");
    }
    assert(stats(&warm).cold_parses == 1);
    assert(stats(&warm).incremental_parses == sizeof(cases) / sizeof(*cases) - 1);
    assert(stats(&cold).cold_parses == sizeof(cases) / sizeof(*cases));
    assert(!stats(&cold).incremental_parses);
    fixture_finish(&cold);
    fixture_finish(&warm);
}
static void test_hashes_and_reopen(void) {
    fixture f;
    fixture_start(&f);
    const char *first =
        "package p\nfunc Alpha() int { return 1 }\nfunc Beta() int { return 2 }\nvar A, B = 3, 4\n";
    write_source(&f, "hash.go", first);
    write_source(&f, "README.md", "indexed text\n");
    full_index(&f);
    yyjson_doc *before = description(&f, "hash.go");
    yyjson_val *root = yyjson_doc_get_root(before);
    assert(!strcmp(fg_json_str(root, "hash_algorithm"), "fnv1a64"));
    assert(yyjson_get_uint(yyjson_obj_get(root, "symbol_count")) == 4);
    assert(symbol_hash(before, "A") && symbol_hash(before, "B"));
    char hash[17];
    snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)fg_hash(first, strlen(first)));
    assert(!strcmp(hash, fg_json_str(root, "source_hash")));
    uint64_t generation = forge_repo_generation(f.repo);
    forge_index_stats prior = stats(&f);
    delta_index(&f, "hash.go");
    assert(forge_repo_generation(f.repo) == generation);
    assert(stats(&f).unchanged_files == prior.unchanged_files + 1);
    assert(stats(&f).cold_parses == prior.cold_parses && !stats(&f).incremental_parses);
    const char *shifted = "\n// inserted before declarations\npackage p\nfunc Alpha() int { return "
                          "1 }\nfunc Beta() int { return 2 }\nvar A, B = 3, 4\n";
    write_source(&f, "hash.go", shifted);
    yyjson_doc *stale = description(&f, "hash.go");
    assert(!strcmp(fg_json_str(yyjson_doc_get_root(stale), "source_hash"), hash));
    yyjson_doc_free(stale);
    delta_index(&f, "hash.go");
    yyjson_doc *after = description(&f, "hash.go");
    assert(!strcmp(symbol_hash(before, "Alpha"), symbol_hash(after, "Alpha")));
    assert(!strcmp(symbol_hash(before, "Beta"), symbol_hash(after, "Beta")));
    assert(!strcmp(fg_json_str(root, "symbol_hash"),
                   fg_json_str(yyjson_doc_get_root(after), "symbol_hash")));
    assert(
        strcmp(fg_json_str(root, "ast_hash"), fg_json_str(yyjson_doc_get_root(after), "ast_hash")));
    yyjson_doc_free(after);
    write_source(&f, "hash.go",
                 "package p\nfunc Alpha() int { return 1 }\nfunc Beta() int { return 9 }\nvar A, B "
                 "= 3, 4\n");
    delta_index(&f, "hash.go");
    after = description(&f, "hash.go");
    assert(!strcmp(symbol_hash(before, "Alpha"), symbol_hash(after, "Alpha")));
    assert(strcmp(symbol_hash(before, "Beta"), symbol_hash(after, "Beta")));
    yyjson_doc_free(after);
    yyjson_doc_free(before);
    forge_repo_close(f.repo);
    f.repo = forge_repo_open(f.root, &f.error);
    assert(f.repo && !stats(&f).cached_files && !stats(&f).cold_parses);
    full_index(&f);
    assert(!stats(&f).cold_parses); /* Persisted hashes do not imply a retained AST. */
    write_source(&f, "hash.go", first);
    delta_index(&f, "hash.go");
    assert(stats(&f).cold_parses == 1 && !stats(&f).incremental_parses);
    after = description(&f, "README.md");
    assert(yyjson_is_null(yyjson_obj_get(yyjson_doc_get_root(after), "ast_hash")));
    yyjson_doc_free(after);
    fixture_finish(&f);
}
static void test_deletion_recreation_and_skips(void) {
    fixture f;
    fixture_start(&f);
    const char *source = "package p\nimport \"fmt\"\nfunc Kept() { fmt.Println(1) }\n";
    write_source(&f, "nested/a.go", source);
    full_index(&f);
    assert(stats(&f).cached_files == 1 && !f.repo->go_index_incomplete);
    uint64_t generation = forge_repo_generation(f.repo);
    remove_source(&f, "nested/a.go");
    const char *duplicates[] = {"nested/a.go", "nested\\a.go"};
    assert(forge_repo_index_paths(f.repo, duplicates, 2, &f.error) == FORGE_OK);
    assert(forge_repo_generation(f.repo) == generation + 1);
    assert(!f.repo->go_index_incomplete && !stats(&f).cached_files);
    assert(stats(&f).files_removed == 1);
    assert(!scalar(&f, "SELECT count(*) FROM symbols"));
    assert(!scalar(&f, "SELECT count(*) FROM symbol_hashes"));
    assert(!scalar(&f, "SELECT count(*) FROM refs"));
    assert(!scalar(&f, "SELECT count(*) FROM imports"));
    assert(!forge_repo_index_describe(f.repo, "nested/a.go", &f.error));
    assert(f.error.code == FORGE_ERR_NOT_FOUND);
    write_source(&f, "nested/a.go", source);
    delta_index(&f, "nested/a.go");
    assert(stats(&f).cold_parses == 2 && !stats(&f).incremental_parses);
    const char invalid[] = "package p\nvar A = \xff\n";
    write_bytes(&f, "nested/a.go", invalid, sizeof(invalid) - 1);
    delta_index(&f, "nested/a.go");
    assert(f.repo->go_index_incomplete && !stats(&f).cached_files);
    assert(!forge_repo_index_describe(f.repo, "nested/a.go", &f.error));
    const char binary[] = "package p\n\0func Hidden() {}\n";
    write_bytes(&f, "nested/a.go", binary, sizeof(binary) - 1);
    delta_index(&f, "nested/a.go");
    assert(!scalar(&f, "SELECT count(*) FROM files"));
    char *oversized = malloc(FORGE_INDEX_MAX_FILE_BYTES + 1);
    assert(oversized);
    memset(oversized, ' ', FORGE_INDEX_MAX_FILE_BYTES + 1);
    write_bytes(&f, "nested/a.go", oversized, FORGE_INDEX_MAX_FILE_BYTES + 1);
    free(oversized);
    delta_index(&f, "nested/a.go");
    assert(!scalar(&f, "SELECT count(*) FROM files"));
    write_source(&f, "nested/a.go", source);
    delta_index(&f, "nested/a.go");
    assert(f.repo->go_index_incomplete); /* Deltas cannot prove unrelated omissions repaired. */
    full_index(&f);
    assert(!f.repo->go_index_incomplete);
    remove_source(&f, "nested/a.go");
    full_index(&f);
    assert(!stats(&f).cached_files && !f.repo->go_index_incomplete);
    fixture_finish(&f);
}
static int reject_commit(void *userdata) {
    unsigned *calls = userdata;
    (*calls)++;
    return 1;
}
static void assert_description(fixture *f, const char *path, const char *expected) {
    char *actual = describe(f, path);
    assert(!strcmp(expected, actual));
    free(actual);
}
static void test_transaction_rollback(void) {
    fixture f;
    fixture_start(&f);
    write_source(&f, "a.go", "package p\nfunc First() {}\n");
    write_source(&f, "b.go", "package p\nfunc Second() {}\n");
    full_index(&f);
    char *before_a = describe(&f, "a.go"), *before_b = describe(&f, "b.go");
    char *before_tree = ts_node_string(ts_tree_root_node(cached_tree(&f, "a.go")->tree));
    uint64_t generation = forge_repo_generation(f.repo), scan = f.repo->scan;
    database(&f, "CREATE TRIGGER fail_symbol BEFORE INSERT ON symbols WHEN new.name='Fail' "
                 "BEGIN SELECT RAISE(ABORT,'forced symbol failure'); END");
    write_source(&f, "a.go", "package p\nfunc Later() {}\n");
    write_source(&f, "b.go", "package p\nfunc Fail() {}\n");
    const char *paths[] = {"a.go", "b.go"};
    assert(forge_repo_index_paths(f.repo, paths, 2, &f.error) == FORGE_ERR_IO);
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert(stats(&f).rollbacks == 1 && stats(&f).cached_files == 2 && !f.repo->pending_trees);
    assert_description(&f, "a.go", before_a);
    assert_description(&f, "b.go", before_b);
    char *after_tree = ts_node_string(ts_tree_root_node(cached_tree(&f, "a.go")->tree));
    assert(!strcmp(before_tree, after_tree));
    free(before_tree);
    free(after_tree);
    database(&f, "DROP TRIGGER fail_symbol");
    assert(forge_repo_index_paths(f.repo, paths, 2, &f.error) == FORGE_OK);
    assert(forge_repo_generation(f.repo) == generation + 1);
    free(before_a);
    free(before_b);
    before_a = describe(&f, "a.go");
    generation = forge_repo_generation(f.repo);
    scan = f.repo->scan;
    write_source(&f, "a.go", "package p\nfunc CommitRejected() {}\n");
    unsigned rejected = 0;
    sqlite3_commit_hook(f.repo->db, reject_commit, &rejected);
    assert(forge_repo_index_paths(f.repo, paths, 1, &f.error) == FORGE_ERR_IO);
    sqlite3_commit_hook(f.repo->db, NULL, NULL);
    assert(rejected == 1 && sqlite3_get_autocommit(f.repo->db));
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert(!f.repo->pending_trees);
    assert_description(&f, "a.go", before_a);
    free(before_a);
    delta_index(&f, "a.go");
    fixture_finish(&f);
}
static void test_cache_limits_and_global_file_limit(void) {
    fixture f;
    fixture_start(&f);
    forge_index_limits limits = {1, 1024, 1000};
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_OK);
    write_source(&f, "a.go", "package p\nfunc One() {}\n");
    write_source(&f, "b.go", "package p\nfunc Two() {}\n");
    full_index(&f);
    forge_index_stats current = stats(&f);
    assert(current.cached_files == 1 && current.peak_cached_files == 1);
    assert(current.cached_source_bytes <= limits.max_cached_source_bytes);
    assert(current.cached_nodes <= limits.max_cached_nodes && current.cache_evictions);
    char retained[FG_PATH_MAX];
    strcpy(retained, f.repo->trees->path);
    write_source(&f, retained, "package p\nfunc Updated() {}\n");
    delta_index(&f, retained);
    assert(stats(&f).incremental_parses == current.incremental_parses + 1);
    assert(stats(&f).cached_files == 1 && stats(&f).peak_cached_files == 1);
    char *before = describe(&f, retained);
    write_source(&f, retained, "package p\nfunc RolledBack() {}\n");
    unsigned rejected = 0;
    sqlite3_commit_hook(f.repo->db, reject_commit, &rejected);
    const char *paths[] = {retained};
    assert(forge_repo_index_paths(f.repo, paths, 1, &f.error) == FORGE_ERR_IO);
    sqlite3_commit_hook(f.repo->db, NULL, NULL);
    /* One combined retention slot cannot preserve both old and staged trees.
     * Rollback may lose warmth, but cannot publish the rejected tree. */
    assert(!stats(&f).cached_files && !f.repo->pending_trees);
    assert_description(&f, retained, before);
    free(before);
    current = stats(&f);
    delta_index(&f, retained);
    assert(stats(&f).cold_parses == current.cold_parses + 1);
    limits.max_cached_source_bytes = 1;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_OK);
    assert(!stats(&f).cached_files);
    write_source(&f, retained, "package p\nfunc SourceTooLargeForCache() {}\n");
    delta_index(&f, retained);
    assert(!stats(&f).cached_files && stats(&f).cache_skips);
    limits.max_cached_source_bytes = 1024;
    limits.max_cached_nodes = 1;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_OK);
    write_source(&f, retained, "package p\nfunc TooManyNodesForCache() {}\n");
    delta_index(&f, retained);
    assert(!stats(&f).cached_files);
    limits.max_cached_nodes = FORGE_INDEX_MAX_CACHE_NODES + 1;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_ERR_LIMIT);
    limits = forge_default_index_limits();
    limits.max_cached_files = FORGE_INDEX_MAX_CACHE_FILES + 1;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_ERR_LIMIT);
    limits = forge_default_index_limits();
    limits.max_cached_source_bytes = FORGE_INDEX_MAX_CACHE_SOURCE_BYTES + 1;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_ERR_LIMIT);
    limits = forge_default_index_limits();
    limits.max_cached_files = 0;
    assert(forge_repo_set_index_limits(f.repo, &limits, &f.error) == FORGE_OK);
    uint64_t generation = forge_repo_generation(f.repo), scan = f.repo->scan;
    /* Seed database rows cheaply to exercise the aggregate limit for deltas,
     * which must not merely count the small set of requested paths. */
    database(&f, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100000) "
                 "INSERT INTO files(path,language,size,hash,generation,seen) "
                 "SELECT printf('limit/%06d.c',x),'text',0,'',0,0 FROM n");
    assert(forge_repo_index_paths(f.repo, paths, 1, &f.error) == FORGE_ERR_LIMIT);
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert(sqlite3_get_autocommit(f.repo->db));
    fixture_finish(&f);
}
static void test_multiple_handles_and_metadata_upgrade(void) {
    fixture f;
    fixture_start(&f);
    write_source(&f, "a.go", "package p\nfunc First() {}\n");
    write_source(&f, "b.go", "package p\nfunc Second() {}\n");
    full_index(&f);
    uint64_t generation = forge_repo_generation(f.repo);
    forge_repo *other = forge_repo_open(f.root, &f.error);
    assert(other);
    write_source(&f, "a.go", "package p\nfunc FirstUpdated() {}\n");
    delta_index(&f, "a.go");
    write_source(&f, "b.go", "package p\nfunc FromOtherHandle() {}\n");
    const char *paths[] = {"b.go"};
    assert(forge_repo_index_paths(other, paths, 1, &f.error) == FORGE_OK);
    assert(forge_repo_generation(other) == generation + 2);
    forge_index_stats previous = stats(&f);
    write_source(&f, "b.go", "package p\nfunc AfterOtherHandle() {}\n");
    delta_index(&f, "b.go");
    assert(forge_repo_generation(f.repo) == generation + 3);
    assert(stats(&f).cold_parses == previous.cold_parses + 1); /* Stale cached bytes rejected. */
    forge_repo_close(other);
    database(&f, "DELETE FROM file_syntax; DELETE FROM symbol_hashes");
    yyjson_doc *doc = description(&f, "a.go");
    assert(!yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "metadata_complete")));
    yyjson_doc_free(doc);
    full_index(&f);
    doc = description(&f, "a.go");
    assert(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "metadata_complete")));
    assert(symbol_hash(doc, "FirstUpdated"));
    yyjson_doc_free(doc);
    database(&f, "UPDATE file_digests SET version=4294967297; DELETE FROM symbol_digests");
    doc = description(&f, "a.go");
    assert(!yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "digest_metadata_complete")));
    yyjson_doc_free(doc);
    full_index(&f);
    doc = description(&f, "a.go");
    assert(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "digest_metadata_complete")));
    const char *digest = fg_json_str(yyjson_doc_get_root(doc), "source_sha256");
    assert(digest && strlen(digest) == 64);
    yyjson_doc_free(doc);
    fixture_finish(&f);
}
typedef struct {
    forge_repo *repo;
    uint64_t parses;
    unsigned calls, after_calls;
    bool immediate;
} cancel_probe;
static bool cancelled(void *userdata) {
    cancel_probe *probe = userdata;
    probe->calls++;
    if (probe->immediate || (probe->after_calls && probe->calls >= probe->after_calls))
        return true;
    forge_index_stats value;
    return probe->repo && forge_repo_get_index_stats(probe->repo, &value) &&
           value.cold_parses + value.incremental_parses > probe->parses;
}
static void assert_scope_restored(fixture *f) {
    assert(!f->repo->index_deadline && !f->repo->index_cancelled && !f->repo->index_userdata);
    assert(!f->repo->index_scope_active);
    assert(scalar(f, "PRAGMA busy_timeout") == 1234);
}
static void test_cancellation_and_deadlines(void) {
    fixture f;
    fixture_start(&f);
    write_source(&f, "a.go", "package p\nfunc Original() {}\n");
    full_index(&f);
    database(&f, "PRAGMA busy_timeout=1234");
    uint64_t generation = forge_repo_generation(f.repo), scan = f.repo->scan;
    char *before = describe(&f, "a.go");
    cancel_probe probe = {0};
    probe.immediate = true;
    assert(fg_repo_index_until(f.repo, NULL, 0, true, 0, cancelled, &probe, &f.error) ==
           FORGE_ERR_CANCELLED);
    assert_scope_restored(&f);
    assert(fg_repo_index_until(f.repo, NULL, 0, true, 1, NULL, NULL, &f.error) == FORGE_ERR_LIMIT);
    assert(fg_repo_index_until(f.repo, NULL, 0, false, 1, NULL, NULL, &f.error) == FORGE_ERR_LIMIT);
    assert(fg_repo_note_change_until(f.repo, 0, cancelled, &probe, &f.error) ==
           FORGE_ERR_CANCELLED);
    assert(fg_repo_note_change_until(f.repo, 1, NULL, NULL, &f.error) == FORGE_ERR_LIMIT);
    assert_scope_restored(&f);
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert_description(&f, "a.go", before);
    write_source(&f, "a.go", "package p\nfunc CancelAfterParse() {}\n");
    forge_index_stats prior = stats(&f);
    memset(&probe, 0, sizeof(probe));
    probe.repo = f.repo;
    probe.parses = prior.cold_parses + prior.incremental_parses;
    const char *paths[] = {"a.go"};
    assert(fg_repo_index_until(f.repo, paths, 1, false, fg_now_ms() + 5000, cancelled, &probe,
                               &f.error) == FORGE_ERR_CANCELLED);
    assert(stats(&f).incremental_parses == prior.incremental_parses + 1);
    assert(stats(&f).rollbacks == prior.rollbacks + 1 && !f.repo->pending_trees);
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert_description(&f, "a.go", before);
    assert_scope_restored(&f);
    /* Cancel after the generation row was updated but before COMMIT. */
    memset(&probe, 0, sizeof(probe));
    probe.after_calls = 3;
    assert(fg_repo_note_change_until(f.repo, 0, cancelled, &probe, &f.error) ==
           FORGE_ERR_CANCELLED);
    assert(forge_repo_generation(f.repo) == generation);
    assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == generation);
    assert_scope_restored(&f);
    /* A competing writer exercises bounded SQLite busy handling, independent
     * of Git or parser timing. Cancellation is checked during lock retries. */
    forge_repo *other = forge_repo_open(f.root, &f.error);
    assert(other);
    assert(sqlite3_exec(other->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    uint64_t start = fg_now_ms();
    assert(fg_repo_note_change_until(f.repo, start + 30, NULL, NULL, &f.error) == FORGE_ERR_LIMIT);
    assert(fg_now_ms() - start < 2000);
    assert_scope_restored(&f);
    memset(&probe, 0, sizeof(probe));
    probe.after_calls = 5;
    start = fg_now_ms();
    assert(fg_repo_index_until(f.repo, paths, 1, false, 0, cancelled, &probe, &f.error) ==
           FORGE_ERR_CANCELLED);
    assert(fg_now_ms() - start < 2000);
    assert_scope_restored(&f);
    assert(sqlite3_exec(other->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    forge_repo_close(other);
    assert(forge_repo_generation(f.repo) == generation && f.repo->scan == scan);
    assert_description(&f, "a.go", before);
    free(before);
    delta_index(&f, "a.go"); /* Temporary cancellation fields did not poison the public API. */
    assert(forge_repo_generation(f.repo) == generation + 1);
    fixture_finish(&f);
}
static void test_observed_change_transactions(void) {
    fixture f;
    fixture_start(&f);
    write_source(&f, "a.go", "package p\nfunc Original() {}\n");
    full_index(&f);
    write_source(&f, "unsupported.txt", "observed watcher change\n");
    uint64_t generation = forge_repo_generation(f.repo);
    delta_index(&f, "unsupported.txt");
    assert(forge_repo_generation(f.repo) == generation);
    uint64_t scan = f.repo->scan;
    forge_repo *other = forge_repo_open(f.root, &f.error);
    assert(other);
    assert(fg_repo_note_change(f.repo, &f.error) == FORGE_OK);
    assert(forge_repo_generation(f.repo) == generation + 1 && f.repo->scan == scan);
    assert(fg_repo_note_change(other, &f.error) == FORGE_OK);
    assert(forge_repo_generation(other) ==
           generation + 2); /* Fresh persisted value, not stale handle. */
    forge_repo_close(other);
    uint64_t local_generation = forge_repo_generation(f.repo);
    generation = scalar(&f, "SELECT value FROM meta WHERE key='generation'");
    database(&f, "CREATE TRIGGER ignore_generation BEFORE UPDATE OF value ON meta "
                 "WHEN new.key='generation' BEGIN SELECT RAISE(IGNORE); END");
    assert(fg_repo_note_change(f.repo, &f.error) == FORGE_ERR_IO);
    assert(forge_repo_generation(f.repo) == local_generation);
    assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == generation);
    database(&f, "DROP TRIGGER ignore_generation");
    unsigned rejected = 0;
    sqlite3_commit_hook(f.repo->db, reject_commit, &rejected);
    assert(fg_repo_note_change(f.repo, &f.error) == FORGE_ERR_IO);
    sqlite3_commit_hook(f.repo->db, NULL, NULL);
    assert(rejected == 1 && forge_repo_generation(f.repo) == local_generation);
    assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == generation);
    database(&f, "UPDATE meta SET value=9223372036854775807 WHERE key='generation'");
    assert(fg_repo_note_change(f.repo, &f.error) == FORGE_ERR_LIMIT);
    assert(forge_repo_generation(f.repo) == local_generation);
    assert(scalar(&f, "SELECT value FROM meta WHERE key='generation'") == INT64_MAX);
    assert(forge_repo_index(f.repo, &f.error) == FORGE_ERR_LIMIT);
    assert(forge_repo_generation(f.repo) == local_generation && f.repo->scan == scan);
    database(&f, "UPDATE meta SET value=-1 WHERE key='generation'");
    assert(fg_repo_note_change(f.repo, &f.error) == FORGE_ERR_PARSE);
    assert(forge_repo_generation(f.repo) == local_generation);
    assert(sqlite3_get_autocommit(f.repo->db));
    assert(fg_repo_note_change(NULL, &f.error) == FORGE_ERR_ARGUMENT);
    assert(fg_repo_index_until(NULL, NULL, 0, true, 0, NULL, NULL, &f.error) == FORGE_ERR_ARGUMENT);
    assert(fg_repo_note_change_until(NULL, 0, NULL, NULL, &f.error) == FORGE_ERR_ARGUMENT);
    fixture_finish(&f);
}
static void test_description_bounds(void) {
    fixture f;
    fixture_start(&f);
    fg_buf source = {0};
    assert(fg_buf_puts(&source, "package p\n"));
    for (size_t i = 0; i <= FORGE_INDEX_SYMBOL_LIMIT; i++)
        assert(fg_buf_printf(&source, "var Value%zu = %zu\n", i, i));
    write_bytes(&f, "many.go", source.data, source.len);
    fg_buf_clear(&source);
    full_index(&f);
    yyjson_doc *doc = description(&f, "many.go");
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(yyjson_get_uint(yyjson_obj_get(root, "symbol_count")) == FORGE_INDEX_SYMBOL_LIMIT + 1);
    assert(yyjson_get_bool(yyjson_obj_get(root, "symbols_truncated")));
    assert(yyjson_arr_size(yyjson_obj_get(root, "symbols")) == FORGE_INDEX_SYMBOL_LIMIT);
    yyjson_doc_free(doc);
    assert(!forge_repo_index_describe(f.repo, "../escape.go", &f.error));
    assert(f.error.code == FORGE_ERR_POLICY);
    assert(!forge_repo_index_describe(f.repo, "absent.go", &f.error));
    assert(f.error.code == FORGE_ERR_NOT_FOUND);
    database(&f, "BEGIN IMMEDIATE");
    assert(!forge_repo_index_describe(f.repo, "many.go", &f.error));
    assert(f.error.code == FORGE_ERR_CONFLICT);
    assert(forge_repo_set_index_limits(f.repo, NULL, &f.error) == FORGE_ERR_CONFLICT);
    database(&f, "ROLLBACK");
    forge_index_stats value;
    assert(!forge_repo_get_index_stats(NULL, &value));
    assert(!forge_repo_get_index_stats(f.repo, NULL));
    fixture_finish(&f);
}
static void test_git_delta_eligibility(void) {
    fixture f;
    fixture_start(&f);
    const char *initialize[] = {"git", "init", "--quiet", NULL};
    fg_process_result result = {0};
    forge_status status =
        fg_process(f.root, initialize, 10000, 8192, NULL, NULL, &result, &f.error);
    bool available = status == FORGE_OK && !result.exit_code;
    fg_process_free(&result);
    if (!available) {
        puts("Git eligibility case skipped: git init unavailable");
        fixture_finish(&f);
        return;
    }
    write_source(&f, ".gitignore", ".forge/\nignored.go\n");
    write_source(&f, "tracked.go", "package p\nfunc Tracked() {}\n");
    write_source(&f, "free.go", "package p\nfunc Untracked() {}\n");
    write_source(&f, "ignored.go", "package p\nfunc Ignored() {}\n");
    const char *track[] = {"git", "add", "--", "tracked.go", NULL};
    assert(fg_process(f.root, track, 10000, 8192, NULL, NULL, &result, &f.error) == FORGE_OK);
    assert(!result.exit_code);
    fg_process_free(&result);
    full_index(&f);
    assert(!f.repo->filesystem_scan && stats(&f).cached_files == 2);
    assert(!forge_repo_index_describe(f.repo, "ignored.go", &f.error));
    assert(f.error.code == FORGE_ERR_NOT_FOUND);
    uint64_t generation = forge_repo_generation(f.repo);
    delta_index(&f, "ignored.go");
    assert(forge_repo_generation(f.repo) == generation);
    remove_source(&f, "tracked.go");
    delta_index(&f, "tracked.go");
    assert(!f.repo->go_index_incomplete && !cached_tree(&f, "tracked.go"));
    write_source(&f, "tracked.go", "package p\nfunc RecreatedTracked() {}\n");
    delta_index(&f, "tracked.go");
    assert(cached_tree(&f, "tracked.go"));
    write_source(&f, ".gitignore", ".forge/\nignored.go\nfree.go\n");
    delta_index(&f, "free.go");
    assert(!cached_tree(&f, "free.go"));
    assert(!forge_repo_index_describe(f.repo, "free.go", &f.error));
    assert(f.error.code == FORGE_ERR_NOT_FOUND);
    char *before = describe(&f, "tracked.go");
    cancel_probe probe = {0};
    probe.after_calls = 5; /* Interrupt the Git eligibility stage. */
    const char *paths[] = {"tracked.go"};
    write_source(&f, "tracked.go", "package p\nfunc GitCancelled() {}\n");
    assert(fg_repo_index_until(f.repo, paths, 1, false, 0, cancelled, &probe, &f.error) ==
           FORGE_ERR_CANCELLED);
    assert_description(&f, "tracked.go", before);
    free(before);
    delta_index(&f, "tracked.go");
    fixture_finish(&f);
}
int main(void) {
    test_edit_parity();
    test_hashes_and_reopen();
    test_deletion_recreation_and_skips();
    test_transaction_rollback();
    test_cache_limits_and_global_file_limit();
    test_multiple_handles_and_metadata_upgrade();
    test_cancellation_and_deadlines();
    test_observed_change_transactions();
    test_description_bounds();
    test_git_delta_eligibility();
    puts("Incremental repository index tests passed");
    return 0;
}
