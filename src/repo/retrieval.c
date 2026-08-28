#include "repo_internal.h"
#include "graph.h"
#include "core/digest.h"
#include "forge/retrieval.h"

enum { EXACT, GRAPH, LITERAL, FTS, STAGES };
static const char *const stage_names[] = {"exact_symbol", "package_graph", "literal", "fts5"};
typedef struct {
    bool attempted;
    size_t candidates, accepted, duplicates, omitted;
    const char *reason;
} stage_trace;
typedef struct {
    forge_retrieval_options options;
    forge_repo *repo;
    fg_repo_snapshot snapshot;
    forge_error *error;
    yyjson_mut_doc *doc;
    yyjson_mut_val *results;
    const char *paths[FORGE_RETRIEVAL_MAX_RESULTS];
    size_t starts[FORGE_RETRIEVAL_MAX_RESULTS], ends[FORGE_RETRIEVAL_MAX_RESULTS];
    unsigned stages[FORGE_RETRIEVAL_MAX_RESULTS];
    size_t count, candidates, source_bytes, graph_reasons;
    stage_trace trace[STAGES];
    bool truncated, halt, graph_incomplete, graph_loaded;
} retrieval;

forge_retrieval_options forge_default_retrieval_options(void) {
    forge_retrieval_options o = {0};
    o.graph_depth = 1;
    o.include_dependents = true;
    o.max_results = 16;
    o.max_output_bytes = 64u * 1024u;
    o.max_snippet_bytes = 2048;
    o.max_candidates = 256;
    o.max_source_bytes = 16u * 1024u * 1024u;
    o.timeout_ms = 5000;
    o.max_vm_steps = 50000000;
    return o;
}
static bool fail(retrieval *b, forge_status code, const char *message) {
    if (!b->error->code)
        fg_error(b->error, code, "%s", message);
    return false;
}
static bool stopped(retrieval *b) {
    return fg_repo_snapshot_stopped(&b->snapshot);
}
static bool text_valid(const char *text, size_t maximum, bool empty) {
    if (!text)
        return false;
    size_t n = 0;
    while (n <= maximum && text[n])
        n++;
    return n <= maximum && (empty || n) && fg_utf8_valid(text, n);
}
static const char *column(retrieval *b, sqlite3_stmt *s, int col, size_t maximum) {
    if (sqlite3_column_type(s, col) != SQLITE_TEXT || sqlite3_column_bytes(s, col) < 0 ||
        (size_t)sqlite3_column_bytes(s, col) > maximum) {
        fail(b, FORGE_ERR_PARSE, "Invalid indexed retrieval text type or length");
        return NULL;
    }
    size_t n = (size_t)sqlite3_column_bytes(s, col);
    const char *value = (const char *)sqlite3_column_text(s, col);
    if (!value || memchr(value, 0, n) || !fg_utf8_valid(value, n)) {
        fail(b, FORGE_ERR_PARSE, "Indexed retrieval text is not valid UTF-8");
        return NULL;
    }
    return value;
}
static bool integer(retrieval *b, sqlite3_stmt *s, int col, size_t maximum, size_t *out) {
    if (sqlite3_column_type(s, col) != SQLITE_INTEGER)
        return fail(b, FORGE_ERR_PARSE, "Invalid indexed retrieval integer type");
    sqlite3_int64 value = sqlite3_column_int64(s, col);
    if (value < 0 || (uint64_t)value > maximum)
        return fail(b, FORGE_ERR_PARSE, "Invalid indexed retrieval integer");
    *out = (size_t)value;
    return true;
}
static sqlite3_stmt *prepare(retrieval *b, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (stopped(b))
        return NULL;
    if (sqlite3_prepare_v2(b->repo->db, sql, -1, &s, NULL) != SQLITE_OK) {
        fail(b, FORGE_ERR_IO, "Cannot prepare indexed retrieval query");
        return NULL;
    }
    return s;
}
static void limit(retrieval *b, unsigned stage, const char *reason) {
    b->halt = b->truncated = true;
    b->trace[stage].reason = reason;
}
static size_t line_at(const char *text, size_t offset) {
    size_t line = 1;
    for (size_t i = 0; i < offset; i++)
        line += text[i] == '\n';
    return line;
}
static bool boundary(const char *text, size_t n, size_t pos) {
    return pos <= n && (pos == n || ((unsigned char)text[pos] & 0xc0u) != 0x80u);
}
static void directory(const char *path, char out[FG_PATH_MAX]) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (!n)
        strcpy(out, ".");
    else {
        memcpy(out, path, n);
        out[n] = 0;
    }
}
static bool duplicate(retrieval *b, unsigned stage, const char *path, size_t start, size_t end) {
    for (size_t i = 0; i < b->count; i++)
        if (!strcmp(b->paths[i], path) &&
            (stage != EXACT || (b->starts[i] == start && b->ends[i] == end)))
            return true;
    return false;
}

