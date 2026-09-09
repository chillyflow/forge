#ifdef NDEBUG
#undef NDEBUG
#endif
#include "internal.h"
#include <assert.h>

/* This deterministic sequence-I/O fixture is not a model or KV-cache proof.
 * Its template appends two suffix tokens; its tokenizer merges literal "ab".
 * Only the manager, ownership/cancellation paths and scheduling are real. */
#define TOKEN_CAP 1024u
typedef enum { STAGE_NONE, STAGE_PROBE, STAGE_SIZE, STAGE_GET, STAGE_SET } stage;
typedef struct {
    forge_model model;
    int32_t live[TOKEN_CAP];
    size_t count, generate_calls, prefill_calls, probe_calls, get_calls, set_calls, clear_calls;
    size_t decoded_tokens, capture_counts[64], captured_count, last_restored_count;
    size_t size_override, extra_probe_bytes;
    int token_salt;
    bool physical, unsupported, short_get, short_set, fail_accept, fail_live_match;
    bool user_cancelled, reenter, reentered, bad_probe_count;
    stage cancel_stage, expire_stage;
    uint64_t deadline;
} fixture;

static bool stopped(forge_cancel_fn cancel, void *user, uint64_t deadline) {
    return (cancel && cancel(user)) || (deadline && fg_now_ms() >= deadline);
}

static size_t tokenize(fixture *f, const char *text, size_t length, int32_t *tokens) {
    assert(length + 4 <= TOKEN_CAP);
    size_t n = 0;
    tokens[n++] = 1; /* user-template opening */
    tokens[n++] = 2;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == 'a' && i + 1 < length && text[i + 1] == 'b') {
            tokens[n++] = 600 + f->token_salt;
            i++;
        } else
            tokens[n++] = 100 + (unsigned char)text[i] + f->token_salt;
    }
    tokens[n++] = 3; /* closing user + assistant suffix */
    tokens[n++] = 4;
    return n;
}

static size_t token_count(fixture *f, const char *text) {
    int32_t tokens[TOKEN_CAP];
    return tokenize(f, text, strlen(text), tokens);
}

static void signal_stage(fixture *f, stage where) {
    if (f->cancel_stage == where)
        f->user_cancelled = true;
    if (f->expire_stage == where) {
        assert(f->deadline && f->deadline <= fg_now_ms() + 1000);
        while (fg_now_ms() < f->deadline) {
        }
    }
}

static forge_status supported(forge_model *model, forge_error *error) {
    fixture *f = model->backend;
    return f->unsupported ? fg_error(error, FORGE_ERR_UNSUPPORTED, "Unsupported fixture")
                          : FORGE_OK;
}

static size_t live_prefix(forge_model *model, const int32_t *tokens, size_t count) {
    fixture *f = model->backend;
    size_t common = 0;
    if (f->physical && model->config.reuse_prefix)
        while (common < count && common < f->count && tokens[common] == f->live[common])
            common++;
    return common;
}

static void clear_live(forge_model *model) {
    fixture *f = model->backend;
    f->physical = false;
    f->count = 0;
    f->clear_calls++;
}

static bool live_matches(forge_model *model, const int32_t *tokens, size_t count) {
    fixture *f = model->backend;
    return !f->fail_live_match && f->physical && f->count == count &&
           !memcmp(f->live, tokens, count * sizeof(*tokens));
}

static forge_status prefill(forge_model *model, const char *prompt, int32_t **out_tokens,
                            size_t *out_count, forge_metrics *metrics, forge_cancel_fn cancel,
                            void *user, uint64_t deadline, forge_error *error) {
    fixture *f = model->backend;
    f->prefill_calls++;
    if (stopped(cancel, user, deadline))
        return fg_error(error, FORGE_ERR_CANCELLED, "Fixture prefill cancelled");
    int32_t tokens[TOKEN_CAP];
    size_t n = tokenize(f, prompt, strlen(prompt), tokens);
    *out_tokens = malloc(n * sizeof(*tokens));
    assert(*out_tokens);
    memcpy(*out_tokens, tokens, n * sizeof(*tokens));
    *out_count = n;
    memcpy(f->live, tokens, n * sizeof(*tokens));
    f->count = n;
    f->physical = true;
    metrics->prompt_tokens = metrics->prefill_tokens = n;
    return FORGE_OK;
}

