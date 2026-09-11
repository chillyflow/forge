#include "impact.h"
#include "graph.h"
#include <ctype.h>

#define IM_MAX_FILES 4096u
#define IM_MAX_BYTES (64u * 1024u * 1024u)
#define IM_MAX_SYMBOLS 16384u
#define IM_MAX_STEPS UINT64_C(4000000)

typedef struct {
    char *path, *source;
    size_t bytes, first, count;
    bool go, python, changed;
} im_file;
typedef struct {
    char *name, *kind;
    size_t file, start, end, line;
    bool affected;
} im_symbol;
struct fg_impact_snapshot {
    char root[FG_PATH_MAX];
    uint64_t generation;
    im_file *files;
    im_symbol *symbols;
    size_t file_count, symbol_count, bytes;
};
typedef struct {
    uint64_t deadline, steps;
    forge_cancel_fn cancel;
    void *user;
    forge_error *error;
    bool failed, fallback;
    yyjson_mut_doc *doc;
    yyjson_mut_val *root, *reasons, *changes, *symbols, *callers, *tests, *dependents;
} im_work;

static bool im_fail(im_work *w, forge_status code, const char *message) {
    if (!w->failed)
        fg_error(w->error, code, "%s", message);
    w->failed = true;
    return false;
}
static bool im_step(im_work *w) {
    if (w->failed)
        return false;
    if (++w->steps > IM_MAX_STEPS)
        return im_fail(w, FORGE_ERR_LIMIT, "Structural impact work budget exhausted");
    if (w->cancel && w->cancel(w->user))
        return im_fail(w, FORGE_ERR_CANCELLED, "Structural impact cancelled");
    if (w->deadline && fg_now_ms() >= w->deadline)
        return im_fail(w, FORGE_ERR_LIMIT, "Structural impact deadline reached");
    return true;
}
static bool im_suffix(const char *s, const char *suffix) {
    size_t n = strlen(s), z = strlen(suffix);
    return n >= z && !strcmp(s + n - z, suffix);
}
static const char *im_base(const char *s) {
    const char *p = strrchr(s, '/');
    return p ? p + 1 : s;
}
static void im_dir(const char *s, char out[FG_PATH_MAX]) {
    const char *p = strrchr(s, '/');
    if (!p)
        strcpy(out, ".");
    else {
        memcpy(out, s, (size_t)(p - s));
        out[p - s] = 0;
    }
}
static bool im_ident(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 128;
}
static char *im_copy(im_work *w, const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) {
        im_fail(w, FORGE_ERR_MEMORY, "Structural impact allocation failed");
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}
static size_t im_find_file(const fg_impact_snapshot *s, const char *path) {
    size_t a = 0, b = s->file_count;
    while (a < b) {
        size_t m = a + (b - a) / 2;
        int c = strcmp(s->files[m].path, path);
        if (!c)
            return m;
        if (c < 0)
            a = m + 1;
        else
            b = m;
    }
    return SIZE_MAX;
}
static bool im_add_symbol(im_work *w, fg_impact_snapshot *s, size_t file, const char *name,
                          size_t length, const char *kind, size_t start, size_t end, size_t line) {
    if (s->symbol_count >= IM_MAX_SYMBOLS)
        return im_fail(w, FORGE_ERR_LIMIT, "Structural impact declaration limit exceeded");
    im_symbol *v = &s->symbols[s->symbol_count++];
    v->file = file;
    v->start = start;
    v->end = end;
    v->line = line;
    v->name = im_copy(w, name, length);
    v->kind = im_copy(w, kind, strlen(kind));
    s->files[file].count++;
    return v->name && v->kind;
}
/* Deliberately lexical: decorators, strings, nested scopes and dynamic imports
 * are not resolved. Every Python report records this and keeps broad fallback. */