/* All stages return the same columns. The stored chunk is the sole source of
 * snippets; its complete SHA-256 is checked before any candidate is published. */
#define FILE_COLUMNS "f.path,chunks.content,d.sha256,d.version,f.size,chunks.path,"
#define FILE_JOINS                                                                                 \
    " FROM files f JOIN chunks ON chunks.rowid=f.id LEFT JOIN file_digests d ON d.file_id=f.id "

static bool add_row(retrieval *b, sqlite3_stmt *s, unsigned stage, size_t distance,
                    const char *query) {
    if (b->count == b->options.max_results) {
        limit(b, stage, "result_limit");
        return true;
    }
    if (b->candidates == b->options.max_candidates) {
        limit(b, stage, "candidate_limit");
        return true;
    }
    b->candidates++;
    b->trace[stage].candidates++;
    const char *path = column(b, s, 0, FG_PATH_MAX - 1);
    const char *source = column(b, s, 1, FORGE_INDEX_MAX_FILE_BYTES);
    if (sqlite3_column_type(s, 2) == SQLITE_NULL)
        return fail(b, FORGE_ERR_CONFLICT, "Retrieval requires SHA-256 metadata; reindex first");
    const char *sha = column(b, s, 2, 64);
    const char *chunk_path = column(b, s, 5, FG_PATH_MAX - 1);
    size_t version, n;
    if (!path || !source || !sha || !chunk_path || !integer(b, s, 3, 1, &version) ||
        !integer(b, s, 4, FORGE_INDEX_MAX_FILE_BYTES, &n))
        return false;
    char canonical[FG_PATH_MAX], computed[65];
    if (version != 1 || !fg_relative_path(path, canonical, NULL) || strcmp(path, canonical) ||
        strcmp(path, chunk_path) || n != (size_t)sqlite3_column_bytes(s, 1) ||
        !fg_sha256_valid_hex(sha))
        return fail(b, FORGE_ERR_PARSE, "Inconsistent indexed retrieval source metadata");
    if (n > b->options.max_source_bytes - b->source_bytes) {
        limit(b, stage, "source_budget");
        return true;
    }
    b->source_bytes += n;
    if (!fg_sha256_hex(source, n, computed) || strcmp(sha, computed))
        return fail(b, FORGE_ERR_PARSE, "Indexed retrieval source digest mismatch");
    if (stopped(b))
        return false;
    size_t start = 0, end = n, line = 1, symbol_end = 0;
    const char *name = NULL, *kind = "file", *snippet = source;
    bool clipped = false, positions = stage != FTS;
    if (stage == EXACT) {
        name = column(b, s, 6, FORGE_RETRIEVAL_MAX_QUERY_BYTES);
        kind = column(b, s, 7, 64);
        if (!name || !kind || !integer(b, s, 8, n + 1, &line) || !integer(b, s, 9, n, &start) ||
            !integer(b, s, 10, n, &end))
            return false;
        if (!*name || !*kind || start >= end || !boundary(source, n, start) ||
            !boundary(source, n, end) || line != line_at(source, start))
            return fail(b, FORGE_ERR_PARSE, "Invalid indexed retrieval symbol span");
        symbol_end = end;
    } else if (stage == LITERAL) {
        const char *match = strstr(source, query);
        if (!match)
            return fail(b, FORGE_ERR_PARSE, "Literal query returned a nonmatching indexed source");
        size_t match_at = (size_t)(match - source);
        start = match_at;
        size_t before = FG_MIN((size_t)256, b->options.max_snippet_bytes / 2);
        while (start && source[start - 1] != '\n' && match_at - start < before)
            start--;
        while (!boundary(source, n, start))
            start++;
        line = line_at(source, start);
    } else if (stage == FTS) {
        snippet = column(b, s, 11, FORGE_INDEX_MAX_FILE_BYTES + 64);
        if (!snippet)
            return false;
        start = 0;
        end = (size_t)sqlite3_column_bytes(s, 11);
        line = 0;
        clipped = true; /* FTS token-window excerpts have no absolute byte locator. */
    }
    if (duplicate(b, stage, path, start, stage == EXACT ? symbol_end : end)) {
        b->trace[stage].duplicates++;
        return true;
    }
    size_t full_end = end;
    if (end - start > b->options.max_snippet_bytes)
        end = start + b->options.max_snippet_bytes;
    while (end > start && !boundary(snippet, full_end, end))
        end--;
    clipped |= start != 0 && stage != EXACT;
    clipped |= end != full_end;
    b->truncated |= clipped;
    yyjson_mut_val *row = yyjson_mut_obj(b->doc);
    bool ok = row && yyjson_mut_obj_add_str(b->doc, row, "stage", stage_names[stage]) &&
              yyjson_mut_obj_add_strcpy(b->doc, row, "path", path) &&
              yyjson_mut_obj_add_strcpy(b->doc, row, "kind", kind) &&
              (name ? yyjson_mut_obj_add_strcpy(b->doc, row, "name", name)
                    : yyjson_mut_obj_add_null(b->doc, row, "name")) &&
              yyjson_mut_obj_add_strcpy(b->doc, row, "source_sha256", sha) &&
              yyjson_mut_obj_add_strncpy(b->doc, row, "snippet", snippet + start, end - start) &&
              yyjson_mut_obj_add_bool(b->doc, row, "excerpt_truncated", clipped);
    if (positions)
        ok = ok && yyjson_mut_obj_add_uint(b->doc, row, "line", line) &&
             yyjson_mut_obj_add_uint(b->doc, row, "start_byte", start) &&
             yyjson_mut_obj_add_uint(b->doc, row, "end_byte", end);
    else
        ok = ok && yyjson_mut_obj_add_null(b->doc, row, "line") &&
             yyjson_mut_obj_add_null(b->doc, row, "start_byte") &&
             yyjson_mut_obj_add_null(b->doc, row, "end_byte");
    if (stage == EXACT)
        ok = ok && yyjson_mut_obj_add_uint(b->doc, row, "symbol_end_byte", symbol_end);
    if (stage == GRAPH)
        ok = ok && yyjson_mut_obj_add_uint(b->doc, row, "graph_distance", distance);
    if (!ok || !yyjson_mut_arr_add_val(b->results, row))
        return fail(b, FORGE_ERR_MEMORY, "Cannot allocate retrieval result");
    b->paths[b->count] = yyjson_mut_get_str(yyjson_mut_obj_get(row, "path"));
    b->starts[b->count] = start;
    b->ends[b->count] = stage == EXACT ? symbol_end : end;
    b->stages[b->count++] = stage;
    b->trace[stage].accepted++;
    return true;
}