static size_t state_size(forge_model *model) {
    fixture *f = model->backend;
    signal_stage(f, STAGE_SIZE);
    return f->size_override == SIZE_MAX ? 0
           : f->size_override           ? f->size_override
                                        : 8 + f->count * sizeof(*f->live);
}

static size_t state_get(forge_model *model, uint8_t *bytes, size_t length) {
    fixture *f = model->backend;
    f->get_calls++;
    assert(f->physical && length == 8 + f->count * sizeof(*f->live));
    assert(f->captured_count < sizeof(f->capture_counts) / sizeof(*f->capture_counts));
    f->capture_counts[f->captured_count++] = f->count;
    uint32_t header[2] = {UINT32_C(0xcac4e001), (uint32_t)f->count};
    memcpy(bytes, header, sizeof(header));
    memcpy(bytes + sizeof(header), f->live, f->count * sizeof(*f->live));
    signal_stage(f, STAGE_GET);
    return f->short_get ? length - 1 : length;
}

static size_t state_set(forge_model *model, const uint8_t *bytes, size_t length) {
    fixture *f = model->backend;
    f->set_calls++;
    uint32_t header[2];
    assert(length >= sizeof(header));
    memcpy(header, bytes, sizeof(header));
    assert(header[0] == UINT32_C(0xcac4e001) && header[1] <= TOKEN_CAP);
    assert(length == sizeof(header) + (size_t)header[1] * sizeof(*f->live));
    f->count = header[1];
    f->last_restored_count = f->count;
    memcpy(f->live, bytes + sizeof(header), f->count * sizeof(*f->live));
    f->physical = true;
    signal_stage(f, STAGE_SET);
    return f->short_set ? length - 1 : length;
}

static bool accept_tokens(forge_model *model, const int32_t *tokens, size_t count) {
    fixture *f = model->backend;
    return !f->fail_accept && live_matches(model, tokens, count);
}

static forge_status probe(forge_model *model, const char *prompt, size_t end, const int32_t *full,
                          size_t full_count, size_t *common,
                          const fg_checkpoint_allocator *allocator, forge_cancel_fn cancel,
                          void *user, uint64_t deadline, forge_error *error) {
    fixture *f = model->backend;
    f->probe_calls++;
    *common = 0;
    if (stopped(cancel, user, deadline))
        return fg_error(error, FORGE_ERR_CANCELLED, "Fixture probe cancelled");
    if (f->extra_probe_bytes) {
        void *extra = allocator->allocate(allocator->userdata, f->extra_probe_bytes, error);
        if (!extra)
            return error->code;
        allocator->release(allocator->userdata, extra, f->extra_probe_bytes);
    }
    char *prefix = allocator->allocate(allocator->userdata, end + 1, error);
    if (!prefix)
        return error->code;
    memcpy(prefix, prompt, end);
    prefix[end] = 0;
    size_t bytes = (end + 4) * sizeof(int32_t);
    int32_t *tokens = allocator->allocate(allocator->userdata, bytes, error);
    if (!tokens) {
        allocator->release(allocator->userdata, prefix, end + 1);
        return error->code;
    }
    forge_checkpoint_cache_stats during;
    assert(forge_checkpoint_cache_get_stats(model, &during));
    assert(during.pending_bytes >= end + 1 + bytes);
    assert(during.resident_bytes + during.pending_bytes <= during.max_bytes);
    size_t count = tokenize(f, prefix, end, tokens);
    while (*common < count && *common < full_count && tokens[*common] == full[*common])
        (*common)++;
    if (f->bad_probe_count)
        *common = full_count + 1;
    allocator->release(allocator->userdata, tokens, bytes);
    allocator->release(allocator->userdata, prefix, end + 1);
    signal_stage(f, STAGE_PROBE);
    return stopped(cancel, user, deadline)
               ? fg_error(error, FORGE_ERR_CANCELLED, "Fixture probe stopped after allocation")
               : FORGE_OK;
}

