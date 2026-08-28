#include "edit_journal.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define EDIT_FILE_LIMIT (16u * 1024u * 1024u)
#define EDIT_OUTCOME_RESERVE 512u

static bool check(forge_cancel_fn cancelled, void *user, uint64_t deadline, forge_error *error) {
    if ((cancelled && cancelled(user)) || (deadline && fg_now_ms() >= deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Edit evidence cancelled or deadline reached");
        return false;
    }
    return true;
}

static bool lines(forge_slice text, size_t *count, forge_cancel_fn cancelled, void *user,
                  uint64_t deadline, forge_error *error) {
    *count = 0;
    for (size_t i = 0; i < text.len; i++) {
        if (!(i & 65535u) && !check(cancelled, user, deadline, error))
            return false;
        *count += text.ptr[i] == '\n';
    }
    if (text.len && text.ptr[text.len - 1] != '\n')
        (*count)++;
    return true;
}

static void quoted_path(fg_buf *out, char side, const char *path) {
    fg_buf_printf(out, "\"%c/", side);
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 32 || *p >= 127 || *p == '\\' || *p == '"')
            fg_buf_printf(out, "\\%03o", (unsigned)*p);
        else
            fg_buf_add(out, (const char *)p, 1);
    }
    fg_buf_puts(out, "\"");
}

/* out has already been allocated to the exact final size. */
static bool diff_lines(char *out, size_t *used, char sign, forge_slice text,
                       forge_cancel_fn cancelled, void *user, uint64_t deadline,
                       forge_error *error) {
    size_t offset = 0, next_check = 0;
    while (offset < text.len) {
        if (offset >= next_check) {
            if (!check(cancelled, user, deadline, error))
                return false;
            next_check = offset + 65536u;
        }
        const char *newline = memchr(text.ptr + offset, '\n', text.len - offset);
        size_t n = newline ? (size_t)(newline - (text.ptr + offset)) + 1 : text.len - offset;
        out[(*used)++] = sign;
        memcpy(out + *used, text.ptr + offset, n);
        *used += n;
        offset += n;
        if (!newline) {
            const char marker[] = "\n\\ No newline at end of file\n";
            memcpy(out + *used, marker, sizeof(marker) - 1);
            *used += sizeof(marker) - 1;
        }
    }
    return true;
}

