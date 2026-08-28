#include "repo_internal.h"
#include "forge/validation.h"
#include <ctype.h>
extern const TSLanguage *tree_sitter_go(void);
static bool sql(forge_repo *r, const char *query, forge_error *e) {
    char *message = NULL;
    int rc = sqlite3_exec(r->db, query, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fg_error(e, FORGE_ERR_IO, "Repository database: %s",
                 message ? message : sqlite3_errmsg(r->db));
        sqlite3_free(message);
        return false;
    }
    return true;
}
static sqlite3_stmt *prepare(forge_repo *r, const char *query) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(r->db, query, -1, &s, NULL) != SQLITE_OK)
        fg_error(r->error, FORGE_ERR_IO, "Index query: %s", sqlite3_errmsg(r->db));
    return s;
}
static char *slice(const char *text, TSNode n) {
    uint32_t start = ts_node_start_byte(n), end = ts_node_end_byte(n);
    char *s = malloc((size_t)(end - start) + 1);
    if (s) {
        memcpy(s, text + start, end - start);
        s[end - start] = 0;
    }
    return s;
}
static bool add_symbol(forge_repo *r, sqlite3_int64 file, const char *text, TSNode node,
                       const char *kind) {
    TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name))
        return true;
    char *n = slice(text, name);
    if (!n)
        return false;
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    uint32_t begin = ts_node_start_byte(node), end = ts_node_end_byte(node),
             sig_end = ts_node_is_null(body) ? end : ts_node_start_byte(body);
    size_t sig_len = FG_MIN((size_t)(sig_end - begin), 512);
    char signature[513];
    memcpy(signature, text + begin, sig_len);
    signature[sig_len] = 0;
    sqlite3_stmt *s =
        prepare(r, "INSERT INTO symbols(file_id,name,kind,start_byte,end_byte,line,signature) "
                   "VALUES(?,?,?,?,?,?,?)");
    if (!s) {
        free(n);
        return false;
    }
    sqlite3_bind_int64(s, 1, file);
    sqlite3_bind_text(s, 2, n, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, kind, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, begin);
    sqlite3_bind_int64(s, 5, end);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)ts_node_start_point(node).row + 1);
    sqlite3_bind_text(s, 7, signature, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    free(n);
    return ok;
}
static bool visit(forge_repo *r, sqlite3_int64 file, const char *text, TSNode node,
                  unsigned depth) {
    if (depth > 256)
        return true;
    const char *type = ts_node_type(node);
    if (!strcmp(type, "function_declaration") || !strcmp(type, "method_declaration") ||
        !strcmp(type, "type_spec") || !strcmp(type, "const_spec") || !strcmp(type, "var_spec"))
        if (!add_symbol(r, file, text, node, type))
            return false;
    if (!strcmp(type, "identifier") || !strcmp(type, "field_identifier") ||
        !strcmp(type, "type_identifier") || !strcmp(type, "package_identifier")) {
        char *name = slice(text, node);
        if (!name)
            return false;
        sqlite3_stmt *s = prepare(r, "INSERT INTO refs(file_id,name,line,byte) VALUES(?,?,?,?)");
        if (!s) {
            free(name);
            return false;
        }
        sqlite3_bind_int64(s, 1, file);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)ts_node_start_point(node).row + 1);
        sqlite3_bind_int64(s, 4, ts_node_start_byte(node));
        bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        free(name);
        if (!ok)
            return false;
    }
    if (!strcmp(type, "import_spec")) {
        TSNode p = ts_node_child_by_field_name(node, "path", 4);
        if (ts_node_is_null(p)) {
            r->go_index_incomplete = true;
            return true;
        }
        char *path = slice(text, p);
        if (!path)
            return false;
        size_t n = strlen(path);
        if (n > 1) {
            path[n - 1] = 0;
            sqlite3_stmt *s = prepare(r, "INSERT INTO imports(file_id,path) VALUES(?,?)");
            if (!s) {
                free(path);
                return false;
            }
            sqlite3_bind_int64(s, 1, file);
            sqlite3_bind_text(s, 2, path + 1, -1, SQLITE_TRANSIENT);
            bool ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
            if (!ok) {
                free(path);
                return false;
            }
        }
        free(path);
    }
    uint32_t n = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < n; i++)
        if (!visit(r, file, text, ts_node_named_child(node, i), depth + 1))
            return false;
    return true;
}
static bool go_metadata(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return !strcmp(base, "go.mod") || !strcmp(base, "go.sum") || !strcmp(base, "go.work") ||
           !strcmp(base, "go.work.sum");
}
static bool track_go_module(forge_repo *r, const char *path) {
    const char *base = strrchr(path, '/');
    if (strcmp(base ? base + 1 : path, "go.mod"))
        return true;
    /* Keep module boundaries even if their metadata exceeds the index limit
     * or cannot be read. Otherwise a nested module could disappear from the
     * broad verification stage while the parent ./... silently excludes it. */
    sqlite3_stmt *s =
        prepare(r, "INSERT OR IGNORE INTO go_module_boundaries(path,seen) VALUES(?,?)");
    if (!s)
        return false;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)r->scan);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    r->changed += (size_t)sqlite3_changes(r->db);
    sqlite3_finalize(s);
    if (!ok)
        return false;
    s = prepare(r, "UPDATE go_module_boundaries SET seen=? WHERE path=?");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok;
}
static bool supported(const char *path) {
    if (go_metadata(path))
        return true;
    const char *ext = strrchr(path, '.');
    if (!ext)
        return false;
    const char *const extensions[] = {".go",   ".c",    ".h",    ".cpp", ".hpp", ".rs",
                                      ".py",   ".ts",   ".tsx",  ".js",  ".jsx", ".md",
                                      ".toml", ".json", ".yaml", ".yml", NULL};
    for (size_t i = 0; extensions[i]; i++)
        if (!strcmp(ext, extensions[i]))
            return true;
    return false;
}
static bool go_file_metadata(forge_repo *r, sqlite3_int64 id, const char *path, const char *text,
                             TSNode root) {
    char *package = NULL;
    uint32_t count = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < count; i++) {
        TSNode node = ts_node_named_child(root, i);
        if (!strcmp(ts_node_type(node), "package_clause") && ts_node_named_child_count(node)) {
            package = slice(text, ts_node_named_child(node, 0));
            break;
        }
    }
    size_t n = strlen(path);
    bool is_test = n >= 8 && !strcmp(path + n - 8, "_test.go");
    bool constraints = strstr(text, "//go:build") || strstr(text, "// +build");
    sqlite3_stmt *s = prepare(r, "INSERT INTO go_files(file_id,package_name,is_test,"
                                 "build_constraints,parse_error) VALUES(?,?,?,?,?)");
    if (!s) {
        free(package);
        return false;
    }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, package ? package : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, is_test ? 1 : 0);
    sqlite3_bind_int(s, 4, constraints ? 1 : 0);
    sqlite3_bind_int(s, 5, ts_node_has_error(root) || !package ? 1 : 0);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    free(package);
    return ok;
}
static bool index_file(const char *path, void *user) {
    forge_repo *r = user;
    if (!supported(path))
        return true;
    const char *ext = strrchr(path, '.');
    bool is_go = ext && !strcmp(ext, ".go");
    bool relevant = is_go || go_metadata(path);
    if (++r->files > 100000) {
        fg_error(r->error, FORGE_ERR_LIMIT, "Repository exceeds 100000 files");
        return false;
    }
    char full[FG_PATH_MAX];
    forge_error ignored = {0};
    if (!fg_safe_path(r->root, path, false, full, &ignored)) {
        if (relevant)
            r->go_index_incomplete = true;
        return true;
    }
    if (!track_go_module(r, path))
        return false;
    size_t size = 0;
    char *text = fg_read_file(full, 2u * 1024u * 1024u, &size, &ignored);
    if (!text) {
        if (relevant)
            r->go_index_incomplete = true;
        return true;
    }
    if (memchr(text, 0, size)) {
        if (relevant)
            r->go_index_incomplete = true;
        free(text);
        return true;
    }
    char hash[32];
    snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)fg_hash(text, size));
    sqlite3_stmt *s = prepare(r, "SELECT f.id,f.hash,g.file_id FROM files f LEFT JOIN go_files g "
                                 "ON g.file_id=f.id WHERE f.path=?");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_int64 id = 0;
    bool unchanged = false;
    if (sqlite3_step(s) == SQLITE_ROW) {
        id = sqlite3_column_int64(s, 0);
        unchanged = !strcmp((const char *)sqlite3_column_text(s, 1), hash);
        /* Old indexes are upgraded lazily even when source bytes are unchanged. */
        if (is_go && sqlite3_column_type(s, 2) == SQLITE_NULL)
            unchanged = false;
    }
    sqlite3_finalize(s);
    if (unchanged) {
        s = prepare(r, "UPDATE files SET seen=? WHERE id=?");
        if (!s) {
            free(text);
            return false;
        }
        sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
        sqlite3_bind_int64(s, 2, id);
        bool ok = sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
        free(text);
        return ok;
    }
    r->changed++;
    if (id) {
        s = prepare(r, "DELETE FROM files WHERE id=?");
        if (!s) {
            free(text);
            return false;
        }
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) != SQLITE_DONE) {
            sqlite3_finalize(s);
            free(text);
            return false;
        }
        sqlite3_finalize(s);
    }
    const char *lang = is_go ? "go" : "text";
    s = prepare(r,
                "INSERT INTO files(path,language,size,hash,generation,seen) VALUES(?,?,?,?,?,?)");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, lang, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)size);
    sqlite3_bind_text(s, 4, hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)r->generation + 1);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)r->scan);
    bool ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok) {
        free(text);
        return false;
    }
    id = sqlite3_last_insert_rowid(r->db);
    s = prepare(r, "INSERT INTO chunks(rowid,path,content) VALUES(?,?,?)");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, text, (int)size, SQLITE_TRANSIENT);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (ok && !strcmp(lang, "go")) {
        TSTree *tree = ts_parser_parse_string(r->parser, NULL, text, (uint32_t)size);
        if (!tree)
            ok = false;
        else {
            TSNode root = ts_tree_root_node(tree);
            ok = go_file_metadata(r, id, path, text, root) && visit(r, id, text, root, 0);
            ts_tree_delete(tree);
        }
    }
    free(text);
    return ok;
}
forge_repo *forge_repo_open(const char *root, forge_error *e) {
    forge_repo *r = calloc(1, sizeof(*r));
    if (!r) {
        fg_error(e, FORGE_ERR_MEMORY, "Repository allocation failed");
        return NULL;
    }
    r->error = e;
    char base[FG_PATH_MAX], path[FG_PATH_MAX];
    if (!fg_workspace(root, r->root, e) || !fg_path_join(base, r->root, ".forge") ||
        !fg_mkdir(base, e) || !fg_path_join(path, base, "index.db")) {
        free(r);
        return NULL;
    }
    char auxiliary[FG_PATH_MAX];
    if (!fg_regular_target(path, e)) {
        free(r);
        return NULL;
    }
    snprintf(auxiliary, sizeof(auxiliary), "%s-wal", path);
    if (!fg_regular_target(auxiliary, e)) {
        free(r);
        return NULL;
    }
    snprintf(auxiliary, sizeof(auxiliary), "%s-shm", path);
    if (!fg_regular_target(auxiliary, e)) {
        free(r);
        return NULL;
    }
    if (sqlite3_open(path, &r->db) != SQLITE_OK) {
        fg_error(e, FORGE_ERR_IO, "Cannot open repository database");
        forge_repo_close(r);
        return NULL;
    }
    sqlite3_busy_timeout(r->db, 5000);
    if (!sql(r,
             "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;"
             "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value INTEGER NOT NULL);"
             "INSERT OR IGNORE INTO meta VALUES('generation',0); INSERT OR IGNORE INTO meta "
             "VALUES('scan',0);"
             "INSERT OR IGNORE INTO meta VALUES('go_index_incomplete',0);"
             "INSERT OR IGNORE INTO meta VALUES('filesystem_scan',0);"
             "CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY,path TEXT UNIQUE,language "
             "TEXT,size INTEGER,hash TEXT,generation INTEGER,seen INTEGER);"
             "CREATE TABLE IF NOT EXISTS symbols(id INTEGER PRIMARY KEY,file_id INTEGER REFERENCES "
             "files(id) ON DELETE CASCADE,name TEXT,kind TEXT,start_byte INTEGER,end_byte "
             "INTEGER,line INTEGER,signature TEXT);"
             "CREATE INDEX IF NOT EXISTS symbol_name ON symbols(name);"
             "CREATE TABLE IF NOT EXISTS refs(file_id INTEGER REFERENCES files(id) ON DELETE "
             "CASCADE,name TEXT,line INTEGER,byte INTEGER);"
             "CREATE INDEX IF NOT EXISTS ref_name ON refs(name);"
             "CREATE TABLE IF NOT EXISTS imports(file_id INTEGER REFERENCES files(id) ON DELETE "
             "CASCADE,path TEXT);"
             "CREATE TABLE IF NOT EXISTS go_files(file_id INTEGER PRIMARY KEY REFERENCES "
             "files(id) ON DELETE CASCADE,package_name TEXT,is_test INTEGER,"
             "build_constraints INTEGER,parse_error INTEGER);"
             "CREATE TABLE IF NOT EXISTS go_module_boundaries(path TEXT PRIMARY KEY,seen INTEGER);"
             "CREATE VIRTUAL TABLE IF NOT EXISTS chunks USING fts5(path UNINDEXED,content);"
             "CREATE TRIGGER IF NOT EXISTS files_delete AFTER DELETE ON files BEGIN DELETE FROM "
             "chunks WHERE rowid=old.id; END;",
             e)) {
        forge_repo_close(r);
        return NULL;
    }
    sqlite3_stmt *s = prepare(r, "SELECT key,value FROM meta");
    if (!s) {
        forge_repo_close(r);
        return NULL;
    }
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(s, 0);
        uint64_t v = (uint64_t)sqlite3_column_int64(s, 1);
        if (!strcmp(key, "generation"))
            r->generation = v;
        else if (!strcmp(key, "scan"))
            r->scan = v;
        else if (!strcmp(key, "go_index_incomplete"))
            r->go_index_incomplete = v != 0;
        else if (!strcmp(key, "filesystem_scan"))
            r->filesystem_scan = v != 0;
    }
    sqlite3_finalize(s);
    r->parser = ts_parser_new();
    if (!r->parser || !ts_parser_set_language(r->parser, tree_sitter_go())) {
        fg_error(e, FORGE_ERR_PARSE, "Go parser ABI mismatch");
        forge_repo_close(r);
        return NULL;
    }
    return r;
}
forge_status forge_repo_index(forge_repo *r, forge_error *e) {
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    r->error = e;
    uint64_t previous_scan = r->scan;
    bool previous_incomplete = r->go_index_incomplete, previous_filesystem = r->filesystem_scan;
    r->changed = 0;
    r->files = 0;
    if (!sql(r, "BEGIN IMMEDIATE", e))
        return FORGE_ERR_IO;
    r->go_index_incomplete = false;
    r->filesystem_scan = false;
    r->scan++;
    const char *args[] = {"git",      "-c",       "core.fsmonitor=false", "ls-files", "-z",
                          "--cached", "--others", "--exclude-standard",   NULL};
    fg_process_result result = {0};
    forge_error ignored = {0};
    forge_status run =
        fg_process(r->root, args, 30000, 16u * 1024u * 1024u, NULL, NULL, &result, &ignored);
    bool ok = true;
    if (run == FORGE_OK && result.exit_code == 0 && !result.truncated) {
        size_t offset = 0;
        while (offset < result.out_len) {
            const char *p = result.out + offset;
            size_t remaining = result.out_len - offset;
            const char *end = memchr(p, 0, remaining);
            if (!end) {
                ok = false;
                break;
            }
            if (!index_file(p, r)) {
                ok = false;
                break;
            }
            offset += (size_t)(end - p) + 1;
        }
    } else {
        r->filesystem_scan = true;
        ok = fg_walk(r->root, "", index_file, r, e);
    }
    fg_process_free(&result);
    if (ok) {
        sqlite3_stmt *s = prepare(r, "DELETE FROM files WHERE seen<>?");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
            ok = sqlite3_step(s) == SQLITE_DONE;
            r->changed += (size_t)sqlite3_changes(r->db);
            sqlite3_finalize(s);
        }
    }
    if (ok) {
        sqlite3_stmt *s = prepare(r, "DELETE FROM go_module_boundaries WHERE seen<>?");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
            ok = sqlite3_step(s) == SQLITE_DONE;
            r->changed += (size_t)sqlite3_changes(r->db);
            sqlite3_finalize(s);
        }
    }
    uint64_t generation =
        r->generation + (r->changed || previous_incomplete != r->go_index_incomplete ? 1u : 0u);
    if (ok) {
        sqlite3_stmt *s =
            prepare(r, "UPDATE meta SET value=CASE key WHEN 'scan' THEN ? WHEN 'generation' "
                       "THEN ? WHEN 'go_index_incomplete' THEN ? WHEN 'filesystem_scan' THEN ? "
                       "ELSE value END");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)generation);
            sqlite3_bind_int(s, 3, r->go_index_incomplete ? 1 : 0);
            sqlite3_bind_int(s, 4, r->filesystem_scan ? 1 : 0);
            ok = sqlite3_step(s) == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    if (!ok) {
        sql(r, "ROLLBACK", NULL);
        r->scan = previous_scan;
        r->go_index_incomplete = previous_incomplete;
        r->filesystem_scan = previous_filesystem;
        return fg_error(e, FORGE_ERR_IO, "Repository indexing failed: %s", sqlite3_errmsg(r->db));
    }
    if (!sql(r, "COMMIT", e)) {
        sql(r, "ROLLBACK", NULL);
        r->scan = previous_scan;
        r->go_index_incomplete = previous_incomplete;
        r->filesystem_scan = previous_filesystem;
        return FORGE_ERR_IO;
    }
    r->generation = generation;
    return FORGE_OK;
}
char *forge_repo_inspect(forge_repo *r, const char *name, int depth, forge_error *e) {
    if (!r || !name || depth < 0 || depth > 3) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid symbol query");
        return NULL;
    }
    r->error = e;
    sqlite3_stmt *s = prepare(
        r, "SELECT f.path,s.kind,s.line,s.signature,s.start_byte,s.end_byte FROM symbols s JOIN "
           "files f ON f.id=s.file_id WHERE s.name=? ORDER BY f.path,s.start_byte LIMIT 32");
    if (!s)
        return NULL;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    fg_buf b = {0};
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(s, 0);
        fg_buf_printf(&b, "%s:%d %s %s\n", path, sqlite3_column_int(s, 2),
                      (const char *)sqlite3_column_text(s, 1),
                      (const char *)sqlite3_column_text(s, 3));
        if (depth) {
            char full[FG_PATH_MAX];
            if (fg_safe_path(r->root, path, false, full, e)) {
                size_t n;
                char *text = fg_read_file(full, 2u * 1024u * 1024u, &n, e);
                if (text) {
                    size_t a = (size_t)sqlite3_column_int64(s, 4),
                           z = (size_t)sqlite3_column_int64(s, 5);
                    if (depth == 3) {
                        a = 0;
                        z = n;
                    } else if (depth == 2) {
                        a = a > 256 ? a - 256 : 0;
                        z = FG_MIN(z + 256, n);
                    }
                    if (a <= z && z <= n)
                        fg_buf_add(&b, text + a, FG_MIN(z - a, 32768));
                    fg_buf_puts(&b, "\n");
                    free(text);
                }
            }
        }
    }
    sqlite3_finalize(s);
    if (!b.len)
        fg_buf_puts(&b, "No matching Go symbol. Try search_text for other languages.\n");
    return fg_buf_take(&b);
}
char *forge_repo_references(forge_repo *r, const char *name, forge_error *e) {
    if (!r || !name) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid reference query");
        return NULL;
    }
    r->error = e;
    sqlite3_stmt *s = prepare(r, "SELECT f.path,r.line FROM refs r JOIN files f ON f.id=r.file_id "
                                 "WHERE r.name=? ORDER BY f.path,r.line LIMIT 100");
    if (!s)
        return NULL;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    fg_buf b = {0};
    fg_buf_puts(&b, "Syntactic identifier occurrences (not type-resolved):\n");
    while (sqlite3_step(s) == SQLITE_ROW)
        fg_buf_printf(&b, "%s:%d\n", (const char *)sqlite3_column_text(s, 0),
                      sqlite3_column_int(s, 1));
    sqlite3_finalize(s);
    return fg_buf_take(&b);
}
char *forge_repo_summary(forge_repo *r, forge_error *e) {
    r->error = e;
    sqlite3_stmt *s = prepare(r, "SELECT path,language FROM files ORDER BY path LIMIT 80");
    if (!s)
        return NULL;
    fg_buf b = {0};
    fg_buf_puts(&b, "Repository files (up to 80, sorted):\n");
    while (sqlite3_step(s) == SQLITE_ROW)
        fg_buf_printf(&b, "%s [%s]\n", (const char *)sqlite3_column_text(s, 0),
                      (const char *)sqlite3_column_text(s, 1));
    sqlite3_finalize(s);
    return fg_buf_take(&b);
}
char *fg_repo_search(forge_repo *r, const char *query, size_t limit, forge_error *e) {
    if (!query || !*query) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Search query is empty");
        return NULL;
    }
    r->error = e;
    /* Literal search avoids exposing FTS query syntax and reports exact source lines. */
    sqlite3_stmt *s =
        prepare(r, "SELECT path,content FROM chunks WHERE instr(content,?)>0 ORDER BY path");
    if (!s)
        return NULL;
    sqlite3_bind_text(s, 1, query, -1, SQLITE_TRANSIENT);
    fg_buf b = {0};
    size_t hits = 0;
    while (hits < limit && sqlite3_step(s) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(s, 0),
                   *text = (const char *)sqlite3_column_text(s, 1);
        const char *p = text;
        size_t line = 1;
        while (*p && hits < limit) {
            const char *end = strchr(p, '\n');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            const char *match = strstr(p, query);
            if (match && match < p + n) {
                fg_buf_printf(&b, "%s:%zu:", path, line);
                fg_buf_add(&b, p, FG_MIN(n, 512));
                fg_buf_puts(&b, "\n");
                hits++;
            }
            if (!end)
                break;
            p = end + 1;
            line++;
        }
    }
    sqlite3_finalize(s);
    if (!hits)
        fg_buf_puts(&b, "No matches.\n");
    if (hits == limit)
        fg_buf_puts(&b, "[result limit reached]\n");
    return fg_buf_take(&b);
}
char *fg_repo_targets(forge_repo *r, const char *path, forge_error *e) {
    const char *paths[] = {path};
    char *plan = forge_repo_validation_plan(r, paths, 1, e);
    if (!plan)
        return NULL;
    yyjson_doc *doc = yyjson_read(plan, strlen(plan), 0);
    free(plan);
    if (!doc) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot read targeted validation suggestion");
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *affected = yyjson_obj_get(root, "affected_packages");
    yyjson_val *dependents = yyjson_obj_get(root, "reverse_dependents");
    fg_buf b = {0};
    fg_buf_puts(&b, "compile/test affected Go packages");
    size_t count = yyjson_arr_size(affected);
    for (size_t i = 0; i < FG_MIN(count, 4u); i++)
        fg_buf_printf(&b, "%s%s", i ? ", " : ": ", yyjson_get_str(yyjson_arr_get(affected, i)));
    if (count > 4)
        fg_buf_printf(&b, " (+%zu more)", count - 4);
    fg_buf_printf(&b,
                  "; %zu reverse dependents; then vet and full module tests. "
                  "Use the validation plan for exact argv and module working directories.",
                  yyjson_arr_size(dependents));
    yyjson_doc_free(doc);
    return fg_buf_take(&b);
}
uint64_t forge_repo_generation(const forge_repo *r) {
    return r ? r->generation : 0;
}
void forge_repo_close(forge_repo *r) {
    if (r) {
        if (r->parser)
            ts_parser_delete(r->parser);
        if (r->db)
            sqlite3_close(r->db);
        free(r);
    }
}