static const fg_checkpoint_backend backend = {supported,   prefill,       state_size, state_get,
                                              state_set,   accept_tokens, clear_live, probe,
                                              live_prefix, live_matches};

static forge_status generate(forge_model *model, const char *prompt, const char *grammar,
                             const fg_decode_policy *policy, size_t maximum,
                             forge_token_fn callback, void *callback_user, char **out,
                             forge_metrics *metrics, forge_cancel_fn cancel, void *cancel_user,
                             uint64_t deadline, forge_error *error) {
    (void)grammar;
    (void)policy;
    fixture *f = model->backend;
    f->generate_calls++;
    int32_t tokens[TOKEN_CAP];
    size_t count = tokenize(f, prompt, strlen(prompt), tokens);
    if (count + maximum > TOKEN_CAP)
        return fg_error(error, FORGE_ERR_LIMIT, "Fixture context exceeded");
    fg_checkpoint_cache_operation operation = {0};
    forge_status status = fg_checkpoint_cache_begin(model, prompt, tokens, count, cancel,
                                                    cancel_user, deadline, &operation, error);
    if (status != FORGE_OK)
        return status;
    size_t prefix = live_prefix(model, tokens, count);
    if (prefix == count)
        prefix--;
    f->count = prefix; /* Equivalent to trimming the live sequence at its LCP. */
    f->physical = prefix != 0;
    metrics->simulated = true;
    metrics->prompt_tokens += count;
    metrics->cached_tokens += prefix;
    fg_checkpoint_cache_note_reuse(&operation, prefix);
    if (prefix &&
        (status = fg_checkpoint_cache_capture(model, &operation, prefix, error)) != FORGE_OK)
        goto failed;
    for (size_t position = prefix; position < count;) {
        if (stopped(cancel, cancel_user, deadline)) {
            status = fg_error(error, FORGE_ERR_CANCELLED, "Fixture generation cancelled");
            goto failed;
        }
        size_t end =
            fg_checkpoint_cache_next(&operation, position, position + FG_MIN(count - position, 7));
        assert(end > position && end <= count);
        memcpy(f->live + position, tokens + position, (end - position) * sizeof(*tokens));
        f->count = end;
        f->physical = true;
        f->decoded_tokens += end - position;
        metrics->prefill_tokens += end - position;
        position = end;
        status = fg_checkpoint_cache_capture(model, &operation, position, error);
        if (status != FORGE_OK)
            goto failed;
    }
    *out = fg_strdup("fixture output");
    assert(*out && f->count == count && !memcmp(f->live, tokens, count * sizeof(*tokens)));
    metrics->generated_tokens++;
    if (callback && !callback(*out, strlen(*out), callback_user)) {
        free(*out);
        *out = NULL;
        status = fg_error(error, FORGE_ERR_CANCELLED, "Fixture output callback cancelled");
        goto failed;
    }
    return FORGE_OK;
failed:
    clear_live(model);
    return status;
}

static bool user_cancel(void *user) {
    fixture *f = user;
    if (f->reenter && !f->reentered) {
        f->reentered = true;
        forge_error error = {0};
        forge_metrics metrics;
        assert(forge_checkpoint_cache_configure(&f->model, NULL, &error) == FORGE_ERR_CONFLICT);
        assert(forge_checkpoint_cache_clear(&f->model, &error) == FORGE_ERR_CONFLICT);
        assert(!forge_checkpoint_save(&f->model, "nested", NULL, NULL, &error));
        assert(error.code == FORGE_ERR_CONFLICT);
        assert(forge_complete(&f->model, "nested", 1, NULL, NULL, &metrics, &error) ==
               FORGE_ERR_CONFLICT);
    }
    return f->user_cancelled;
}

