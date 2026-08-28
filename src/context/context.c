#include "internal.h"
#define CTX_MAX_SEGMENTS 4096u
#define CTX_MAX_DEPENDENCIES 65536u
#define CTX_MAX_PARENTS 256u
#define CTX_OVERHEAD 16u

typedef struct {
    forge_segment_view view;
    char *owned;
    size_t *dependencies;
    size_t dependency_count, dependency_capacity;
} segment;
struct forge_context {
    segment *items;
    size_t count, capacity, reserve, allocated;
    size_t text_bytes, dependency_count, planned_tokens, planned_evicted;
    bool planned;
    uint64_t next_id;
    forge_count_tokens_fn count_tokens;
    void *user;
};
static const char *const labels[] = {"SYSTEM",        "TOOLS",  "REPOSITORY", "TASK",
                                     "WORKING_STATE", "SOURCE", "ACTION",     "TOOL_RESULT"};
static size_t segment_index(const forge_context *c, uint64_t id) {
    if (!c || !id)
        return SIZE_MAX;
    size_t lo = 0, hi = c->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->items[mid].view.id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < c->count && c->items[lo].view.id == id ? lo : SIZE_MAX;
}
static void clear_selection(forge_context *c) {
    c->planned = false;
    c->planned_tokens = c->planned_evicted = 0;
    for (size_t i = 0; i < c->count; i++)
        c->items[i].view.selected = false;
}
static bool text_cost(forge_context *c, const char *text, size_t *length, size_t *tokens) {
    size_t n = 0;
    while (n <= FG_MAX_JSON && text[n])
        n++;
    if (n > FG_MAX_JSON)
        return false;
    size_t cost = c->count_tokens(text, c->user);
    if (cost > SIZE_MAX - CTX_OVERHEAD)
        return false;
    *length = n;
    *tokens = cost + CTX_OVERHEAD;
    return true;
}
static void mark_stale(segment *s) {
    s->view.stale = true;
    s->view.pinned = false;
    s->view.selected = false;
}
static void propagate_stale(forge_context *c) {
    /* Every edge points to an older segment, so one ascending pass reaches
     * the full transitive closure without recursion or repeated scans. */
    for (size_t i = 0; i < c->count; i++) {
        segment *s = &c->items[i];
        for (size_t j = 0; j < s->dependency_count; j++)
            if (c->items[s->dependencies[j]].view.stale) {
                mark_stale(s);
                break;
            }
    }
}
static void invalidate_dependents(forge_context *c, size_t dependency) {
    for (size_t i = dependency + 1; i < c->count; i++) {
        segment *s = &c->items[i];
        for (size_t j = 0; j < s->dependency_count; j++)
            if (s->dependencies[j] == dependency || c->items[s->dependencies[j]].view.stale) {
                mark_stale(s);
                break;
            }
    }
}
forge_context *forge_context_create(size_t capacity, size_t reserve, forge_count_tokens_fn fn,
                                    void *u) {
    if (!fn || !capacity || reserve >= capacity)
        return NULL;
    forge_context *c = calloc(1, sizeof(*c));
    if (c) {
        c->capacity = capacity;
        c->reserve = reserve;
        c->count_tokens = fn;
        c->user = u;
        c->next_id = 1;
    }
    return c;
}
uint64_t forge_context_add(forge_context *c, forge_segment_kind kind, const char *text,
                           int priority, bool pinned, uint64_t dependency, uint64_t generation) {
    if (!c || !text || kind < FORGE_SEG_SYSTEM || kind > FORGE_SEG_RESULT ||
        c->count >= CTX_MAX_SEGMENTS || c->next_id == UINT64_MAX)
        return 0;
    size_t parent = dependency ? segment_index(c, dependency) : SIZE_MAX;
    if (dependency && (parent == SIZE_MAX || c->dependency_count == CTX_MAX_DEPENDENCIES))
        return 0;
    size_t length, tokens;
    if (!text_cost(c, text, &length, &tokens) || length > FG_MAX_JSON - c->text_bytes)
        return 0;
    if (c->count == c->allocated) {
        size_t n = c->allocated ? c->allocated * 2 : 32;
        segment *p = realloc(c->items, n * sizeof(*p));
        if (!p)
            return 0;
        c->items = p;
        c->allocated = n;
    }
    char *copy = fg_strdup(text);
    if (!copy)
        return 0;
    size_t *parents = NULL;
    if (dependency) {
        parents = malloc(sizeof(*parents));
        if (!parents) {
            free(copy);
            return 0;
        }
        parents[0] = parent;
    }
    clear_selection(c);
    segment *s = &c->items[c->count++];
    memset(s, 0, sizeof(*s));
    s->owned = copy;
    s->view.id = c->next_id++;
    s->view.content_hash = fg_hash(text, length);
    s->view.version = 1;
    s->view.generation = generation;
    s->view.dependency = dependency;
    s->view.kind = kind;
    s->view.tokens = tokens;
    s->view.priority = priority;
    s->view.pinned = pinned;
    s->view.text = copy;
    s->dependencies = parents;
    s->dependency_count = s->dependency_capacity = dependency ? 1u : 0u;
    s->view.dependency_count = s->dependency_count;
    c->text_bytes += length;
    c->dependency_count += s->dependency_count;
    if (dependency && c->items[parent].view.stale)
        mark_stale(s);
    return s->view.id;
}
forge_status forge_context_update(forge_context *c, uint64_t id, const char *text,
                                  uint64_t generation) {
    if (!c || !text)
        return FORGE_ERR_ARGUMENT;
    size_t index = segment_index(c, id);
    if (index == SIZE_MAX)
        return FORGE_ERR_NOT_FOUND;
    segment *s = &c->items[index];
    bool same = !strcmp(s->owned, text);
    if (s->view.immutable)
        return same ? FORGE_OK : FORGE_ERR_POLICY;
    for (size_t i = 0; i < s->dependency_count; i++)
        if (c->items[s->dependencies[i]].view.stale)
            return FORGE_ERR_CONFLICT;
    if (same) {
        if (s->view.generation != generation || s->view.stale) {
            clear_selection(c);
            s->view.generation = generation;
            s->view.stale = false;
        }
        return FORGE_OK;
    }
    size_t length, tokens, old_length = strlen(s->owned);
    if (s->view.version == UINT64_MAX || !text_cost(c, text, &length, &tokens) ||
        length > FG_MAX_JSON - (c->text_bytes - old_length))
        return FORGE_ERR_LIMIT;
    char *copy = fg_strdup(text);
    if (!copy)
        return FORGE_ERR_MEMORY;
    clear_selection(c);
    free(s->owned);
    s->owned = copy;
    s->view.text = copy;
    s->view.content_hash = fg_hash(text, length);
    s->view.version++;
    s->view.generation = generation;
    s->view.tokens = tokens;
    s->view.stale = false;
    c->text_bytes = c->text_bytes - old_length + length;
    invalidate_dependents(c, index);
    return FORGE_OK;
}
forge_status forge_context_add_dependency(forge_context *c, uint64_t id, uint64_t dependency) {
    if (!c || !id || !dependency)
        return FORGE_ERR_ARGUMENT;
    size_t index = segment_index(c, id), parent = segment_index(c, dependency);
    if (index == SIZE_MAX || parent == SIZE_MAX)
        return FORGE_ERR_NOT_FOUND;
    if (parent >= index)
        return FORGE_ERR_CONFLICT;
    segment *s = &c->items[index];
    size_t position = 0;
    while (position < s->dependency_count && s->dependencies[position] < parent)
        position++;
    if (position < s->dependency_count && s->dependencies[position] == parent)
        return FORGE_OK;
    if (s->view.immutable)
        return FORGE_ERR_POLICY;
    if (s->dependency_count == CTX_MAX_PARENTS || c->dependency_count == CTX_MAX_DEPENDENCIES)
        return FORGE_ERR_LIMIT;
    if (s->dependency_count == s->dependency_capacity) {
        size_t cap = s->dependency_capacity ? s->dependency_capacity * 2 : 4;
        cap = FG_MIN(cap, CTX_MAX_PARENTS);
        size_t *parents = realloc(s->dependencies, cap * sizeof(*parents));
        if (!parents)
            return FORGE_ERR_MEMORY;
        s->dependencies = parents;
        s->dependency_capacity = cap;
    }
    clear_selection(c);
    memmove(s->dependencies + position + 1, s->dependencies + position,
            (s->dependency_count - position) * sizeof(*s->dependencies));
    s->dependencies[position] = parent;
    s->dependency_count++;
    c->dependency_count++;
    s->view.dependency_count = s->dependency_count;
    s->view.dependency = c->items[s->dependencies[0]].view.id;
    if (c->items[parent].view.stale) {
        mark_stale(s);
        propagate_stale(c);
    }
    return FORGE_OK;
}
size_t forge_context_dependency_count(const forge_context *c, uint64_t id) {
    size_t index = segment_index(c, id);
    return index == SIZE_MAX ? 0 : c->items[index].dependency_count;
}
bool forge_context_get_dependency(const forge_context *c, uint64_t id, size_t index,
                                  uint64_t *dependency) {
    size_t item = segment_index(c, id);
    if (!dependency || item == SIZE_MAX || index >= c->items[item].dependency_count)
        return false;
    *dependency = c->items[c->items[item].dependencies[index]].view.id;
    return true;
}
forge_status forge_context_set_flags(forge_context *c, uint64_t id, bool immutable,
                                     bool cacheable) {
    if (!c)
        return FORGE_ERR_ARGUMENT;
    size_t index = segment_index(c, id);
    if (index == SIZE_MAX)
        return FORGE_ERR_NOT_FOUND;
    segment *s = &c->items[index];
    if (s->view.immutable && (!immutable || s->view.cacheable != cacheable))
        return FORGE_ERR_POLICY;
    s->view.immutable = immutable;
    s->view.cacheable = cacheable;
    return FORGE_OK;
}
void forge_context_invalidate(forge_context *c, uint64_t dependency, uint64_t generation) {
    if (!c)
        return;
    bool invalidated = false;
    for (size_t i = 0; i < c->count; i++) {
        segment *s = &c->items[i];
        if ((!dependency || s->view.source_hash == dependency ||
             s->view.source_hash == UINT64_MAX) &&
            s->view.source_hash && s->view.generation < generation) {
            mark_stale(s);
            invalidated = true;
        }
    }
    if (invalidated) {
        clear_selection(c);
        propagate_stale(c);
    }
}
void forge_context_bind_source(forge_context *c, uint64_t id, uint64_t source) {
    size_t index = segment_index(c, id);
    if (index != SIZE_MAX && !c->items[index].view.immutable)
        c->items[index].view.source_hash = source;
}
void forge_context_pin(forge_context *c, uint64_t id, bool pinned) {
    size_t index = segment_index(c, id);
    if (index != SIZE_MAX && !c->items[index].view.stale && c->items[index].view.pinned != pinned) {
        clear_selection(c);
        c->items[index].view.pinned = pinned;
    }
}
typedef struct {
    size_t *stack, *nodes, count;
    uint32_t *seen, epoch;
} closure;
static forge_status collect_bundle(forge_context *c, size_t index, closure *work, size_t *cost) {
    work->epoch++;
    work->count = 0;
    size_t pending = 0;
    work->stack[pending++] = index;
    work->seen[index] = work->epoch;
    *cost = 0;
    while (pending) {
        size_t i = work->stack[--pending];
        segment *s = &c->items[i];
        if (s->view.stale)
            return FORGE_ERR_CONFLICT;
        if (s->view.selected)
            continue; /* Selected bundles already include their full closure. */
        if (s->view.tokens > SIZE_MAX - *cost)
            return FORGE_ERR_LIMIT;
        *cost += s->view.tokens;
        work->nodes[work->count++] = i;
        for (size_t j = 0; j < s->dependency_count; j++) {
            size_t parent = s->dependencies[j];
            if (work->seen[parent] != work->epoch) {
                work->seen[parent] = work->epoch;
                work->stack[pending++] = parent;
            }
        }
    }
    return FORGE_OK;
}
static void select_bundle(forge_context *c, const closure *work) {
    for (size_t i = 0; i < work->count; i++)
        c->items[work->nodes[i]].view.selected = true;
}
static char *render_selected_anchor(const forge_context *c, size_t *anchor) {
    fg_buf b = {0};
    bool eligible = true;
    if (anchor)
        *anchor = 0;
    /* Stable roles, then chronological source/action/result history, then
     * volatile working state. Updating MEMORY leaves the history prefix intact. */
    for (int group = 0; group <= 5; group++)
        for (size_t i = 0; i < c->count; i++) {
            const segment *s = &c->items[i];
            int rank = s->view.kind <= FORGE_SEG_TASK     ? (int)s->view.kind
                       : s->view.kind == FORGE_SEG_MEMORY ? 5
                                                          : 4;
            if (rank == group && s->view.selected) {
                if (!fg_buf_printf(&b, "\n[%s]\n%s\n", labels[s->view.kind], s->owned)) {
                    fg_buf_clear(&b);
                    return NULL;
                }
                if (anchor && group <= 1) {
                    eligible &= s->view.immutable && s->view.cacheable && !s->view.stale;
                    /* A prefix depending on non-prefix context is not one of
                     * this first cache slice's self-contained stable anchors. */
                    for (size_t j = 0; j < s->dependency_count; j++) {
                        const forge_segment_view *parent = &c->items[s->dependencies[j]].view;
                        eligible &= parent->kind <= FORGE_SEG_TOOLS && parent->selected &&
                                    parent->immutable && parent->cacheable && !parent->stale;
                    }
                    if (eligible)
                        *anchor = b.len;
                }
            }
        }
    if (anchor && !eligible)
        *anchor = 0;
    return fg_buf_take(&b);
}
static char *render_selected(const forge_context *c) {
    return render_selected_anchor(c, NULL);
}

