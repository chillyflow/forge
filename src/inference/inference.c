#include "internal.h"
forge_limits forge_default_limits(void) {
    return (forge_limits){16384,  2048,   32, 32768, 262144, 65536, 2u * 1024u * 1024u,
                          120000, 1800000};
}
forge_model_config forge_default_model_config(void) {
    return (forge_model_config){NULL, NULL, NULL, 16384, 0, 0, 42, 0.0f, true};
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
                                    size_t max_tokens, forge_token_fn cb, void *user, char **out,
                                    forge_metrics *stats, forge_cancel_fn cancel, void *cu,
                                    uint64_t deadline, forge_error *e) {
    (void)grammar;
    if ((cancel && cancel(cu)) || (deadline && fg_now_ms() >= deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Generation cancelled");
    yyjson_val *steps = yyjson_doc_get_root(m->script),
               *step = yyjson_arr_get(steps, m->script_cursor++);
    if (!step)
        return fg_error(e, FORGE_ERR_MODEL, "Scripted test fixture exhausted");
    char *json = yyjson_val_write(step, 0, NULL);
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
        if (m->destroy)
            m->destroy(m);
        if (m->script)
            yyjson_doc_free(m->script);
        free(m->previous_prompt);
        free(m);
    }
}
forge_status fg_model_generate(forge_model *m, const char *p, const char *g, size_t max_tokens,
                               forge_token_fn cb, void *u, char **out, forge_metrics *stats,
                               forge_cancel_fn cancel, void *cu, uint64_t deadline,
                               forge_error *e) {
    if (!m || !p || !max_tokens || !out || !stats)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid generation request");
    *out = NULL;
    return m->generate(m, p, g, max_tokens, cb, u, out, stats, cancel, cu, deadline, e);
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