static void init(fixture *f) {
    memset(f, 0, sizeof(*f));
    forge_error error = {0};
    assert(fg_model_instance_init(&f->model, &error));
    f->model.backend = f;
    f->model.checkpoint = &backend;
    f->model.generate = generate;
    f->model.config = forge_default_model_config();
    f->model.config.context_tokens = TOKEN_CAP;
}

static forge_checkpoint_cache_stats stats(fixture *f) {
    forge_checkpoint_cache_stats value;
    assert(forge_checkpoint_cache_get_stats(&f->model, &value));
    assert(!value.pending_bytes && value.resident_bytes <= value.max_bytes &&
           value.peak_bytes <= value.max_bytes);
    return value;
}

static forge_checkpoint_cache_options configure(fixture *f, size_t entries) {
    forge_checkpoint_cache_options options = forge_default_checkpoint_cache_options();
    options.max_bytes = 65536;
    options.max_entries = entries;
    options.min_prefix_tokens = 1;
    forge_error error = {0};
    assert(forge_checkpoint_cache_configure(&f->model, &options, &error) == FORGE_OK);
    assert(stats(f).enabled && stats(f).entries == 0);
    return options;
}

static forge_checkpoint_cache_request request_for(const size_t *anchors, size_t count) {
    return (forge_checkpoint_cache_request){"workspace-one", "context-one", 7, anchors, count};
}

static forge_status run(fixture *f, const char *prompt,
                        const forge_checkpoint_cache_request *request, forge_metrics *metrics,
                        forge_error *error) {
    memset(metrics, 0, sizeof(*metrics));
    memset(error, 0, sizeof(*error));
    char *output = NULL;
    forge_status status =
        fg_model_generate_with_cache(&f->model, prompt, NULL, 1, NULL, NULL, &output, metrics,
                                     user_cancel, f, f->deadline, request, error);
    assert(!f->model.operation_active && !f->model.cache_request);
    assert((status == FORGE_OK) == (output != NULL));
    free(output);
    (void)stats(f);
    return status;
}

static forge_metrics run_ok(fixture *f, const char *prompt,
                            const forge_checkpoint_cache_request *request) {
    forge_metrics metrics;
    forge_error error = {0};
    forge_status status = run(f, prompt, request, &metrics, &error);
    if (status != FORGE_OK)
        fprintf(stderr, "cache fixture failed: %s\n", error.message);
    assert(status == FORGE_OK);
    return metrics;
}

static void finish(fixture *f) {
    forge_error error = {0};
    assert(forge_checkpoint_cache_configure(&f->model, NULL, &error) == FORGE_OK);
    assert(!stats(f).enabled && !f->model.cache);
}

