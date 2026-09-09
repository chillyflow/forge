#include "repo_internal.h"
#include "forge/validation.h"
#include "core/digest.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#endif
extern const TSLanguage *tree_sitter_go(void);
static bool sql(forge_repo *r, const char *query, forge_error *e) {
    char *message = NULL;
    int rc = sqlite3_exec(r->db, query, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        if (!e || (e->code != FORGE_ERR_CANCELLED && e->code != FORGE_ERR_LIMIT))
            fg_error(e, FORGE_ERR_IO, "Repository database: %s",
                     message ? message : sqlite3_errmsg(r->db));
        sqlite3_free(message);
        return false;
    }
    return true;
}
static sqlite3_stmt *prepare(forge_repo *r, const char *query) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(r->db, query, -1, &s, NULL) != SQLITE_OK &&
        (!r->error || (r->error->code != FORGE_ERR_CANCELLED && r->error->code != FORGE_ERR_LIMIT)))
        fg_error(r->error, FORGE_ERR_IO, "Index query: %s", sqlite3_errmsg(r->db));
    return s;
}
static bool done(forge_repo *r, sqlite3_stmt *s) {
    if (sqlite3_step(s) == SQLITE_DONE)
        return true;
    if (!r->error || (r->error->code != FORGE_ERR_CANCELLED && r->error->code != FORGE_ERR_LIMIT))
        fg_error(r->error, FORGE_ERR_IO, "Index update: %s", sqlite3_errmsg(r->db));
    return false;
}
static bool index_stopped(forge_repo *r) {
    if (r->index_cancelled && r->index_cancelled(r->index_userdata)) {
        fg_error(r->error, FORGE_ERR_CANCELLED, "Repository indexing cancelled");
        return true;
    }
    if (r->index_deadline && fg_now_ms() >= r->index_deadline) {
        fg_error(r->error, FORGE_ERR_LIMIT, "Repository indexing deadline exceeded");
        return true;
    }
    return false;
}
static uint64_t index_wait(forge_repo *r, uint64_t maximum) {
    uint64_t now = fg_now_ms();
    if (r->index_deadline)
        return r->index_deadline > now ? FG_MIN(maximum, r->index_deadline - now) : 1;
    return maximum;
}
static int index_busy(void *userdata, int previous_calls) {
    forge_repo *r = userdata;
    uint64_t now = fg_now_ms();
    if (!previous_calls)
        r->index_busy_started = now;
    if (index_stopped(r) || r->index_busy_limit <= 0)
        return 0;
    uint64_t elapsed = now - r->index_busy_started;
    if (elapsed >= (uint64_t)r->index_busy_limit)
        return 0;
    uint64_t remaining = (uint64_t)r->index_busy_limit - elapsed;
    sqlite3_sleep((int)index_wait(r, FG_MIN(remaining, 10u)));
    return !index_stopped(r);
}
static forge_status index_process(forge_repo *r, const char *const *args, size_t cap,
                                  fg_process_result *result, forge_error *e) {
    if (index_stopped(r))
        return r->error->code;
    forge_status status = fg_process(r->root, args, index_wait(r, 30000), cap, r->index_cancelled,
                                     r->index_userdata, result, e);
    if (index_stopped(r))
        return r->error->code;
    return status;
}
static void count_add(uint64_t *value, uint64_t amount) {
    *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount;
}
static uint64_t cache_tick(forge_repo *r) {
    count_add(&r->cache_clock, 1);
    return r->cache_clock;
}
static fg_repo_tree *cache_find(forge_repo *r, const char *path) {
    for (fg_repo_tree *entry = r->trees; entry; entry = entry->next)
        if (!strcmp(entry->path, path))
            return entry;
    return NULL;
}
static void cache_remove(forge_repo *r, fg_repo_tree **slot) {
    fg_repo_tree *entry = *slot;
    *slot = entry->next;
    r->index_stats.cached_files--;
    r->index_stats.cached_source_bytes -= entry->bytes;
    r->index_stats.cached_nodes -= entry->nodes;
    ts_tree_delete(entry->tree);
    free(entry->source);
    free(entry->path);
    free(entry);
}
static void cache_evict(forge_repo *r) {
    fg_repo_tree **oldest = NULL;
    fg_repo_tree **lists[] = {&r->trees, &r->pending_trees};
    for (size_t i = 0; i < sizeof(lists) / sizeof(*lists); i++)
        for (fg_repo_tree **p = lists[i]; *p; p = &(*p)->next)
            if (!oldest || (*p)->used < (*oldest)->used ||
                ((*p)->used == (*oldest)->used && strcmp((*p)->path, (*oldest)->path) < 0))
                oldest = p;
    if (oldest) {
        cache_remove(r, oldest);
        count_add(&r->index_stats.cache_evictions, 1);
    }
}
static bool cache_enabled(const forge_repo *r) {
    return r->index_limits.max_cached_files && r->index_limits.max_cached_source_bytes &&
           r->index_limits.max_cached_nodes;
}
/* Staging and committed entries share one retention budget. Eviction may discard
 * an old committed entry during a failed transaction, but no uncommitted tree
 * ever becomes a committed entry. The next update safely cold-parses a miss. */
static bool cache_stage(forge_repo *r, const char *path, char *source, size_t bytes, TSTree *tree,
                        size_t nodes) {
    if (!cache_enabled(r) || bytes > r->index_limits.max_cached_source_bytes ||
        nodes > r->index_limits.max_cached_nodes) {
        count_add(&r->index_stats.cache_skips, 1);
        return false;
    }
    fg_repo_tree *entry = calloc(1, sizeof(*entry));
    if (!entry || !(entry->path = fg_strdup(path))) {
        free(entry);
        count_add(&r->index_stats.cache_skips, 1);
        return false;
    }
    /* fg_read_file grows geometrically. Do not retain its spare capacity. */
    char *exact = realloc(source, bytes + 1);
    if (!exact) {
        free(entry->path);
        free(entry);
        count_add(&r->index_stats.cache_skips, 1);
        return false;
    }
    source = exact;
    for (fg_repo_tree **p = &r->pending_trees; *p; p = &(*p)->next)
        if (!strcmp((*p)->path, path)) {
            cache_remove(r, p);
            break;
        }
    while (r->index_stats.cached_files >= r->index_limits.max_cached_files ||
           bytes > r->index_limits.max_cached_source_bytes - r->index_stats.cached_source_bytes ||
           nodes > r->index_limits.max_cached_nodes - r->index_stats.cached_nodes)
        cache_evict(r);
    entry->source = source;
    entry->bytes = bytes;
    entry->tree = tree;
    entry->nodes = nodes;
    entry->used = cache_tick(r);
    entry->next = r->pending_trees;
    r->pending_trees = entry;
    forge_index_stats *stats = &r->index_stats;
    stats->cached_files++;
    stats->cached_source_bytes += bytes;
    stats->cached_nodes += nodes;
    stats->peak_cached_files = FG_MAX(stats->peak_cached_files, stats->cached_files);
    stats->peak_cached_source_bytes =
        FG_MAX(stats->peak_cached_source_bytes, stats->cached_source_bytes);
    stats->peak_cached_nodes = FG_MAX(stats->peak_cached_nodes, stats->cached_nodes);
    return true;
}
static void cache_begin(forge_repo *r) {
    for (fg_repo_tree *entry = r->trees; entry; entry = entry->next)
        entry->touched = entry->keep = false;
}
static void cache_finish(forge_repo *r, bool commit, bool full) {
    if (!commit) {
        while (r->pending_trees)
            cache_remove(r, &r->pending_trees);
        return;
    }
    for (fg_repo_tree **p = &r->trees; *p;)
        if ((full || (*p)->touched) && !(*p)->keep) {
            cache_remove(r, p);
            count_add(&r->index_stats.cache_invalidations, 1);
        } else
            p = &(*p)->next;
    while (r->pending_trees) {
        fg_repo_tree *entry = r->pending_trees;
        r->pending_trees = entry->next;
        entry->next = r->trees;
        r->trees = entry;
    }
}
forge_index_limits forge_default_index_limits(void) {
    forge_index_limits limits = {256, 16u * 1024u * 1024u, 500000};
    return limits;
}
forge_status forge_repo_set_index_limits(forge_repo *r, const forge_index_limits *requested,
                                         forge_error *e) {
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    forge_index_limits limits = requested ? *requested : forge_default_index_limits();
    if (limits.max_cached_files > FORGE_INDEX_MAX_CACHE_FILES ||
        limits.max_cached_source_bytes > FORGE_INDEX_MAX_CACHE_SOURCE_BYTES ||
        limits.max_cached_nodes > FORGE_INDEX_MAX_CACHE_NODES)
        return fg_error(e, FORGE_ERR_LIMIT, "Repository cache limits exceed supported bounds");
    if (!sqlite3_get_autocommit(r->db))
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot change cache limits during a transaction");
    r->index_limits = limits;
    while (r->index_stats.cached_files &&
           (!cache_enabled(r) || r->index_stats.cached_files > limits.max_cached_files ||
            r->index_stats.cached_source_bytes > limits.max_cached_source_bytes ||
            r->index_stats.cached_nodes > limits.max_cached_nodes))
        cache_evict(r);
    if (e)
        memset(e, 0, sizeof(*e));
    return FORGE_OK;
}
bool forge_repo_get_index_stats(const forge_repo *r, forge_index_stats *out) {
    if (!r || !out)
        return false;
    *out = r->index_stats;
    return true;
}

