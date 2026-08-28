#include "internal.h"
/* Go's -json protocol is preferred. Generic diagnostics keep failures and a
 * bounded tail; every raw byte remains in the session's tool artifacts. */
char *fg_compress_output(const char *raw, size_t budget, size_t *visible, forge_error *e) {
    if (!raw || budget < 64) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid output budget");
        return NULL;
    }
    fg_buf result = {0};
    const char *p = raw;
    size_t pass = 0, fail = 0, skipped = 0;
    uint64_t seen[256] = {0};
    size_t count = 0;
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
            if (action && !strcmp(action, "pass"))
                pass++;
            if (action && !strcmp(action, "fail")) {
                fail++;
                fg_buf_printf(&result, "FAIL %s %s\n", pkg ? pkg : "", test ? test : "");
            }
            if (output) {
                owned = fg_strdup(output);
                line = owned;
                n = strlen(output);
            } else
                n = 0;
        }
        char *bounded = malloc(n + 1);
        if (!bounded) {
            free(owned);
            if (d)
                yyjson_doc_free(d);
            fg_buf_clear(&result);
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
            if (!duplicate && result.len + FG_MIN(n, 1024) + 1 < budget - 64) {
                if (count < 256)
                    seen[count++] = hash;
                fg_buf_add(&result, line, FG_MIN(n, 1024));
                if (!n || line[n - 1] != '\n')
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
    if (!result.len) {
        size_t n = strlen(raw);
        if (n > budget - 80) {
            fg_buf_puts(&result, "[tail; full output saved]\n");
            fg_buf_add(&result, raw + n - (budget - 80), budget - 80);
        } else
            fg_buf_puts(&result, raw);
    }
    if (pass || fail || skipped)
        fg_buf_printf(&result, "\nsummary: pass=%zu fail=%zu omitted=%zu\n", pass, fail, skipped);
    if (result.len > budget) {
        result.len = budget;
        result.data[budget] = 0;
    }
    if (visible)
        *visible = result.len;
    return fg_buf_take(&result);
}