static void test_suffix_merge_and_full_hit(void) {
    fixture f;
    init(&f);
    configure(&f, 8);
    size_t anchor = 3;
    forge_checkpoint_cache_request request = request_for(&anchor, 1);
    forge_metrics metrics = run_ok(&f, "ABC tail", &request);
    assert(f.capture_counts[0] == 5 && token_count(&f, "ABC") == 7);
    assert(metrics.prefill_tokens == metrics.prompt_tokens && f.prefill_calls == 0);
    assert(f.decoded_tokens == metrics.prompt_tokens); /* No separate prefix prefill. */
    assert(metrics.checkpoint_lookups == 1 && metrics.checkpoint_misses == 1 &&
           metrics.checkpoint_captures == 1 && !metrics.checkpoint_hits);
    clear_live(&f.model);
    metrics = run_ok(&f, "ABC more", &request);
    assert(f.last_restored_count == 5 && metrics.cached_tokens == 5);
    assert(stats(&f).hits == 1 && stats(&f).restored_tokens_reused == 5);
    assert(metrics.checkpoint_hits == 1 && metrics.checkpoint_restored_tokens == 5 &&
           metrics.checkpoint_reused_tokens == 5 && metrics.checkpoint_additional_tokens == 5);
    assert(metrics.checkpoint_peak_bytes == stats(&f).peak_bytes);

    forge_error error = {0};
    assert(forge_checkpoint_cache_clear(&f.model, &error) == FORGE_OK);
    clear_live(&f.model);
    anchor = 4; /* "ST a" ends inside the full prompt's merged "ab" token. */
    size_t captures = f.captured_count;
    metrics = run_ok(&f, "ST ab tail", &request);
    assert(f.capture_counts[captures] == 5 && token_count(&f, "ST a") == 8);
    clear_live(&f.model);
    metrics = run_ok(&f, "ST ac tail", &request);
    assert(f.last_restored_count == 5 && metrics.cached_tokens == 5);
    assert(f.capture_counts[f.captured_count - 1] == 6);

    assert(forge_checkpoint_cache_clear(&f.model, &error) == FORGE_OK);
    clear_live(&f.model);
    anchor = strlen("whole prompt");
    metrics = run_ok(&f, "whole prompt", &request);
    size_t full_count = metrics.prompt_tokens;
    clear_live(&f.model);
    forge_checkpoint_cache_stats before = stats(&f);
    metrics = run_ok(&f, "whole prompt", &request);
    assert(f.last_restored_count == full_count && metrics.cached_tokens == full_count - 1);
    assert(metrics.prefill_tokens == 1);
    assert(stats(&f).tokens_restored - before.tokens_restored == full_count);
    assert(stats(&f).restored_tokens_reused - before.restored_tokens_reused == full_count - 1);
    finish(&f);
}

static void test_longest_exact_prefix_and_live_choice(void) {
    fixture f;
    init(&f);
    configure(&f, 8);
    size_t anchors[] = {3, 6};
    forge_checkpoint_cache_request request = request_for(anchors, 2);
    run_ok(&f, "ABCDEF tail", &request);
    assert(f.captured_count == 2 && f.capture_counts[0] == 5 && f.capture_counts[1] == 8);
    run_ok(&f, "UVWXYZ tail", &request);
    forge_metrics metrics = run_ok(&f, "ABCDEF more", &request);
    assert(f.last_restored_count == 8 && metrics.cached_tokens == 8);
    size_t sets = f.set_calls;
    forge_checkpoint_cache_stats before = stats(&f);
    metrics = run_ok(&f, "ABCDEF more", &request);
    assert(f.set_calls == sets && metrics.cached_tokens == metrics.prompt_tokens - 1);
    assert(stats(&f).skipped_live_prefix == before.skipped_live_prefix + 1);
    /* Remembered IDs without physical sequence coverage cannot beat a snapshot. */
    f.physical = false;
    run_ok(&f, "ABCDEF more", &request);
    assert(f.set_calls == sets + 1 && f.last_restored_count == 8);
    clear_live(&f.model);
    metrics = run_ok(&f, "ABCDEX tail", &request);
    assert(f.last_restored_count == 5 && metrics.cached_tokens == 5);
    /* Identical raw prompt hashes cannot authorize different actual token IDs. */
    clear_live(&f.model);
    f.token_salt = 10;
    sets = f.set_calls;
    run_ok(&f, "ABCDEF tail", &request);
    assert(f.set_calls == sets);
    finish(&f);
}

