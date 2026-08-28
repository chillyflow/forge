#include "internal.h"
#include "forge/memory.h"
#include "edit_journal.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static const fg_tool_def definitions[] = {
    {"apply_patch",
     "Replace one exact unique text span; empty old_text creates a new file. Parent directory must "
     "exist.",
     "path:string old_text:string new_text:string", NULL, FORGE_CAP_WRITE},
    {"expand_output",
     "Read up to 8192 bytes of recorded UTF-8 tool text by id. A byte offset inside a character "
     "advances to the next character.",
     "id:uint offset:uint", NULL, FORGE_CAP_READ},
    {"find_symbol", "Inspect a Go symbol. depth 0=signature, 1=body, 2=neighbors, 3=file.",
     "name:string depth:uint", NULL, FORGE_CAP_READ},
    {"get_references", "Find Go identifier occurrences; these are syntactic, not type-resolved.",
     "name:string", NULL, FORGE_CAP_READ},
    /* Git may execute configured clean/process filters while inspecting the
     * worktree. Disabling external diff/textconv is not process isolation. */
    {"git_diff", "Inspect Git diff; configured filters require process authorization.", "", NULL,
     FORGE_CAP_PROCESS},
    {"git_status", "Inspect Git status; configured filters require process authorization.", "",
     NULL, FORGE_CAP_PROCESS},
    {"list_directory", "List indexed repository paths.", "", NULL, FORGE_CAP_READ},
    {"read_file", "Read inclusive lines: start>=1, end>=start, at most 2001 lines.",
     "path:string start:line end:line", NULL, FORGE_CAP_READ},
    {"run_command", "Run an argv array without a shell. Requires explicit process authorization.",
     "argv:strings", NULL, FORGE_CAP_PROCESS},
    {"search_text", "Literal text search across indexed source files.", "query:string", NULL,
     FORGE_CAP_READ}};
