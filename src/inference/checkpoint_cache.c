#include "internal.h"
#include <assert.h>

typedef struct {
    forge_checkpoint *checkpoint;
    size_t bytes, token_count;
    uint64_t id, used;
} cache_entry;

struct fg_checkpoint_cache {
    forge_checkpoint_cache_options options;
    forge_checkpoint_cache_stats stats;
    char instance_nonce[33];
    char *namespaces;
    size_t namespace_bytes, context_offset, base_bytes;
    uint64_t generation, clock;
    cache_entry *entries;
};

static void count_add(uint64_t *counter, size_t amount) {
    *counter = amount > UINT64_MAX - *counter ? UINT64_MAX : *counter + (uint64_t)amount;
}

static bool stopped(forge_cancel_fn cancelled, void *user, uint64_t deadline) {
    return (cancelled && cancelled(user)) || (deadline && fg_now_ms() >= deadline);
}

static forge_status cancelled(fg_checkpoint_cache *cache, forge_error *error) {
    count_add(&cache->stats.cancellations, 1);
    return fg_error(error, FORGE_ERR_CANCELLED, "Automatic checkpoint work cancelled or expired");
}

static void peak(fg_checkpoint_cache *cache) {
    size_t bytes = cache->stats.resident_bytes + cache->stats.pending_bytes;
    assert(bytes <= cache->options.max_bytes);
    cache->stats.peak_bytes = FG_MAX(cache->stats.peak_bytes, bytes);
}

static void erase_entry(fg_checkpoint_cache *cache, size_t index, bool eviction) {
    cache_entry *entry = &cache->entries[index];
    if (!entry->checkpoint)
        return;
    assert(entry->bytes <= cache->stats.resident_bytes && cache->stats.entries);
    cache->stats.resident_bytes -= entry->bytes;
    cache->stats.entries--;
    if (eviction)
        count_add(&cache->stats.evictions, 1);
    forge_checkpoint_destroy(entry->checkpoint);
    memset(entry, 0, sizeof(*entry));
}

static void clear_entries(fg_checkpoint_cache *cache, bool invalidate) {
    if (invalidate)
        count_add(&cache->stats.invalidations, cache->stats.entries);
    for (size_t i = 0; i < cache->options.max_entries; i++)
        erase_entry(cache, i, false);
}

static size_t least_recent(const fg_checkpoint_cache *cache) {
    size_t selected = SIZE_MAX;
    for (size_t i = 0; i < cache->options.max_entries; i++) {
        const cache_entry *entry = &cache->entries[i];
        if (entry->checkpoint &&
            (selected == SIZE_MAX || entry->used < cache->entries[selected].used ||
             (entry->used == cache->entries[selected].used &&
              entry->id < cache->entries[selected].id)))
            selected = i;
    }
    return selected;
}

/* Rebase finite LRU ranks on counter exhaustion, preserving the same ordering. */
static uint64_t touch(fg_checkpoint_cache *cache) {
    if (cache->clock == UINT64_MAX) {
        size_t order[FORGE_CHECKPOINT_CACHE_MAX_ENTRIES], count = 0;
        for (size_t i = 0; i < cache->options.max_entries; i++)
            if (cache->entries[i].checkpoint) {
                size_t position = count;
                while (position) {
                    cache_entry *a = &cache->entries[order[position - 1]];
                    cache_entry *b = &cache->entries[i];
                    if (a->used < b->used || (a->used == b->used && a->id < b->id))
                        break;
                    order[position] = order[position - 1];
                    position--;
                }
                order[position] = i;
                count++;
            }
        for (size_t i = 0; i < count; i++)
            cache->entries[order[i]].used = (uint64_t)i + 1;
        cache->clock = (uint64_t)count;
    }
    return ++cache->clock;
}

