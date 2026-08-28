#include "internal.h"
#include "forge/state.h"

#define STATE_FIELD_COUNT 5u
#define STATE_MAX_JSON_BYTES (64u * 1024u)
#define STATE_MAX_DETAIL_INPUT (16u * 1024u * 1024u)

static const char *const field_names[STATE_FIELD_COUNT] = {"facts", "hypotheses", "decisions",
                                                           "relevant_files", "remaining"};

typedef struct {
    char *items[STATE_FIELD_COUNT][FORGE_STATE_MAX_ITEMS];
    size_t counts[STATE_FIELD_COUNT], bytes;
} model_fields;

typedef struct {
    char *path;
    uint64_t first_generation, last_generation, observations, last_observation;
} changed_path;

typedef struct {
    uint64_t tool_call_id, generation, ordinal;
    char tool_name[FORGE_STATE_MAX_TOOL_NAME_BYTES + 1];
    char *path;
    forge_status result;
    char detail[FORGE_STATE_MAX_DETAIL_BYTES + 1];
    size_t detail_bytes_omitted;
    bool changed;
} outcome;

struct forge_working_state {
    char *goal, *model_notes;
    size_t notes_bytes;
    model_fields fields;
    uint64_t generation, notes_generation, fields_generation;
    bool has_notes, has_fields, notes_stale, fields_stale;
    changed_path changes[FORGE_STATE_MAX_CHANGED_PATHS];
    size_t change_count;
    outcome recent[FORGE_STATE_MAX_RECENT_OUTCOMES];
    size_t recent_start, recent_count;
    forge_state_validation_status validation;
    uint64_t validation_generation;
    char validation_detail[FORGE_STATE_MAX_DETAIL_BYTES + 1];
    size_t validation_detail_omitted;
    uint64_t observations_recorded, outcomes_evicted, outcome_bytes_omitted;
    uint64_t validation_bytes_omitted, changed_paths_rejected;
    bool evidence_incomplete, counters_saturated;
};

static bool length_within(const char *text, size_t limit, size_t *length) {
    if (!text)
        return false;
    size_t n = 0;
    while (n <= limit && text[n])
        n++;
    if (n > limit)
        return false;
    *length = n;
    return true;
}

static bool valid_utf8(const char *text, size_t length) {
    const unsigned char *s = (const unsigned char *)text;
    for (size_t i = 0; i < length;) {
        unsigned char a = s[i++];
        if (a < 0x80)
            continue;
        size_t extra;
        if (a >= 0xc2 && a <= 0xdf)
            extra = 1;
        else if (a >= 0xe0 && a <= 0xef)
            extra = 2;
        else if (a >= 0xf0 && a <= 0xf4)
            extra = 3;
        else
            return false;
        if (extra > length - i)
            return false;
        unsigned char b = s[i];
        if ((a == 0xe0 && b < 0xa0) || (a == 0xed && b > 0x9f) || (a == 0xf0 && b < 0x90) ||
            (a == 0xf4 && b > 0x8f))
            return false;
        for (size_t j = 0; j < extra; j++)
            if ((s[i + j] & 0xc0) != 0x80)
                return false;
        i += extra;
    }
    return true;
}

static void add_counter(forge_working_state *s, uint64_t *counter, uint64_t amount) {
    if (amount > UINT64_MAX - *counter) {
        *counter = UINT64_MAX;
        s->counters_saturated = true;
    } else
        *counter += amount;
}

static void clear_fields(model_fields *fields) {
    for (size_t i = 0; i < STATE_FIELD_COUNT; i++)
        for (size_t j = 0; j < fields->counts[i]; j++)
            free(fields->items[i][j]);
    memset(fields, 0, sizeof(*fields));
}

static void invalidate_validation(forge_working_state *s, const char *reason) {
    s->validation = FORGE_STATE_UNVERIFIED;
    s->validation_generation = s->generation;
    strcpy(s->validation_detail, reason);
    s->validation_detail_omitted = 0;
}

static void advance_generation(forge_working_state *s, uint64_t generation) {
    if (generation == s->generation)
        return;
    s->generation = generation;
    if (s->has_notes)
        s->notes_stale = true;
    if (s->has_fields)
        s->fields_stale = true;
    invalidate_validation(s, "Repository generation changed; validation is required.");
}