static bool im_python(im_work *w, fg_impact_snapshot *s, size_t file) {
    im_file *f = &s->files[file];
    size_t line = 1;
    for (size_t start = 0; start < f->bytes; line++) {
        if (!im_step(w))
            return false;
        size_t end = start;
        while (end < f->bytes && f->source[end] != '\n')
            end++;
        size_t p = start;
        while (p < end && (f->source[p] == ' ' || f->source[p] == '\t'))
            p++;
        size_t indent = p - start;
        if (end - p >= 6 && !memcmp(f->source + p, "async ", 6))
            p += 6;
        if (end - p > 4 && !memcmp(f->source + p, "def ", 4)) {
            size_t name = p + 4, stop = name;
            while (stop < end && im_ident((unsigned char)f->source[stop]))
                stop++;
            if (stop > name && stop < end && f->source[stop] == '(') {
                size_t body_end = end < f->bytes ? end + 1 : end;
                while (body_end < f->bytes) {
                    size_t q = body_end;
                    while (q < f->bytes && (f->source[q] == ' ' || f->source[q] == '\t'))
                        q++;
                    if (q < f->bytes && f->source[q] != '\n' && f->source[q] != '\r' &&
                        f->source[q] != '#' && q - body_end <= indent)
                        break;
                    while (body_end < f->bytes && f->source[body_end] != '\n')
                        body_end++;
                    if (body_end < f->bytes)
                        body_end++;
                }
                if (!im_add_symbol(w, s, file, f->source + name, stop - name,
                                   "python_definition_candidate", start, body_end, line))
                    return false;
            }
        }
        start = end < f->bytes ? end + 1 : end;
    }
    return true;
}
void fg_impact_snapshot_destroy(fg_impact_snapshot *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->file_count; i++) {
        free(s->files[i].path);
        free(s->files[i].source);
    }
    for (size_t i = 0; i < s->symbol_count; i++) {
        free(s->symbols[i].name);
        free(s->symbols[i].kind);
    }
    free(s->files);
    free(s->symbols);
    free(s);
}
fg_impact_snapshot *fg_impact_snapshot_take(forge_repo *repo, uint64_t deadline,
                                            forge_cancel_fn cancel, void *user,
                                            forge_error *error) {
    forge_error local = {0};
    if (!error)
        error = &local;
    memset(error, 0, sizeof(*error));
    im_work w = {.deadline = deadline, .cancel = cancel, .user = user, .error = error};
    if (!repo) {
        im_fail(&w, FORGE_ERR_ARGUMENT, "Structural impact requires a repository");
        return NULL;
    }
    fg_impact_snapshot *out = calloc(1, sizeof(*out));
    if (!out) {
        im_fail(&w, FORGE_ERR_MEMORY, "Structural impact snapshot allocation failed");
        return NULL;
    }
    strcpy(out->root, repo->root);
    out->files = calloc(IM_MAX_FILES, sizeof(*out->files));
    out->symbols = calloc(IM_MAX_SYMBOLS, sizeof(*out->symbols));
    fg_repo_snapshot scope = {0};
    sqlite3_stmt *files = NULL, *symbols = NULL;
    if (!out->files || !out->symbols) {
        im_fail(&w, FORGE_ERR_MEMORY, "Structural impact snapshot allocation failed");
        goto done;
    }
    if (fg_repo_snapshot_begin(repo, &scope, false, deadline, cancel, user, UINT64_C(100000000),
                               error) != FORGE_OK) {
        w.failed = true;
        goto done;
    }
    out->generation = scope.generation;
    if (sqlite3_prepare_v2(
            repo->db,
            "SELECT f.id,f.path,c.content FROM files f JOIN chunks c ON c.rowid=f.id "
            "ORDER BY f.path",
            -1, &files, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(repo->db,
                           "SELECT name,kind,start_byte,end_byte,line FROM symbols WHERE file_id=? "
                           "ORDER BY start_byte,end_byte,name,kind",
                           -1, &symbols, NULL) != SQLITE_OK) {
        im_fail(&w, FORGE_ERR_IO, "Cannot query indexed structural impact inputs");
        goto done;
    }
    int rc = SQLITE_DONE;
    while (im_step(&w) && (rc = sqlite3_step(files)) == SQLITE_ROW) {
        if (out->file_count >= IM_MAX_FILES) {
            im_fail(&w, FORGE_ERR_LIMIT, "Structural impact file limit exceeded");
            break;
        }
        const char *path = (const char *)sqlite3_column_text(files, 1);
        const char *source = (const char *)sqlite3_column_text(files, 2);
        int path_size = sqlite3_column_bytes(files, 1),
            source_size = sqlite3_column_bytes(files, 2);
        if (!path || !source || path_size <= 0 || path_size >= FG_PATH_MAX ||
            (size_t)path_size != strlen(path) || source_size < 0 ||
            (size_t)source_size > IM_MAX_BYTES - out->bytes) {
            im_fail(&w, FORGE_ERR_LIMIT, "Invalid or oversized structural impact input");
            break;
        }
        size_t at = out->file_count++;
        im_file *f = &out->files[at];
        f->path = im_copy(&w, path, (size_t)path_size);
        f->source = im_copy(&w, source, (size_t)source_size);
        f->bytes = (size_t)source_size;
        f->go = im_suffix(path, ".go");
        f->python = im_suffix(path, ".py");
        f->first = out->symbol_count;
        out->bytes += f->bytes;
        if (w.failed)
            break;
        if (f->python) {
            if (!im_python(&w, out, at))
                break;
            continue;
        }
        sqlite3_reset(symbols);
        sqlite3_bind_int64(symbols, 1, sqlite3_column_int64(files, 0));
        int sr = SQLITE_DONE;
        while (im_step(&w) && (sr = sqlite3_step(symbols)) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(symbols, 0);
            const char *kind = (const char *)sqlite3_column_text(symbols, 1);
            sqlite3_int64 a = sqlite3_column_int64(symbols, 2),
                          b = sqlite3_column_int64(symbols, 3);
            sqlite3_int64 line = sqlite3_column_int64(symbols, 4);
            if (!name || !kind || !*name || strlen(name) > 1024 || strlen(kind) > 128 || a < 0 ||
                b < a || (uint64_t)b > f->bytes || line < 1) {
                im_fail(&w, FORGE_ERR_PARSE, "Invalid indexed declaration range");
                break;
            }
            if (!im_add_symbol(&w, out, at, name, strlen(name), kind, (size_t)a, (size_t)b,
                               (size_t)line))
                break;
        }
        if (!w.failed && sr != SQLITE_DONE)
            im_fail(&w, FORGE_ERR_IO, "Cannot read indexed declarations");
    }
    if (!w.failed && rc != SQLITE_DONE)
        im_fail(&w, FORGE_ERR_IO, "Cannot read indexed structural impact inputs");
done:
    sqlite3_finalize(symbols);
    sqlite3_finalize(files);
    if (scope.internal && fg_repo_snapshot_end(&scope, !w.failed, error) != FORGE_OK)
        w.failed = true;
    if (w.failed) {
        fg_impact_snapshot_destroy(out);
        return NULL;
    }
    return out;
}

static bool im_json(im_work *w, bool ok) {
    return ok || im_fail(w, FORGE_ERR_MEMORY, "Structural impact JSON allocation failed");
}
static yyjson_mut_val *im_object(im_work *w, yyjson_mut_val *array) {
    yyjson_mut_val *v = yyjson_mut_obj(w->doc);
    im_json(w, v && yyjson_mut_arr_append(array, v));
    return v;
}
static bool im_string(im_work *w, yyjson_mut_val *v, const char *key, const char *text) {
    return im_json(w, v && yyjson_mut_obj_add_strcpy(w->doc, v, key, text));
}
static bool im_reason(im_work *w, const char *code, const char *path, const char *detail) {
    if (w->failed)
        return false;
    w->fallback = true;
    size_t n = yyjson_mut_arr_size(w->reasons);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(w->reasons, i);
        if (!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(v, "code")), code))
            return true;
    }
    yyjson_mut_val *r = im_object(w, w->reasons);
    return im_string(w, r, "code", code) && im_string(w, r, "path", path) &&
           im_string(w, r, "detail", detail);
}
static bool im_file_equal(const im_file *a, const im_file *b) {
    return a && b && a->bytes == b->bytes && !memcmp(a->source, b->source, a->bytes);
}
static size_t im_match_symbol(im_work *w, const fg_impact_snapshot *s, const im_file *f,
                              const im_symbol *needle, bool *ambiguous) {
    size_t found = SIZE_MAX;
    *ambiguous = false;
    for (size_t j = 0; f && j < f->count; j++) {
        if (!im_step(w))
            break;
        size_t at = f->first + j;
        const im_symbol *v = &s->symbols[at];
        if (strcmp(v->name, needle->name) || strcmp(v->kind, needle->kind))
            continue;
        if (found != SIZE_MAX)
            *ambiguous = true;
        found = at;
    }
    return found;
}
static bool im_symbol_change(im_work *w, const fg_impact_snapshot *s, const im_symbol *v,
                             const char *change) {
    const im_file *f = &s->files[v->file];
    yyjson_mut_val *out = im_object(w, w->symbols);
    return im_string(w, out, "path", f->path) && im_string(w, out, "name", v->name) &&
           im_string(w, out, "kind", v->kind) && im_string(w, out, "change", change) &&
           im_string(w, out, "range_evidence",
                     f->go ? "indexed_go_ast" : "python_lexical_candidate") &&
           im_json(w, yyjson_mut_obj_add_uint(w->doc, out, "start_byte", v->start)) &&
           im_json(w, yyjson_mut_obj_add_uint(w->doc, out, "end_byte", v->end)) &&
           im_json(w, yyjson_mut_obj_add_uint(w->doc, out, "line", v->line));
}
static char *im_outside(im_work *w, const fg_impact_snapshot *s, const im_file *f) {
    fg_buf b = {0};
    size_t position = 0;
    for (size_t i = 0; i < f->count; i++) {
        const im_symbol *v = &s->symbols[f->first + i];
        if (v->start > position)
            fg_buf_add(&b, f->source + position, v->start - position);
        if (v->end > position)
            position = v->end;
    }
    if (position < f->bytes)
        fg_buf_add(&b, f->source + position, f->bytes - position);
    if (b.failed) {
        fg_buf_clear(&b);
        im_fail(w, FORGE_ERR_MEMORY, "Cannot compare non-declaration input changes");
        return NULL;
    }
    return b.data ? fg_buf_take(&b) : im_copy(w, "", 0);
}
static bool im_changes(im_work *w, const fg_impact_snapshot *before, fg_impact_snapshot *after) {
    for (size_t i = 0; i < before->file_count; i++) {
        if (!im_step(w))
            return false;
        const im_file *old = &before->files[i];
        if (im_find_file(after, old->path) != SIZE_MAX)
            continue;
        im_json(w, yyjson_mut_arr_add_strcpy(w->doc, w->changes, old->path));
        im_reason(w, "deleted_file", old->path, "Removed files require broad verification.");
        for (size_t j = 0; j < old->count; j++)
            im_symbol_change(w, before, &before->symbols[old->first + j], "removed");
    }
    for (size_t i = 0; i < after->file_count; i++) {
        if (!im_step(w))
            return false;
        im_file *now = &after->files[i];
        size_t old_at = im_find_file(before, now->path);
        const im_file *old = old_at == SIZE_MAX ? NULL : &before->files[old_at];
        if (im_file_equal(old, now))
            continue;
        now->changed = true;
        im_json(w, yyjson_mut_arr_add_strcpy(w->doc, w->changes, now->path));
        size_t changed = 0;
        if (!now->go && !now->python)
            im_reason(w, "unsupported_change", now->path,
                      "Non-Go/Python changes, configuration and data require broad verification.");
        if (now->python)
            im_reason(w, "python_structure_unresolved", now->path,
                      "Python definition and caller candidates are lexical; decorators, scopes and "
                      "imports are unresolved.");
        if (now->go && old) {
            char *old_outside = im_outside(w, before, old);
            char *new_outside = im_outside(w, after, now);
            if (old_outside && new_outside && strcmp(old_outside, new_outside))
                im_reason(w, "outside_declaration_change", now->path,
                          "Imports, package clauses, directives or bytes outside declarations "
                          "changed alongside the patch.");
            free(old_outside);
            free(new_outside);
        }
        for (size_t j = 0; old && j < old->count; j++) {
            const im_symbol *v = &before->symbols[old->first + j];
            bool ambiguous = false;
            size_t match = im_match_symbol(w, after, now, v, &ambiguous);
            if (ambiguous)
                im_reason(w, "ambiguous_symbol", now->path,
                          "Duplicate declaration identities are not type-resolved.");
            if (match == SIZE_MAX) {
                im_symbol_change(w, before, v, "removed");
                im_reason(w, "removed_or_renamed_symbol", now->path,
                          "Removed or renamed declarations can leave broken references; broad "
                          "compilation and tests are required.");
                changed++;
            }
        }
        for (size_t j = 0; j < now->count; j++) {
            im_symbol *v = &after->symbols[now->first + j];
            bool ambiguous = false;
            size_t match = im_match_symbol(w, before, old, v, &ambiguous);
            if (ambiguous)
                im_reason(w, "ambiguous_symbol", now->path,
                          "Duplicate declaration identities are not type-resolved.");
            const im_symbol *previous = match == SIZE_MAX ? NULL : &before->symbols[match];
            bool equal =
                previous && previous->end - previous->start == v->end - v->start &&
                !memcmp(old->source + previous->start, now->source + v->start, v->end - v->start);
            if (equal)
                continue;
            v->affected = true;
            changed++;
            im_symbol_change(w, after, v, previous ? "modified" : "added");
            if (now->go && strcmp(v->kind, "function_declaration"))
                im_reason(w, "non_function_or_method_change", now->path,
                          "Types, globals and methods require broad checking because receiver/type "
                          "relationships are unresolved.");
        }
        if (!changed)
            im_reason(w, "unmapped_change", now->path,
                      "Changed bytes are outside identified declarations; imports, directives and "
                      "other effects require broad verification.");
    }
    if (!yyjson_mut_arr_size(w->changes))
        im_reason(w, "no_indexed_changes", ".",
                  "No indexed source delta was found; use broad verification.");
    if (yyjson_mut_arr_size(w->changes) > 1024)
        im_reason(w, "too_many_changed_paths", ".",
                  "More than 1024 changed files require the ordinary broad plan.");
    return !w->failed;
}
/* Identifier-boundary matches deliberately over-approximate (including comments
 * and strings). They are reported as candidates, never as resolved call edges. */