static TSPoint point_advance(TSPoint point, const char *text, size_t start, size_t end) {
    for (size_t i = start; i < end; i++)
        if (text[i] == '\n') {
            point.row++;
            point.column = 0;
        } else
            point.column++;
    return point;
}
static bool continuation(char byte) {
    return ((unsigned char)byte & 0xc0u) == 0x80u;
}
static TSInputEdit source_edit(const char *old, size_t old_size, const char *text, size_t size) {
    size_t start = 0, old_end = old_size, new_end = size;
    while (start < old_size && start < size && old[start] == text[start])
        start++;
    /* A shared UTF-8 lead byte is not a valid edit boundary. */
    while (start && ((start < old_size && continuation(old[start])) ||
                     (start < size && continuation(text[start]))))
        start--;
    while (old_end > start && new_end > start && old[old_end - 1] == text[new_end - 1]) {
        old_end--;
        new_end--;
    }
    while (old_end < old_size && new_end < size &&
           (continuation(old[old_end]) || continuation(text[new_end]))) {
        old_end++;
        new_end++;
    }
    TSPoint origin = {0, 0};
    TSInputEdit edit = {0};
    edit.start_byte = (uint32_t)start;
    edit.old_end_byte = (uint32_t)old_end;
    edit.new_end_byte = (uint32_t)new_end;
    edit.start_point = point_advance(origin, old, 0, start);
    edit.old_end_point = point_advance(edit.start_point, old, start, old_end);
    edit.new_end_point = point_advance(edit.start_point, text, start, new_end);
    return edit;
}
static TSTree *parse_go(forge_repo *r, const char *text, size_t size, fg_repo_tree *cached) {
    if (index_stopped(r))
        return NULL;
    TSTree *edited = NULL;
    if (cached) {
        edited = ts_tree_copy(cached->tree);
        if (!edited) {
            fg_error(r->error, FORGE_ERR_MEMORY, "Cannot copy cached Go syntax tree");
            return NULL;
        }
        TSInputEdit edit = source_edit(cached->source, cached->bytes, text, size);
        ts_tree_edit(edited, &edit);
        cached->used = cache_tick(r);
        count_add(&r->index_stats.cache_hits, 1);
        count_add(&r->index_stats.incremental_parses, 1);
    } else {
        count_add(&r->index_stats.cache_misses, 1);
        count_add(&r->index_stats.cold_parses, 1);
    }
    TSTree *tree = ts_parser_parse_string(r->parser, edited, text, (uint32_t)size);
    if (edited)
        ts_tree_delete(edited);
    if (index_stopped(r)) {
        if (tree)
            ts_tree_delete(tree);
        ts_parser_reset(r->parser);
        return NULL;
    }
    if (!tree) {
        ts_parser_reset(r->parser);
        fg_error(r->error, FORGE_ERR_PARSE, "Cannot parse Go source");
    }
    return tree;
}

typedef struct {
    uint64_t ast, symbols;
    size_t nodes, symbol_count;
} syntax_hashes;
static void hash_bytes(uint64_t *hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) {
        *hash ^= bytes[i];
        *hash *= UINT64_C(1099511628211);
    }
}
static void hash_number(uint64_t *hash, uint64_t value) {
    unsigned char bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(value & 255u);
        value >>= 8;
    }
    hash_bytes(hash, bytes, sizeof(bytes));
}
static void hash_text(uint64_t *hash, const char *text) {
    size_t n = text ? strlen(text) : 0;
    hash_number(hash, n);
    hash_bytes(hash, text, n);
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
                       TSNode name, const char *kind, uint64_t source_hash,
                       const char source_digest[65], syntax_hashes *hashes) {
    if (ts_node_is_null(name))
        return true;
    char *n = slice(text, name);
    if (!n) {
        fg_error(r->error, FORGE_ERR_MEMORY, "Cannot allocate Go symbol name");
        return false;
    }
    TSNode body = ts_node_child_by_field_name(node, "body", 4);
    uint32_t begin = ts_node_start_byte(node), end = ts_node_end_byte(node),
             sig_end = ts_node_is_null(body) ? end : ts_node_start_byte(body);
    size_t sig_len = fg_utf8_prefix(text + begin, sig_end - begin, 512);
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
    bool ok = done(r, s);
    sqlite3_finalize(s);
    if (ok) {
        hash_text(&hashes->symbols, n);
        hash_text(&hashes->symbols, kind);
        hash_number(&hashes->symbols, source_hash);
        hashes->symbol_count++;
        sqlite3_int64 symbol = sqlite3_last_insert_rowid(r->db);
        char hash[17];
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)source_hash);
        s = prepare(r, "INSERT INTO symbol_hashes(symbol_id,source_hash) VALUES(?,?)");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, symbol);
            sqlite3_bind_text(s, 2, hash, -1, SQLITE_TRANSIENT);
            ok = done(r, s);
            sqlite3_finalize(s);
        }
        if (ok) {
            s = prepare(r, "INSERT INTO symbol_digests(symbol_id,sha256,version) VALUES(?,?,1)");
            if (!s)
                ok = false;
            else {
                sqlite3_bind_int64(s, 1, symbol);
                sqlite3_bind_text(s, 2, source_digest, -1, SQLITE_TRANSIENT);
                ok = done(r, s);
                sqlite3_finalize(s);
            }
        }
    }
    free(n);
    return ok;
}
static bool visit_node(forge_repo *r, sqlite3_int64 file, const char *text, TSNode node,
                       syntax_hashes *hashes) {
    const char *type = ts_node_type(node);
    if (!strcmp(type, "function_declaration") || !strcmp(type, "method_declaration") ||
        !strcmp(type, "type_spec") || !strcmp(type, "type_alias") || !strcmp(type, "const_spec") ||
        !strcmp(type, "var_spec")) {
        if (index_stopped(r))
            return false;
        uint32_t start = ts_node_start_byte(node), end = ts_node_end_byte(node);
        uint64_t source_hash = fg_hash(text + start, end - start);
        char source_digest[65];
        if (!fg_sha256_hex(text + start, end - start, source_digest))
            return false;
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            if (!(i & 255u) && index_stopped(r))
                return false;
            const char *field = ts_node_field_name_for_child(node, i);
            if (field && !strcmp(field, "name") &&
                !add_symbol(r, file, text, node, ts_node_child(node, i), type, source_hash,
                            source_digest, hashes))
                return false;
        }
    }
    if (!strcmp(type, "identifier") || !strcmp(type, "field_identifier") ||
        !strcmp(type, "type_identifier") || !strcmp(type, "package_identifier")) {
        char *name = slice(text, node);
        if (!name) {
            fg_error(r->error, FORGE_ERR_MEMORY, "Cannot allocate Go reference");
            return false;
        }
        sqlite3_stmt *s = prepare(r, "INSERT INTO refs(file_id,name,line,byte) VALUES(?,?,?,?)");
        if (!s) {
            free(name);
            return false;
        }
        sqlite3_bind_int64(s, 1, file);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)ts_node_start_point(node).row + 1);
        sqlite3_bind_int64(s, 4, ts_node_start_byte(node));
        bool ok = done(r, s);
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
        if (!path) {
            fg_error(r->error, FORGE_ERR_MEMORY, "Cannot allocate Go import");
            return false;
        }
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
            bool ok = done(r, s);
            sqlite3_finalize(s);
            if (!ok) {
                free(path);
                return false;
            }
        }
        free(path);
    }
    return true;
}
/* Traverse every exposed node without a C recursion/depth cutoff. Hashes omit
 * identity pointers and has_changes, which depend on incremental parse history. */