forge_working_state *forge_working_state_create(const char *goal, forge_error *e) {
    size_t length = 0;
    if (!goal || !*goal) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Working state requires a nonempty goal");
        return NULL;
    }
    if (!length_within(goal, FORGE_STATE_MAX_GOAL_BYTES, &length)) {
        fg_error(e, FORGE_ERR_LIMIT, "Working-state goal exceeds 8192 UTF-8 bytes");
        return NULL;
    }
    if (!valid_utf8(goal, length)) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Working-state goal is not valid UTF-8");
        return NULL;
    }
    forge_working_state *s = calloc(1, sizeof(*s));
    if (!s) {
        fg_error(e, FORGE_ERR_MEMORY, "Working-state allocation failed");
        return NULL;
    }
    s->goal = fg_strdup(goal);
    if (!s->goal) {
        free(s);
        fg_error(e, FORGE_ERR_MEMORY, "Working-state goal allocation failed");
        return NULL;
    }
    invalidate_validation(s, "Not yet validated.");
    return s;
}

void forge_working_state_destroy(forge_working_state *s) {
    if (!s)
        return;
    free(s->goal);
    free(s->model_notes);
    clear_fields(&s->fields);
    for (size_t i = 0; i < s->change_count; i++)
        free(s->changes[i].path);
    for (size_t i = 0; i < FORGE_STATE_MAX_RECENT_OUTCOMES; i++)
        free(s->recent[i].path);
    free(s);
}

static forge_status parsed_text(yyjson_val *value, size_t limit, const char **text, size_t *length,
                                forge_error *e) {
    if (!yyjson_is_str(value))
        return fg_error(e, FORGE_ERR_PARSE, "Working-state values must be strings");
    const char *p = yyjson_get_str(value);
    size_t n = yyjson_get_len(value);
    if (memchr(p, 0, n) || !valid_utf8(p, n))
        return fg_error(e, FORGE_ERR_PARSE, "Working-state strings must be UTF-8 without NUL");
    if (n > limit)
        return fg_error(e, FORGE_ERR_LIMIT, "Working-state string exceeds %zu bytes", limit);
    *text = p;
    *length = n;
    return FORGE_OK;
}