static bool reserve_pending(fg_checkpoint_cache *cache, size_t bytes, forge_error *error) {
    size_t fixed = cache->base_bytes + cache->namespace_bytes;
    if (!bytes || bytes > cache->options.max_bytes - fixed ||
        cache->stats.pending_bytes > cache->options.max_bytes - fixed - bytes) {
        fg_error(error, FORGE_ERR_LIMIT, "Automatic checkpoint aggregate byte cap exceeded");
        return false;
    }
    while (bytes >
           cache->options.max_bytes - cache->stats.resident_bytes - cache->stats.pending_bytes) {
        size_t victim = least_recent(cache);
        if (victim == SIZE_MAX) {
            fg_error(error, FORGE_ERR_LIMIT, "Automatic checkpoint has no evictable capacity");
            return false;
        }
        erase_entry(cache, victim, true);
    }
    cache->stats.pending_bytes += bytes;
    peak(cache);
    return true;
}

static void release_pending(fg_checkpoint_cache *cache, size_t bytes) {
    assert(bytes <= cache->stats.pending_bytes);
    cache->stats.pending_bytes -= bytes;
}

static void *probe_allocate(void *user, size_t bytes, forge_error *error) {
    fg_checkpoint_cache *cache = user;
    if (!reserve_pending(cache, bytes, error))
        return NULL;
    void *memory = malloc(bytes);
    if (!memory) {
        release_pending(cache, bytes);
        fg_error(error, FORGE_ERR_MEMORY, "Automatic checkpoint probe allocation failed");
    }
    return memory;
}

static void probe_release(void *user, void *memory, size_t bytes) {
    if (memory) {
        free(memory);
        release_pending(user, bytes);
    }
}

static bool bounded_text(const char *text, size_t maximum, size_t *length) {
    if (!text)
        return false;
    size_t n = 0;
    while (n <= maximum && text[n])
        n++;
    if (!n || n > maximum || !fg_utf8_valid(text, n))
        return false;
    *length = n;
    return true;
}

forge_status fg_checkpoint_cache_validate_request(const char *prompt,
                                                  const forge_checkpoint_cache_request *request,
                                                  forge_error *error) {
    if (!request)
        return FORGE_OK;
    size_t workspace, context, length;
    if (!bounded_text(request->workspace, FORGE_CHECKPOINT_CACHE_MAX_WORKSPACE_BYTES, &workspace) ||
        !bounded_text(request->context_id, FORGE_CHECKPOINT_CACHE_MAX_CONTEXT_BYTES, &context))
        return fg_error(error, FORGE_ERR_ARGUMENT,
                        "Checkpoint namespaces must be bounded nonempty UTF-8 strings");
    if (!bounded_text(prompt, FG_MAX_JSON, &length))
        return fg_error(error, FORGE_ERR_ARGUMENT,
                        "Cached prompt must be nonempty UTF-8 within 16 MiB");
    if (request->anchor_count > FORGE_CHECKPOINT_CACHE_MAX_ANCHORS ||
        (request->anchor_count && !request->anchor_ends))
        return fg_error(error, FORGE_ERR_ARGUMENT, "Invalid checkpoint anchor array");
    size_t previous = 0;
    for (size_t i = 0; i < request->anchor_count; i++) {
        size_t end = request->anchor_ends[i];
        if (end <= previous || end > length ||
            (end < length && ((unsigned char)prompt[end] & 0xc0u) == 0x80u))
            return fg_error(error, FORGE_ERR_ARGUMENT,
                            "Checkpoint anchors must increase at UTF-8 byte boundaries");
        previous = end;
    }
    return FORGE_OK;
}

forge_checkpoint_cache_options forge_default_checkpoint_cache_options(void) {
    return (forge_checkpoint_cache_options){(size_t)256 * 1024 * 1024, 8, 128, 2};
}

static forge_status supported(forge_model *model, forge_error *error) {
    const fg_checkpoint_backend *backend = model->checkpoint;
    if (!backend || !backend->supported || !backend->state_size || !backend->state_get ||
        !backend->state_set || !backend->accept_tokens || !backend->clear ||
        !backend->probe_prefix || !backend->live_prefix || !backend->live_matches)
        return fg_error(error, FORGE_ERR_UNSUPPORTED,
                        "Backend does not support automatic physical checkpoint caching");
    return backend->supported(model, error);
}