static bool visit(forge_repo *r, sqlite3_int64 file, const char *text, size_t size, TSNode root,
                  syntax_hashes *hashes) {
    hashes->ast = fg_hash("forge-go-ast-1", 14);
    hashes->symbols = fg_hash("forge-go-symbols-1", 18);
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool ok = true, finished = false;
    while (!finished && ok) {
        if (!(hashes->nodes & 255u) && index_stopped(r)) {
            ok = false;
            break;
        }
        TSNode node = ts_tree_cursor_current_node(&cursor);
        uint32_t start = ts_node_start_byte(node), end = ts_node_end_byte(node);
        if (start > end || end > size) {
            fg_error(r->error, FORGE_ERR_PARSE, "Go syntax node exceeds source bounds");
            ok = false;
            break;
        }
        TSPoint a = ts_node_start_point(node), b = ts_node_end_point(node);
        uint64_t flags = (ts_node_is_named(node) ? 1u : 0u) | (ts_node_is_extra(node) ? 2u : 0u) |
                         (ts_node_is_missing(node) ? 4u : 0u) | (ts_node_is_error(node) ? 8u : 0u) |
                         (ts_node_has_error(node) ? 16u : 0u);
        uint32_t children = ts_node_child_count(node);
        hash_text(&hashes->ast, ts_node_type(node));
        hash_text(&hashes->ast, ts_tree_cursor_current_field_name(&cursor));
        hash_number(&hashes->ast, start);
        hash_number(&hashes->ast, end);
        hash_number(&hashes->ast, a.row);
        hash_number(&hashes->ast, a.column);
        hash_number(&hashes->ast, b.row);
        hash_number(&hashes->ast, b.column);
        hash_number(&hashes->ast, flags);
        hash_number(&hashes->ast, children);
        if (!children) {
            hash_number(&hashes->ast, end - start);
            hash_bytes(&hashes->ast, text + start, end - start);
        }
        hashes->nodes++;
        ok = visit_node(r, file, text, node, hashes);
        if (ts_tree_cursor_goto_first_child(&cursor))
            continue;
        while (!ts_tree_cursor_goto_next_sibling(&cursor))
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                finished = true;
                break;
            }
    }
    ts_tree_cursor_delete(&cursor);
    return ok;
}
static bool save_hashes(forge_repo *r, sqlite3_int64 file, const syntax_hashes *hashes) {
    char ast[17], symbols[17];
    snprintf(ast, sizeof(ast), "%016llx", (unsigned long long)hashes->ast);
    snprintf(symbols, sizeof(symbols), "%016llx", (unsigned long long)hashes->symbols);
    sqlite3_stmt *s = prepare(r, "INSERT INTO file_syntax(file_id,ast_hash,symbol_hash,node_count,"
                                 "symbol_count,version) VALUES(?,?,?,?,?,1)");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, file);
    sqlite3_bind_text(s, 2, ast, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, symbols, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)hashes->nodes);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)hashes->symbol_count);
    bool ok = done(r, s);
    sqlite3_finalize(s);
    return ok;
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
    bool ok = done(r, s);
    if (ok)
        r->changed += (size_t)sqlite3_changes(r->db);
    sqlite3_finalize(s);
    if (!ok)
        return false;
    s = prepare(r, "UPDATE go_module_boundaries SET seen=? WHERE path=?");
    if (!s)
        return false;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
    ok = done(r, s);
    sqlite3_finalize(s);
    return ok;
}
static bool supported(const char *path) {
    if (go_metadata(path))
        return true;
    const char *base = strrchr(path, '/');
    const char *windows_base = strrchr(path, '\\');
    if (!base || (windows_base && windows_base > base))
        base = windows_base;
    base = base ? base + 1 : path;
    if (!strcmp(base, "pytest.ini") || !strcmp(base, ".pytest.ini") || !strcmp(base, "setup.cfg") ||
        !strcmp(base, "tox.ini"))
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
            if (!package) {
                fg_error(r->error, FORGE_ERR_MEMORY, "Cannot allocate Go package metadata");
                return false;
            }
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
    bool ok = done(r, s);
    sqlite3_finalize(s);
    free(package);
    return ok;
}
static bool index_file(const char *path, void *user) {
    forge_repo *r = user;
    if (index_stopped(r))
        return false;
    fg_repo_tree *cached = cache_find(r, path);
    if (cached) {
        cached->touched = true;
        cached->keep = false;
    }
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
        if (relevant && ignored.code != FORGE_ERR_NOT_FOUND)
            r->go_index_incomplete = true;
        return true;
    }
    if (!track_go_module(r, path))
        return false;
    size_t size = 0;
    char *text = fg_read_file(full, FORGE_INDEX_MAX_FILE_BYTES, &size, &ignored);
    if (text) {
        count_add(&r->index_stats.files_read, 1);
        count_add(&r->index_stats.source_bytes_read, size);
    }
    if (index_stopped(r)) {
        free(text);
        return false;
    }
    if (!text) {
        if (ignored.code == FORGE_ERR_MEMORY) {
            *r->error = ignored;
            return false;
        }
        if (relevant)
            r->go_index_incomplete = true;
        return true;
    }
    if (memchr(text, 0, size) || !fg_utf8_valid(text, size)) {
        if (relevant)
            r->go_index_incomplete = true;
        free(text);
        return true;
    }
    char hash[32], digest[65];
    snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)fg_hash(text, size));
    if (!fg_sha256_hex(text, size, digest)) {
        free(text);
        fg_error(r->error, FORGE_ERR_LIMIT, "Indexed source exceeds digest limits");
        return false;
    }
    sqlite3_stmt *s =
        prepare(r, "SELECT f.id,f.hash,c.content,g.file_id,x.version,d.sha256,d.version,"
                   "(SELECT count(*) FROM symbols z LEFT JOIN symbol_digests h "
                   "ON h.symbol_id=z.id WHERE z.file_id=f.id AND "
                   "(h.version IS NULL OR h.version<>1 OR length(h.sha256)<>64)) "
                   "FROM files f "
                   "LEFT JOIN chunks c ON c.rowid=f.id "
                   "LEFT JOIN go_files g ON g.file_id=f.id "
                   "LEFT JOIN file_syntax x ON x.file_id=f.id "
                   "LEFT JOIN file_digests d ON d.file_id=f.id WHERE f.path=?");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_int64 id = 0;
    bool unchanged = false, cache_matches = false;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(s, 0);
        const char *old_hash = (const char *)sqlite3_column_text(s, 1);
        const char *old_source = (const char *)sqlite3_column_text(s, 2);
        size_t old_size = (size_t)sqlite3_column_bytes(s, 2);
        unchanged = old_hash && old_source && !strcmp(old_hash, hash) && old_size == size &&
                    !memcmp(old_source, text, size);
        cache_matches = cached && old_source && cached->bytes == old_size &&
                        !memcmp(cached->source, old_source, old_size);
        /* Old indexes are upgraded lazily even when source bytes are unchanged. */
        if (is_go && (sqlite3_column_type(s, 3) == SQLITE_NULL || sqlite3_column_int(s, 4) != 1))
            unchanged = false;
        const char *old_digest = (const char *)sqlite3_column_text(s, 5);
        if (!old_digest || strcmp(old_digest, digest) ||
            sqlite3_column_type(s, 6) != SQLITE_INTEGER || sqlite3_column_int64(s, 6) != 1 ||
            sqlite3_column_int64(s, 7) != 0)
            unchanged = false;
    } else if (rc != SQLITE_DONE) {
        fg_error(r->error, FORGE_ERR_IO, "Cannot read indexed source: %s", sqlite3_errmsg(r->db));
        sqlite3_finalize(s);
        free(text);
        return false;
    }
    sqlite3_finalize(s);
    if (unchanged) {
        count_add(&r->index_stats.unchanged_files, 1);
        if (cache_matches) {
            cached->keep = true;
            cached->used = cache_tick(r);
        }
        s = prepare(r, "UPDATE files SET seen=? WHERE id=?");
        if (!s) {
            free(text);
            return false;
        }
        sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
        sqlite3_bind_int64(s, 2, id);
        bool ok = done(r, s);
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
        if (!done(r, s)) {
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
    bool ok = done(r, s);
    sqlite3_finalize(s);
    if (!ok) {
        free(text);
        return false;
    }
    id = sqlite3_last_insert_rowid(r->db);
    s = prepare(r, "INSERT INTO file_digests(file_id,sha256,version) VALUES(?,?,1)");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, digest, -1, SQLITE_TRANSIENT);
    ok = done(r, s);
    sqlite3_finalize(s);
    if (!ok) {
        free(text);
        return false;
    }
    s = prepare(r, "INSERT INTO chunks(rowid,path,content) VALUES(?,?,?)");
    if (!s) {
        free(text);
        return false;
    }
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, text, (int)size, SQLITE_TRANSIENT);
    ok = done(r, s);
    sqlite3_finalize(s);
    if (ok && is_go) {
        TSTree *tree = parse_go(r, text, size, cache_matches ? cached : NULL);
        if (!tree)
            ok = false;
        else {
            TSNode root = ts_tree_root_node(tree);
            syntax_hashes hashes = {0};
            ok = go_file_metadata(r, id, path, text, root) &&
                 visit(r, id, text, size, root, &hashes) && save_hashes(r, id, &hashes);
            if (ok && cache_stage(r, path, text, size, tree, hashes.nodes))
                text = NULL;
            else
                ts_tree_delete(tree);
        }
    }
    if (ok)
        count_add(&r->index_stats.files_indexed, 1);
    free(text);
    return ok;
}
typedef struct {
    uint64_t generation, scan;
    bool incomplete, filesystem;
} repo_state;
static repo_state current_state(const forge_repo *r) {
    repo_state state = {r->generation, r->scan, r->go_index_incomplete, r->filesystem_scan};
    return state;
}
static void apply_state(forge_repo *r, repo_state state) {
    r->generation = state.generation;
    r->scan = state.scan;
    r->go_index_incomplete = state.incomplete;
    r->filesystem_scan = state.filesystem;
}
static bool read_state(forge_repo *r, repo_state *state) {
    sqlite3_stmt *s = prepare(r, "SELECT key,value FROM meta WHERE key IN "
                                 "('generation','scan','go_index_incomplete','filesystem_scan')");
    if (!s)
        return false;
    unsigned found = 0;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(s, 0);
        sqlite3_int64 value = sqlite3_column_int64(s, 1);
        if (sqlite3_column_type(s, 1) != SQLITE_INTEGER || value < 0) {
            fg_error(r->error, FORGE_ERR_PARSE, "Invalid repository metadata: %s", key);
            sqlite3_finalize(s);
            return false;
        }
        if (!strcmp(key, "generation")) {
            state->generation = (uint64_t)value;
            found |= 1;
        } else if (!strcmp(key, "scan")) {
            state->scan = (uint64_t)value;
            found |= 2;
        } else {
            if (value > 1) {
                fg_error(r->error, FORGE_ERR_PARSE, "Invalid repository metadata: %s", key);
                sqlite3_finalize(s);
                return false;
            }
            if (!strcmp(key, "go_index_incomplete")) {
                state->incomplete = value != 0;
                found |= 4;
            } else {
                state->filesystem = value != 0;
                found |= 8;
            }
        }
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE || found != 15) {
        fg_error(r->error, FORGE_ERR_IO, "Cannot read repository generation metadata");
        return false;
    }
    return true;
}
static forge_status begin_update(forge_repo *r, repo_state *previous, forge_error *e) {
    *previous = current_state(r);
    if (index_stopped(r))
        return e->code;
    if (!sql(r, "BEGIN IMMEDIATE", e))
        return index_stopped(r) ? e->code : FORGE_ERR_IO;
    repo_state persisted = {0};
    if (!read_state(r, &persisted) || persisted.scan >= INT64_MAX ||
        persisted.generation >= INT64_MAX) {
        if (!e->code)
            fg_error(e, FORGE_ERR_LIMIT, "Repository generation space exhausted");
        sql(r, "ROLLBACK", NULL);
        count_add(&r->index_stats.rollbacks, 1);
        return e->code;
    }
    apply_state(r, persisted);
    r->scan++;
    r->changed = r->files = 0;
    cache_begin(r);
    return FORGE_OK;
}
static forge_status rollback_update(forge_repo *r, repo_state previous, forge_error *e) {
    if (!e->code)
        fg_error(e, FORGE_ERR_IO, "Repository indexing failed: %s", sqlite3_errmsg(r->db));
    sql(r, "ROLLBACK", NULL);
    apply_state(r, previous);
    r->changed = r->files = 0;
    cache_finish(r, false, false);
    count_add(&r->index_stats.rollbacks, 1);
    return e->code;
}
static forge_status finish_update(forge_repo *r, repo_state previous, bool full,
                                  bool previous_incomplete, forge_error *e) {
    if (index_stopped(r))
        return rollback_update(r, previous, e);
    sqlite3_stmt *s = prepare(r, "SELECT count(*) FROM files");
    if (!s)
        return rollback_update(r, previous, e);
    bool ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok && sqlite3_column_int64(s, 0) > 100000) {
        fg_error(e, FORGE_ERR_LIMIT, "Repository exceeds 100000 indexed files");
        ok = false;
    }
    sqlite3_finalize(s);
    repo_state next = current_state(r);
    if (r->changed || previous_incomplete != r->go_index_incomplete)
        next.generation++;
    if (ok) {
        s = prepare(
            r, "UPDATE meta SET value=CASE key WHEN 'scan' THEN ? WHEN 'generation' "
               "THEN ? WHEN 'go_index_incomplete' THEN ? WHEN 'filesystem_scan' THEN ? "
               "END WHERE key IN ('scan','generation','go_index_incomplete','filesystem_scan')");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)next.scan);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)next.generation);
            sqlite3_bind_int(s, 3, next.incomplete ? 1 : 0);
            sqlite3_bind_int(s, 4, next.filesystem ? 1 : 0);
            ok = done(r, s) && sqlite3_changes(r->db) == 4;
            sqlite3_finalize(s);
        }
    }
    repo_state stored = {0};
    if (ok)
        ok = read_state(r, &stored) && stored.scan == next.scan &&
             stored.generation == next.generation && stored.incomplete == next.incomplete &&
             stored.filesystem == next.filesystem;
    if (ok && index_stopped(r))
        ok = false;
    if (!ok || !sql(r, "COMMIT", e))
        return rollback_update(r, previous, e);
    apply_state(r, next);
    cache_finish(r, true, full);
    count_add(&r->index_stats.commits, 1);
    memset(e, 0, sizeof(*e));
    return FORGE_OK;
}
forge_repo *forge_repo_open(const char *root, forge_error *e) {
    if (e)
        memset(e, 0, sizeof(*e));
    forge_repo *r = calloc(1, sizeof(*r));
    if (!r) {
        fg_error(e, FORGE_ERR_MEMORY, "Repository allocation failed");
        return NULL;
    }
    r->error = e;
    r->index_limits = forge_default_index_limits();
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
             "CREATE TABLE IF NOT EXISTS symbol_hashes(symbol_id INTEGER PRIMARY KEY REFERENCES "
             "symbols(id) ON DELETE CASCADE,source_hash TEXT NOT NULL);"
             "CREATE TABLE IF NOT EXISTS file_digests(file_id INTEGER PRIMARY KEY REFERENCES "
             "files(id) ON DELETE CASCADE,sha256 TEXT NOT NULL,version INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS symbol_digests(symbol_id INTEGER PRIMARY KEY REFERENCES "
             "symbols(id) ON DELETE CASCADE,sha256 TEXT NOT NULL,version INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS summary_cache(id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "cache_key TEXT NOT NULL UNIQUE,recipe_hash TEXT NOT NULL,dependency_hash TEXT NOT "
             "NULL,"
             "manifest TEXT NOT NULL,content TEXT NOT NULL,content_hash TEXT NOT NULL,"
             "created_generation INTEGER NOT NULL,validated_generation INTEGER NOT NULL,"
             "version INTEGER NOT NULL);"
             "CREATE TABLE IF NOT EXISTS file_syntax(file_id INTEGER PRIMARY KEY REFERENCES "
             "files(id) ON DELETE CASCADE,ast_hash TEXT NOT NULL,symbol_hash TEXT NOT NULL,"
             "node_count INTEGER NOT NULL,symbol_count INTEGER NOT NULL,version INTEGER NOT NULL);"
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
    repo_state state = {0};
    if (!read_state(r, &state)) {
        forge_repo_close(r);
        return NULL;
    }
    apply_state(r, state);
    r->parser = ts_parser_new();
    if (!r->parser || !ts_parser_set_language(r->parser, tree_sitter_go())) {
        fg_error(e, FORGE_ERR_PARSE, "Go parser ABI mismatch");
        forge_repo_close(r);
        return NULL;
    }
    return r;
}
static bool fallback_path(const char *path);
static bool index_walk(forge_repo *r, const char *relative, unsigned depth) {
    if (index_stopped(r))
        return false;
    if (depth > 256) {
        fg_error(r->error, FORGE_ERR_LIMIT, "Repository directory nesting exceeds 256");
        return false;
    }
    /* Keep path storage off the C stack even for deeply nested repositories. */
    typedef struct {
        char directory[FG_PATH_MAX], next[FG_PATH_MAX], full[FG_PATH_MAX];
    } walk_paths;
    walk_paths *paths = malloc(sizeof(*paths));
    if (!paths) {
        fg_error(r->error, FORGE_ERR_MEMORY, "Cannot allocate repository traversal paths");
        return false;
    }
    bool ok = true;
    if (*relative)
        ok = fg_path_join(paths->directory, r->root, relative);
    else
        strcpy(paths->directory, r->root);
    if (!ok) {
        fg_error(r->error, FORGE_ERR_LIMIT, "Repository path exceeds supported length");
        free(paths);
        return false;
    }
#ifdef _WIN32
    WIN32_FIND_DATAA entry;
    HANDLE iterator = INVALID_HANDLE_VALUE;
    if (fg_path_join(paths->full, paths->directory, "*"))
        iterator = FindFirstFileA(paths->full, &entry);
    if (iterator == INVALID_HANDLE_VALUE) {
        fg_error(r->error, FORGE_ERR_IO, "Cannot enumerate repository directory: %s", relative);
        free(paths);
        return false;
    }
    bool more = true;
    while (more && ok) {
        if (index_stopped(r)) {
            ok = false;
            break;
        }
        const char *name = entry.cFileName;
        if (fallback_path(name) && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (*relative)
                ok = fg_path_join(paths->next, relative, name);
            else
                strcpy(paths->next, name);
            if (ok)
                ok = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                         ? index_walk(r, paths->next, depth + 1)
                         : index_file(paths->next, r);
        }
        if (ok) {
            more = FindNextFileA(iterator, &entry) != 0;
            if (!more && GetLastError() != ERROR_NO_MORE_FILES) {
                fg_error(r->error, FORGE_ERR_IO, "Repository enumeration failed: %s", relative);
                ok = false;
            }
        }
    }
    FindClose(iterator);
#else
    DIR *directory = opendir(paths->directory);
    if (!directory) {
        fg_error(r->error, FORGE_ERR_IO, "Cannot enumerate repository directory: %s", relative);
        free(paths);
        return false;
    }
    while (ok) {
        if (index_stopped(r)) {
            ok = false;
            break;
        }
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) {
                fg_error(r->error, FORGE_ERR_IO, "Repository enumeration failed: %s", relative);
                ok = false;
            }
            break;
        }
        if (!fallback_path(entry->d_name))
            continue;
        if (*relative)
            ok = fg_path_join(paths->next, relative, entry->d_name);
        else
            strcpy(paths->next, entry->d_name);
        if (ok)
            ok = fg_path_join(paths->full, r->root, paths->next);
        if (!ok)
            break;
        struct stat info;
        if (lstat(paths->full, &info) != 0 || S_ISLNK(info.st_mode))
            continue;
        if (S_ISDIR(info.st_mode))
            ok = index_walk(r, paths->next, depth + 1);
        else if (S_ISREG(info.st_mode))
            ok = index_file(paths->next, r);
    }
    closedir(directory);