static void test_lru_and_scope_invalidation(void) {
    fixture f;
    init(&f);
    configure(&f, 2);
    size_t anchor = 3;
    forge_checkpoint_cache_request request = request_for(&anchor, 1);
    run_ok(&f, "AAA tail", &request);
    run_ok(&f, "BBB tail", &request);
    run_ok(&f, "AAA tail", &request); /* A is now more recent than B. */
    run_ok(&f, "CCC tail", &request);
    assert(stats(&f).entries == 2 && stats(&f).evictions == 1);
    size_t sets = f.set_calls;
    run_ok(&f, "AAA tail", &request);
    assert(f.set_calls == sets + 1); /* A survived C's admission. */
    run_ok(&f, "BBB tail", &request);
    assert(f.set_calls == sets + 1); /* B was evicted. */
    assert(stats(&f).entries == 2);
    for (unsigned change = 0; change < 3; change++) {
        sets = f.set_calls;
        uint64_t invalidated = stats(&f).invalidations;
        if (change == 0)
            request.repo_generation++;
        else if (change == 1)
            request.context_id = "context-two";
        else
            request.workspace = "workspace-two";
        clear_live(&f.model);
        run_ok(&f, "BBB tail", &request);
        assert(f.set_calls == sets && stats(&f).invalidations > invalidated);
    }
    char nonce[sizeof(f.model.instance_nonce)];
    memcpy(nonce, f.model.instance_nonce, sizeof(nonce));
    f.model.instance_nonce[0] = nonce[0] == '0' ? '1' : '0';
    forge_metrics metrics;
    forge_error error = {0};
    sets = f.set_calls;
    assert(run(&f, "BBB tail", &request, &metrics, &error) == FORGE_ERR_CONFLICT);
    assert(f.set_calls == sets && stats(&f).entries == 0);
    memcpy(f.model.instance_nonce, nonce, sizeof(nonce));
    finish(&f);
}

static void test_aggregate_budget_and_capture_failures(void) {
    fixture f;
    init(&f);
    forge_checkpoint_cache_options options = configure(&f, 2);
    size_t base = stats(&f).resident_bytes;
    size_t anchor = 3;
    forge_checkpoint_cache_request request = request_for(&anchor, 1);
    size_t namespaces = strlen(request.workspace) + strlen(request.context_id) + 2;
    size_t allocation = fg_checkpoint_allocation_bytes(5, 8 + 5 * sizeof(int32_t));
    assert(allocation && !fg_checkpoint_allocation_bytes(SIZE_MAX, 8));
    assert(!fg_checkpoint_allocation_bytes(1, SIZE_MAX));
    options.max_bytes = base + namespaces + 2 * allocation - 1;
    forge_error error = {0};
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_OK);
    run_ok(&f, "AAA tail", &request);
    assert(stats(&f).resident_bytes == base + namespaces + allocation);
    run_ok(&f, "BBB tail", &request);
    assert(stats(&f).entries == 1 && stats(&f).evictions == 1);
    assert(stats(&f).resident_bytes == base + namespaces + allocation);
    assert(stats(&f).peak_bytes > stats(&f).resident_bytes); /* Probe scratch was charged. */
    size_t resident = stats(&f).resident_bytes;
    options.max_bytes = base - 1;
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_ERR_ARGUMENT);
    assert(stats(&f).resident_bytes == resident); /* Invalid config preserves the cache. */

    options.max_bytes = base;
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_OK);
    run_ok(&f, "AAA tail", &request);
    assert(stats(&f).resident_bytes == base && stats(&f).entries == 0 && stats(&f).skipped_budget);

    configure(&f, 2);
    f.extra_probe_bytes = SIZE_MAX;
    run_ok(&f, "AAA tail", &request);
    assert(stats(&f).skipped_budget && stats(&f).entries == 0);
    f.extra_probe_bytes = 0;
    f.size_override = FORGE_CHECKPOINT_MAX_STATE_BYTES + 1;
    clear_live(&f.model);
    size_t gets = f.get_calls;
    run_ok(&f, "AAA tail", &request);
    assert(f.get_calls == gets && stats(&f).entries == 0);
    f.size_override = SIZE_MAX; /* zero reported size */
    clear_live(&f.model);
    run_ok(&f, "AAA tail", &request);
    assert(f.get_calls == gets && stats(&f).capture_failures);
    f.size_override = 0;
    f.short_get = true;
    clear_live(&f.model);
    run_ok(&f, "AAA tail", &request);
    assert(stats(&f).entries == 0 && stats(&f).resident_bytes == base + namespaces);
    f.short_get = false;
    f.fail_live_match = true;
    clear_live(&f.model);
    gets = f.get_calls;
    run_ok(&f, "AAA tail", &request);
    assert(f.get_calls == gets && stats(&f).entries == 0);
    f.fail_live_match = false;
    f.bad_probe_count = true;
    clear_live(&f.model);
    run_ok(&f, "AAA tail", &request);
    assert(stats(&f).entries == 0 && stats(&f).probe_failures);
    finish(&f);
}