void fg_checkpoint_cache_destroy(fg_checkpoint_cache *cache) {
    if (cache) {
        assert(!cache->stats.pending_bytes);
        clear_entries(cache, false);
        free(cache->namespaces);
        free(cache);
    }
}

forge_status forge_checkpoint_cache_configure(forge_model *model,
                                              const forge_checkpoint_cache_options *options,
                                              forge_error *error) {
    if (error)
        memset(error, 0, sizeof(*error));
    if (!model)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing checkpoint cache model");
    if (model->operation_active)
        return fg_error(error, FORGE_ERR_CONFLICT, "Cannot configure an active model");
    size_t bytes = 0;
    if (options) {
        if (!options->max_entries || options->max_entries > FORGE_CHECKPOINT_CACHE_MAX_ENTRIES ||
            !options->max_bytes || options->max_bytes > FORGE_CHECKPOINT_CACHE_MAX_BYTES ||
            !options->min_prefix_tokens || options->min_prefix_tokens > 1048576 ||
            !options->max_captures_per_prompt ||
            options->max_captures_per_prompt > FORGE_CHECKPOINT_CACHE_MAX_ANCHORS)
            return fg_error(error, FORGE_ERR_ARGUMENT, "Invalid checkpoint cache limits");
        bytes = sizeof(fg_checkpoint_cache) + options->max_entries * sizeof(cache_entry);
        if (bytes > options->max_bytes)
            return fg_error(error, FORGE_ERR_ARGUMENT,
                            "Checkpoint cap cannot hold manager metadata");
        forge_status status = supported(model, error);
        if (status != FORGE_OK)
            return status;
        if (!model->instance_nonce[0])
            return fg_error(error, FORGE_ERR_CONFLICT, "Model instance is not initialized");
    }
    fg_checkpoint_cache_destroy(model->cache);
    model->cache = NULL;
    if (!options)
        return FORGE_OK;
    fg_checkpoint_cache *cache = calloc(1, bytes);
    if (!cache)
        return fg_error(error, FORGE_ERR_MEMORY, "Checkpoint manager allocation failed");
    cache->options = *options;
    /* One charged allocation, with the entry table after aligned metadata.
     * Avoid a flexible array member because MSVC reports it as an extension. */
    cache->entries = (cache_entry *)(cache + 1);
    memcpy(cache->instance_nonce, model->instance_nonce, sizeof(cache->instance_nonce));
    cache->stats.enabled = true;
    cache->stats.max_bytes = options->max_bytes;
    cache->stats.resident_bytes = cache->stats.peak_bytes = cache->base_bytes = bytes;
    model->cache = cache;
    return FORGE_OK;
}

forge_status forge_checkpoint_cache_clear(forge_model *model, forge_error *error) {
    if (error)
        memset(error, 0, sizeof(*error));
    if (!model)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing checkpoint cache model");
    if (model->operation_active)
        return fg_error(error, FORGE_ERR_CONFLICT, "Cannot clear an active model cache");
    fg_checkpoint_cache *cache = model->cache;
    if (cache) {
        clear_entries(cache, true);
        free(cache->namespaces);
        cache->namespaces = NULL;
        cache->stats.resident_bytes -= cache->namespace_bytes;
        cache->namespace_bytes = cache->context_offset = 0;
    }
    return FORGE_OK;
}

bool forge_checkpoint_cache_get_stats(const forge_model *model,
                                      forge_checkpoint_cache_stats *stats) {
    if (!model || !stats)
        return false;
    *stats = model->cache ? model->cache->stats : (forge_checkpoint_cache_stats){0};
    return true;
}