forge_status forge_context_cache_anchor(const forge_context *c, const char *prompt,
                                        size_t *byte_end, forge_error *error) {
    if (error)
        memset(error, 0, sizeof(*error));
    if (byte_end)
        *byte_end = 0;
    if (!c || !prompt || !byte_end)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing context/cache anchor input");
    if (!c->planned)
        return fg_error(error, FORGE_ERR_CONFLICT, "Checkpoint anchor requires a current plan");
    size_t length = 0;
    while (length <= FG_MAX_JSON && prompt[length])
        length++;
    if (length > FG_MAX_JSON || !fg_utf8_valid(prompt, length))
        return fg_error(error, FORGE_ERR_ARGUMENT, "Checkpoint prompt is not bounded UTF-8");
    size_t anchor = 0;
    char *rendered = render_selected_anchor(c, &anchor);
    if (!rendered)
        return fg_error(error, FORGE_ERR_MEMORY, "Cannot verify checkpoint plan rendering");
    bool same = strlen(rendered) == length && !memcmp(rendered, prompt, length);
    free(rendered);
    if (!same)
        return fg_error(error, FORGE_ERR_CONFLICT, "Checkpoint prompt differs from selected plan");
    *byte_end = anchor;
    return FORGE_OK;
}

char *forge_context_plan(forge_context *c, size_t *tokens, size_t *evicted, forge_error *e) {
    if (tokens)
        *tokens = 0;
    if (evicted)
        *evicted = 0;
    if (!c) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing context");
        return NULL;
    }
    size_t budget = c->capacity - c->reserve, used = 0;
    clear_selection(c);
    size_t n = c->count ? c->count : 1;
    closure work = {0};
    work.stack = malloc(n * sizeof(*work.stack));
    work.nodes = malloc(n * sizeof(*work.nodes));
    work.seen = calloc(n, sizeof(*work.seen));
    bool *considered = calloc(n, sizeof(*considered));
    char *out = NULL;
    if (!work.stack || !work.nodes || !work.seen || !considered) {
        fg_error(e, FORGE_ERR_MEMORY, "Context closure allocation failed");
        goto finish;
    }
    for (size_t i = 0; i < c->count; i++)
        if (c->items[i].view.pinned) {
            size_t cost;
            forge_status status = collect_bundle(c, i, &work, &cost);
            if (status == FORGE_ERR_CONFLICT) {
                fg_error(e, status, "Pinned context depends on stale context");
                goto finish;
            }
            if (status != FORGE_OK || cost > budget - used) {
                fg_error(e, FORGE_ERR_LIMIT, "Pinned context exceeds the input budget (%zu tokens)",
                         budget);
                goto finish;
            }
            used += cost;
            select_bundle(c, &work);
        }
    for (size_t pass = 0; pass < c->count; pass++) {
        size_t best = SIZE_MAX;
        double score = -1;
        for (size_t i = 0; i < c->count; i++)
            if (!considered[i] && !c->items[i].view.stale && !c->items[i].view.selected) {
                double candidate = ((double)c->items[i].view.priority + 1.0) *
                                   (1.0 + (double)i / (double)(c->count + 1)) /
                                   (double)FG_MAX(c->items[i].view.tokens, 1);
                if (best == SIZE_MAX || candidate > score || (candidate == score && i > best)) {
                    score = candidate;
                    best = i;
                }
            }
        if (best == SIZE_MAX)
            break;
        considered[best] = true;
        size_t cost;
        if (collect_bundle(c, best, &work, &cost) == FORGE_OK && cost <= budget - used) {
            used += cost;
            select_bundle(c, &work);
        }
    }
    size_t dropped = 0;
    for (size_t i = 0; i < c->count; i++)
        if (!c->items[i].view.selected && !c->items[i].view.stale)
            dropped++;
    out = render_selected(c);
    if (!out) {
        fg_error(e, FORGE_ERR_MEMORY, "Prompt allocation failed");
        goto finish;
    }
    size_t actual = c->count_tokens(out, c->user);
    if (actual > budget) {
        free(out);
        out = NULL;
        fg_error(e, FORGE_ERR_LIMIT, "Rendered prompt exceeds context budget");
        goto finish;
    }
    if (tokens)
        *tokens = actual;
    if (evicted)
        *evicted = dropped;
    c->planned = true;
    c->planned_tokens = actual;
    c->planned_evicted = dropped;
