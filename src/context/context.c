#include "internal.h"
typedef struct {
    forge_segment_view view;
    char *owned;
    bool stale;
    uint64_t source;
} segment;
struct forge_context {
    segment *items;
    size_t count, capacity, reserve, allocated;
    uint64_t next_id;
    forge_count_tokens_fn count_tokens;
    void *user;
};
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
    if (!c || !text || c->count >= 4096)
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
    segment *s = &c->items[c->count++];
    memset(s, 0, sizeof(*s));
    s->owned = copy;
    s->view = (forge_segment_view){
        c->next_id++, fg_hash(text, strlen(text)),         1,        generation, dependency,
        kind,         c->count_tokens(text, c->user) + 16, priority, pinned,     false,
        copy};
    return s->view.id;
}
forge_status forge_context_update(forge_context *c, uint64_t id, const char *text,
                                  uint64_t generation) {
    if (!c || !text)
        return FORGE_ERR_ARGUMENT;
    for (size_t i = 0; i < c->count; i++)
        if (c->items[i].view.id == id) {
            segment *s = &c->items[i];
            char *copy = fg_strdup(text);
            if (!copy)
                return FORGE_ERR_MEMORY;
            free(s->owned);
            s->owned = copy;
            s->view.text = copy;
            s->view.content_hash = fg_hash(text, strlen(text));
            s->view.version++;
            s->view.generation = generation;
            s->view.tokens = c->count_tokens(text, c->user) + 16;
            s->stale = false;
            return FORGE_OK;
        }
    return FORGE_ERR_NOT_FOUND;
}
void forge_context_invalidate(forge_context *c, uint64_t dependency, uint64_t generation) {
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++) {
        segment *s = &c->items[i];
        if ((!dependency || s->source == dependency || s->source == UINT64_MAX) && s->source &&
            s->view.generation < generation) {
            s->stale = true;
            s->view.pinned = false;
        }
    }
}
void forge_context_bind_source(forge_context *c, uint64_t id, uint64_t source) {
    if (c)
        for (size_t i = 0; i < c->count; i++)
            if (c->items[i].view.id == id)
                c->items[i].source = source;
}
void forge_context_pin(forge_context *c, uint64_t id, bool pinned) {
    if (c)
        for (size_t i = 0; i < c->count; i++)
            if (c->items[i].view.id == id)
                c->items[i].view.pinned = pinned;
}
static size_t parent_index(forge_context *c, size_t i) {
    uint64_t d = c->items[i].view.dependency;
    if (!d)
        return SIZE_MAX;
    for (size_t j = 0; j < i; j++)
        if (c->items[j].view.id == d)
            return j;
    return SIZE_MAX;
}
static size_t bundle_cost(forge_context *c, size_t i) {
    size_t cost = 0, steps = 0;
    while (i != SIZE_MAX && steps++ <= c->count) {
        if (c->items[i].stale)
            return SIZE_MAX;
        if (!c->items[i].view.selected) {
            if (c->items[i].view.tokens > SIZE_MAX - cost)
                return SIZE_MAX;
            cost += c->items[i].view.tokens;
        }
        i = parent_index(c, i);
    }
    return cost;
}
static void select_bundle(forge_context *c, size_t i) {
    while (i != SIZE_MAX) {
        c->items[i].view.selected = true;
        i = parent_index(c, i);
    }
}
char *forge_context_plan(forge_context *c, size_t *tokens, size_t *evicted, forge_error *e) {
    if (!c) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Missing context");
        return NULL;
    }
    size_t budget = c->capacity - c->reserve, used = 0;
    for (size_t i = 0; i < c->count; i++)
        c->items[i].view.selected = false;
    for (size_t i = 0; i < c->count; i++)
        if (c->items[i].view.pinned) {
            size_t cost = bundle_cost(c, i);
            if (cost > budget - used) {
                fg_error(e, FORGE_ERR_LIMIT, "Pinned context exceeds the input budget (%zu tokens)",
                         budget);
                return NULL;
            }
            used += cost;
            select_bundle(c, i);
        }
    bool *considered = calloc(c->count ? c->count : 1, sizeof(bool));
    if (!considered) {
        fg_error(e, FORGE_ERR_MEMORY, "Context allocation failed");
        return NULL;
    }
    for (size_t pass = 0; pass < c->count; pass++) {
        size_t best = SIZE_MAX;
        double score = -1;
        for (size_t i = 0; i < c->count; i++)
            if (!considered[i] && !c->items[i].stale && !c->items[i].view.selected) {
                double candidate = ((double)c->items[i].view.priority + 1.0) *
                                   (1.0 + (double)i / (double)(c->count + 1)) /
                                   (double)FG_MAX(c->items[i].view.tokens, 1);
                if (candidate > score || (candidate == score && i > best)) {
                    score = candidate;
                    best = i;
                }
            }
        if (best == SIZE_MAX)
            break;
        considered[best] = true;
        size_t cost = bundle_cost(c, best);
        if (cost <= budget - used) {
            used += cost;
            select_bundle(c, best);
        }
    }
    free(considered);
    fg_buf b = {0};
    size_t dropped = 0;
    static const char *const labels[] = {"SYSTEM",        "TOOLS",  "REPOSITORY", "TASK",
                                         "WORKING_STATE", "SOURCE", "ACTION",     "TOOL_RESULT"};
    /* Immutable roles precede history; dynamic action/result pairs stay chronological. */
    for (int group = 0; group <= 5; group++)
        for (size_t i = 0; i < c->count; i++) {
            segment *s = &c->items[i];
            int rank = s->view.kind <= FORGE_SEG_MEMORY ? (int)s->view.kind : 5;
            if (rank != group || !s->view.selected)
                continue;
            fg_buf_printf(&b, "\n[%s]\n%s\n", labels[s->view.kind], s->owned);
        }
    for (size_t i = 0; i < c->count; i++)
        if (!c->items[i].view.selected && !c->items[i].stale)
            dropped++;
    char *out = fg_buf_take(&b);
    if (!out) {
        fg_error(e, FORGE_ERR_MEMORY, "Prompt allocation failed");
        return NULL;
    }
    size_t actual = c->count_tokens(out, c->user);
    if (actual > budget) {
        free(out);
        fg_error(e, FORGE_ERR_LIMIT, "Rendered prompt exceeds context budget");
        return NULL;
    }
    if (tokens)
        *tokens = actual;
    if (evicted)
        *evicted = dropped;
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
void forge_context_destroy(forge_context *c) {
    if (c) {
        for (size_t i = 0; i < c->count; i++)
            free(c->items[i].owned);
        free(c->items);
        free(c);
    }
}