static bool set_scope(fg_checkpoint_cache *cache, const forge_checkpoint_cache_request *request,
                      forge_error *error) {
    if (cache->namespaces && cache->generation == request->repo_generation &&
        !strcmp(cache->namespaces, request->workspace) &&
        !strcmp(cache->namespaces + cache->context_offset, request->context_id))
        return true;
    clear_entries(cache, true);
    free(cache->namespaces);
    cache->namespaces = NULL;
    cache->stats.resident_bytes -= cache->namespace_bytes;
    cache->namespace_bytes = cache->context_offset = 0;
    size_t workspace = strlen(request->workspace) + 1, context = strlen(request->context_id) + 1;
    size_t bytes = workspace + context;
    if (!reserve_pending(cache, bytes, error))
        return false;
    char *namespaces = malloc(bytes);
    if (!namespaces) {
        release_pending(cache, bytes);
        fg_error(error, FORGE_ERR_MEMORY, "Checkpoint namespace allocation failed");
        return false;
    }
    memcpy(namespaces, request->workspace, workspace);
    memcpy(namespaces + workspace, request->context_id, context);
    cache->namespaces = namespaces;
    cache->namespace_bytes = bytes;
    cache->context_offset = workspace;
    cache->generation = request->repo_generation;
    release_pending(cache, bytes);
    cache->stats.resident_bytes += bytes;
    return true;
}

static void skipped_failure(fg_checkpoint_cache *cache, forge_status status, uint64_t *failures) {
    if (status == FORGE_ERR_LIMIT)
        count_add(&cache->stats.skipped_budget, 1);
    else if (status == FORGE_ERR_UNSUPPORTED)
        count_add(&cache->stats.skipped_unsupported, 1);
    else
        count_add(failures, 1);
}