forge_status forge_working_state_update_json(forge_working_state *s, const char *json,
                                             forge_error *e) {
    if (!s || !json)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing working state or model update");
    size_t input_length = 0;
    if (!length_within(json, STATE_MAX_JSON_BYTES, &input_length))
        return fg_error(e, FORGE_ERR_LIMIT, "Model memory JSON exceeds 64 KiB");
    yyjson_doc *doc = yyjson_read(json, input_length, 0);
    if (!doc)
        return fg_error(e, FORGE_ERR_PARSE, "Invalid model memory JSON");
    yyjson_val *root = yyjson_doc_get_root(doc);
    forge_status status = FORGE_OK;
    model_fields replacement = {0};
    if (yyjson_is_str(root)) {
        const char *text = NULL;
        size_t length = 0;
        status = parsed_text(root, FORGE_STATE_MAX_MODEL_BYTES, &text, &length, e);
        if (status != FORGE_OK)
            goto finish;
        if (length > FORGE_STATE_MAX_MODEL_BYTES - s->fields.bytes) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Combined model memory exceeds 8192 bytes");
            goto finish;
        }
        char *copy = fg_strdup(text);
        if (!copy) {
            status = fg_error(e, FORGE_ERR_MEMORY, "Model notes allocation failed");
            goto finish;
        }
        free(s->model_notes);
        s->model_notes = copy;
        s->notes_bytes = length;
        s->notes_generation = s->generation;
        s->has_notes = true;
        s->notes_stale = false;
        goto finish;
    }
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != STATE_FIELD_COUNT) {
        status = fg_error(e, FORGE_ERR_PARSE,
                          "Model memory requires exactly facts, hypotheses, decisions, "
                          "relevant_files, and remaining arrays");
        goto finish;
    }
    bool seen[STATE_FIELD_COUNT] = {0};
    size_t index, count;
    yyjson_val *key, *array;
    yyjson_obj_foreach(root, index, count, key, array) {
        const char *name = yyjson_get_str(key);
        size_t field = STATE_FIELD_COUNT;
        if (yyjson_get_len(key) != strlen(name)) {
            status = fg_error(e, FORGE_ERR_PARSE, "Model memory field contains NUL");
            goto finish;
        }
        for (size_t i = 0; i < STATE_FIELD_COUNT; i++)
            if (!strcmp(name, field_names[i]))
                field = i;
        if (field == STATE_FIELD_COUNT || seen[field] || !yyjson_is_arr(array)) {
            status =
                fg_error(e, FORGE_ERR_PARSE, "Unknown, duplicate, or non-array model memory field");
            goto finish;
        }
        seen[field] = true;
        if (yyjson_arr_size(array) > FORGE_STATE_MAX_ITEMS) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Model memory array exceeds 32 items");
            goto finish;
        }
        size_t item_index, item_count;
        yyjson_val *value;
        yyjson_arr_foreach(array, item_index, item_count, value) {
            const char *text = NULL;
            size_t length = 0;
            status = parsed_text(value, FORGE_STATE_MAX_ITEM_BYTES, &text, &length, e);
            if (status != FORGE_OK)
                goto finish;
            if (length > FORGE_STATE_MAX_MODEL_BYTES - s->notes_bytes - replacement.bytes) {
                status = fg_error(e, FORGE_ERR_LIMIT, "Combined model memory exceeds 8192 bytes");
                goto finish;
            }
            char *copy = fg_strdup(text);
            if (!copy) {
                status = fg_error(e, FORGE_ERR_MEMORY, "Model memory allocation failed");
                goto finish;
            }
            replacement.items[field][replacement.counts[field]++] = copy;
            replacement.bytes += length;
        }
    }
    for (size_t i = 0; i < STATE_FIELD_COUNT; i++)
        if (!seen[i]) {
            status = fg_error(e, FORGE_ERR_PARSE, "Missing model memory field");
            goto finish;
        }
    clear_fields(&s->fields);
    s->fields = replacement;
    memset(&replacement, 0, sizeof(replacement));
    s->fields_generation = s->generation;
    s->has_fields = true;
    s->fields_stale = false;
finish:
    clear_fields(&replacement);
    yyjson_doc_free(doc);
    return status;
}

static forge_status detail_prefix(const char *text, char out[FORGE_STATE_MAX_DETAIL_BYTES + 1],
                                  size_t *omitted, forge_error *e) {
    if (!text)
        text = "";
    size_t length = 0;
    if (!length_within(text, STATE_MAX_DETAIL_INPUT, &length))
        return fg_error(e, FORGE_ERR_LIMIT, "Observation detail exceeds 16 MiB");
    if (!valid_utf8(text, length))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Observation detail is not valid UTF-8");
    size_t keep = FG_MIN(length, (size_t)FORGE_STATE_MAX_DETAIL_BYTES);
    while (keep < length && keep && ((unsigned char)text[keep] & 0xc0) == 0x80)
        keep--;
    memcpy(out, text, keep);
    out[keep] = 0;
    *omitted = length - keep;
    return FORGE_OK;
}

static char *normalized_path(const char *path, forge_error *e) {
    size_t length = 0;
    if (!length_within(path, FORGE_STATE_MAX_PATH_BYTES, &length)) {
        fg_error(e, FORGE_ERR_LIMIT, "Observed path exceeds 4095 bytes");
        return NULL;
    }
    if (!length || !valid_utf8(path, length) || path[0] == '/' || path[0] == '\\' ||
        strchr(path, ':')) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Observed path must be relative UTF-8");
        return NULL;
    }
    char *copy = fg_strdup(path);
    if (!copy) {
        fg_error(e, FORGE_ERR_MEMORY, "Observed path allocation failed");
        return NULL;
    }
    for (size_t i = 0; i < length; i++)
        if (copy[i] == '\\')
            copy[i] = '/';
    const char *part = copy;
    for (size_t i = 0; i <= length; i++)
        if (!copy[i] || copy[i] == '/') {
            size_t n = (size_t)(copy + i - part);
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) {
                free(copy);
                fg_error(e, FORGE_ERR_ARGUMENT, "Observed path contains an invalid component");
                return NULL;
            }
            part = copy + i + 1;
        }
    return copy;
}