static void test_failed_restore_falls_back_once(void) {
    for (unsigned failure = 0; failure < 2; failure++) {
        fixture f;
        init(&f);
        configure(&f, 8);
        size_t anchors[] = {3, 6};
        forge_checkpoint_cache_request request = request_for(anchors, 2);
        run_ok(&f, "ABCDEF tail", &request);
        clear_live(&f.model);
        f.short_set = failure == 0;
        f.fail_accept = failure == 1;
        size_t clears = f.clear_calls;
        forge_metrics metrics = run_ok(&f, "ABCDEF tail", &request);
        assert(f.set_calls == 1 && f.clear_calls > clears);
        assert(metrics.cached_tokens == 0 && metrics.prefill_tokens == metrics.prompt_tokens);
        assert(stats(&f).hits == 0 && stats(&f).restore_failures == 1);
        assert(stats(&f).tokens_restored == 0 && f.physical);
        finish(&f);
    }
}

static void test_cancellation_deadline_and_reentry(void) {
    fixture f;
    init(&f);
    configure(&f, 8);
    size_t anchor = 3;
    forge_checkpoint_cache_request request = request_for(&anchor, 1);
    forge_metrics metrics;
    forge_error error = {0};
    f.user_cancelled = true;
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_CANCELLED);
    assert(!f.generate_calls && !f.probe_calls && !f.get_calls && !f.set_calls);
    f.user_cancelled = false;
    f.deadline = fg_now_ms() - 1;
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_CANCELLED);
    assert(!f.generate_calls);
    f.deadline = 0;
    f.reenter = true;
    run_ok(&f, "AAA tail", &request);
    assert(f.reentered && stats(&f).captures == 1);
    finish(&f);

    const stage stages[] = {STAGE_PROBE, STAGE_SIZE, STAGE_GET, STAGE_SET};
    for (size_t i = 0; i < sizeof(stages) / sizeof(*stages); i++) {
        init(&f);
        configure(&f, 8);
        if (stages[i] == STAGE_SET) {
            run_ok(&f, "AAA tail", &request);
            clear_live(&f.model);
        }
        f.cancel_stage = stages[i];
        assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_CANCELLED);
        assert(!f.physical && !f.count && stats(&f).cancellations == 1);
        assert(stats(&f).entries == 0);
        finish(&f);
    }
    for (unsigned after_write = 0; after_write < 2; after_write++) {
        init(&f);
        configure(&f, 8);
        if (after_write) {
            run_ok(&f, "AAA tail", &request);
            clear_live(&f.model);
        }
        f.deadline = fg_now_ms() + 30;
        f.expire_stage = after_write ? STAGE_SET : STAGE_PROBE;
        assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_CANCELLED);
        assert(!f.physical && stats(&f).entries == 0 && stats(&f).cancellations == 1);
        finish(&f);
    }
}