const fg_tool_def *fg_tools(size_t *n) {
    if (n)
        *n = sizeof(definitions) / sizeof(*definitions);
    return definitions;
}
char *fg_tool_schema(void) {
    fg_buf b = {0};
    fg_buf_puts(
        &b,
        "Return exactly ONE JSON object: {\"tool\":\"NAME\",\"args\":{...}}, "
        "{\"memory\":{\"facts\":[],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],"
        "\"remaining\":[]}}, or {\"final\":\"answer\"}. Memory fields are arrays of strings, "
        "at most 32 strings each, 512 bytes per string, 8192 bytes total; preserve useful facts "
        "when replacing memory. Model memory cannot mark host verification passed. "
        "A legacy {\"memory\":\"notes\"} is also accepted. Use fields in listed order. No "
        "markdown.\n");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        fg_buf_printf(&b, "%s(%s): %s\n", definitions[i].name, definitions[i].fields,
                      definitions[i].description);
    return fg_buf_take(&b);
}
static void literal(fg_buf *b, const char *s) {
    char *q = fg_json_string(s);
    if (!q)
        b->failed = true;
    else {
        fg_buf_puts(b, q);
        free(q);
    }
}
char *fg_tool_grammar(void) {
    fg_buf b = {0};
    fg_buf_puts(&b, "root ::= ws (final | memory");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        fg_buf_printf(&b, " | call%zu", i);
    fg_buf_puts(&b, ") ws\n");
    fg_buf_puts(&b, "final ::= ");
    literal(&b, "{\"final\":");
    fg_buf_puts(&b, " ws string ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\n");
    fg_buf_puts(&b, "memory ::= ");
    literal(&b, "{\"memory\":");
    fg_buf_puts(&b, " ws (string | workingstate) ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\n");
    fg_buf_puts(&b, "workingstate ::= ");
    literal(&b, "{");
    const char *memory_fields[] = {"facts", "hypotheses", "decisions", "relevant_files",
                                   "remaining"};
    for (size_t i = 0; i < sizeof(memory_fields) / sizeof(*memory_fields); i++) {
        fg_buf field = {0};
        fg_buf_printf(&field, "%s\"%s\":", i ? "," : "", memory_fields[i]);
        fg_buf_puts(&b, " ws ");
        literal(&b, field.data);
        fg_buf_clear(&field);
        fg_buf_puts(&b, " ws memoryitems");
    }
    fg_buf_puts(&b, " ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\nmemoryitems ::= \"[\" ws (string (ws \",\" ws string){0,31})? ws \"]\"\n");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        fg_buf_printf(&b, "call%zu ::= ", i);
        fg_buf prefix = {0};
        fg_buf_printf(&prefix, "{\"tool\":\"%s\",\"args\":{", definitions[i].name);
        literal(&b, prefix.data);
        fg_buf_clear(&prefix);
        const char *p = definitions[i].fields;
        bool first = true;
        while (*p) {
            char key[32], type[32];
            size_t k = 0, t = 0;
            while (*p && *p != ':')
                key[k++] = *p++;
            key[k] = 0;
            if (*p)
                p++;
            while (*p && *p != ' ')
                type[t++] = *p++;
            type[t] = 0;
            if (*p)
                p++;
            fg_buf_puts(&b, " ws ");
            fg_buf keylit = {0};
            fg_buf_printf(&keylit, "%s\"%s\":", first ? "" : ",", key);
            literal(&b, keylit.data);
            fg_buf_clear(&keylit);
            fg_buf_printf(&b, " ws %s",
                          !strcmp(type, "line") ? "line"
                          : !strcmp(type, "uint")
                              ? "uint"
                              : (!strcmp(type, "strings") ? "strings" : "string"));
            first = false;
        }
        fg_buf_puts(&b, " ws ");
        literal(&b, "}}");
        fg_buf_puts(&b, "\n");
    }
    fg_buf_puts(&b,
                "ws ::= [ \\t\\n\\r]*\nline ::= [1-9] [0-9]{0,8}\nuint ::= \"0\" | line\nstrings "
                "::= \"[\" ws string "
                "(ws \",\" ws string){0,63} ws \"]\"\nstring ::= \"\\\"\" char* \"\\\"\"\nchar ::= "
                "[^\"\\\\\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]{4})\n");
    return fg_buf_take(&b);
}
bool fg_tool_validate(const char *name, yyjson_val *args, forge_error *e) {
    const fg_tool_def *def = NULL;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!strcmp(name, definitions[i].name))
            def = &definitions[i];
    if (!def || !yyjson_is_obj(args)) {
        fg_error(e, FORGE_ERR_PARSE, "Unknown tool or non-object arguments");
        return false;
    }
    const char *p = def->fields;
    size_t expected = 0;
    while (*p) {
        char key[32], type[32];
        size_t k = 0, t = 0;
        while (*p && *p != ':')
            key[k++] = *p++;
        key[k] = 0;
        if (*p)
            p++;
        while (*p && *p != ' ')
            type[t++] = *p++;
        type[t] = 0;
        if (*p)
            p++;
        expected++;
        yyjson_val *v = yyjson_obj_get(args, key);
        bool ok = false;
        if (!strcmp(type, "string"))
            ok = yyjson_is_str(v) && yyjson_get_len(v) == strlen(yyjson_get_str(v)) &&
                 yyjson_get_len(v) <= 2u * 1024u * 1024u;
        else if (!strcmp(type, "uint"))
            ok = yyjson_is_uint(v) && yyjson_get_uint(v) <= 999999999;
        else if (!strcmp(type, "line"))
            ok = yyjson_is_uint(v) && yyjson_get_uint(v) >= 1 && yyjson_get_uint(v) <= 999999999;
        else if (yyjson_is_arr(v) && yyjson_arr_size(v) > 0 && yyjson_arr_size(v) <= 64) {
            ok = true;
            size_t i, max;
            yyjson_val *item;
            yyjson_arr_foreach(v, i, max, item) {
                if (!yyjson_is_str(item) || yyjson_get_len(item) != strlen(yyjson_get_str(item)) ||
                    yyjson_get_len(item) > 8192)
                    ok = false;
            }
        }
        if (!ok) {
            fg_error(e, FORGE_ERR_PARSE, "Invalid or missing %s for %s", key, name);
            return false;
        }
    }
    if (yyjson_obj_size(args) != expected) {
        fg_error(e, FORGE_ERR_PARSE, "Unexpected or duplicate tool argument");
        return false;
    }
    return true;
}
uint64_t fg_tool_signature(const char *name, yyjson_val *args, uint64_t generation,
                           uint64_t diagnostic_hash) {
    fg_buf canonical = {0};
    fg_buf_printf(&canonical, "%s:%llu:%llu", name, (unsigned long long)generation,
                  (unsigned long long)diagnostic_hash);
    /* Registry field order makes JSON whitespace/key order irrelevant to loops. */
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        if (strcmp(name, definitions[i].name))
            continue;
        const char *field = definitions[i].fields;
        while (*field) {
            char key[32];
            size_t n = 0;
            while (*field && *field != ':')
                key[n++] = *field++;
            key[n] = 0;
            while (*field && *field != ' ')
                field++;
            if (*field)
                field++;
            char *value = yyjson_val_write(yyjson_obj_get(args, key), 0, NULL);
            if (!value)
                canonical.failed = true;
            fg_buf_printf(&canonical, "|%s=%s", key, value ? value : "null");
            free(value);
        }
        break;
    }
    uint64_t hash = canonical.failed ? 0 : fg_hash(canonical.data, canonical.len);
    fg_buf_clear(&canonical);
    return hash;
}
static char *read_lines(fg_tool_context *c, yyjson_val *args, forge_error *e) {
    char full[FG_PATH_MAX];
    const char *path = fg_json_str(args, "path");
    size_t start, end;
    fg_json_uint(args, "start", &start, 1);
    fg_json_uint(args, "end", &end, 200);
    if (!start || end < start || end - start > 2000) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid line range (maximum 2001 lines)");
        return NULL;
    }
    if (!fg_safe_path(c->root, path, false, full, e))
        return NULL;
    forge_file_view *view =
        forge_file_view_open(full, c->config.limits.max_file_bytes, FORGE_FILE_VIEW_READ, e);
    if (!view)
        return NULL;
    forge_slice file = forge_file_view_slice(view);
    if ((file.len && memchr(file.ptr, 0, file.len)) || !fg_utf8_valid(file.ptr, file.len)) {
        forge_file_view_close(view);
        fg_error(e, FORGE_ERR_PARSE, "Binary or invalid UTF-8 files are not supported");
        return NULL;
    }
    size_t line = 1, offset = 0;
    fg_buf b = {0};
    while (offset < file.len && line <= end) {
        const char *p = file.ptr + offset;
        const char *z = memchr(p, '\n', file.len - offset);
        size_t n = z ? (size_t)(z - p) : file.len - offset;
        if (line >= start) {
            fg_buf_printf(&b, "%zu: ", line);
            fg_buf_add(&b, p, n);
            fg_buf_puts(&b, "\n");
        }
        if (!z)
            break;
        offset += n + 1;
        line++;
    }
    forge_file_view_close(view);
    return fg_buf_take(&b);
}
static char *patch(fg_tool_context *c, yyjson_val *args, bool *changed, forge_error *e) {
    const char *path = fg_json_str(args, "path"), *old = fg_json_str(args, "old_text"),
               *replacement = fg_json_str(args, "new_text");
    char full[FG_PATH_MAX];
    if (!fg_safe_path(c->root, path, true, full, e))
        return NULL;
    struct stat st;
    bool exists = stat(full, &st) == 0;
    if (exists && (!*old || (st.st_mode & S_IFMT) != S_IFREG)) {
        fg_error(e, FORGE_ERR_CONFLICT,
                 "Empty old_text is only for creating a missing regular file");
        return NULL;
    }
    if (!exists && *old) {
        fg_error(e, FORGE_ERR_NOT_FOUND, "Patch target does not exist");
        return NULL;
    }
#ifdef _WIN32
    if (exists) {
        HANDLE h = CreateFileA(full, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        bool linked = h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &info) ||
                      info.nNumberOfLinks > 1;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (linked) {
            fg_error(e, FORGE_ERR_POLICY, "Refusing a hard-linked or inaccessible patch target");
            return NULL;
        }
    }
#endif
    size_t len = 0;
    char *text =
        exists ? fg_read_file(full, c->config.limits.max_file_bytes, &len, e) : fg_strdup("");
    if (!text)
        return NULL;
    if (memchr(text, 0, len)) {
        free(text);
        fg_error(e, FORGE_ERR_PARSE, "Cannot patch binary content");
        return NULL;
    }
    char *match = *old ? strstr(text, old) : text;
    if (!match || (*old && strstr(match + 1, old))) {
        free(text);
        fg_error(e, FORGE_ERR_CONFLICT,
                 "old_text must match exactly once; read current source before retrying");
        return NULL;
    }
    fg_buf out = {0};
    size_t prefix = (size_t)(match - text);
    fg_buf_add(&out, text, prefix);
    fg_buf_puts(&out, replacement);
    fg_buf_puts(&out, match + strlen(old));
    if (out.failed || out.len > c->config.limits.max_file_bytes) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_LIMIT, "Patched file exceeds file budget");
        return NULL;
    }
    if (exists && out.len == len && !memcmp(out.data, text, len)) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_CONFLICT,
                 "No edit performed: old_text and new_text are identical. Supply different "
                 "replacement text; encode line breaks as \\n when changing multiple statements.");
        return NULL;
    }
    char temp[FG_PATH_MAX], random[17];
    if (!fg_random_hex(random, 8) ||
        snprintf(temp, sizeof(temp), "%s.forge-%s.tmp", full, random) >= (int)sizeof(temp)) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_IO, "Cannot create patch staging path");
        return NULL;
    }
    FILE *f = fopen(temp, "wbx");
    if (!f) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_IO, "Cannot exclusively create patch staging file");
        return NULL;
    }
    bool ok = fwrite(out.data, 1, out.len, f) == out.len;
    if (fclose(f) != 0)
        ok = false;