#endif
    free(paths);
    if (!ok && !r->error->code)
        fg_error(r->error, FORGE_ERR_LIMIT, "Repository path exceeds supported length");
    return ok && !index_stopped(r);
}
forge_status forge_repo_index(forge_repo *r, forge_error *e) {
    forge_error local = {0};
    if (!e)
        e = &local;
    memset(e, 0, sizeof(*e));
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    if (r->snapshot_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot index during a repository snapshot");
    r->error = e;
    count_add(&r->index_stats.full_attempts, 1);
    repo_state previous;
    forge_status status = begin_update(r, &previous, e);
    if (status != FORGE_OK)
        return status;
    bool previous_incomplete = r->go_index_incomplete;
    r->go_index_incomplete = false;
    r->filesystem_scan = false;
    /* Sparse-index expansion may otherwise lazily fetch missing tree objects.
     * Older Git must fail into the native fallback, not retry without this. */
    const char *args[] = {"git", "--no-lazy-fetch", "-c",       "core.fsmonitor=false", "ls-files",
                          "-z",  "--cached",        "--others", "--exclude-standard",   NULL};
    fg_process_result result = {0};
    forge_status run = index_process(r, args, 16u * 1024u * 1024u, &result, e);
    bool ok = true;
    if (run == FORGE_ERR_CANCELLED || run == FORGE_ERR_LIMIT) {
        ok = false;
    } else if (run == FORGE_OK && result.exit_code == 0 && !result.truncated) {
        size_t offset = 0;
        while (offset < result.out_len) {
            const char *p = result.out + offset;
            size_t remaining = result.out_len - offset;
            const char *end = memchr(p, 0, remaining);
            if (!end) {
                fg_error(e, FORGE_ERR_PARSE, "Invalid NUL-delimited Git path response");
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
        memset(e, 0, sizeof(*e));
        ok = index_walk(r, "", 0);
    }
    fg_process_free(&result);
    if (ok) {
        sqlite3_stmt *s = prepare(r, "DELETE FROM files WHERE seen<>?");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
            ok = done(r, s);
            if (ok) {
                size_t removed = (size_t)sqlite3_changes(r->db);
                r->changed += removed;
                count_add(&r->index_stats.files_removed, removed);
            }
            sqlite3_finalize(s);
        }
    }
    if (ok) {
        sqlite3_stmt *s = prepare(r, "DELETE FROM go_module_boundaries WHERE seen<>?");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)r->scan);
            ok = done(r, s);
            if (ok)
                r->changed += (size_t)sqlite3_changes(r->db);
            sqlite3_finalize(s);
        }
    }
    if (!ok)
        return rollback_update(r, previous, e);
    return finish_update(r, previous, true, previous_incomplete, e);
}