char *fg_edit_diff(const char *path, bool before_exists, forge_slice before, forge_slice after,
                   size_t *length, forge_cancel_fn cancelled, void *user, uint64_t deadline,
                   forge_error *error) {
    if (length)
        *length = 0;
    char canonical[FG_PATH_MAX];
    if (!length || !fg_relative_path(path, canonical, error) || (!before.ptr && before.len) ||
        (!after.ptr && after.len) || (!before_exists && before.len)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid edit diff input");
        return NULL;
    }
    if (before.len > EDIT_FILE_LIMIT || after.len > EDIT_FILE_LIMIT) {
        fg_error(error, FORGE_ERR_LIMIT, "Edit diff exceeds the 16 MiB per-file bound");
        return NULL;
    }
    if (!check(cancelled, user, deadline, error))
        return NULL;
    if ((before.len &&
         (memchr(before.ptr, 0, before.len) || !fg_utf8_valid(before.ptr, before.len))) ||
        (after.len && (memchr(after.ptr, 0, after.len) || !fg_utf8_valid(after.ptr, after.len)))) {
        fg_error(error, FORGE_ERR_PARSE, "Edit diff requires NUL-free UTF-8 text");
        return NULL;
    }
    if (before_exists && before.len == after.len &&
        (!before.len || !memcmp(before.ptr, after.ptr, before.len))) {
        fg_error(error, FORGE_ERR_CONFLICT, "An unchanged file has no edit diff");
        return NULL;
    }
    size_t old_lines, new_lines;
    if (!lines(before, &old_lines, cancelled, user, deadline, error) ||
        !lines(after, &new_lines, cancelled, user, deadline, error))
        return NULL;
    fg_buf old_path = {0}, new_path = {0}, header = {0};
    quoted_path(&old_path, 'a', canonical);
    quoted_path(&new_path, 'b', canonical);
    if (!old_path.failed && !new_path.failed) {
        fg_buf_printf(&header, "diff --git %s %s\n", old_path.data, new_path.data);
        if (!before_exists)
            fg_buf_puts(&header, "new file mode 100644\n");
        /* A mode-only new-file diff is the standard representation of a new
         * empty regular file. File modes beyond this are outside our contract. */
        if (old_lines || new_lines) {
            fg_buf_printf(&header, "--- %s\n+++ %s\n@@ -%u,%zu +%u,%zu @@\n",
                          before_exists ? old_path.data : "/dev/null", new_path.data,
                          old_lines ? 1u : 0u, old_lines, new_lines ? 1u : 0u, new_lines);
        }
    } else
        header.failed = true;
    fg_buf_clear(&old_path);
    fg_buf_clear(&new_path);
    if (header.failed) {
        fg_buf_clear(&header);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate edit diff header");
        return NULL;
    }
    const size_t marker_bytes = sizeof("\n\\ No newline at end of file\n") - 1;
    size_t size = header.len + before.len + after.len + old_lines + new_lines;
    if (before.len && before.ptr[before.len - 1] != '\n')
        size += marker_bytes;
    if (after.len && after.ptr[after.len - 1] != '\n')
        size += marker_bytes;
    /* Inputs and paths have fixed bounds, so this sum is below 65 MiB. */
    char *out = malloc(size + 1);
    if (!out) {
        fg_buf_clear(&header);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate bounded edit diff");
        return NULL;
    }
    memcpy(out, header.data, header.len);
    size_t used = header.len;
    fg_buf_clear(&header);
    if (!diff_lines(out, &used, '-', before, cancelled, user, deadline, error) ||
        !diff_lines(out, &used, '+', after, cancelled, user, deadline, error) ||
        !check(cancelled, user, deadline, error)) {
        free(out);
        return NULL;
    }
    if (used != size) {
        free(out);
        fg_error(error, FORGE_ERR_IO, "Edit diff length invariant failed");
        return NULL;
    }
    out[size] = 0;
    *length = size;
    return out;
}

static bool artifact(fg_tool_context *context, const char *name, const char *bytes, size_t length,
                     bool cancellable, forge_error *error) {
    char path[FG_PATH_MAX];
    if (!fg_safe_path(context->session->dir, name, true, path, error))
        return false;
    if (cancellable &&
        !check(context->config.cancelled, context->config.userdata, context->deadline, error))
        return false;
#ifdef _WIN32
    wchar_t wide[FG_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, FG_PATH_MAX)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Edit artifact path is not valid UTF-8");
        return false;
    }
    FILE *file = _wfopen(wide, L"wbx");
#else
    FILE *file = fopen(path, "wbx");
#endif
    if (!file) {
        fg_error(error, FORGE_ERR_IO, "Cannot exclusively create edit artifact %s", name);
        return false;
    }
    bool ok = true;
    for (size_t offset = 0; offset < length && ok;) {
        if (cancellable &&
            !check(context->config.cancelled, context->config.userdata, context->deadline, error)) {
            ok = false;
            break;
        }
        size_t chunk = FG_MIN((size_t)65536, length - offset);
        ok = fwrite(bytes + offset, 1, chunk, file) == chunk;
        offset += chunk;
    }
    if (fclose(file) != 0)
        ok = false;
    if (!ok && (!error || error->code != FORGE_ERR_CANCELLED))
        fg_error(error, FORGE_ERR_IO, "Cannot finish edit artifact %s", name);
    return ok;
}