forge_status fg_checkpoint_cache_begin(forge_model *model, const char *prompt,
                                       const int32_t *tokens, size_t count, forge_cancel_fn cancel,
                                       void *user, uint64_t deadline,
                                       fg_checkpoint_cache_operation *operation,
                                       forge_error *error) {
    if (!model || !model->operation_active || !operation || !tokens || !count || count > 1048576)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Invalid prepared checkpoint request");
    memset(operation, 0, sizeof(*operation));
    fg_checkpoint_cache *cache = model->cache;
    if (!cache)
        return FORGE_OK;
    const forge_checkpoint_cache_request *request = model->cache_request;
    if (!request) {
        count_add(&cache->stats.skipped_no_request, 1);
        return FORGE_OK;
    }
    count_add(&cache->stats.requests, 1);
    if (stopped(cancel, user, deadline))
        return cancelled(cache, error);
    if (memcmp(cache->instance_nonce, model->instance_nonce, sizeof(cache->instance_nonce))) {
        clear_entries(cache, true);
        return fg_error(error, FORGE_ERR_CONFLICT,
                        "Checkpoint manager belongs to another instance");
    }
    forge_status status = fg_checkpoint_cache_validate_request(prompt, request, error);
    if (status != FORGE_OK)
        return status;
    forge_error local = {0};
    if (!set_scope(cache, request, &local)) {
        skipped_failure(cache, local.code, &cache->stats.probe_failures);
        return stopped(cancel, user, deadline) ? cancelled(cache, error) : FORGE_OK;
    }
    if (!model->config.reuse_prefix) {
        count_add(&cache->stats.skipped_ablation, 1);
        return FORGE_OK;
    }
    if (!request->anchor_count) {
        count_add(&cache->stats.skipped_no_anchor, 1);
        return FORGE_OK;
    }
    if (supported(model, &local) != FORGE_OK) {
        count_add(&cache->stats.skipped_unsupported, 1);
        clear_entries(cache, true);
        return FORGE_OK;
    }
    operation->cache = cache;
    operation->tokens = tokens;
    operation->token_count = count;
    operation->context_hash = fg_hash(prompt, strlen(prompt));
    operation->cancelled = cancel;
    operation->userdata = user;
    operation->deadline = deadline;
    fg_checkpoint_allocator allocator = {cache, probe_allocate, probe_release};
    uint64_t probe_start = fg_now_ms();
    for (size_t i = 0; i < request->anchor_count; i++) {
        if (stopped(cancel, user, deadline)) {
            cache->stats.probe_ms += (double)(fg_now_ms() - probe_start);
            return cancelled(cache, error);
        }
        size_t common = 0;
        memset(&local, 0, sizeof(local));
        status =
            model->checkpoint->probe_prefix(model, prompt, request->anchor_ends[i], tokens, count,
                                            &common, &allocator, cancel, user, deadline, &local);
        assert(!cache->stats.pending_bytes);
        if (status == FORGE_ERR_CANCELLED || stopped(cancel, user, deadline)) {
            cache->stats.probe_ms += (double)(fg_now_ms() - probe_start);
            return cancelled(cache, error);
        }
        if (status != FORGE_OK) {
            skipped_failure(cache, status, &cache->stats.probe_failures);
            continue;
        }
        if (common > count) {
            count_add(&cache->stats.probe_failures, 1);
            continue;
        }
        if (common < cache->options.min_prefix_tokens)
            continue;
        size_t position = 0;
        while (position < operation->anchor_count && operation->anchors[position] < common)
            position++;
        if (position < operation->anchor_count && operation->anchors[position] == common)
            continue;
        memmove(operation->anchors + position + 1, operation->anchors + position,
                (operation->anchor_count - position) * sizeof(*operation->anchors));
        operation->anchors[position] = common;
        operation->anchor_count++;
    }
    cache->stats.probe_ms += (double)(fg_now_ms() - probe_start);
    if (!operation->anchor_count) {
        count_add(&cache->stats.skipped_no_anchor, 1);
        return FORGE_OK;
    }
    operation->active = true;
    size_t live = model->checkpoint->live_prefix(model, tokens, count);
    if (live > count)
        return fg_error(error, FORGE_ERR_MODEL, "Invalid live checkpoint prefix length");
    operation->previous_live_prefix = live == count ? live - 1 : live;
    size_t selected = SIZE_MAX, usable = 0;
    size_t maximum = operation->anchors[operation->anchor_count - 1];
    for (size_t i = 0; i < cache->options.max_entries; i++) {
        if (stopped(cancel, user, deadline))
            return cancelled(cache, error);
        cache_entry *entry = &cache->entries[i];
        if (!entry->checkpoint || entry->token_count > maximum ||
            !fg_checkpoint_matches_prefix(entry->checkpoint, tokens, count))
            continue;
        size_t prefix = entry->token_count == count ? count - 1 : entry->token_count;
        if (selected == SIZE_MAX || prefix > usable ||
            (prefix == usable && entry->id < cache->entries[selected].id)) {
            selected = i;
            usable = prefix;
        }
    }
    count_add(&cache->stats.lookups, 1);
    if (selected == SIZE_MAX) {
        count_add(&cache->stats.misses, 1);
        return FORGE_OK;
    }
    if (usable <= operation->previous_live_prefix) {
        count_add(&cache->stats.misses, 1);
        count_add(&cache->stats.skipped_live_prefix, 1);
        return FORGE_OK;
    }
    forge_checkpoint_stats restored = {0};
    status = fg_checkpoint_restore_active(model, cache->entries[selected].checkpoint,
                                          cache->generation, cache->options.max_bytes, cancel, user,
                                          deadline, &restored, &local);
    cache->stats.restore_ms += restored.restore_ms;
    if (status != FORGE_OK) {
        count_add(&cache->stats.misses, 1);
        count_add(&cache->stats.restore_failures, 1);
        erase_entry(cache, selected, false);
        if (status == FORGE_ERR_CANCELLED || stopped(cancel, user, deadline))
            return cancelled(cache, error);
        /* The shared restore helper clears partial physical writes. Prefill can
         * now continue cold; do not retry another cache entry in the same call. */
        return FORGE_OK;
    }
    cache->entries[selected].used = touch(cache);
    count_add(&cache->stats.hits, 1);
    count_add(&cache->stats.tokens_restored, restored.restored_tokens);
    operation->restored_tokens = restored.restored_tokens;
    return FORGE_OK;
}

size_t fg_checkpoint_cache_next(const fg_checkpoint_cache_operation *operation, size_t position,
                                size_t end) {
    if (operation && operation->active)
        for (size_t i = 0; i < operation->anchor_count; i++)
            if (operation->anchors[i] > position && operation->anchors[i] < end)
                return operation->anchors[i];
    return end;
}