forge_status forge_working_state_observe(forge_working_state *s,
                                         const forge_state_observation *observation,
                                         forge_error *e) {
    if (!s || !observation || !observation->tool_name || !*observation->tool_name ||
        observation->result < FORGE_OK || observation->result > FORGE_ERR_UNSUPPORTED ||
        (observation->changed && !observation->path))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid working-state observation");
    if (observation->generation < s->generation)
        return fg_error(e, FORGE_ERR_CONFLICT, "Observation belongs to an older generation");
    size_t name_length = 0;
    if (!length_within(observation->tool_name, FORGE_STATE_MAX_TOOL_NAME_BYTES, &name_length))
        return fg_error(e, FORGE_ERR_LIMIT, "Observed tool name exceeds 128 bytes");
    if (!valid_utf8(observation->tool_name, name_length))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Observed tool name is not valid UTF-8");
    outcome next = {0};
    next.tool_call_id = observation->tool_call_id;
    next.generation = observation->generation;
    next.result = observation->result;
    next.changed = observation->changed;
    next.ordinal =
        s->observations_recorded == UINT64_MAX ? UINT64_MAX : s->observations_recorded + 1;
    memcpy(next.tool_name, observation->tool_name, name_length + 1);
    forge_status status =
        detail_prefix(observation->detail, next.detail, &next.detail_bytes_omitted, e);
    if (status != FORGE_OK)
        return status;
    if (observation->path) {
        forge_error path_error = {0};
        next.path = normalized_path(observation->path, &path_error);
        if (!next.path) {
            if (e)
                *e = path_error;
            return path_error.code;
        }
    }
    size_t change_index = s->change_count;
    char *new_changed_path = NULL;
    if (next.changed) {
        for (size_t i = 0; i < s->change_count; i++)
            if (!strcmp(s->changes[i].path, next.path)) {
                change_index = i;
                break;
            }
        if (change_index == s->change_count) {
            if (s->change_count == FORGE_STATE_MAX_CHANGED_PATHS) {
                free(next.path);
                advance_generation(s, next.generation);
                add_counter(s, &s->changed_paths_rejected, 1);
                s->evidence_incomplete = true;
                s->notes_stale = s->has_notes;
                s->fields_stale = s->has_fields;
                invalidate_validation(s, "Changed-file limit exceeded; evidence is incomplete.");
                return fg_error(e, FORGE_ERR_LIMIT,
                                "Working state already records 1024 changed paths");
            }
            new_changed_path = fg_strdup(next.path);
            if (!new_changed_path) {
                free(next.path);
                return fg_error(e, FORGE_ERR_MEMORY, "Changed-file evidence allocation failed");
            }
        }
    }
    advance_generation(s, next.generation);
    if (next.changed) {
        /* A host-reported mutation also invalidates evidence when its repository
         * integration has not yet advanced the generation counter. */
        s->notes_stale = s->has_notes;
        s->fields_stale = s->has_fields;
        invalidate_validation(s, "Repository change observed; validation is required.");
        if (new_changed_path) {
            s->changes[s->change_count++] =
                (changed_path){new_changed_path, next.generation, next.generation, 1, next.ordinal};
        } else {
            s->changes[change_index].last_generation = next.generation;
            s->changes[change_index].last_observation = next.ordinal;
            add_counter(s, &s->changes[change_index].observations, 1);
        }
    }
    size_t slot = (s->recent_start + s->recent_count) % FORGE_STATE_MAX_RECENT_OUTCOMES;
    if (s->recent_count == FORGE_STATE_MAX_RECENT_OUTCOMES) {
        slot = s->recent_start;
        free(s->recent[slot].path);
        s->recent_start = (s->recent_start + 1) % FORGE_STATE_MAX_RECENT_OUTCOMES;
        add_counter(s, &s->outcomes_evicted, 1);
    } else
        s->recent_count++;
    s->recent[slot] = next;
    add_counter(s, &s->observations_recorded, 1);
    add_counter(s, &s->outcome_bytes_omitted, (uint64_t)next.detail_bytes_omitted);
    return FORGE_OK;
}