bool fg_edit_prepare(fg_tool_context *context, const char *path, bool before_exists,
                     forge_slice before, forge_slice after, fg_edit_record *record,
                     forge_error *error) {
    if (!record || !context || !context->session || !context->session->events ||
        !context->call_id) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Edit evidence requires an active tool session");
        return false;
    }
    memset(record, 0, sizeof(*record));
    if (!fg_relative_path(path, record->path, error))
        return false;
    record->call_id = context->call_id;
    record->before_bytes = before.len;
    record->after_bytes = after.len;
    record->before_exists = before_exists;
    snprintf(record->before, sizeof(record->before), "tool/%06zu.before", record->call_id);
    snprintf(record->after, sizeof(record->after), "tool/%06zu.after", record->call_id);
    snprintf(record->diff, sizeof(record->diff), "tool/%06zu.patch", record->call_id);
    snprintf(record->intent, sizeof(record->intent), "tool/%06zu.edit.json", record->call_id);
    snprintf(record->outcome, sizeof(record->outcome), "tool/%06zu.edit-result.json",
             record->call_id);
    size_t diff_length = 0;
    char *diff =
        fg_edit_diff(record->path, before_exists, before, after, &diff_length,
                     context->config.cancelled, context->config.userdata, context->deadline, error);
    if (!diff)
        return false;
    char *quoted = fg_json_string(record->path);
    fg_buf manifest = {0};
    if (quoted)
        fg_buf_printf(&manifest,
                      "{\"schema_version\":1,\"tool_call\":%zu,\"state\":\"prepared\","
                      "\"path\":%s,\"scope\":\"one_agent_text_edit\",\"before_exists\":%s,"
                      "\"before_bytes\":%zu,\"after_bytes\":%zu,\"before_artifact\":\"%s\","
                      "\"after_artifact\":\"%s\",\"diff_artifact\":\"%s\","
                      "\"diff_format\":\"unified_full_file\",\"content_only\":true,"
                      "\"outcome_artifact\":\"%s\"}",
                      record->call_id, quoted, before_exists ? "true" : "false", before.len,
                      after.len, record->before, record->after, record->diff, record->outcome);
    else
        manifest.failed = true;
    free(quoted);
    if (manifest.failed) {
        free(diff);
        fg_buf_clear(&manifest);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate edit manifest");
        return false;
    }
    size_t reserve = before.len + after.len + diff_length + manifest.len + EDIT_OUTCOME_RESERVE;
    fg_session *session = context->session;
    if (session->edit_bytes_reserved > session->edit_bytes_limit ||
        reserve > session->edit_bytes_limit - session->edit_bytes_reserved) {
        free(diff);
        fg_buf_clear(&manifest);
        fg_error(error, FORGE_ERR_LIMIT, "Session edit-artifact byte budget exhausted");
        return false;
    }
    /* Keep reservations after failed/partial writes too; never hide their disk
     * cost by refunding them. This is not the entire session's storage budget. */
    session->edit_bytes_reserved += reserve;
    bool ok = artifact(context, record->before, before.ptr, before.len, true, error) &&
              artifact(context, record->after, after.ptr, after.len, true, error) &&
              artifact(context, record->diff, diff, diff_length, true, error) &&
              artifact(context, record->intent, manifest.data, manifest.len, true, error);
    if (ok && !fg_session_emit(session, "edit_prepared", manifest.data, error)) {
        /* Event failure can leave a gap or partial line in the session log.
         * The unchanged target does not make that audit stream usable. */
        context->evidence_failed = true;
        if (!error || !error->code)
            fg_error(error, FORGE_ERR_IO, "Edit preparation event could not be recorded");
        ok = false;
    }
    free(diff);
    fg_buf_clear(&manifest);
    record->prepared = ok;
    return ok;
}

bool fg_edit_finish(fg_tool_context *context, fg_edit_record *record, bool applied,
                    forge_status status, forge_error *error) {
    if (!context || !context->session || !record || !record->prepared || record->finished ||
        context->call_id != record->call_id || (applied && status != FORGE_OK) ||
        (!applied && status == FORGE_OK)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid prepared edit outcome");
        return false;
    }
    char outcome[EDIT_OUTCOME_RESERVE];
    int length = snprintf(outcome, sizeof(outcome),
                          "{\"schema_version\":1,\"tool_call\":%zu,\"state\":\"%s\","
                          "\"status\":\"%s\",\"intent_artifact\":\"%s\"}",
                          record->call_id, applied ? "applied" : "aborted",
                          forge_status_string(status), record->intent);
    bool ok = length > 0 && (size_t)length < sizeof(outcome) &&
              artifact(context, record->outcome, outcome, (size_t)length, false, error) &&
              fg_session_emit(context->session, "edit_result", outcome, error);
    record->finished = true;
    if (!ok) {
        context->evidence_failed = true;
        if (!error || !error->code)
            fg_error(error, FORGE_ERR_IO, "Prepared edit outcome could not be recorded");
    }
    return ok;
}