static bool im_occurs(const char *source, size_t length, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; n <= length && i <= length - n; i++) {
        if ((i == 0 || !im_ident((unsigned char)source[i - 1])) && !memcmp(source + i, name, n) &&
            (i + n == length || !im_ident((unsigned char)source[i + n])))
            return true;
    }
    return false;
}
static bool im_python_test(const char *path) {
    return !strncmp(im_base(path), "test_", 5) || im_suffix(path, "_test.py");
}
static bool im_go_test(const im_file *f, const im_symbol *v) {
    return im_suffix(f->path, "_test.go") && !strcmp(v->kind, "function_declaration") &&
           (!strncmp(v->name, "Test", 4) || !strncmp(v->name, "Example", 7));
}
static bool im_callers(im_work *w, fg_impact_snapshot *after) {
    size_t *queue = malloc((after->symbol_count ? after->symbol_count : 1) * sizeof(*queue));
    if (!queue)
        return im_fail(w, FORGE_ERR_MEMORY, "Cannot allocate structural caller traversal");
    size_t used = 0;
    for (size_t i = 0; i < after->symbol_count; i++)
        if (after->symbols[i].affected)
            queue[used++] = i;
    for (size_t head = 0; head < used && !w->failed; head++) {
        const im_symbol *target = &after->symbols[queue[head]];
        const im_file *target_file = &after->files[target->file];
        char target_directory[FG_PATH_MAX];
        im_dir(target_file->path, target_directory);
        for (size_t i = 0; i < after->symbol_count; i++) {
            if (!im_step(w))
                break;
            im_symbol *caller = &after->symbols[i];
            if (caller->affected)
                continue;
            const im_file *file = &after->files[caller->file];
            if (file->go != target_file->go || file->python != target_file->python)
                continue;
            char directory[FG_PATH_MAX];
            im_dir(file->path, directory);
            if (file->go && strcmp(directory, target_directory))
                continue; /* Cross-package edges come from the existing import graph. */
            if (!im_occurs(file->source + caller->start, caller->end - caller->start, target->name))
                continue;
            caller->affected = true;
            queue[used++] = i;
            yyjson_mut_val *v = im_object(w, w->callers);
            im_string(w, v, "path", file->path);
            im_string(w, v, "name", caller->name);
            im_string(w, v, "referenced_symbol", target->name);
            im_string(w, v, "resolution", "identifier_occurrence_candidate");
        }
    }
    free(queue);
    /* Removed names have no current declaration to seed traversal. Retain
     * their surviving occurrences explicitly as possible broken references. */
    for (size_t i = 0; i < yyjson_mut_arr_size(w->symbols) && !w->failed; i++) {
        yyjson_mut_val *removed = yyjson_mut_arr_get(w->symbols, i);
        if (strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(removed, "change")), "removed"))
            continue;
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(removed, "name"));
        for (size_t j = 0; j < after->symbol_count; j++) {
            if (!im_step(w))
                break;
            const im_symbol *caller = &after->symbols[j];
            const im_file *f = &after->files[caller->file];
            if (!im_occurs(f->source + caller->start, caller->end - caller->start, name))
                continue;
            yyjson_mut_val *v = im_object(w, w->callers);
            im_string(w, v, "path", f->path);
            im_string(w, v, "name", caller->name);
            im_string(w, v, "referenced_symbol", name);
            im_string(w, v, "resolution", "possible_broken_reference_to_removed_name");
        }
    }
    return !w->failed;
}
static bool im_test_add(im_work *w, const im_file *f, const im_symbol *s, const char *module,
                        const char *reason) {
    char directory[FG_PATH_MAX];
    im_dir(f->path, directory);
    yyjson_mut_val *v = im_object(w, w->tests);
    return im_string(w, v, "language", f->go ? "go" : "python") &&
           im_string(w, v, "path", f->path) && im_string(w, v, "name", s ? s->name : "") &&
           im_string(w, v, "package_directory", directory) &&
           im_string(w, v, "module_directory", module) && im_string(w, v, "reason", reason);
}
static bool im_targets(im_work *w, forge_repo *repo, fg_impact_snapshot *after) {
    fg_repo_snapshot scope = {0};
    fg_go_graph *graph = NULL;
    bool *affected = NULL, *dependent = NULL;
    if (fg_repo_snapshot_begin(repo, &scope, false, w->deadline, w->cancel, w->user,
                               UINT64_C(100000000), w->error) != FORGE_OK) {
        w->failed = true;
        return false;
    }
    if (scope.generation != after->generation) {
        im_fail(w, FORGE_ERR_CONFLICT, "Repository changed during structural impact planning");
        goto done;
    }
    graph = fg_go_graph_load(&scope, NULL, 0, w->error);
    if (!graph) {
        w->failed = true;
        goto done;
    }
    size_t np = 0, nm = 0, ne = 0, nr = 0;
    const fg_go_package *packages = fg_go_graph_packages(graph, &np);
    const fg_go_module *modules = fg_go_graph_modules(graph, &nm);
    const fg_go_edge *edges = fg_go_graph_edges(graph, &ne);
    const fg_go_reason *reasons = fg_go_graph_reasons(graph, &nr);
    for (size_t i = 0; i < nr; i++)
        im_reason(w, reasons[i].code, reasons[i].path, reasons[i].detail);
    affected = calloc(np ? np : 1, sizeof(*affected));
    dependent = calloc(np ? np : 1, sizeof(*dependent));
    if (!affected || !dependent) {
        im_fail(w, FORGE_ERR_MEMORY, "Structural impact graph allocation failed");
        goto done;
    }
    for (size_t i = 0; i < after->file_count; i++) {
        const im_file *f = &after->files[i];
        if (!im_step(w))
            goto done;
        if (f->go && (strstr(f->source, "\"reflect\"") || strstr(f->source, "`reflect`") ||
                      strstr(f->source, "\"unsafe\"") || strstr(f->source, "`unsafe`") ||
                      strstr(f->source, "\"plugin\"") || strstr(f->source, "`plugin`") ||
                      strstr(f->source, "//go:embed") || strstr(f->source, "//go:generate") ||
                      im_occurs(f->source, f->bytes, "interface")))
            im_reason(w, "go_dynamic_or_generated_behavior", f->path,
                      "Reflection, interfaces, unsafe/plugin behavior and generated or embedded "
                      "inputs require broad fallback.");
        if (f->python && (strstr(f->source, "getattr(") || strstr(f->source, "eval(") ||
                          strstr(f->source, "exec(") || strstr(f->source, "importlib") ||
                          strstr(f->source, "__import__") || strstr(f->source, "setattr(")))
            im_reason(w, "python_dynamic_behavior", f->path,
                      "Dynamic Python names or imports require full-suite fallback.");
        if (!f->go || !f->changed)
            continue;
        char directory[FG_PATH_MAX];
        im_dir(f->path, directory);
        size_t at = fg_go_graph_find_package(graph, directory);
        if (at == FG_GO_GRAPH_NONE)
            im_reason(w, "unresolved_package", f->path,
                      "Changed package is absent from the indexed graph.");
        else
            affected[at] = true;
    }
    /* Package-level names can collide across files, including mutually
     * exclusive build variants. Never guess which declaration a caller means. */
    for (size_t i = 0; i < after->symbol_count; i++) {
        const im_symbol *a = &after->symbols[i];
        const im_file *af = &after->files[a->file];
        if (!a->affected || !af->go)
            continue;
        char ad[FG_PATH_MAX];
        im_dir(af->path, ad);
        for (size_t j = 0; j < after->symbol_count; j++) {
            if (!im_step(w))
                goto done;
            const im_symbol *b = &after->symbols[j];
            if (i == j || strcmp(a->name, b->name) || strcmp(a->kind, b->kind))
                continue;
            char bd[FG_PATH_MAX];
            im_dir(after->files[b->file].path, bd);
            if (!strcmp(ad, bd))
                im_reason(w, "ambiguous_symbol", af->path,
                          "Multiple declarations share a package/name/kind identity; references "
                          "are unresolved.");
        }
    }
    bool progress = true;
    while (progress && !w->failed) {
        progress = false;
        for (size_t i = 0; i < ne; i++) {
            if (!im_step(w))
                goto done;
            if ((affected[edges[i].to] || dependent[edges[i].to]) && !affected[edges[i].from] &&
                !dependent[edges[i].from]) {
                dependent[edges[i].from] = true;
                progress = true;
            }
        }
    }
    for (size_t i = 0; i < np; i++)
        if (dependent[i])
            im_json(w, yyjson_mut_arr_add_strcpy(w->doc, w->dependents, packages[i].directory));
    for (size_t i = 0; i < after->file_count; i++) {
        const im_file *f = &after->files[i];
        bool python_selected = f->python && f->changed && im_python_test(f->path);
        char directory[FG_PATH_MAX];
        im_dir(f->path, directory);
        size_t at = f->go ? fg_go_graph_find_package(graph, directory) : FG_GO_GRAPH_NONE;
        for (size_t j = 0; j < f->count; j++) {
            const im_symbol *v = &after->symbols[f->first + j];
            if (f->python && v->affected && im_python_test(f->path))
                python_selected = true;
            if (!f->go || at == FG_GO_GRAPH_NONE || !im_go_test(f, v))
                continue;
            if (v->affected || dependent[at])
                im_test_add(w, f, v, modules[packages[at].module].directory,
                            dependent[at] ? "reverse_import_package"
                                          : "changed_symbol_or_caller_candidate");
        }
        if (python_selected)
            im_test_add(w, f, NULL, ".", "python_identifier_candidate_full_file");
    }
    if (!yyjson_mut_arr_size(w->tests))
        im_reason(w, "no_related_tests", ".",
                  "No preliminary test candidates were found; keep the broad schedule.");
done:
    free(affected);
    free(dependent);
    fg_go_graph_destroy(graph);
    if (fg_repo_snapshot_end(&scope, !w->failed, w->error) != FORGE_OK)
        w->failed = true;
    return !w->failed;
}
char *fg_impact_analyze(forge_repo *repo, const fg_impact_snapshot *before, uint64_t deadline,
                        forge_cancel_fn cancel, void *user, forge_error *error) {
    forge_error local = {0};
    if (!error)
        error = &local;
    memset(error, 0, sizeof(*error));
    im_work w = {.deadline = deadline, .cancel = cancel, .user = user, .error = error};
    if (!repo || !before || strcmp(repo->root, before->root)) {
        im_fail(&w, FORGE_ERR_ARGUMENT, "Structural impact baseline must belong to this workspace");
        return NULL;
    }
    fg_impact_snapshot *after = fg_impact_snapshot_take(repo, deadline, cancel, user, error);
    if (!after)
        return NULL;
    w.doc = yyjson_mut_doc_new(NULL);
    w.root = w.doc ? yyjson_mut_obj(w.doc) : NULL;
    if (!w.root) {
        im_fail(&w, FORGE_ERR_MEMORY, "Structural impact report allocation failed");
        goto done;
    }
    yyjson_mut_doc_set_root(w.doc, w.root);
    w.reasons = yyjson_mut_obj_add_arr(w.doc, w.root, "fallback_reasons");
    w.changes = yyjson_mut_obj_add_arr(w.doc, w.root, "changed_paths");
    w.symbols = yyjson_mut_obj_add_arr(w.doc, w.root, "changed_symbols");
    w.callers = yyjson_mut_obj_add_arr(w.doc, w.root, "caller_candidates");
    w.tests = yyjson_mut_obj_add_arr(w.doc, w.root, "targeted_tests");
    w.dependents = yyjson_mut_obj_add_arr(w.doc, w.root, "reverse_dependents");
    if (!im_json(&w, w.reasons && w.changes && w.symbols && w.callers && w.tests && w.dependents))
        goto done;
    im_json(&w, yyjson_mut_obj_add_uint(w.doc, w.root, "schema_version", 1));
    im_json(&w, yyjson_mut_obj_add_uint(w.doc, w.root, "baseline_generation", before->generation));
    im_json(&w, yyjson_mut_obj_add_uint(w.doc, w.root, "generation", after->generation));
    im_json(&w, yyjson_mut_obj_add_bool(w.doc, w.root, "sound", false));
    im_json(&w, yyjson_mut_obj_add_bool(w.doc, w.root, "broad_verification_required", true));
    im_string(&w, w.root, "selection_kind", "preliminary_syntactic_symbol_candidates");
    im_string(
        &w, w.root, "limitations",
        "Go ranges come from the AST; caller candidates are name occurrences, not resolved calls. "
        "Python ranges and references are lexical candidates. Selected tests are preliminary, not "
        "coverage; "
        "the existing broad final verification is always required. Only indexed files enter "
        "the impact map; unindexed resources are covered by the validation input snapshot "
        "and broad final checks, not by symbol selection.");
    if (!im_changes(&w, before, after) || !im_callers(&w, after) || !im_targets(&w, repo, after))
        goto done;
    im_json(&w, yyjson_mut_obj_add_bool(w.doc, w.root, "fallback", w.fallback));
done:
    size_t length = 0;
    char *out = !w.failed ? yyjson_mut_write(w.doc, 0, &length) : NULL;
    if (!w.failed && (!out || length > FG_MAX_JSON)) {
        free(out);
        out = NULL;
        im_fail(&w, length > FG_MAX_JSON ? FORGE_ERR_LIMIT : FORGE_ERR_MEMORY,
                "Structural impact report exceeds its allocation or output limit");
    }
    yyjson_mut_doc_free(w.doc);
    fg_impact_snapshot_destroy(after);
    return out;
}