#ifndef _WIN32
    if (ok && exists && chmod(temp, st.st_mode & 0777) != 0)
        ok = false;
#endif
    if (!ok) {
        free(text);
        fg_buf_clear(&out);
        remove(temp);
        fg_error(e, FORGE_ERR_IO, "Cannot write patch staging file");
        return NULL;
    }
    fg_edit_record edit = {0};
    if (!fg_edit_prepare(c, path, exists, (forge_slice){text, len},
                         (forge_slice){out.data, out.len}, &edit, e)) {
        free(text);
        fg_buf_clear(&out);
        remove(temp);
        return NULL;
    }
    /* Recheck the exact source before atomic replacement to catch ordinary concurrent edits. */
    if (ok && exists) {
        size_t current_len = 0;
        char *current = fg_read_file(full, c->config.limits.max_file_bytes, &current_len, e);
        ok = current && current_len == len && !memcmp(current, text, len);
        free(current);
    }
    if (ok && !exists) {
        struct stat current;
        if (stat(full, &current) == 0)
            ok = false;
    }
    if (ok && !fg_safe_path(c->root, path, true, full, e))
        ok = false;
    if (ok && ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
               (c->deadline && fg_now_ms() >= c->deadline))) {
        fg_error(e, FORGE_ERR_CANCELLED, "Patch cancelled before target replacement");
        ok = false;
    }