typedef struct {
    char *path;
    bool eligible;
} index_path;
static int compare_index_path(const void *a, const void *b) {
    return strcmp(((const index_path *)a)->path, ((const index_path *)b)->path);
}
static bool fallback_path(const char *path) {
    /* Match fg_walk's deliberately limited non-Git discovery policy. */
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t size = end ? (size_t)(end - p) : strlen(p);
        static const char *const excluded[] = {"node_modules", "vendor", "target", "dist",
                                               "__pycache__"};
        if (*p == '.' || (size >= 5 && !strncmp(p, "build", 5)))
            return false;
        for (size_t i = 0; i < sizeof(excluded) / sizeof(*excluded); i++)
            if (strlen(excluded[i]) == size && !memcmp(p, excluded[i], size))
                return false;
        if (!end)
            break;
        p = end + 1;
    }
    return true;
}
static bool git_path_eligibility(forge_repo *r, index_path *paths, size_t count, forge_error *e) {
    for (size_t start = 0; start < count;) {
        const char *args[267] = {"git",
                                 "--no-lazy-fetch",
                                 "--literal-pathspecs",
                                 "-c",
                                 "core.fsmonitor=false",
                                 "ls-files",
                                 "-z",
                                 "--cached",
                                 "--others",
                                 "--exclude-standard",
                                 "--"};
        size_t end = start, bytes = 0, argc = 11;
        /* Keep Windows command-line expansion well below its UTF-16 limit. */
        while (end < count && end - start < 255) {
            size_t n = strlen(paths[end].path) + 4;
            if (end > start && n > 8192 - bytes)
                break;
            args[argc++] = paths[end++].path;
            bytes += n;
        }
        args[argc] = NULL;
        fg_process_result result = {0};
        forge_status status = index_process(r, args, 2u * 1024u * 1024u, &result, e);
        if (status != FORGE_OK || result.exit_code != 0 || result.truncated) {
            fg_process_free(&result);
            if (status != FORGE_ERR_CANCELLED && status != FORGE_ERR_LIMIT)
                fg_error(e, FORGE_ERR_CONFLICT,
                         "Git path eligibility could not be established; perform a full index");
            return false;
        }
        size_t offset = 0;
        while (offset < result.out_len) {
            const char *path = result.out + offset;
            const char *zero = memchr(path, 0, result.out_len - offset);
            if (!zero) {
                fg_process_free(&result);
                fg_error(e, FORGE_ERR_PARSE, "Invalid NUL-delimited Git path response");
                return false;
            }
            for (size_t i = start; i < end; i++)
                if (!strcmp(paths[i].path, path))
                    paths[i].eligible = true;
            offset += (size_t)(zero - path) + 1;
        }
        fg_process_free(&result);
        start = end;
    }
    return true;
}
static bool delete_unseen_path(forge_repo *r, const char *path) {
    static const char *const queries[] = {
        "DELETE FROM files WHERE path=? AND seen<>?",
        "DELETE FROM go_module_boundaries WHERE path=? AND seen<>?"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(*queries); i++) {
        sqlite3_stmt *s = prepare(r, queries[i]);
        if (!s)
            return false;
        sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)r->scan);
        bool ok = done(r, s);
        if (ok) {
            size_t removed = (size_t)sqlite3_changes(r->db);
            r->changed += removed;
            if (!i)
                count_add(&r->index_stats.files_removed, removed);
        }
        sqlite3_finalize(s);
        if (!ok)
            return false;
    }
    return true;
}
forge_status forge_repo_index_paths(forge_repo *r, const char *const *requested, size_t count,
                                    forge_error *e) {
    forge_error local = {0};
    if (!e)
        e = &local;
    memset(e, 0, sizeof(*e));
    if (!r || (count && !requested))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository or paths");
    if (r->snapshot_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot index during a repository snapshot");
    r->error = e;
    count_add(&r->index_stats.delta_attempts, 1);
    if (index_stopped(r))
        return e->code;
    if (count > 4096)
        return fg_error(e, FORGE_ERR_LIMIT, "Delta index exceeds 4096 paths");
    if (!count) {
        if (!r->scan)
            return fg_error(e, FORGE_ERR_CONFLICT, "A full index must precede delta indexing");
        return FORGE_OK;
    }
    index_path *paths = calloc(count, sizeof(*paths));
    if (!paths)
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate delta index paths");
    forge_status status = FORGE_OK;
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        if (index_stopped(r)) {
            status = e->code;
            goto finish;
        }
        char path[FG_PATH_MAX], full[FG_PATH_MAX];
        if (!fg_relative_path(requested[i], path, e)) {
            status = e ? e->code : FORGE_ERR_POLICY;
            goto finish;
        }
        size_t n = strlen(path);
        if (n > 1024u * 1024u - bytes) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Delta paths exceed 1 MiB");
            goto finish;
        }
        bytes += n;
        forge_error check = {0};
        if (!fg_safe_path(r->root, path, false, full, &check)) {
            if (check.code != FORGE_ERR_NOT_FOUND) {
                if (e)
                    *e = check;
                status = check.code;
                goto finish;
            }
        } else {
#ifdef _WIN32
            DWORD attributes = GetFileAttributesA(full);
            bool directory =
                attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
            struct stat info;
            bool directory = lstat(full, &info) == 0 && S_ISDIR(info.st_mode);
#endif
            if (directory) {
                status = fg_error(e, FORGE_ERR_ARGUMENT,
                                  "Directory deltas require a full index: %s", path);
                goto finish;
            }
        }
        paths[i].path = fg_strdup(path);
        if (!paths[i].path) {
            status = fg_error(e, FORGE_ERR_MEMORY, "Cannot copy delta path");
            goto finish;
        }
    }
    qsort(paths, count, sizeof(*paths), compare_index_path);
    r->error = e;
    repo_state previous;
    status = begin_update(r, &previous, e);
    if (status != FORGE_OK)
        goto finish;
    if (r->scan == 1) {
        fg_error(e, FORGE_ERR_CONFLICT, "A full index must precede delta indexing");
        status = rollback_update(r, previous, e);
        goto finish;
    }
    bool previous_incomplete = r->go_index_incomplete;
    if (r->filesystem_scan) {
        for (size_t i = 0; i < count; i++)
            paths[i].eligible = fallback_path(paths[i].path);
    } else if (!git_path_eligibility(r, paths, count, e)) {
        status = rollback_update(r, previous, e);
        goto finish;
    }
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        if (index_stopped(r)) {
            ok = false;
            break;
        }
        if (i && !strcmp(paths[i - 1].path, paths[i].path))
            continue;
        fg_repo_tree *cached = cache_find(r, paths[i].path);
        if (cached)
            cached->touched = true;
        if (paths[i].eligible)
            ok = index_file(paths[i].path, r);
        if (ok)
            ok = delete_unseen_path(r, paths[i].path);
    }
    status = ok ? finish_update(r, previous, false, previous_incomplete, e)
                : rollback_update(r, previous, e);