static bool rows(retrieval *b, unsigned stage, const char *sql, const char *parameter,
                 const char *query, size_t distance) {
    b->trace[stage].attempted = true;
    sqlite3_stmt *s = prepare(b, sql);
    if (!s)
        return false;
    bool ok =
        sqlite3_bind_text(s, 1, parameter, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(s, 2, (sqlite3_int64)(b->options.max_candidates - b->candidates + 1)) ==
            SQLITE_OK;
    int rc = SQLITE_DONE;
    while (ok && !b->halt && !stopped(b) && (rc = sqlite3_step(s)) == SQLITE_ROW)
        ok = add_row(b, s, stage, distance, query);
    if (stopped(b))
        ok = false;
    if (ok && !b->halt && rc != SQLITE_DONE)
        ok = fail(b, FORGE_ERR_IO, "Indexed retrieval query failed");
    if (sqlite3_finalize(s) != SQLITE_OK && ok)
        ok = fail(b, FORGE_ERR_IO, "Cannot finalize indexed retrieval query");
    if (!ok && !b->error->code)
        fail(b, FORGE_ERR_IO, "Cannot bind indexed retrieval query");
    return ok;
}

static bool graph_stage(retrieval *b, const char *seed) {
    if (!b->options.graph_depth) {
        b->trace[GRAPH].reason = "disabled";
        return true;
    }
    if (!b->count && !seed) {
        b->trace[GRAPH].reason = "no_seed";
        return true;
    }
    b->trace[GRAPH].attempted = true;
    fg_go_graph *graph = fg_go_graph_load(&b->snapshot, NULL, 0, b->error);
    if (!graph)
        return false;
    b->graph_loaded = true;
    size_t count, edge_count;
    const fg_go_package *packages = fg_go_graph_packages(graph, &count);
    const fg_go_edge *edges = fg_go_graph_edges(graph, &edge_count);
    (void)edge_count;
    fg_go_graph_reasons(graph, &b->graph_reasons);
    b->graph_incomplete = b->graph_reasons != 0 || !fg_go_graph_applicable(graph);
    size_t *distances = count ? malloc(count * 2 * sizeof(size_t)) : NULL;
    bool ok = !count || distances != NULL;
    if (!ok)
        fail(b, FORGE_ERR_MEMORY, "Cannot allocate retrieval graph traversal");
    size_t *queue = distances ? distances + count : NULL;
    size_t head = 0, tail = 0;
    for (size_t i = 0; ok && i < count; i++)
        distances[i] = SIZE_MAX;
    size_t exact_count = b->count;
    for (size_t i = 0; ok && i < exact_count + (seed ? 1 : 0); i++) {
        char dir[FG_PATH_MAX];
        directory(i < exact_count ? b->paths[i] : seed, dir);
        size_t index = fg_go_graph_find_package(graph, dir);
        if (index != FG_GO_GRAPH_NONE && distances[index] == SIZE_MAX) {
            distances[index] = 0;
            queue[tail++] = index;
        }
    }
    if (!tail)
        b->trace[GRAPH].reason = "no_go_package_seed";
    while (ok && head < tail) {
        if (stopped(b)) {
            ok = false;
            break;
        }
        size_t index = queue[head++];
        if (distances[index] >= b->options.graph_depth)
            continue;
        for (unsigned direction = 0; direction < (b->options.include_dependents ? 2u : 1u);
             direction++) {
            size_t edge = direction ? packages[index].to_head : packages[index].from_head;
            while (edge != FG_GO_GRAPH_NONE) {
                size_t next = direction ? edges[edge].from : edges[edge].to;
                if (distances[next] == SIZE_MAX) {
                    distances[next] = distances[index] + 1;
                    queue[tail++] = next;
                }
                edge = direction ? edges[edge].next_to : edges[edge].next_from;
            }
        }
    }
    /* Graph package order is canonical. Distance first then directory provides
     * deterministic ties without using volatile SQLite row IDs. */
    for (size_t depth = 0; ok && !b->halt && depth <= b->options.graph_depth; depth++)
        for (size_t i = 0; ok && !b->halt && i < count; i++) {
            if (distances[i] != depth || !packages[i].present)
                continue;
            char prefix[FG_PATH_MAX];
            if (!strcmp(packages[i].directory, "."))
                prefix[0] = 0;
            else {
                int n = snprintf(prefix, sizeof(prefix), "%s/", packages[i].directory);
                if (n < 0 || (size_t)n >= sizeof(prefix)) {
                    ok = fail(b, FORGE_ERR_LIMIT, "Retrieval graph directory is too long");
                    break;
                }
            }
            ok = rows(b, GRAPH,
                      "SELECT " FILE_COLUMNS "NULL,NULL,0,0,0,NULL" FILE_JOINS
                      "WHERE f.language='go' AND substr(f.path,1,length(?1))=?1 "
                      "AND instr(substr(f.path,length(?1)+1),'/')=0 ORDER BY f.path LIMIT ?2",
                      prefix, NULL, depth);
        }
    free(distances);
    fg_go_graph_destroy(graph);
    return ok;
}

/* Extract at most 32 literal terms. Non-ASCII UTF-8 bytes remain together;
 * SQLite's tokenizer decides their search semantics. Query operators, quotes,
 * punctuation and column selectors never enter MATCH as syntax. */
static char *fts_terms(const char *query, forge_error *error) {
    fg_buf terms = {0};
    size_t count = 0, start = 0, length = strlen(query);
    for (size_t i = 0; i <= length; i++) {
        unsigned char c = (unsigned char)query[i];
        bool word = c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
        if (word)
            continue;
        if (i > start) {
            if (count == 32) {
                fg_buf_clear(&terms);
                fg_error(error, FORGE_ERR_LIMIT, "Retrieval query exceeds 32 FTS terms");
                return NULL;
            }
            if ((count && !fg_buf_puts(&terms, " OR ")) || !fg_buf_puts(&terms, "\"") ||
                !fg_buf_add(&terms, query + start, i - start) || !fg_buf_puts(&terms, "\"")) {
                fg_buf_clear(&terms);
                fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate quoted retrieval query");
                return NULL;
            }
            count++;
        }
        start = i + 1;
    }
    if (!terms.data && !fg_buf_puts(&terms, "")) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate empty retrieval query");
        return NULL;
    }
    return fg_buf_take(&terms);
}