void fg_checkpoint_cache_note_reuse(fg_checkpoint_cache_operation *operation, size_t prefix) {
    if (!operation || !operation->active || operation->reuse_recorded)
        return;
    operation->reuse_recorded = true;
    if (operation->restored_tokens) {
        size_t matched = FG_MIN(prefix, operation->restored_tokens);
        count_add(&operation->cache->stats.restored_tokens_reused, matched);
        if (matched > operation->previous_live_prefix)
            count_add(&operation->cache->stats.additional_matched_tokens,
                      matched - operation->previous_live_prefix);
    }
}

forge_status fg_checkpoint_cache_capture(forge_model *model,
                                         fg_checkpoint_cache_operation *operation, size_t count,
                                         forge_error *error) {
    if (!operation || !operation->active)
        return FORGE_OK;
    fg_checkpoint_cache *cache = operation->cache;
    if (stopped(operation->cancelled, operation->userdata, operation->deadline))
        return cancelled(cache, error);
    bool anchor = false;
    for (size_t i = 0; i < operation->anchor_count; i++)
        anchor |= operation->anchors[i] == count;
    if (!anchor || operation->capture_attempts >= cache->options.max_captures_per_prompt)
        return FORGE_OK;
    for (size_t i = 0; i < cache->options.max_entries; i++)
        if (cache->entries[i].checkpoint && cache->entries[i].token_count == count &&
            fg_checkpoint_matches_prefix(cache->entries[i].checkpoint, operation->tokens, count))
            return FORGE_OK;
    operation->capture_attempts++;
    uint64_t start = fg_now_ms();
    size_t state_bytes = model->checkpoint->state_size(model);
    if (stopped(operation->cancelled, operation->userdata, operation->deadline)) {
        cache->stats.capture_ms += (double)(fg_now_ms() - start);
        return cancelled(cache, error);
    }
    size_t allocation = fg_checkpoint_allocation_bytes(count, state_bytes);
    if (!allocation) {
        count_add(state_bytes > FORGE_CHECKPOINT_MAX_STATE_BYTES ? &cache->stats.skipped_budget
                                                                 : &cache->stats.capture_failures,
                  1);
        cache->stats.capture_ms += (double)(fg_now_ms() - start);
        return FORGE_OK;
    }
    forge_error local = {0};
    if (!reserve_pending(cache, allocation, &local)) {
        count_add(&cache->stats.skipped_budget, 1);
        cache->stats.capture_ms += (double)(fg_now_ms() - start);
        return FORGE_OK;
    }
    size_t slot = 0;
    while (slot < cache->options.max_entries && cache->entries[slot].checkpoint)
        slot++;
    if (slot == cache->options.max_entries) {
        slot = least_recent(cache);
        assert(slot != SIZE_MAX);
        erase_entry(cache, slot, true);
    }
    forge_checkpoint *checkpoint = fg_checkpoint_capture_live(
        model, operation->tokens, count, state_bytes, cache->generation, operation->context_hash,
        operation->cancelled, operation->userdata, operation->deadline, &local);
    release_pending(cache, allocation);
    cache->stats.capture_ms += (double)(fg_now_ms() - start);
    if (!checkpoint) {
        skipped_failure(cache, local.code, &cache->stats.capture_failures);
        if (local.code == FORGE_ERR_CANCELLED ||
            stopped(operation->cancelled, operation->userdata, operation->deadline))
            return cancelled(cache, error);
        return FORGE_OK;
    }
    forge_checkpoint_info info;
    bool described = forge_checkpoint_get_info(checkpoint, &info);
    assert(described);
    (void)described;
    cache->entries[slot] = (cache_entry){checkpoint, allocation, count, info.id, touch(cache)};
    cache->stats.resident_bytes += allocation;
    cache->stats.entries++;
    count_add(&cache->stats.captures, 1);
    peak(cache);
    return FORGE_OK;
}