finish:
    for (size_t i = 0; i < count; i++)
        free(paths[i].path);
    free(paths);
    return status;
}
typedef struct {
    uint64_t deadline;
    forge_cancel_fn cancelled;
    void *userdata;
    int busy_timeout;
    uint64_t busy_started;
    int busy_limit;
    bool active;
} index_scope;
static index_scope enter_scope(forge_repo *r, uint64_t deadline, forge_cancel_fn cancelled,
                               void *userdata) {
    index_scope previous = {r->index_deadline,     r->index_cancelled,  r->index_userdata,    5000,
                            r->index_busy_started, r->index_busy_limit, r->index_scope_active};
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(r->db, "PRAGMA busy_timeout", -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        previous.busy_timeout = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    r->index_deadline = deadline;
    r->index_cancelled = cancelled;
    r->index_userdata = userdata;
    r->index_busy_limit = previous.active ? previous.busy_limit : previous.busy_timeout;
    r->index_scope_active = true;
    sqlite3_busy_handler(r->db, index_busy, r);
    return previous;
}
static void leave_scope(forge_repo *r, index_scope previous) {
    r->index_deadline = previous.deadline;
    r->index_cancelled = previous.cancelled;
    r->index_userdata = previous.userdata;
    r->index_busy_started = previous.busy_started;
    r->index_busy_limit = previous.busy_limit;
    r->index_scope_active = previous.active;
    if (previous.active)
        sqlite3_busy_handler(r->db, index_busy, r);
    else
        sqlite3_busy_timeout(r->db, previous.busy_timeout);
}
typedef struct {
    index_scope previous;
    forge_error local, *error, *previous_error;
    uint64_t steps, max_steps;
    int interval;
    forge_status stopped;
} snapshot_state;
bool fg_repo_snapshot_stopped(fg_repo_snapshot *scope) {
    if (!scope || !scope->internal || !scope->repo)
        return true;
    snapshot_state *state = scope->internal;
    if (!state->stopped && index_stopped(scope->repo))
        state->stopped = state->error->code;
    if (state->stopped) {
        if (state->error->code != state->stopped)
            fg_error(state->error, state->stopped, "Repository snapshot interrupted");
        return true;
    }
    return false;
}
static int snapshot_progress(void *userdata) {
    fg_repo_snapshot *scope = userdata;
    snapshot_state *state = scope->internal;
    if (fg_repo_snapshot_stopped(scope))
        return 1;
    uint64_t interval = (uint64_t)state->interval;
    if (state->max_steps && interval >= state->max_steps - state->steps) {
        state->stopped = FORGE_ERR_LIMIT;
        fg_error(state->error, FORGE_ERR_LIMIT, "Repository snapshot SQLite work budget exhausted");
        return 1;
    }
    count_add(&state->steps, interval);
    return 0;
}
forge_status fg_repo_snapshot_begin(forge_repo *r, fg_repo_snapshot *scope, bool write,
                                    uint64_t deadline, forge_cancel_fn cancelled, void *userdata,
                                    uint64_t max_vm_steps, forge_error *e) {
    if (e)
        memset(e, 0, sizeof(*e));
    if (!r || !scope)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository snapshot arguments");
    if (r->snapshot_active || r->index_scope_active || !sqlite3_get_autocommit(r->db))
        return fg_error(e, FORGE_ERR_CONFLICT, "Repository snapshots cannot be nested");
    memset(scope, 0, sizeof(*scope));
    snapshot_state *state = calloc(1, sizeof(*state));
    if (!state)
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate repository snapshot");
    scope->repo = r;
    scope->internal = state;
    state->error = e ? e : &state->local;
    state->previous_error = r->error;
    r->error = state->error;
    state->max_steps = max_vm_steps;
    state->interval = max_vm_steps && max_vm_steps < 1000 ? (int)max_vm_steps : 1000;
    state->previous = enter_scope(r, deadline, cancelled, userdata);
    r->snapshot_active = true;
    sqlite3_progress_handler(r->db, state->interval, snapshot_progress, scope);
    repo_state persisted = {0};
    if (fg_repo_snapshot_stopped(scope) ||
        !sql(r, write ? "BEGIN IMMEDIATE" : "BEGIN", state->error) || !read_state(r, &persisted) ||
        fg_repo_snapshot_stopped(scope)) {
        fg_repo_snapshot_stopped(scope);
        forge_status status = state->error->code;
        if (!status)
            status = fg_error(state->error, FORGE_ERR_IO, "Cannot begin repository snapshot");
        fg_repo_snapshot_end(scope, false, e);
        return status;
    }
    scope->generation = persisted.generation;
    scope->go_index_incomplete = persisted.incomplete;
    scope->filesystem_scan = persisted.filesystem;
    return FORGE_OK;
}
forge_status fg_repo_snapshot_end(fg_repo_snapshot *scope, bool commit, forge_error *e) {
    if (!scope || !scope->internal || !scope->repo)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Repository snapshot is not active");
    snapshot_state *state = scope->internal;
    forge_repo *r = scope->repo;
    if (state->stopped)
        fg_repo_snapshot_stopped(scope);
    if (commit && fg_repo_snapshot_stopped(scope))
        commit = false;
    if (state->error->code)
        commit = false;
    if (commit && !sql(r, "COMMIT", state->error))
        commit = false;
    /* Cleanup must remain possible after cancellation or a VM interruption. */
    sqlite3_progress_handler(r->db, 0, NULL, NULL);
    if (!commit && !sqlite3_get_autocommit(r->db)) {
        char *message = NULL;
        int rc = sqlite3_exec(r->db, "ROLLBACK", NULL, NULL, &message);
        if (rc != SQLITE_OK && !state->error->code)
            fg_error(state->error, FORGE_ERR_IO, "Cannot roll back repository snapshot: %s",
                     message ? message : sqlite3_errmsg(r->db));
        sqlite3_free(message);
    }
    forge_status result = state->error->code;
    if (e)
        *e = *state->error;
    leave_scope(r, state->previous);
    r->error = state->previous_error;
    r->snapshot_active = false;
    free(state);
    memset(scope, 0, sizeof(*scope));
    return result;
}
forge_status fg_repo_index_until(forge_repo *r, const char *const *paths, size_t count, bool full,
                                 uint64_t deadline, forge_cancel_fn cancelled, void *userdata,
                                 forge_error *e) {
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    if (r->snapshot_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot index during a repository snapshot");
    index_scope previous = enter_scope(r, deadline, cancelled, userdata);
    forge_status status =
        full ? forge_repo_index(r, e) : forge_repo_index_paths(r, paths, count, e);
    leave_scope(r, previous);
    return status;
}
forge_status fg_repo_note_change(forge_repo *r, forge_error *e) {
    forge_error local = {0};
    if (!e)
        e = &local;
    memset(e, 0, sizeof(*e));
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    if (r->snapshot_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot change generation during a snapshot");
    r->error = e;
    if (index_stopped(r))
        return e->code;
    if (!sql(r, "BEGIN IMMEDIATE", e))
        return index_stopped(r) ? e->code : FORGE_ERR_IO;
    repo_state next = {0};
    bool ok = read_state(r, &next) && !index_stopped(r);
    if (ok && next.generation >= INT64_MAX) {
        fg_error(e, FORGE_ERR_LIMIT, "Repository generation space exhausted");
        ok = false;
    }
    if (ok) {
        sqlite3_stmt *s = prepare(r, "UPDATE meta SET value=? WHERE key='generation' AND value=?");
        if (!s)
            ok = false;
        else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)next.generation + 1);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)next.generation);
            ok = done(r, s) && sqlite3_changes(r->db) == 1;
            sqlite3_finalize(s);
            next.generation++;
        }
    }
    repo_state stored = {0};
    if (ok)
        ok = read_state(r, &stored) && stored.generation == next.generation &&
             stored.scan == next.scan && stored.incomplete == next.incomplete &&
             stored.filesystem == next.filesystem;
    if (ok && index_stopped(r))
        ok = false;
    if (!ok || !sql(r, "COMMIT", e)) {
        if (!e->code)
            fg_error(e, FORGE_ERR_IO, "Cannot persist observed repository change: %s",
                     sqlite3_errmsg(r->db));
        sql(r, "ROLLBACK", NULL);
        return e->code;
    }
    apply_state(r, next);
    count_add(&r->index_stats.observed_change_bumps, 1);
    return FORGE_OK;
}
forge_status fg_repo_note_change_until(forge_repo *r, uint64_t deadline, forge_cancel_fn cancelled,
                                       void *userdata, forge_error *e) {
    if (!r)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
    if (r->snapshot_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Cannot change generation during a snapshot");
    index_scope previous = enter_scope(r, deadline, cancelled, userdata);
    forge_status status = fg_repo_note_change(r, e);
    leave_scope(r, previous);
    return status;
}

static bool json_column(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
                        sqlite3_stmt *s, int column) {
    const char *text = (const char *)sqlite3_column_text(s, column);
    return text ? yyjson_mut_obj_add_strcpy(doc, object, key, text)
                : yyjson_mut_obj_add_null(doc, object, key);
}
char *forge_repo_index_describe(forge_repo *r, const char *relative_path, forge_error *e) {
    forge_error local = {0};
    if (!e)
        e = &local;
    memset(e, 0, sizeof(*e));
    if (!r) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing repository");
        return NULL;
    }
    char path[FG_PATH_MAX];
    if (!fg_relative_path(relative_path, path, e))
        return NULL;
    if (!sqlite3_get_autocommit(r->db)) {
        fg_error(e, FORGE_ERR_CONFLICT, "Index descriptions require a committed transaction");
        return NULL;
    }
    r->error = e;
    /* Keep this statement active while reading symbols so another connection's
     * commit cannot mix file and declaration versions in one description. */
    sqlite3_stmt *file = prepare(
        r, "SELECT f.id,f.language,f.size,f.hash,f.generation,x.ast_hash,x.symbol_hash,"
           "COALESCE(x.node_count,0),COALESCE(x.symbol_count,(SELECT count(*) FROM symbols s "
           "WHERE s.file_id=f.id)),g.parse_error,x.version,(SELECT value FROM meta "
           "WHERE key='generation'),d.sha256,d.version FROM files f "
           "LEFT JOIN file_syntax x ON x.file_id=f.id "
           "LEFT JOIN go_files g ON g.file_id=f.id "
           "LEFT JOIN file_digests d ON d.file_id=f.id WHERE f.path=?");
    if (!file)
        return NULL;
    sqlite3_bind_text(file, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(file);
    if (rc != SQLITE_ROW) {
        fg_error(e, rc == SQLITE_DONE ? FORGE_ERR_NOT_FOUND : FORGE_ERR_IO,
                 "No committed index entry for %s", path);
        sqlite3_finalize(file);
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *symbols = doc ? yyjson_mut_arr(doc) : NULL;
    if (!root || !symbols) {
        if (doc)
            yyjson_mut_doc_free(doc);
        sqlite3_finalize(file);
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate index description");
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    const char *language = (const char *)sqlite3_column_text(file, 1);
    bool go = language && !strcmp(language, "go"), complete = sqlite3_column_int(file, 10) == 1;
    uint64_t count = (uint64_t)sqlite3_column_int64(file, 8);
    bool ok =
        yyjson_mut_obj_add_uint(doc, root, "schema", 1) &&
        yyjson_mut_obj_add_strcpy(doc, root, "path", path) &&
        yyjson_mut_obj_add_str(doc, root, "hash_algorithm", "fnv1a64") &&
        yyjson_mut_obj_add_str(doc, root, "digest_algorithm", "sha256") &&
        json_column(doc, root, "source_sha256", file, 12) &&
        yyjson_mut_obj_add_bool(doc, root, "digest_metadata_complete",
                                sqlite3_column_type(file, 13) == SQLITE_INTEGER &&
                                    sqlite3_column_int64(file, 13) == 1) &&
        json_column(doc, root, "language", file, 1) &&
        yyjson_mut_obj_add_uint(doc, root, "source_bytes",
                                (uint64_t)sqlite3_column_int64(file, 2)) &&
        json_column(doc, root, "source_hash", file, 3) &&
        yyjson_mut_obj_add_uint(doc, root, "generation", (uint64_t)sqlite3_column_int64(file, 4)) &&
        yyjson_mut_obj_add_uint(doc, root, "repo_generation",
                                (uint64_t)sqlite3_column_int64(file, 11)) &&
        yyjson_mut_obj_add_bool(doc, root, "metadata_complete", !go || complete) &&
        yyjson_mut_obj_add_bool(doc, root, "parse_error", go && sqlite3_column_int(file, 9) != 0) &&
        yyjson_mut_obj_add_uint(doc, root, "ast_nodes", (uint64_t)sqlite3_column_int64(file, 7)) &&
        yyjson_mut_obj_add_uint(doc, root, "symbol_count", count) &&
        yyjson_mut_obj_add_uint(doc, root, "symbol_limit", FORGE_INDEX_SYMBOL_LIMIT) &&
        yyjson_mut_obj_add_bool(doc, root, "symbols_truncated", count > FORGE_INDEX_SYMBOL_LIMIT) &&
        yyjson_mut_obj_add_val(doc, root, "symbols", symbols);
    if (complete)
        ok = ok && json_column(doc, root, "ast_hash", file, 5) &&
             json_column(doc, root, "symbol_hash", file, 6);
    else
        ok = ok && yyjson_mut_obj_add_null(doc, root, "ast_hash") &&
             yyjson_mut_obj_add_null(doc, root, "symbol_hash");
    sqlite3_stmt *s =
        prepare(r, "SELECT s.name,s.kind,s.start_byte,s.end_byte,s.line,h.source_hash,d.sha256 "
                   "FROM symbols s LEFT JOIN symbol_hashes h ON h.symbol_id=s.id "
                   "LEFT JOIN symbol_digests d ON d.symbol_id=s.id "
                   "WHERE s.file_id=? ORDER BY s.start_byte,s.end_byte,s.name,s.kind LIMIT ?");
    if (!s)
        ok = false;
    else {
        sqlite3_bind_int64(s, 1, sqlite3_column_int64(file, 0));
        sqlite3_bind_int64(s, 2, (sqlite3_int64)FORGE_INDEX_SYMBOL_LIMIT);
        while (ok && (rc = sqlite3_step(s)) == SQLITE_ROW) {
            yyjson_mut_val *symbol = yyjson_mut_obj(doc);
            ok = symbol && yyjson_mut_arr_append(symbols, symbol) &&
                 json_column(doc, symbol, "name", s, 0) && json_column(doc, symbol, "kind", s, 1) &&
                 yyjson_mut_obj_add_uint(doc, symbol, "start_byte",
                                         (uint64_t)sqlite3_column_int64(s, 2)) &&
                 yyjson_mut_obj_add_uint(doc, symbol, "end_byte",
                                         (uint64_t)sqlite3_column_int64(s, 3)) &&
                 yyjson_mut_obj_add_uint(doc, symbol, "line",
                                         (uint64_t)sqlite3_column_int64(s, 4)) &&
                 json_column(doc, symbol, "source_hash", s, 5) &&
                 json_column(doc, symbol, "source_sha256", s, 6);
        }
        if (ok && rc != SQLITE_DONE) {
            fg_error(e, FORGE_ERR_IO, "Cannot read indexed symbols");
            ok = false;
        }
        sqlite3_finalize(s);
    }
    sqlite3_finalize(file);
    size_t size = 0;
    char *json = ok ? yyjson_mut_write(doc, YYJSON_WRITE_ESCAPE_UNICODE, &size) : NULL;
    yyjson_mut_doc_free(doc);
    if (json && size > FG_MAX_JSON) {
        free(json);
        json = NULL;
        fg_error(e, FORGE_ERR_LIMIT, "Index description exceeds 16 MiB");
    }
    if (!json && !e->code)
        fg_error(e, FORGE_ERR_MEMORY, "Cannot serialize index description");
    return json;
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
        while (r->trees)
            cache_remove(r, &r->trees);
        while (r->pending_trees)
            cache_remove(r, &r->pending_trees);
        if (r->parser)
            ts_parser_delete(r->parser);
        if (r->db)
            sqlite3_close(r->db);
        free(r);
    }
}
