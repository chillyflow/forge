#include "internal.h"
forge_limits forge_default_limits(void) {
    return (forge_limits){16384,  2048,   32, 32768, 262144, 65536, 2u * 1024u * 1024u,
                          120000, 1800000};
}
forge_model_config forge_default_model_config(void) {
    return (forge_model_config){NULL, NULL, NULL, 16384, 0, 0, 42, 0.0f, true, true};
}
bool fg_model_instance_init(forge_model *m, forge_error *e) {
    if (!m || !fg_random_hex(m->instance_nonce, 16)) {
        fg_error(e, FORGE_ERR_IO, "Cannot create a unique model instance identity");
        return false;
    }
    m->next_checkpoint_id = 1;
    m->operation_active = false;
    return true;
}
static size_t script_count(forge_model *m, const char *text) {
    (void)m;
    return (strlen(text) + 3) / 4;
}
size_t fg_model_count(const char *text, void *model) {
    forge_model *m = model;
    return m->count(m, text);
}
static forge_status script_generate(forge_model *m, const char *prompt, const char *grammar,
                                    const char *grammar_trigger, size_t max_tokens,
                                    forge_token_fn cb, void *user, char **out, forge_metrics *stats,
                                    forge_cancel_fn cancel, void *cu, uint64_t deadline,
                                    forge_error *e) {
    (void)grammar;
    (void)grammar_trigger;
    if ((cancel && cancel(cu)) || (deadline && fg_now_ms() >= deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Generation cancelled");
    yyjson_val *steps = yyjson_doc_get_root(m->script),
               *step = yyjson_arr_get(steps, m->script_cursor++);
    if (!step)
        return fg_error(e, FORGE_ERR_MODEL, "Scripted test fixture exhausted");
    /* Object steps preserve the original concise fixture format. String steps
     * are exact raw model text, used to exercise routed reasoning before the
     * action JSON without giving the test backend special agent knowledge. */
    char *json = yyjson_is_str(step) && yyjson_get_len(step) == strlen(yyjson_get_str(step))
                     ? fg_strdup(yyjson_get_str(step))
                     : yyjson_val_write(step, 0, NULL);
    if (!json)
        return fg_error(e, FORGE_ERR_MEMORY, "Script allocation failed");
    size_t count = script_count(m, json);
    if (count > max_tokens) {
        free(json);
        return fg_error(e, FORGE_ERR_LIMIT, "Scripted response exceeds output reserve");
    }
    size_t common = 0;
    if (m->config.reuse_prefix && m->previous_prompt)
        while (prompt[common] && m->previous_prompt[common] &&
               prompt[common] == m->previous_prompt[common])
            common++;
    size_t tokens = script_count(m, prompt), cached = FG_MIN(common / 4, tokens);
    stats->simulated = true;
    stats->prompt_tokens += tokens;
    stats->cached_tokens += cached;
    stats->prefill_tokens += tokens - cached;
    stats->generated_tokens += count;
    free(m->previous_prompt);
    m->previous_prompt = fg_strdup(prompt);
    if (cb && !cb(json, strlen(json), user)) {
        free(json);
        return fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
    }
    *out = json;
    return FORGE_OK;
}
forge_model *forge_model_load(const forge_model_config *config, forge_error *e) {
    if (!config || config->context_tokens < 128 || config->context_tokens > 1048576 ||
        (!config->model_path == !config->script_path)) {
        fg_error(
            e, FORGE_ERR_ARGUMENT,
            "Choose exactly one model path or explicit script fixture, with valid context size");
        return NULL;
    }
    forge_model *m = calloc(1, sizeof(*m));
    if (!m) {
        fg_error(e, FORGE_ERR_MEMORY, "Model allocation failed");
        return NULL;
    }
    if (!fg_model_instance_init(m, e)) {
        free(m);
        return NULL;
    }
    m->config = *config;
    if (config->script_path) {
        char *data = fg_read_file(config->script_path, FG_MAX_JSON, NULL, e);
        if (!data) {
            free(m);
            return NULL;
        }
        m->script = yyjson_read(data, strlen(data), 0);
        free(data);
        if (!m->script || !yyjson_is_arr(yyjson_doc_get_root(m->script))) {
            forge_model_destroy(m);
            fg_error(e, FORGE_ERR_PARSE, "Script must be a JSON array of actions");
            return NULL;
        }
        m->count = script_count;
        m->generate = script_generate;
        return m;
    }
#ifdef FORGE_WITH_LLAMA
    if (!fg_llama_init(m, e)) {
        forge_model_destroy(m);
        return NULL;
    }
    return m;
#else
    free(m);
    fg_error(e, FORGE_ERR_MODEL,
             "This build has no inference backend. Rebuild with FORGE_WITH_LLAMA=ON; --script is "
             "only for tests.");
    return NULL;
#endif
}
void forge_model_destroy(forge_model *m) {
    if (m) {
        fg_checkpoint_cache_destroy(m->cache);
        if (m->destroy)
            m->destroy(m);
        if (m->script)
            yyjson_doc_free(m->script);
        free(m->previous_prompt);
        free(m);
    }
}
static void cache_metrics(forge_metrics *metrics, const forge_checkpoint_cache_stats *before,
                          const forge_checkpoint_cache_stats *after) {
#define CACHE_COUNT(field, source)                                                                 \
    do {                                                                                           \
        uint64_t change = after->source - before->source;                                          \
        metrics->field =                                                                           \
            change > UINT64_MAX - metrics->field ? UINT64_MAX : metrics->field + change;           \
    } while (0)
    CACHE_COUNT(checkpoint_lookups, lookups);
    CACHE_COUNT(checkpoint_hits, hits);
    CACHE_COUNT(checkpoint_misses, misses);
    CACHE_COUNT(checkpoint_captures, captures);
    CACHE_COUNT(checkpoint_evictions, evictions);
    CACHE_COUNT(checkpoint_restored_tokens, tokens_restored);
    CACHE_COUNT(checkpoint_reused_tokens, restored_tokens_reused);
    CACHE_COUNT(checkpoint_additional_tokens, additional_matched_tokens);
#undef CACHE_COUNT
    metrics->checkpoint_peak_bytes = FG_MAX(metrics->checkpoint_peak_bytes, after->peak_bytes);
    metrics->checkpoint_probe_ms += after->probe_ms - before->probe_ms;
    metrics->checkpoint_capture_ms += after->capture_ms - before->capture_ms;
    metrics->checkpoint_restore_ms += after->restore_ms - before->restore_ms;
}
forge_status fg_model_generate(forge_model *m, const char *p, const char *g, size_t max_tokens,
                               forge_token_fn cb, void *u, char **out, forge_metrics *stats,
                               forge_cancel_fn cancel, void *cu, uint64_t deadline,
                               forge_error *e) {
    return fg_model_generate_with_cache(m, p, g, max_tokens, cb, u, out, stats, cancel, cu,
                                        deadline, NULL, e);
}
forge_status fg_model_generate_active(forge_model *m, const char *prompt, size_t max_tokens,
                                      forge_token_fn callback, void *userdata, char **output,
                                      forge_metrics *metrics, forge_cancel_fn cancel,
                                      void *cancel_userdata, uint64_t deadline, forge_error *e) {
    if (!m || !m->operation_active || m->cache_request || !m->generate || !prompt || !max_tokens ||
        !output || !metrics)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid guarded generation request");
    *output = NULL;
    if ((cancel && cancel(cancel_userdata)) || (deadline && fg_now_ms() >= deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Generation cancelled before tokenization");
    return m->generate(m, prompt, NULL, NULL, max_tokens, callback, userdata, output, metrics,
                       cancel, cancel_userdata, deadline, e);
}
forge_status fg_model_generate_with_cache(forge_model *m, const char *p, const char *g,
                                          size_t max_tokens, forge_token_fn cb, void *u, char **out,
                                          forge_metrics *stats, forge_cancel_fn cancel, void *cu,
                                          uint64_t deadline,
                                          const forge_checkpoint_cache_request *request,
                                          forge_error *e) {
    return fg_model_generate_routed_with_cache(m, p, g, NULL, max_tokens, cb, u, out, stats, cancel,
                                               cu, deadline, request, e);
}
forge_status fg_model_generate_routed_with_cache(
    forge_model *m, const char *p, const char *g, const char *trigger, size_t max_tokens,
    forge_token_fn cb, void *u, char **out, forge_metrics *stats, forge_cancel_fn cancel, void *cu,
    uint64_t deadline, const forge_checkpoint_cache_request *request, forge_error *e) {
    if (!m || !m->generate || !p || !max_tokens || !out || !stats)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid generation request");
    if (trigger && (!g || !*trigger))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Lazy grammar requires grammar and trigger");
    *out = NULL;
    if (m->operation_active)
        return fg_error(e, FORGE_ERR_CONFLICT, "Another operation is active on this model");
    forge_status status = fg_checkpoint_cache_validate_request(p, request, e);
    if (status != FORGE_OK)
        return status;
    m->operation_active = true;
    m->cache_request = request;
    forge_checkpoint_cache_stats cache_before = {0}, cache_after = {0};
    forge_checkpoint_cache_get_stats(m, &cache_before);
    if ((cancel && cancel(cu)) || (deadline && fg_now_ms() >= deadline))
        status = fg_error(e, FORGE_ERR_CANCELLED, "Generation cancelled before tokenization");
    else
        status =
            m->generate(m, p, g, trigger, max_tokens, cb, u, out, stats, cancel, cu, deadline, e);
    forge_checkpoint_cache_get_stats(m, &cache_after);
    cache_metrics(stats, &cache_before, &cache_after);
    m->cache_request = NULL;
    m->operation_active = false;
    return status;
}
forge_status forge_complete(forge_model *m, const char *p, size_t n, forge_token_fn cb, void *u,
                            forge_metrics *stats, forge_error *e) {
    if (!stats)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Missing metrics");
    memset(stats, 0, sizeof(*stats));
    char *out = NULL;
    forge_status s = fg_model_generate(m, p, NULL, n, cb, u, &out, stats, NULL, NULL, 0, e);
    free(out);
    return s;
}
forge_status forge_complete_with_cache(forge_model *m, const char *prompt,
                                       const forge_checkpoint_cache_request *request,
                                       size_t max_tokens, forge_token_fn cb, void *user,
                                       forge_metrics *stats, forge_error *error) {
    if (!stats)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing metrics");
    memset(stats, 0, sizeof(*stats));
    char *output = NULL;
    forge_status status = fg_model_generate_with_cache(
        m, prompt, NULL, max_tokens, cb, user, &output, stats, NULL, NULL, 0, request, error);
    free(output);
    return status;
}