forge_status forge_working_state_set_validation(forge_working_state *s, uint64_t generation,
                                                forge_state_validation_status status,
                                                const char *detail, forge_error *e) {
    if (!s || status < FORGE_STATE_UNVERIFIED || status > FORGE_STATE_NOT_APPLICABLE)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid working-state validation");
    if (generation < s->generation)
        return fg_error(e, FORGE_ERR_CONFLICT, "Validation belongs to an older generation");
    if (status == FORGE_STATE_PASSED && s->evidence_incomplete)
        return fg_error(e, FORGE_ERR_CONFLICT,
                        "Incomplete observed changes cannot be marked passed");
    char bounded[FORGE_STATE_MAX_DETAIL_BYTES + 1];
    size_t omitted = 0;
    forge_status result = detail_prefix(detail, bounded, &omitted, e);
    if (result != FORGE_OK)
        return result;
    advance_generation(s, generation);
    s->validation = status;
    s->validation_generation = generation;
    memcpy(s->validation_detail, bounded, strlen(bounded) + 1);
    s->validation_detail_omitted = omitted;
    add_counter(s, &s->validation_bytes_omitted, (uint64_t)omitted);
    return FORGE_OK;
}

char *forge_working_state_json(const forge_working_state *s, forge_error *e) {
    if (!s) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing working state");
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        fg_error(e, FORGE_ERR_MEMORY, "State JSON allocation failed");
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root)
        goto memory;
    yyjson_mut_doc_set_root(doc, root);
#define JUINT(obj, key, value)                                                                     \
    do {                                                                                           \
        if (!yyjson_mut_obj_add_uint(doc, obj, key, (uint64_t)(value)))                            \
            goto memory;                                                                           \
    } while (0)
#define JSTR(obj, key, value)                                                                      \
    do {                                                                                           \
        if (!yyjson_mut_obj_add_str(doc, obj, key, value))                                         \
            goto memory;                                                                           \
    } while (0)
#define JBOOL(obj, key, value)                                                                     \
    do {                                                                                           \
        if (!yyjson_mut_obj_add_bool(doc, obj, key, value))                                        \
            goto memory;                                                                           \
    } while (0)
    JUINT(root, "schema_version", FORGE_STATE_SCHEMA_VERSION);
    JSTR(root, "goal", s->goal);
    JUINT(root, "generation", s->generation);
    JSTR(root, "model_notes", s->model_notes ? s->model_notes : "");
    JUINT(root, "model_notes_generation", s->notes_generation);
    JBOOL(root, "model_notes_stale", s->notes_stale);
    JUINT(root, "model_fields_generation", s->fields_generation);
    JBOOL(root, "model_fields_stale", s->fields_stale);
    JBOOL(root, "model_memory_stale", s->notes_stale || s->fields_stale);
    JUINT(root, "model_memory_bytes", s->notes_bytes + s->fields.bytes);
    for (size_t i = 0; i < STATE_FIELD_COUNT; i++) {
        yyjson_mut_val *array = yyjson_mut_arr(doc);
        if (!array || !yyjson_mut_obj_add_val(doc, root, field_names[i], array))
            goto memory;
        for (size_t j = 0; j < s->fields.counts[i]; j++)
            if (!yyjson_mut_arr_add_str(doc, array, s->fields.items[i][j]))
                goto memory;
    }
    yyjson_mut_val *changes = yyjson_mut_arr(doc), *recent = yyjson_mut_arr(doc);
    if (!changes || !recent || !yyjson_mut_obj_add_val(doc, root, "observed_changes", changes) ||
        !yyjson_mut_obj_add_val(doc, root, "recent_outcomes", recent))
        goto memory;
    for (size_t i = 0; i < s->change_count; i++) {
        const changed_path *change = &s->changes[i];
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        if (!obj || !yyjson_mut_arr_add_val(changes, obj))
            goto memory;
        JSTR(obj, "path", change->path);
        JUINT(obj, "first_generation", change->first_generation);
        JUINT(obj, "last_generation", change->last_generation);
        JUINT(obj, "observations", change->observations);
    }
    for (size_t i = 0; i < s->recent_count; i++) {
        const outcome *item = &s->recent[(s->recent_start + i) % FORGE_STATE_MAX_RECENT_OUTCOMES];
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        if (!obj || !yyjson_mut_arr_add_val(recent, obj))
            goto memory;
        JUINT(obj, "tool_call_id", item->tool_call_id);
        JSTR(obj, "tool_name", item->tool_name);
        if (item->path) {
            JSTR(obj, "path", item->path);
        } else if (!yyjson_mut_obj_add_null(doc, obj, "path"))
            goto memory;
        JSTR(obj, "result", forge_status_string(item->result));
        JSTR(obj, "detail", item->detail);
        JUINT(obj, "detail_bytes_omitted", item->detail_bytes_omitted);
        JUINT(obj, "generation", item->generation);
        JBOOL(obj, "changed", item->changed);
    }
    static const char *const statuses[] = {"unverified", "passed", "failed", "denied",
                                           "not_applicable"};
    yyjson_mut_val *validation = yyjson_mut_obj(doc), *overflow = yyjson_mut_obj(doc);
    if (!validation || !overflow || !yyjson_mut_obj_add_val(doc, root, "validation", validation) ||
        !yyjson_mut_obj_add_val(doc, root, "overflow", overflow))
        goto memory;
    JUINT(validation, "generation", s->validation_generation);
    JSTR(validation, "status", statuses[s->validation]);
    JSTR(validation, "detail", s->validation_detail);
    JUINT(validation, "detail_bytes_omitted", s->validation_detail_omitted);
    JUINT(overflow, "recent_outcomes_evicted", s->outcomes_evicted);
    JUINT(overflow, "outcome_detail_bytes_omitted", s->outcome_bytes_omitted);
    JUINT(overflow, "validation_detail_bytes_omitted", s->validation_bytes_omitted);
    JUINT(overflow, "changed_paths_rejected", s->changed_paths_rejected);
    JBOOL(overflow, "counters_saturated", s->counters_saturated);
    JBOOL(root, "evidence_incomplete", s->evidence_incomplete);
    JUINT(root, "observations_recorded", s->observations_recorded);
