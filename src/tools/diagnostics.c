#include "internal.h"
static int compare_hashes(const void *a, const void *b) {
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return left < right ? -1 : left > right ? 1 : 0;
}
/* Diagnostic identity ignores Go event timestamps, elapsed time, progress and
 * event ordering. It is a loop heuristic, never a proof that tests succeeded. */
uint64_t fg_diagnostic_hash(const char *raw) {
    uint64_t hashes[512];
    size_t count = 0;
    const char *p = raw;
    while (p && *p && count < sizeof(hashes) / sizeof(*hashes)) {
        const char *end = strchr(p, '\n');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        yyjson_doc *doc = yyjson_read(p, length, 0);
        yyjson_val *obj = doc ? yyjson_doc_get_root(doc) : NULL;
        const char *action = fg_json_str(obj, "Action");
        const char *output = fg_json_str(obj, "Output");
        if (action && (!strcmp(action, "fail") || !strcmp(action, "build-fail"))) {
            const char *package = fg_json_str(obj, "Package"), *test = fg_json_str(obj, "Test");
            fg_buf identity = {0};
            fg_buf_printf(&identity, "%s:%s:%s", action, package ? package : "", test ? test : "");
            if (!identity.failed)
                hashes[count++] = fg_hash(identity.data, identity.len);
            fg_buf_clear(&identity);
        }
        char *decoded =
            output ? fg_render_bytes(output, yyjson_get_len(yyjson_obj_get(obj, "Output"))) : NULL;
        const char *line = decoded ? decoded : action ? "" : p;
        size_t n = decoded ? strlen(decoded) : action ? 0 : length;
        char *text = malloc(n + 1);
        if (text) {
            memcpy(text, line, n);
            text[n] = 0;
            bool relevant = !strncmp(text, "exit_code=", 10) || strstr(text, ".go:") ||
                            strstr(text, ".c:") || strstr(text, ".rs:") || strstr(text, "error") ||
                            strstr(text, "Error") || strstr(text, "expected") ||
                            strstr(text, "actual") || strstr(text, "panic") ||
                            strstr(text, "Assertion");
            while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' '))
                text[--n] = 0;
            if (relevant && count < sizeof(hashes) / sizeof(*hashes))
                hashes[count++] = fg_hash(text, n);
            free(text);
        }
        free(decoded);
        yyjson_doc_free(doc);
        if (!end)
            break;
        p = end + 1;
    }
    qsort(hashes, count, sizeof(*hashes), compare_hashes);
    size_t unique = 0;
    for (size_t i = 0; i < count; i++)
        if (!i || hashes[i] != hashes[i - 1])
            hashes[unique++] = hashes[i];
    return fg_hash(hashes, unique * sizeof(*hashes));
}
/* Go's -json protocol is preferred. Generic diagnostics keep failures and a
 * bounded tail; every raw byte remains in the session's tool artifacts. */
