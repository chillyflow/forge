#include "internal.h"
/* §32 decode-state routing predicates. These are the pure halves of the llama
 * backend's per-state machine, kept in an always-compiled unit so the routing
 * decisions stay unit-testable without a model or the llama backend. */

/* Returns the opening brace of the first action-object start: '{', optional
 * JSON whitespace, a quoted tool/memory/final key, optional whitespace, ':'.
 * Mirrors FG_ACTION_TRIGGER_PATTERN so the host can tell when the lazy
 * grammar has armed, and where the constrained action region begins. */
const char *fg_action_begin(const char *text) {
    for (const char *candidate = strchr(text, '{'); candidate;
         candidate = strchr(candidate + 1, '{')) {
        const char *cursor = candidate + 1;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            cursor++;
        if (*cursor != '"')
            continue;
        cursor++;
        static const char *const keys[] = {"tool", "memory", "final"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(*keys); i++) {
            size_t length = strlen(keys[i]);
            if (strncmp(cursor, keys[i], length) || cursor[length] != '"')
                continue;
            const char *colon = cursor + length + 1;
            while (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')
                colon++;
            if (*colon == ':')
                return candidate;
        }
    }
    return NULL;
}
bool fg_action_complete(const char *text) {
    for (const char *candidate = strchr(text, '{'); candidate;
         candidate = strchr(candidate + 1, '{')) {
        yyjson_doc *doc = yyjson_read(candidate, strlen(candidate), 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        bool complete = yyjson_is_obj(root) &&
                        (yyjson_obj_get(root, "tool") || yyjson_obj_get(root, "memory") ||
                         yyjson_obj_get(root, "final"));
        yyjson_doc_free(doc);
        if (complete)
            return true;
    }
    return false;
}
bool fg_json_whitespace_only(const char *text, size_t length) {
    if (!text || !length)
        return false;
    for (size_t i = 0; i < length; i++)
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            return false;
    return true;
}

static const char *top_level_value(const char *action, const char *key) {
    size_t key_length = strlen(key);
    int depth = 0;
    for (const char *cursor = action; cursor && *cursor;) {
        if (*cursor == '{' || *cursor == '[') {
            depth++;
            cursor++;
            continue;
        }
        if (*cursor == '}' || *cursor == ']') {
            depth--;
            cursor++;
            continue;
        }
        if (*cursor != '"') {
            cursor++;
            continue;
        }
        const char *start = ++cursor;
        bool escaped = false;
        while (*cursor && *cursor != '"') {
            if (*cursor == '\\' && cursor[1]) {
                escaped = true;
                cursor += 2;
            } else
                cursor++;
        }
        if (!*cursor)
            return NULL;
        const char *end = cursor++;
        if (depth != 1 || escaped || (size_t)(end - start) != key_length ||
            strncmp(start, key, key_length))
            continue;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            cursor++;
        if (*cursor != ':')
            continue;
        do {
            cursor++;
        } while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n');
        return cursor;
    }
    return NULL;
}

fg_action_phase fg_action_decode_phase(const char *text) {
    const char *action = text;
    while (action && (*action == ' ' || *action == '\t' || *action == '\r' || *action == '\n'))
        action++;
    if (!action || *action != '{')
        action = text ? fg_action_begin(text) : NULL;
    if (!action)
        return FG_ACTION_SELECT;
    if (top_level_value(action, "final"))
        return FG_ACTION_FINAL;
    if (top_level_value(action, "memory"))
        return FG_ACTION_MEMORY;
    const char *cursor = top_level_value(action, "tool");
    if (!cursor)
        return FG_ACTION_SELECT;
    if (*cursor++ != '"')
        return FG_ACTION_SELECT;
    const char *name = cursor;
    while (*cursor && *cursor != '"')
        cursor++;
    if (*cursor != '"')
        return FG_ACTION_SELECT;
    return (size_t)(cursor - name) == strlen("apply_patch") &&
                   !strncmp(name, "apply_patch", strlen("apply_patch"))
               ? FG_ACTION_PATCH
               : FG_ACTION_ARGUMENTS;
}
void fg_think_bounds(const fg_decode_policy *policy, size_t max_tokens, size_t *min_think,
                     size_t *think_cap) {
    /* The suppress window exists only when a cue is force-decoded: banning the
     * model's action opener without steering text is the measured prompt-echo
     * death configuration. */
    bool cued = !policy->native_thinking && (!policy->cue || policy->cue[0]);
    size_t window = cued ? FG_MIN(FG_THOUGHT_MIN_PREFIX_TOKENS, max_tokens / 4) : 0;
    /* Half the turn budget is a chosen, unmeasured fraction (mirroring the
     * window's quarter); the unbounded arm measures bounded vs unbounded,
     * not the fraction. */
    size_t cap = (policy->native_thinking || policy->think_unbounded) ? SIZE_MAX
                 : policy->think_budget  ? policy->think_budget
                                         : max_tokens / 2;
    *min_think = FG_MIN(window, cap);
    *think_cap = cap;
}
