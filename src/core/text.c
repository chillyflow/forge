#include "internal.h"

static size_t utf8_width(const char *text, size_t length) {
    if (!length)
        return 0;
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80)
        return 1;
    size_t width = c >= 0xc2 && c <= 0xdf   ? 2
                   : c >= 0xe0 && c <= 0xef ? 3
                   : c >= 0xf0 && c <= 0xf4 ? 4
                                            : 0;
    if (!width || width > length)
        return 0;
    for (size_t j = 1; j < width; j++)
        if (((unsigned char)text[j] & 0xc0) != 0x80)
            return 0;
    unsigned char next = (unsigned char)text[1];
    if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) || (c == 0xf0 && next < 0x90) ||
        (c == 0xf4 && next >= 0x90))
        return 0;
    return width;
}
bool fg_utf8_valid(const char *text, size_t length) {
    if (!text)
        return length == 0;
    for (size_t i = 0; i < length;) {
        size_t width = utf8_width(text + i, length - i);
        if (!width)
            return false;
        i += width;
    }
    return true;
}

size_t fg_utf8_prefix(const char *text, size_t length, size_t maximum) {
    if (!text)
        return 0;
    /* Callers validate or render the full byte string before slicing it. */
    size_t take = FG_MIN(length, maximum);
    if (take < length)
        while (take && ((unsigned char)text[take] & 0xc0) == 0x80)
            take--;
    return take;
}

size_t fg_utf8_forward(const char *text, size_t length, size_t offset) {
    if (!text)
        return 0;
    offset = FG_MIN(offset, length);
    while (offset < length && ((unsigned char)text[offset] & 0xc0) == 0x80)
        offset++;
    return offset;
}

/* Drops a trailing INCOMPLETE UTF-8 sequence (a lead byte with too few
 * continuation bytes before the end), at most one character's worth. Text cut
 * at an arbitrary byte boundary — a budget-forced action swap — is otherwise
 * unfixably invalid. Anything else, including outright invalid bytes, is left
 * for fg_utf8_valid to reject. */
size_t fg_utf8_trim_incomplete(const char *text, size_t length) {
    if (!text)
        return 0;
    size_t lead = length;
    while (lead && length - lead < 3 && ((unsigned char)text[lead - 1] & 0xc0) == 0x80)
        lead--;
    if (!lead)
        return length;
    unsigned char first = (unsigned char)text[lead - 1];
    size_t width = first < 0x80                     ? 1
                   : first >= 0xc2 && first <= 0xdf ? 2
                   : first >= 0xe0 && first <= 0xef ? 3
                   : first >= 0xf0 && first <= 0xf4 ? 4
                                                    : 0;
    return width > length - lead + 1 ? lead - 1 : length;
}

/* Human-readable text is not a lossless encoding: a literal "\\x00" and a
 * rendered NUL look alike. Exact stdout/stderr artifacts remain authoritative. */
static bool render_bytes(fg_buf *result, const char *bytes, size_t length) {
    if (!bytes && length) {
        result->failed = true;
        return false;
    }
    for (size_t i = 0; i < length && !result->failed;) {
        unsigned char c = (unsigned char)bytes[i];
        size_t width = utf8_width(bytes + i, length - i);
        if (width > 1 ||
            (width == 1 && ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t'))) {
            fg_buf_add(result, bytes + i, width);
            i += width;
        } else {
            fg_buf_printf(result, "\\x%02x", (unsigned)c);
            i++;
        }
    }
    return !result->failed;
}
char *fg_render_bytes(const char *bytes, size_t length) {
    fg_buf result = {0};
    render_bytes(&result, bytes, length);
    return fg_buf_take(&result);
}
char *fg_process_render(const fg_process_result *r) {
    if (!r)
        return NULL;
    fg_buf result = {0};
    fg_buf_printf(&result, "exit_code=%d timeout=%s cancelled=%s truncated=%s\nstdout:\n",
                  r->exit_code, r->timed_out ? "true" : "false", r->cancelled ? "true" : "false",
                  r->truncated ? "true" : "false");
    render_bytes(&result, r->out, r->out_len);
    fg_buf_puts(&result, "\nstderr:\n");
    render_bytes(&result, r->err, r->err_len);
    return fg_buf_take(&result);
}

/* Rewrite every occurrence of `needle` in `text` with `with`. */
static char *replace_all(const char *text, const char *needle, const char *with) {
    size_t needle_length = strlen(needle);
    fg_buf out = {0};
    const char *at = text;
    for (;;) {
        const char *hit = strstr(at, needle);
        if (!hit) {
            if (!fg_buf_puts(&out, at))
                break;
            return fg_buf_take(&out);
        }
        if (!fg_buf_add(&out, at, (size_t)(hit - at)) || !fg_buf_puts(&out, with))
            break;
        at = hit + needle_length;
    }
    fg_buf_clear(&out);
    return NULL;
}

/* A rendered prompt must not depend on where the workspace happens to live.
 * Tool output carries absolute paths, so a workspace under a randomly named
 * temporary root produced different prompts for otherwise identical runs and
 * diverged a run that was otherwise reproducible. The workspace root is
 * rewritten to "." — which is also the relative form the model should use.
 *
 * Host text reaches the model through more than one producer (command output,
 * the rendered process result, retained validation feedback), so this is
 * applied at each of them rather than at one shared sink.
 *
 * Command output is embedded as an escaped JSON string, in which every
 * backslash of the path appears doubled, so that spelling is rewritten first;
 * the literal spelling is rewritten second. */
char *fg_normalize_workspace_paths(const char *root, const char *text) {
    if (!root || !*root || !text)
        return NULL;
    fg_buf escaped = {0};
    for (const char *at = root; *at; at++) {
        if (*at == '\\') {
            if (!fg_buf_puts(&escaped, "\\\\")) {
                fg_buf_clear(&escaped);
                return NULL;
            }
        } else if (!fg_buf_add(&escaped, at, 1)) {
            fg_buf_clear(&escaped);
            return NULL;
        }
    }
    char *pattern = fg_buf_take(&escaped);
    if (!pattern)
        return NULL;
    /* The escaped and the literal spelling are the same pattern when root has
     * no backslash, so the first pass already replaced every occurrence. The
     * second pass can only differ by matching text that a replacement "."
     * itself re-formed, which needs a '.' inside root; with none, skipping it
     * is byte-identical. (Roots that do contain a '.' keep both passes.) */
    bool shared = strcmp(pattern, root) == 0 && strchr(root, '.') == NULL;
    char *first = replace_all(text, pattern, ".");
    free(pattern);
    if (!first)
        return NULL;
    if (shared)
        return first;
    char *second = replace_all(first, root, ".");
    free(first);
    return second;
}