char *fg_compress_output(const char *raw, size_t budget, size_t *visible, forge_error *e) {
    if (!raw || budget < 64) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid output budget");
        return NULL;
    }
    char *rendered_raw = fg_render_bytes(raw, strlen(raw));
    if (!rendered_raw) {
        fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
        return NULL;
    }
    raw = rendered_raw;
    fg_buf result = {0};
    const char *p = raw;
    size_t pass = 0, fail = 0, skipped = 0;
    uint64_t seen[256] = {0};
    size_t count = 0;
    bool go_protocol = false;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *line = p;
        char *owned = NULL;
        yyjson_doc *d = yyjson_read(p, n, 0);
        if (d) {
            yyjson_val *o = yyjson_doc_get_root(d);
            const char *action = fg_json_str(o, "Action"), *output = fg_json_str(o, "Output"),
                       *test = fg_json_str(o, "Test"), *pkg = fg_json_str(o, "Package");
            bool go_event =
                action &&
                (!strcmp(action, "start") || !strcmp(action, "run") || !strcmp(action, "pause") ||
                 !strcmp(action, "cont") || !strcmp(action, "output") || !strcmp(action, "pass") ||
                 !strcmp(action, "skip") || !strcmp(action, "fail") ||
                 !strcmp(action, "build-output") || !strcmp(action, "build-fail"));
            go_protocol = go_protocol || go_event;
            if (go_event && !strcmp(action, "pass"))
                pass++;
            if (go_event && (!strcmp(action, "fail") || !strcmp(action, "build-fail"))) {
                fail++;
                fg_buf_printf(&result, "FAIL %s %s\n", pkg ? pkg : "", test ? test : "");
            }
            if (go_event && output) {
                owned = fg_render_bytes(output, yyjson_get_len(yyjson_obj_get(o, "Output")));
                if (!owned) {
                    yyjson_doc_free(d);
                    fg_buf_clear(&result);
                    free(rendered_raw);
                    fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
                    return NULL;
                }
                line = owned;
                n = strlen(owned);
            } else if (go_event)
                n = 0;
        }
        char *bounded = malloc(n + 1);
        if (!bounded) {
            free(owned);
            if (d)
                yyjson_doc_free(d);
            fg_buf_clear(&result);
            free(rendered_raw);
            fg_error(e, FORGE_ERR_MEMORY, "Diagnostic allocation failed");
            return NULL;
        }
        memcpy(bounded, line, n);
        bounded[n] = 0;
        line = bounded;
        bool relevant =
            n && (strstr(line, "exit_code=") || strstr(line, "FAIL") || strstr(line, "error") ||
                  strstr(line, "Error") || strstr(line, "panic") || strstr(line, "expected") ||
                  strstr(line, "actual") || strstr(line, ".go:") || strstr(line, ".c:") ||
                  strstr(line, ".rs:") || strstr(line, "Assertion"));
        if (relevant) {
            uint64_t hash = fg_hash(line, n);
            bool duplicate = false;
            for (size_t i = 0; i < count; i++)
                if (seen[i] == hash)
                    duplicate = true;
            size_t take = fg_utf8_prefix(line, n, 1024);
            if (!duplicate && result.len + take + 1 < budget - 64) {
                if (count < 256)
                    seen[count++] = hash;
                fg_buf_add(&result, line, take);
                if (!take || line[take - 1] != '\n')
                    fg_buf_puts(&result, "\n");
            } else
                skipped++;
        }
        free(bounded);
        free(owned);
        if (d)
            yyjson_doc_free(d);
        if (!end)
            break;
        p = end + 1;
    }
    if (!go_protocol) {
        size_t raw_size = strlen(raw);
        if (raw_size <= budget) {
            /* A successful status header must not hide useful stdout or generic JSON. */
            fg_buf_clear(&result);
            fg_buf_puts(&result, raw);
        } else {
            size_t tail = FG_MIN(budget / 3, 1024);
            size_t prefix_limit = budget > tail + 48 ? budget - tail - 48 : 0;
            if (result.len > prefix_limit) {
                result.len = fg_utf8_prefix(result.data, result.len, prefix_limit);
                result.data[result.len] = 0;
            }
            const char *tail_start = raw + fg_utf8_forward(raw, raw_size, raw_size - tail);
            fg_buf_puts(&result, "\n[tail; full output saved]\n");
            fg_buf_puts(&result, tail_start);
        }
    } else if (!result.len) {
        size_t n = strlen(raw);
        size_t tail_budget = budget > 80 ? budget - 80 : 0;
        if (n > tail_budget) {
            fg_buf_puts(&result, "[tail; full output saved]\n");
            size_t start = fg_utf8_forward(raw, n, n - tail_budget);
            fg_buf_add(&result, raw + start, n - start);
        } else
            fg_buf_puts(&result, raw);
    }
    if (pass || fail || skipped)
        fg_buf_printf(&result, "\nsummary: pass=%zu fail=%zu omitted=%zu\n", pass, fail, skipped);
    if (result.len > budget) {
        result.len = fg_utf8_prefix(result.data, result.len, budget);
        result.data[result.len] = 0;
    }
    if (visible)
        *visible = result.len;
    free(rendered_raw);
    return fg_buf_take(&result);
}