static char *render(retrieval *b, const char *query, size_t *bytes, size_t *tokens) {
    yyjson_mut_val *root = yyjson_mut_obj(b->doc), *trace = yyjson_mut_arr(b->doc);
    if (!root || !trace) {
        fail(b, FORGE_ERR_MEMORY, "Cannot allocate retrieval metadata");
        return NULL;
    }
    yyjson_mut_doc_set_root(b->doc, root);
    yyjson_mut_val *accepted[STAGES], *omitted[STAGES];
    bool ok =
        yyjson_mut_obj_add_uint(b->doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_uint(b->doc, root, "generation", b->snapshot.generation) &&
        yyjson_mut_obj_add_strcpy(b->doc, root, "query", query) &&
        yyjson_mut_obj_add_bool(b->doc, root, "indexed_snapshot_only", true) &&
        yyjson_mut_obj_add_bool(b->doc, root, "go_index_incomplete",
                                b->snapshot.go_index_incomplete) &&
        yyjson_mut_obj_add_bool(b->doc, root, "filesystem_scan", b->snapshot.filesystem_scan) &&
        yyjson_mut_obj_add_str(b->doc, root, "graph_basis", "syntactic_go_package_imports") &&
        yyjson_mut_obj_add_bool(b->doc, root, "graph_loaded", b->graph_loaded) &&
        (b->graph_loaded
             ? yyjson_mut_obj_add_bool(b->doc, root, "graph_incomplete", b->graph_incomplete)
             : yyjson_mut_obj_add_null(b->doc, root, "graph_incomplete")) &&
        yyjson_mut_obj_add_uint(b->doc, root, "graph_reason_count", b->graph_reasons) &&
        yyjson_mut_obj_add_bool(b->doc, root, "graph_includes_dependents",
                                b->options.include_dependents) &&
        yyjson_mut_obj_add_bool(b->doc, root, "truncated", b->truncated) &&
        yyjson_mut_obj_add_val(b->doc, root, "stages", trace) &&
        yyjson_mut_obj_add_val(b->doc, root, "results", b->results);
    for (unsigned i = 0; ok && i < STAGES; i++) {
        yyjson_mut_val *s = yyjson_mut_obj(b->doc);
        ok = s && yyjson_mut_obj_add_str(b->doc, s, "stage", stage_names[i]) &&
             yyjson_mut_obj_add_bool(b->doc, s, "attempted", b->trace[i].attempted) &&
             yyjson_mut_obj_add_uint(b->doc, s, "candidates", b->trace[i].candidates) &&
             yyjson_mut_obj_add_uint(b->doc, s, "accepted", b->trace[i].accepted) &&
             yyjson_mut_obj_add_uint(b->doc, s, "duplicates", b->trace[i].duplicates) &&
             yyjson_mut_obj_add_uint(b->doc, s, "omitted_output_budget", 0) &&
             yyjson_mut_obj_add_str(b->doc, s, "reason", b->trace[i].reason) &&
             yyjson_mut_arr_add_val(trace, s);
        if (ok) {
            accepted[i] = yyjson_mut_obj_get(s, "accepted");
            omitted[i] = yyjson_mut_obj_get(s, "omitted_output_budget");
        }
    }
    if (!ok) {
        fail(b, FORGE_ERR_MEMORY, "Cannot build retrieval metadata");
        return NULL;
    }
    yyjson_mut_val *truncated = yyjson_mut_obj_get(root, "truncated");
    while (!stopped(b)) {
        char *json = yyjson_mut_write(b->doc, 0, bytes);
        if (!json) {
            fail(b, FORGE_ERR_MEMORY, "Cannot serialize retrieval output");
            return NULL;
        }
        *tokens = 0;
        bool fits = *bytes <= b->options.max_output_bytes;
        if (fits && b->options.count_tokens) {
            *tokens = b->options.count_tokens(json, b->options.count_userdata);
            if (stopped(b)) {
                free(json);
                return NULL;
            }
            if (!*tokens || *tokens == SIZE_MAX) {
                free(json);
                fail(b, FORGE_ERR_MODEL, "Cannot count complete retrieval output");
                return NULL;
            }
            fits = !b->options.max_output_tokens || *tokens <= b->options.max_output_tokens;
        }
        if (fits)
            return json;
        free(json);
        if (!b->count) {
            fail(b, FORGE_ERR_LIMIT, "Retrieval output budget cannot hold its metadata");
            return NULL;
        }
        unsigned stage = b->stages[--b->count];
        b->trace[stage].accepted--;
        b->trace[stage].omitted++;
        b->truncated = true;
        yyjson_mut_set_uint(accepted[stage], b->trace[stage].accepted);
        yyjson_mut_set_uint(omitted[stage], b->trace[stage].omitted);
        yyjson_mut_set_bool(truncated, true);
        yyjson_mut_arr_remove_last(b->results);
    }
    return NULL;
}

char *forge_repo_retrieve(forge_repo *repo, const char *query,
                          const forge_retrieval_options *requested, forge_retrieval_stats *stats,
                          forge_error *error) {
    forge_error local = {0};
    if (!error)
        error = &local;
    memset(error, 0, sizeof(*error));
    if (stats)
        memset(stats, 0, sizeof(*stats));
    forge_retrieval_options o = requested ? *requested : forge_default_retrieval_options();
    uint64_t now = fg_now_ms(), deadline = o.deadline_ms;
    if (o.timeout_ms) {
        uint64_t until = o.timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + o.timeout_ms;
        if (!deadline || until < deadline)
            deadline = until;
    }
    if (!repo || !text_valid(query, FORGE_RETRIEVAL_MAX_QUERY_BYTES, false) ||
        (o.max_output_tokens && !o.count_tokens)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid retrieval query or token counter");
        return NULL;
    }
    if (!o.max_results || o.max_results > FORGE_RETRIEVAL_MAX_RESULTS || !o.max_output_bytes ||
        o.max_output_bytes > FORGE_RETRIEVAL_MAX_OUTPUT_BYTES || !o.max_snippet_bytes ||
        o.max_snippet_bytes > FORGE_RETRIEVAL_MAX_SNIPPET_BYTES || !o.max_candidates ||
        o.max_candidates > FORGE_RETRIEVAL_MAX_CANDIDATES || !o.max_source_bytes ||
        o.max_source_bytes > FORGE_RETRIEVAL_MAX_SOURCE_BYTES || o.graph_depth > 8 ||
        !o.max_vm_steps || o.max_vm_steps > UINT64_C(1000000000) || o.timeout_ms > 600000) {
        fg_error(error, FORGE_ERR_LIMIT, "Retrieval options exceed supported limits");
        return NULL;
    }
    char seed[FG_PATH_MAX];
    if (o.seed_file) {
        if (!text_valid(o.seed_file, FG_PATH_MAX - 1, false)) {
            fg_error(error, FORGE_ERR_ARGUMENT, "Invalid retrieval seed file");
            return NULL;
        }
        if (!fg_relative_path(o.seed_file, seed, error))
            return NULL;
    }
    char *terms = fts_terms(query, error);
    if (!terms)
        return NULL;
    retrieval *b = calloc(1, sizeof(*b));
    if (!b) {
        free(terms);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate retrieval state");
        return NULL;
    }
    b->options = o;
    b->repo = repo;
    b->error = error;
    bool begun = fg_repo_snapshot_begin(repo, &b->snapshot, false, deadline, o.cancelled,
                                        o.userdata, o.max_vm_steps, error) == FORGE_OK;
    b->doc = yyjson_mut_doc_new(NULL);
    b->results = b->doc ? yyjson_mut_arr(b->doc) : NULL;
    bool ok = begun && b->results;
    if (begun && !b->results)
        fail(b, FORGE_ERR_MEMORY, "Cannot allocate retrieval document");
    for (unsigned i = 0; i < STAGES; i++)
        b->trace[i].reason = "exhausted";
    if (ok && o.seed_file) {
        sqlite3_stmt *s = prepare(b, "SELECT 1 FROM files WHERE path=?1");
        ok = s && sqlite3_bind_text(s, 1, seed, -1, SQLITE_TRANSIENT) == SQLITE_OK;
        int rc = ok ? sqlite3_step(s) : SQLITE_ERROR;
        if (ok && rc != SQLITE_ROW)
            ok = fail(b, rc == SQLITE_DONE ? FORGE_ERR_NOT_FOUND : FORGE_ERR_IO,
                      "Retrieval seed file is not available in the indexed snapshot");
        if (s && sqlite3_finalize(s) != SQLITE_OK)
            ok = fail(b, FORGE_ERR_IO, "Cannot finalize retrieval seed lookup");
    }
    if (ok)
        ok = rows(b, EXACT,
                  "SELECT " FILE_COLUMNS
                  "s.name,s.kind,s.line,s.start_byte,s.end_byte,NULL" FILE_JOINS
                  "JOIN symbols s ON s.file_id=f.id WHERE s.name=?1 "
                  "ORDER BY f.path,s.start_byte,s.kind LIMIT ?2",
                  query, query, 0);
    if (ok && !b->halt)
        ok = graph_stage(b, o.seed_file ? seed : NULL);
    if (ok && !b->halt)
        ok = rows(b, LITERAL,
                  "SELECT " FILE_COLUMNS "NULL,NULL,0,0,0,NULL" FILE_JOINS
                  "WHERE instr(chunks.content,?1)>0 ORDER BY f.path LIMIT ?2",
                  query, query, 0);
    if (ok && !b->halt && *terms)
        ok = rows(b, FTS,
                  "SELECT " FILE_COLUMNS
                  "NULL,NULL,0,0,0,snippet(chunks,1,'','',' ... ',24)" FILE_JOINS
                  "WHERE chunks MATCH ?1 ORDER BY bm25(chunks),f.path LIMIT ?2",
                  terms, query, 0);
    if (!*terms)
        b->trace[FTS].reason = "no_terms";
    for (unsigned i = 0; i < STAGES; i++)
        if (b->halt && !b->trace[i].attempted && !strcmp(b->trace[i].reason, "exhausted"))
            b->trace[i].reason = "earlier_budget";
    size_t bytes = 0, tokens = 0;
    char *json = ok && !stopped(b) ? render(b, query, &bytes, &tokens) : NULL;
    uint64_t generation = b->snapshot.generation;
    if (begun && fg_repo_snapshot_end(&b->snapshot, json != NULL, error) != FORGE_OK) {
        free(json);
        json = NULL;
    }
    if (json && stats)
        *stats = (forge_retrieval_stats){generation,
                                         b->count,
                                         bytes,
                                         tokens,
                                         b->candidates,
                                         b->source_bytes,
                                         o.count_tokens != NULL,
                                         b->truncated};
    if (!json && !error->code)
        fail(b, FORGE_ERR_IO, "Indexed retrieval failed");
    yyjson_mut_doc_free(b->doc);
    free(b);
    free(terms);
    return json;
}