finish:
    free(work.stack);
    free(work.nodes);
    free(work.seen);
    free(considered);
    if (!out)
        clear_selection(c);
    return out;
}
size_t forge_context_size(const forge_context *c) {
    return c ? c->count : 0;
}
bool forge_context_get(const forge_context *c, size_t i, forge_segment_view *v) {
    if (!c || i >= c->count || !v)
        return false;
    *v = c->items[i].view;
    return true;
}
char *forge_context_export(const forge_context *c, forge_error *e) {
    if (!c) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing context for export");
        return NULL;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        fg_error(e, FORGE_ERR_MEMORY, "Context snapshot allocation failed");
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc), *items = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = root && items && yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
              yyjson_mut_obj_add_uint(doc, root, "capacity", c->capacity) &&
              yyjson_mut_obj_add_uint(doc, root, "reserve", c->reserve) &&
              yyjson_mut_obj_add_uint(doc, root, "next_id", c->next_id) &&
              yyjson_mut_obj_add_bool(doc, root, "planned", c->planned) &&
              yyjson_mut_obj_add_uint(doc, root, "planned_tokens", c->planned_tokens) &&
              yyjson_mut_obj_add_uint(doc, root, "planned_evicted", c->planned_evicted) &&
              yyjson_mut_obj_add_val(doc, root, "segments", items);
    for (size_t i = 0; ok && i < c->count; i++) {
        const segment *s = &c->items[i];
        const forge_segment_view *v = &s->view;
        yyjson_mut_val *item = yyjson_mut_obj(doc), *dependencies = yyjson_mut_arr(doc);
        ok = item && dependencies && yyjson_mut_obj_add_uint(doc, item, "id", v->id) &&
             yyjson_mut_obj_add_uint(doc, item, "content_hash", v->content_hash) &&
             yyjson_mut_obj_add_uint(doc, item, "version", v->version) &&
             yyjson_mut_obj_add_uint(doc, item, "generation", v->generation) &&
             yyjson_mut_obj_add_uint(doc, item, "kind", (uint64_t)v->kind) &&
             yyjson_mut_obj_add_uint(doc, item, "tokens", v->tokens) &&
             yyjson_mut_obj_add_int(doc, item, "priority", v->priority) &&
             yyjson_mut_obj_add_bool(doc, item, "pinned", v->pinned) &&
             yyjson_mut_obj_add_bool(doc, item, "selected", v->selected) &&
             yyjson_mut_obj_add_bool(doc, item, "immutable", v->immutable) &&
             yyjson_mut_obj_add_bool(doc, item, "cacheable", v->cacheable) &&
             yyjson_mut_obj_add_bool(doc, item, "stale", v->stale) &&
             yyjson_mut_obj_add_uint(doc, item, "source_hash", v->source_hash) &&
             yyjson_mut_obj_add_val(doc, item, "dependencies", dependencies) &&
             yyjson_mut_obj_add_strcpy(doc, item, "text", s->owned) &&
             yyjson_mut_arr_append(items, item);
        for (size_t j = 0; ok && j < s->dependency_count; j++)
            ok = yyjson_mut_arr_add_uint(doc, dependencies, c->items[s->dependencies[j]].view.id);
    }
    size_t length = 0;
    char *json = ok ? yyjson_mut_write(doc, 0, &length) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json)
        fg_error(e, FORGE_ERR_MEMORY, "Cannot serialize context snapshot");
    else if (length > FG_MAX_JSON) {
        free(json);
        json = NULL;
        fg_error(e, FORGE_ERR_LIMIT, "Context snapshot exceeds 16 MiB");
    }
    return json;
}
static bool snapshot_fields(yyjson_val *object, const char *const *names, size_t count) {
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != count)
        return false;
    for (size_t i = 0; i < count; i++)
        if (!yyjson_obj_get(object, names[i]))
            return false;
    return true; /* Exact key count + presence also rejects duplicate/unknown keys. */
}
static bool snapshot_uint(yyjson_val *object, const char *key, uint64_t *value) {
    yyjson_val *v = yyjson_obj_get(object, key);
    if (!yyjson_is_uint(v))
        return false;
    *value = yyjson_get_uint(v);
    return true;
}
static bool snapshot_priority(yyjson_val *v, int *value) {
    if (yyjson_is_uint(v)) {
        uint64_t n = yyjson_get_uint(v);
        if (n > INT_MAX)
            return false;
        *value = (int)n;
        return true;
    }
    if (yyjson_is_sint(v)) {
        int64_t n = yyjson_get_sint(v);
        if (n < INT_MIN || n > INT_MAX)
            return false;
        *value = (int)n;
        return true;
    }
    return false;
}
forge_context *forge_context_import(const char *json, forge_count_tokens_fn fn, void *user,
                                    forge_error *e) {
    if (e) {
        e->code = FORGE_OK;
        e->message[0] = 0;
    }
    if (!json || !fn) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Context import requires JSON and a token counter");
        return NULL;
    }
    size_t length = 0;
    while (length <= FG_MAX_JSON && json[length])
        length++;
    if (length > FG_MAX_JSON) {
        fg_error(e, FORGE_ERR_LIMIT, "Context snapshot exceeds 16 MiB");
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(json, length, 0);
    forge_context *c = NULL;
    bool *selection = NULL;
    char *rendered = NULL;
    if (!doc)
        goto invalid;
    yyjson_val *root = yyjson_doc_get_root(doc);
    static const char *const root_fields[] = {"schema_version",  "capacity", "reserve",
                                              "next_id",         "planned",  "planned_tokens",
                                              "planned_evicted", "segments"};
    static const char *const item_fields[] = {
        "id",        "content_hash", "version",     "generation",   "kind",
        "tokens",    "priority",     "pinned",      "selected",     "immutable",
        "cacheable", "stale",        "source_hash", "dependencies", "text"};
    static const char *const bool_fields[] = {"pinned", "selected", "immutable", "cacheable",
                                              "stale"};
    uint64_t schema, capacity, reserve, next_id, planned_tokens, planned_evicted;
    if (!snapshot_fields(root, root_fields, sizeof(root_fields) / sizeof(root_fields[0])) ||
        !snapshot_uint(root, "schema_version", &schema) || schema != 1 ||
        !snapshot_uint(root, "capacity", &capacity) || !capacity || capacity > SIZE_MAX ||
        !snapshot_uint(root, "reserve", &reserve) || reserve >= capacity ||
        !snapshot_uint(root, "next_id", &next_id) || !next_id ||
        !snapshot_uint(root, "planned_tokens", &planned_tokens) || planned_tokens > SIZE_MAX ||
        !snapshot_uint(root, "planned_evicted", &planned_evicted) || planned_evicted > SIZE_MAX ||
        !yyjson_is_bool(yyjson_obj_get(root, "planned")))
        goto invalid;
    bool planned = yyjson_get_bool(yyjson_obj_get(root, "planned"));
    yyjson_val *items = yyjson_obj_get(root, "segments");
    if (!yyjson_is_arr(items))
        goto invalid;
    size_t count = yyjson_arr_size(items);
    if (count > CTX_MAX_SEGMENTS)
        goto limit;
    c = forge_context_create((size_t)capacity, (size_t)reserve, fn, user);
    selection = calloc(count ? count : 1, sizeof(*selection));
    if (!c || !selection)
        goto memory;
    uint64_t previous_id = 0;
    for (size_t i = 0; i < count; i++) {
        yyjson_val *item = yyjson_arr_get(items, i);
        uint64_t id, hash, version, generation, kind, tokens, source;
        int priority;
        if (!snapshot_fields(item, item_fields, sizeof(item_fields) / sizeof(item_fields[0])) ||
            !snapshot_uint(item, "id", &id) || id <= previous_id || id >= next_id ||
            !snapshot_uint(item, "content_hash", &hash) ||
            !snapshot_uint(item, "version", &version) || !version ||
            !snapshot_uint(item, "generation", &generation) ||
            !snapshot_uint(item, "kind", &kind) || kind > FORGE_SEG_RESULT ||
            !snapshot_uint(item, "tokens", &tokens) || tokens > SIZE_MAX ||
            !snapshot_uint(item, "source_hash", &source) ||
            !snapshot_priority(yyjson_obj_get(item, "priority"), &priority))
            goto invalid;
        for (size_t j = 0; j < sizeof(bool_fields) / sizeof(bool_fields[0]); j++)
            if (!yyjson_is_bool(yyjson_obj_get(item, bool_fields[j])))
                goto invalid;
        yyjson_val *text_value = yyjson_obj_get(item, "text");
        const char *text = yyjson_get_str(text_value);
        size_t text_length = yyjson_get_len(text_value);
        if (!text || memchr(text, 0, text_length) || fg_hash(text, text_length) != hash)
            goto invalid;
        yyjson_val *parents = yyjson_obj_get(item, "dependencies");
        if (!yyjson_is_arr(parents))
            goto invalid;
        size_t parent_count = yyjson_arr_size(parents);
        if (parent_count > CTX_MAX_PARENTS ||
            parent_count > CTX_MAX_DEPENDENCIES - c->dependency_count ||
            text_length > FG_MAX_JSON - c->text_bytes)
            goto limit;
        bool stale = yyjson_get_bool(yyjson_obj_get(item, "stale"));
        bool pinned = yyjson_get_bool(yyjson_obj_get(item, "pinned"));
        selection[i] = yyjson_get_bool(yyjson_obj_get(item, "selected"));
        if ((stale && (pinned || selection[i])) || (!planned && selection[i]))
            goto invalid;
        size_t measured_length, measured_tokens;
        if (!text_cost(c, text, &measured_length, &measured_tokens))
            goto limit;
        if (measured_length != text_length || measured_tokens != (size_t)tokens)
            goto tokenizer;
        c->next_id = id;
        if (!forge_context_add(c, (forge_segment_kind)kind, text, priority, pinned, 0, generation))
            goto memory;
        if (c->items[i].view.tokens != (size_t)tokens)
            goto tokenizer;
        uint64_t previous_parent = 0;
        for (size_t j = 0; j < parent_count; j++) {
            yyjson_val *parent = yyjson_arr_get(parents, j);
            if (!yyjson_is_uint(parent))
                goto invalid;
            uint64_t dependency = yyjson_get_uint(parent);
            if (dependency <= previous_parent || dependency >= id)
                goto invalid;
            forge_status status = forge_context_add_dependency(c, id, dependency);
            if (status == FORGE_ERR_MEMORY)
                goto memory;
            if (status == FORGE_ERR_LIMIT)
                goto limit;
            if (status != FORGE_OK)
                goto invalid;
            previous_parent = dependency;
        }
        segment *s = &c->items[i];
        if (s->view.stale && !stale)
            goto invalid; /* A snapshot must preserve transitive invalidation. */
        s->view.version = version;
        s->view.source_hash = source;
        s->view.immutable = yyjson_get_bool(yyjson_obj_get(item, "immutable"));
        s->view.cacheable = yyjson_get_bool(yyjson_obj_get(item, "cacheable"));
        s->view.stale = stale;
        previous_id = id;
    }
    c->next_id = next_id;
    size_t budget = c->capacity - c->reserve, used = 0, dropped = 0;
    for (size_t i = 0; i < c->count; i++)
        c->items[i].view.selected = selection[i];
    for (size_t i = 0; i < c->count; i++) {
        segment *s = &c->items[i];
        if (planned && s->view.pinned && !s->view.selected)
            goto invalid;
        if (s->view.selected) {
            if (s->view.tokens > budget - used)
                goto invalid;
            used += s->view.tokens;
            for (size_t j = 0; j < s->dependency_count; j++)
                if (!c->items[s->dependencies[j]].view.selected)
                    goto invalid;
        } else if (!s->view.stale)
            dropped++;
    }
    if (planned) {
        if (planned_evicted != dropped || planned_tokens > budget)
            goto invalid;
        rendered = render_selected(c);
        if (!rendered)
            goto memory;
        if (c->count_tokens(rendered, c->user) != (size_t)planned_tokens)
            goto tokenizer;
        c->planned = true;
        c->planned_tokens = (size_t)planned_tokens;
        c->planned_evicted = (size_t)planned_evicted;
    } else if (planned_tokens || planned_evicted)
        goto invalid;
    free(rendered);
    free(selection);
    yyjson_doc_free(doc);
    return c;
tokenizer:
    fg_error(e, FORGE_ERR_PARSE, "Context snapshot token counts do not match the supplied counter");
    goto failure;
limit:
    fg_error(e, FORGE_ERR_LIMIT,
             "Context snapshot exceeds segment, dependency, text, or token limits");
    goto failure;
memory:
    fg_error(e, FORGE_ERR_MEMORY, "Context snapshot allocation failed");
    goto failure;
invalid:
    fg_error(e, FORGE_ERR_PARSE,
             "Invalid context snapshot fields, hashes, selection, or dependency graph");
failure:
    free(rendered);
    free(selection);
    forge_context_destroy(c);
    if (doc)
        yyjson_doc_free(doc);
    return NULL;
}
void forge_context_destroy(forge_context *c) {
    if (c) {
        for (size_t i = 0; i < c->count; i++) {
            free(c->items[i].owned);
            free(c->items[i].dependencies);
        }
        free(c->items);
        free(c);
    }
}