#ifdef _WIN32
    if (ok)
        ok = MoveFileExA(temp, full,
                         exists ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                : MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok)
        ok = rename(temp, full) == 0;
#endif
    free(text);
    fg_buf_clear(&out);
    if (!ok) {
        remove(temp);
        if (!e || !e->code)
            fg_error(e, FORGE_ERR_CONFLICT, "Atomic patch failed or source changed concurrently");
        forge_status reason = e && e->code ? e->code : FORGE_ERR_CONFLICT;
        fg_edit_finish(c, &edit, false, reason, e);
        return NULL;
    }
    *changed = true;
    if (!fg_edit_finish(c, &edit, true, FORGE_OK, e))
        return NULL;
    fg_buf result = {0};
    fg_buf_printf(&result, "Patched %s.\nRecorded edit diff: %s\n", path, edit.diff);
    const char *ext = strrchr(path, '.');
    if (ext && !strcmp(ext, ".go")) {
        char *target = fg_repo_targets(c->repo, path, e);
        if (target) {
            fg_buf_printf(
                &result, "Suggested targeted validation (requires command approval): %s\n", target);
            free(target);
        }
    }
    return fg_buf_take(&result);
}
static char *run(fg_tool_context *c, const char *const *argv, forge_error *e) {
    fg_process_result r = {0};
    uint64_t now = fg_now_ms();
    if ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
        (c->deadline && now >= c->deadline)) {
        fg_error(e, FORGE_ERR_CANCELLED, "Command cancelled or absolute deadline reached");
        return NULL;
    }
    uint64_t left = c->deadline ? c->deadline - now : c->config.limits.command_timeout_ms;
    forge_status status =
        fg_process(c->root, argv, FG_MIN(left, c->config.limits.command_timeout_ms),
                   c->config.limits.max_tool_bytes, c->config.cancelled, c->config.userdata, &r, e);
    /* A launched command may have changed files even if capture later fails. */
    c->process_ran = r.started;
    c->process = r;
    c->process.out = c->process.err = NULL;
    if (status != FORGE_OK) {
        fg_process_free(&r);
        return NULL;
    }
    char artifact[64];
    snprintf(artifact, sizeof(artifact), "tool/%06zu.stdout", c->call_id);
    if (!fg_session_artifact_bytes(c->session, artifact, r.out, r.out_len, e)) {
        fg_process_free(&r);
        return NULL;
    }
    snprintf(artifact, sizeof(artifact), "tool/%06zu.stderr", c->call_id);
    if (!fg_session_artifact_bytes(c->session, artifact, r.err, r.err_len, e)) {
        fg_process_free(&r);
        return NULL;
    }
    char *result = fg_process_render(&r);
    fg_process_free(&r);
    return result;
}
char *fg_tool_execute(fg_tool_context *c, const char *name, yyjson_val *args, bool *changed,
                      forge_error *e) {
    *changed = false;
    c->process_ran = false;
    c->evidence_failed = false;
    memset(&c->process, 0, sizeof(c->process));
    if (!fg_tool_validate(name, args, e))
        return NULL;
    const fg_tool_def *def = NULL;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!strcmp(name, definitions[i].name))
            def = &definitions[i];
    bool allowed = def->capability == FORGE_CAP_READ ||
                   (def->capability == FORGE_CAP_WRITE && c->config.allow_write) ||
                   (def->capability == FORGE_CAP_PROCESS && c->config.allow_exec);
    if (c->config.policy) {
        char *json = yyjson_val_write(args, 0, NULL);
        allowed = c->config.policy(name, def->capability, json ? json : "{}", c->config.userdata);
        free(json);
    }
    if (!allowed) {
        fg_error(e, FORGE_ERR_POLICY, "%s denied: explicit %s approval is required", name,
                 def->capability == FORGE_CAP_WRITE ? "write" : "unsandboxed process");
        return NULL;
    }
    if ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
        (c->deadline && fg_now_ms() >= c->deadline)) {
        fg_error(e, FORGE_ERR_CANCELLED,
                 "Tool cancelled or deadline reached after policy approval");
        return NULL;
    }
    if (!strcmp(name, "read_file"))
        return read_lines(c, args, e);
    if (!strcmp(name, "apply_patch"))
        return patch(c, args, changed, e);
    if (!strcmp(name, "find_symbol")) {
        size_t depth;
        fg_json_uint(args, "depth", &depth, 0);
        return forge_repo_inspect(c->repo, fg_json_str(args, "name"), (int)depth, e);
    }
    if (!strcmp(name, "get_references"))
        return forge_repo_references(c->repo, fg_json_str(args, "name"), e);
    if (!strcmp(name, "search_text"))
        return fg_repo_search(c->repo, fg_json_str(args, "query"), 50, e);
    if (!strcmp(name, "list_directory"))
        return forge_repo_summary(c->repo, e);
    if (!strcmp(name, "expand_output")) {
        size_t id, offset;
        fg_json_uint(args, "id", &id, 0);
        fg_json_uint(args, "offset", &offset, 0);
        if (!id || id >= c->call_id) {
            fg_error(e, FORGE_ERR_ARGUMENT, "Unknown prior tool output id");
            return NULL;
        }
        char file[64], path[FG_PATH_MAX];
        snprintf(file, sizeof(file), "tool/%06zu.raw", id);
        fg_path_join(path, c->session->dir, file);
        size_t len;
        char *raw = fg_read_file(path, c->config.limits.max_tool_bytes * 8 + 1024, &len, e);
        if (!raw)
            return NULL;
        if (memchr(raw, 0, len) || !fg_utf8_valid(raw, len)) {
            free(raw);
            fg_error(e, FORGE_ERR_PARSE,
                     "Recorded output is not valid UTF-8 text; inspect the exact stream artifacts");
            return NULL;
        }
        if (offset > len) {
            free(raw);
            fg_error(e, FORGE_ERR_ARGUMENT, "Output offset out of range");
            return NULL;
        }
        fg_buf b = {0};
        offset = fg_utf8_forward(raw, len, offset);
        size_t take = fg_utf8_prefix(raw + offset, len - offset, 8192);
        fg_buf_add(&b, raw + offset, take);
        free(raw);
        return fg_buf_take(&b);
    }
    if (!strcmp(name, "git_diff")) {
        const char *v[] = {
            "git", "-c", "core.fsmonitor=false", "diff", "--no-ext-diff", "--no-textconv",
            "--",  NULL};
        return run(c, v, e);
    }
    if (!strcmp(name, "git_status")) {
        const char *v[] = {"git",
                           "-c",
                           "core.fsmonitor=false",
                           "status",
                           "--porcelain=v1",
                           "--untracked-files=normal",
                           NULL};
        return run(c, v, e);
    }
    if (!strcmp(name, "run_command")) {
        yyjson_val *arr = yyjson_obj_get(args, "argv");
        const char *v[65] = {0};
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(arr, i, n, item) v[i] = yyjson_get_str(item);
        return run(c, v, e);
    }
    fg_error(e, FORGE_ERR_ARGUMENT, "Unimplemented tool");
    return NULL;
}