static void test_api_validation_ablation_and_limits(void) {
    fixture f;
    init(&f);
    assert(!stats(&f).enabled);
    size_t anchor = 3;
    forge_checkpoint_cache_request request = request_for(&anchor, 1);
    run_ok(&f, "AAA tail", &request);
    assert(!f.probe_calls && !f.get_calls);
    forge_checkpoint_cache_options options = forge_default_checkpoint_cache_options();
    forge_error error = {0};
    f.unsupported = true;
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_ERR_UNSUPPORTED);
    assert(!stats(&f).enabled);
    f.unsupported = false;
    options = configure(&f, 8);
    forge_metrics metrics;
    assert(forge_complete(&f.model, "AAA tail", 1, NULL, NULL, &metrics, &error) == FORGE_OK);
    assert(!f.probe_calls && stats(&f).skipped_no_request == 1);
    f.model.config.reuse_prefix = false;
    metrics = run_ok(&f, "AAA tail", &request);
    assert(!metrics.cached_tokens && !f.probe_calls && !f.get_calls &&
           stats(&f).skipped_ablation == 1);
    f.model.config.reuse_prefix = true;
    request.anchor_count = 0;
    run_ok(&f, "AAA tail", &request);
    assert(!f.probe_calls && stats(&f).skipped_no_anchor == 1);
    request.anchor_count = 1;
    f.unsupported = true;
    run_ok(&f, "AAA tail", &request);
    assert(!f.probe_calls && stats(&f).skipped_unsupported == 1);
    f.unsupported = false;

    size_t calls = f.generate_calls;
    const char *invalid_names[] = {NULL, "", "bad\xff"};
    for (size_t i = 0; i < sizeof(invalid_names) / sizeof(*invalid_names); i++) {
        request.workspace = invalid_names[i];
        assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    }
    char oversized[FORGE_CHECKPOINT_CACHE_MAX_WORKSPACE_BYTES + 2];
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    request.workspace = oversized;
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    request = request_for(&anchor, 1);
    request.context_id = oversized;
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    request = request_for(&anchor, 1);
    const size_t bad_anchors[][2] = {{0, 1}, {3, 3}, {4, 3}, {3, 99}};
    for (size_t i = 0; i < sizeof(bad_anchors) / sizeof(*bad_anchors); i++) {
        request.anchor_ends = bad_anchors[i];
        request.anchor_count = 2;
        assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    }
    request = request_for(&anchor, FORGE_CHECKPOINT_CACHE_MAX_ANCHORS + 1);
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    request = request_for(NULL, 1);
    assert(run(&f, "AAA tail", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    anchor = 1;
    request = request_for(&anchor, 1);
    assert(run(&f, "\xc3\xa9x", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    assert(run(&f, "bad\xff", &request, &metrics, &error) == FORGE_ERR_ARGUMENT);
    assert(f.generate_calls == calls); /* Invalid requests never invoke the backend. */
    anchor = 2;
    run_ok(&f, "\xc3\xa9x", &request);

    forge_checkpoint_cache_stats before = stats(&f);
    options.max_bytes = SIZE_MAX;
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_ERR_ARGUMENT);
    assert(stats(&f).entries == before.entries &&
           stats(&f).resident_bytes == before.resident_bytes);
    options = configure(&f, 8);
    options.max_captures_per_prompt = 1;
    assert(forge_checkpoint_cache_configure(&f.model, &options, &error) == FORGE_OK);
    size_t anchors[] = {2, 4, 6, 8};
    request = request_for(anchors, 4);
    clear_live(&f.model);
    run_ok(&f, "ABCDEFGH tail", &request);
    assert(stats(&f).captures == 1 && stats(&f).entries == 1);
    finish(&f);
}

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_suffix_merge_and_full_hit();
    test_longest_exact_prefix_and_live_choice();
    test_lru_and_scope_invalidation();
    test_aggregate_budget_and_capture_failures();
    test_failed_restore_falls_back_once();
    test_cancellation_deadline_and_reentry();
    test_api_validation_ablation_and_limits();
    puts("automatic checkpoint cache tests passed (host seam; not model evidence)");
    return 0;
}