#undef JUINT
#undef JSTR
#undef JBOOL
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        fg_error(e, FORGE_ERR_MEMORY, "State JSON serialization failed");
    return json;
memory:
    yyjson_mut_doc_free(doc);
    fg_error(e, FORGE_ERR_MEMORY, "State JSON allocation failed");
    return NULL;
}

typedef struct {
    yyjson_val *value;
    uint64_t ordinal;
    size_t index;
    bool changed;
} context_candidate;

static int candidate_order(const void *left, const void *right) {
    const context_candidate *a = left, *b = right;
    if (a->ordinal != b->ordinal)
        return a->ordinal > b->ordinal ? -1 : 1;
    if (a->changed != b->changed)
        return a->changed ? -1 : 1;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

static size_t decimal_digits(size_t number) {
    size_t count = 1;
    while (number >= 10) {
        number /= 10;
        count++;
    }
    return count;
}

static char *state_context_json(const forge_working_state *s, size_t max_bytes, bool core_only,
                                forge_error *e) {
    if (!s) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing working state");
        return NULL;
    }
    /* Parse one immutable audit snapshot. Only selected evidence is copied into
     * the prompt document; the bounded full snapshot remains unchanged. */
    char *full = forge_working_state_json(s, e);
    if (!full)
        return NULL;
    yyjson_doc *source = yyjson_read(full, strlen(full), 0);
    free(full);
    yyjson_mut_doc *view = NULL;
    char *result = NULL;
    context_candidate *candidates = NULL;
    if (!source)
        goto memory;
    yyjson_val *source_root = yyjson_doc_get_root(source);
    view = yyjson_mut_doc_new(NULL);
    if (!view)
        goto memory;
    yyjson_mut_val *root = yyjson_mut_obj(view);
    if (!root)
        goto memory;
    yyjson_mut_doc_set_root(view, root);
    size_t index, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(source_root, index, count, key, value) {
        const char *name = yyjson_get_str(key);
        if (!strcmp(name, "observed_changes") || !strcmp(name, "recent_outcomes"))
            continue;
        yyjson_mut_val *copy = yyjson_val_mut_copy(view, value);
        if (!copy || !yyjson_mut_obj_add_val(view, root, name, copy))
            goto memory;
    }
    yyjson_mut_val *changes = yyjson_mut_arr(view), *recent = yyjson_mut_arr(view),
                   *omitted = yyjson_mut_obj(view);
    if (!changes || !recent || !omitted ||
        !yyjson_mut_obj_add_val(view, root, "observed_changes", changes) ||
        !yyjson_mut_obj_add_val(view, root, "recent_outcomes", recent) ||
        !yyjson_mut_obj_add_val(view, root, "context_omitted", omitted) ||
        !yyjson_mut_obj_add_uint(view, omitted, "observed_changes", (uint64_t)s->change_count) ||
        !yyjson_mut_obj_add_uint(view, omitted, "recent_outcomes", (uint64_t)s->recent_count) ||
        !yyjson_mut_obj_add_str(view, root, "full_state_artifact", "working_state.json"))
        goto memory;
    result = yyjson_mut_write(view, 0, NULL);
    if (!result)
        goto memory;
    size_t used = strlen(result);
    if (core_only)
        goto finish;
    free(result);
    result = NULL;
    if (used > max_bytes) {
        fg_error(e, FORGE_ERR_LIMIT,
                 "Goal, model memory, and validation exceed context byte budget");
        goto finish;
    }
    size_t candidate_count = s->change_count + s->recent_count;
    candidates = calloc(candidate_count ? candidate_count : 1, sizeof(*candidates));
    if (!candidates)
        goto memory;
    yyjson_val *all_changes = yyjson_obj_get(source_root, "observed_changes"),
               *all_recent = yyjson_obj_get(source_root, "recent_outcomes");
    for (size_t i = 0; i < s->change_count; i++)
        candidates[i] = (context_candidate){yyjson_arr_get(all_changes, i),
                                            s->changes[i].last_observation, i, true};
    for (size_t i = 0; i < s->recent_count; i++)
        candidates[s->change_count + i] = (context_candidate){
            yyjson_arr_get(all_recent, i),
            s->recent[(s->recent_start + i) % FORGE_STATE_MAX_RECENT_OUTCOMES].ordinal, i, false};
    qsort(candidates, candidate_count, sizeof(*candidates), candidate_order);
    size_t changes_kept = 0, recent_kept = 0;
    for (size_t i = 0; i < candidate_count; i++) {
        context_candidate *candidate = &candidates[i];
        char *encoded = yyjson_val_write(candidate->value, 0, NULL);
        if (!encoded)
            goto memory;
        size_t encoded_bytes = strlen(encoded);
        free(encoded);
        size_t kept = candidate->changed ? changes_kept : recent_kept;
        size_t total = candidate->changed ? s->change_count : s->recent_count;
        size_t previous_omitted = total - kept;
        size_t saved_digits =
            decimal_digits(previous_omitted) - decimal_digits(previous_omitted - 1);
        size_t extra = encoded_bytes + (kept ? 1u : 0u) - saved_digits;
        if (extra > max_bytes - used)
            continue;
        yyjson_mut_val *copy = yyjson_val_mut_copy(view, candidate->value);
        if (!copy || !yyjson_mut_arr_add_val(candidate->changed ? changes : recent, copy))
            goto memory;
        if (candidate->changed)
            changes_kept++;
        else
            recent_kept++;
        used += extra;
    }
    if (!yyjson_mut_set_uint(yyjson_mut_obj_get(omitted, "observed_changes"),
                             (uint64_t)(s->change_count - changes_kept)) ||
        !yyjson_mut_set_uint(yyjson_mut_obj_get(omitted, "recent_outcomes"),
                             (uint64_t)(s->recent_count - recent_kept)))
        goto memory;
    result = yyjson_mut_write(view, 0, NULL);
    if (!result)
        goto memory;
    if (strlen(result) > max_bytes) {
        free(result);
        result = NULL;
        fg_error(e, FORGE_ERR_LIMIT, "Working-state prompt view exceeds byte budget");
    }
    goto finish;
memory:
    free(result);
    result = NULL;
    fg_error(e, FORGE_ERR_MEMORY, "Working-state context allocation failed");
finish:
    free(candidates);
    yyjson_mut_doc_free(view);
    yyjson_doc_free(source);
    return result;
}

char *forge_working_state_context_json(const forge_working_state *s, size_t max_bytes,
                                       forge_error *e) {
    return state_context_json(s, max_bytes, false, e);
}

char *forge_working_state_context_core_json(const forge_working_state *s, forge_error *e) {
    return state_context_json(s, 0, true, e);
}
