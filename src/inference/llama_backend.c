#include "internal.h"
#include "llama.h"
#include <math.h>
typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    llama_token *tokens;
    size_t count, capacity;
    char *template_name;
    double load_ms;
    bool can_reuse;
    const char *checkpoint_unsupported;
} llama_state;
static llama_token sample_token(llama_state *s, struct llama_sampler *sampler,
                                struct llama_sampler *grammar, bool fast, forge_metrics *stats) {
    /* For greedy sampling, an allowed global maximum is also the constrained
     * maximum. Grammar apply does not advance its state; accept does. Stochastic
     * sampling keeps the full mask to preserve its original distribution. */
    if (fast && grammar) {
        const float *logits = llama_get_logits_ith(s->ctx, -1);
        int32_t count = llama_vocab_n_tokens(s->vocab);
        if (logits && count > 0) {
            llama_token best = 0;
            for (llama_token i = 1; i < count; i++)
                if (logits[i] > logits[best])
                    best = i;
            llama_token_data candidate = {best, 1.0f, 0.0f};
            llama_token_data_array candidates = {&candidate, 1, -1, false};
            llama_sampler_apply(grammar, &candidates);
            if (candidate.logit != -INFINITY) {
                llama_sampler_accept(sampler, best);
                stats->grammar_fast_tokens++;
                return best;
            }
        }
    }
    if (grammar)
        stats->grammar_fallback_tokens++;
    return llama_sampler_sample(sampler, s->ctx, -1);
}
static char *format_prompt(llama_state *s, const char *prompt) {
    struct llama_chat_message message = {"user", prompt};
    int32_t n = llama_chat_apply_template(s->template_name, &message, 1, true, NULL, 0);
    if (n < 0 || n > 16 * 1024 * 1024)
        return NULL;
    char *text = malloc((size_t)n + 1);
    if (!text)
        return NULL;
    int32_t got = llama_chat_apply_template(s->template_name, &message, 1, true, text, n + 1);
    if (got < 0 || got > n) {
        free(text);
        return NULL;
    }
    text[got] = 0;
    return text;
}
static llama_token *tokenize(llama_state *s, const char *prompt, int32_t *count) {
    char *text = format_prompt(s, prompt);
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > INT32_MAX) {
        free(text);
        return NULL;
    }
    int32_t n = llama_tokenize(s->vocab, text, (int32_t)len, NULL, 0, true, true);
    if (n >= 0 || n == INT32_MIN) {
        free(text);
        return NULL;
    }
    n = -n;
    llama_token *tokens = malloc((size_t)n * sizeof(*tokens));
    if (!tokens) {
        free(text);
        return NULL;
    }
    *count = llama_tokenize(s->vocab, text, (int32_t)len, tokens, n, true, true);
    free(text);
    if (*count < 0) {
        free(tokens);
        return NULL;
    }
    return tokens;
}
static size_t llama_count(forge_model *m, const char *prompt) {
    int32_t count = 0;
    llama_token *t = tokenize(m->backend, prompt, &count);
    free(t);
    return count > 0 ? (size_t)count : SIZE_MAX / 4;
}
static bool interrupted(forge_cancel_fn cancel, void *u, uint64_t deadline) {
    return (cancel && cancel(u)) || (deadline && fg_now_ms() >= deadline);
}
static forge_status decode_batch(llama_state *s, const llama_token *tokens, size_t count,
                                 size_t pos, forge_error *e) {
    struct llama_batch b = llama_batch_init((int32_t)count, 0, 1);
    if (!b.token)
        return fg_error(e, FORGE_ERR_MEMORY, "llama batch allocation failed");
    b.n_tokens = (int32_t)count;
    for (size_t i = 0; i < count; i++) {
        b.token[i] = tokens[i];
        b.pos[i] = (llama_pos)(pos + i);
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = 0;
        b.logits[i] = (int8_t)(i + 1 == count);
    }
    int rc = llama_decode(s->ctx, b);
    llama_batch_free(b);
    if (rc)
        return fg_error(e, FORGE_ERR_MODEL, "llama_decode failed (%d)", rc);
    return FORGE_OK;
}
static void clear_live_state(forge_model *m) {
    llama_state *s = m->backend;
    llama_synchronize(s->ctx);
    llama_memory_clear(llama_get_memory(s->ctx), false);
    s->count = 0;
}
/* Both generation and explicit checkpoint capture use this exact prefill path.
 * It never samples and only records a token suffix after successful decoding. */
static forge_status prefill_tokens(forge_model *m, const llama_token *tokens, size_t count,
                                   size_t output_reserve, forge_metrics *stats,
                                   forge_cancel_fn cancel, void *cu, uint64_t deadline,
                                   forge_error *e) {
    llama_state *s = m->backend;
    if (!count || count > s->capacity || output_reserve > s->capacity - count)
        return fg_error(e, FORGE_ERR_LIMIT, "Prompt plus output reserve exceeds model context");
    if (interrupted(cancel, cu, deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled before prefill");
    uint64_t begin = fg_now_ms();
    size_t prefix = 0;
    if (m->config.reuse_prefix && s->can_reuse)
        while (prefix < count && prefix < s->count && tokens[prefix] == s->tokens[prefix])
            prefix++;
    /* Re-evaluate the final prompt token to obtain valid logits, even on an exact hit. */
    if (prefix == count && prefix)
        prefix--;
    /* A remembered token list alone does not prove the corresponding KV still
     * exists (for example after a sliding-window eviction or a failed decode). */
    llama_memory_t memory = llama_get_memory(s->ctx);
    if (prefix && (llama_memory_seq_pos_min(memory, 0) != 0 ||
                   llama_memory_seq_pos_max(memory, 0) < (llama_pos)(prefix - 1)))
        prefix = 0;
    if (!prefix)
        llama_memory_clear(memory, false);
    else if (!llama_memory_seq_rm(memory, 0, (llama_pos)prefix, -1)) {
        llama_memory_clear(memory, false);
        prefix = 0;
    }
    s->count = prefix;
    stats->prompt_tokens += count;
    stats->cached_tokens += prefix;
    stats->load_ms = s->load_ms;
    forge_status status = FORGE_OK;
    for (size_t pos = prefix; pos < count;) {
        if (interrupted(cancel, cu, deadline)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled or deadline reached");
            goto done;
        }
        size_t take = FG_MIN(count - pos, 512);
        status = decode_batch(s, tokens + pos, take, pos, e);
        if (status != FORGE_OK)
            goto done;
        pos += take;
        stats->prefill_tokens += take;
    }
    llama_synchronize(s->ctx);
    if (interrupted(cancel, cu, deadline)) {
        status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled after prefill");
        goto done;
    }
    memcpy(s->tokens, tokens, count * sizeof(*tokens));
    s->count = count;
done:
    if (status != FORGE_OK)
        clear_live_state(m);
    stats->prefill_ms += (double)(fg_now_ms() - begin);
    return status;
}
static forge_status llama_generate(forge_model *m, const char *prompt, const char *grammar,
                                   size_t max_tokens, forge_token_fn cb, void *u, char **output,
                                   forge_metrics *stats, forge_cancel_fn cancel, void *cu,
                                   uint64_t deadline, forge_error *e) {
    llama_state *s = m->backend;
    int32_t n = 0;
    llama_token *tokens = tokenize(s, prompt, &n);
    if (!tokens)
        return fg_error(e, FORGE_ERR_MODEL,
                        "Cannot tokenize prompt or apply chat template; use --chat-template chatml "
                        "for a compatible model");
    forge_status status =
        prefill_tokens(m, tokens, (size_t)n, max_tokens, stats, cancel, cu, deadline, e);
    free(tokens);
    if (status != FORGE_OK)
        return status;
    struct llama_sampler *sampler = NULL;
    struct llama_sampler *grammar_sampler = NULL;
    fg_buf out = {0};
    sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!sampler) {
        status = fg_error(e, FORGE_ERR_MEMORY, "Sampler allocation failed");
        goto finish;
    }
    if (grammar) {
        grammar_sampler = llama_sampler_init_grammar(s->vocab, grammar, "root");
        if (!grammar_sampler) {
            status =
                fg_error(e, FORGE_ERR_PARSE, "Generated tool grammar was rejected by llama.cpp");
            goto finish;
        }
        llama_sampler_chain_add(sampler, grammar_sampler);
    }
    if (m->config.temperature <= 0)
        llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
    else {
        llama_sampler_chain_add(sampler, llama_sampler_init_top_k(20));
        llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.8f, 1));
        llama_sampler_chain_add(sampler, llama_sampler_init_temp(m->config.temperature));
        llama_sampler_chain_add(sampler, llama_sampler_init_dist(m->config.seed));
    }
    uint64_t begin = fg_now_ms();
    bool ended = false;
    for (size_t i = 0; i < max_tokens; i++) {
        if (interrupted(cancel, cu, deadline)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled or deadline reached");
            break;
        }
        llama_synchronize(s->ctx);
        uint64_t sampling_start = fg_now_ms();
        llama_token token =
            sample_token(s, sampler, grammar_sampler,
                         m->config.grammar_fast_path && m->config.temperature <= 0, stats);
        stats->sampling_ms += (double)(fg_now_ms() - sampling_start);
        if (llama_vocab_is_eog(s->vocab, token)) {
            ended = true;
            break;
        }
        int32_t cap = 128;
        char small[128], *piece = small;
        int32_t length = llama_token_to_piece(s->vocab, token, piece, cap, 0, false);
        if (length < 0) {
            cap = -length;
            piece = malloc((size_t)cap);
            if (!piece) {
                status = FORGE_ERR_MEMORY;
                break;
            }
            length = llama_token_to_piece(s->vocab, token, piece, cap, 0, false);
        }
        if (length < 0 || !fg_buf_add(&out, piece, (size_t)FG_MAX(length, 0))) {
            if (piece != small)
                free(piece);
            status = fg_error(e, FORGE_ERR_MEMORY, "Token output failed");
            break;
        }
        stats->generated_tokens++;
        if (cb && !cb(piece, (size_t)length, u)) {
            if (piece != small)
                free(piece);
            status = fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
            break;
        }
        if (piece != small)
            free(piece);
        status = decode_batch(s, &token, 1, s->count, e);
        if (status != FORGE_OK)
            break;
        s->tokens[s->count++] = token;
    }
    stats->decode_ms += (double)(fg_now_ms() - begin);
    if (status == FORGE_OK && grammar && !ended) {
        yyjson_doc *d = yyjson_read(out.data ? out.data : "", out.len, 0);
        if (!d)
            status =
                fg_error(e, FORGE_ERR_LIMIT, "Generation limit reached before a complete action");
        yyjson_doc_free(d);
    }
finish:
    if (sampler)
        llama_sampler_free(sampler);
    if (status != FORGE_OK) {
        clear_live_state(m);
        fg_buf_clear(&out);
        return status;
    }
    *output = fg_buf_take(&out);
    return *output ? FORGE_OK : fg_error(e, FORGE_ERR_MEMORY, "Output allocation failed");
}
static forge_status checkpoint_supported(forge_model *m, forge_error *e) {
    llama_state *s = m->backend;
    if (s->checkpoint_unsupported)
        return fg_error(e, FORGE_ERR_UNSUPPORTED, "Physical checkpoints unsupported: %s",
                        s->checkpoint_unsupported);
    return FORGE_OK;
}
static bool checkpoint_positions(llama_state *s, size_t count) {
    llama_memory_t memory = llama_get_memory(s->ctx);
    return count && count <= s->capacity && memory && llama_memory_seq_pos_min(memory, 0) == 0 &&
           llama_memory_seq_pos_max(memory, 0) == (llama_pos)(count - 1);
}
static forge_status checkpoint_prefill(forge_model *m, const char *prompt, int32_t **out_tokens,
                                       size_t *out_count, forge_metrics *stats,
                                       forge_cancel_fn cancel, void *user, uint64_t deadline,
                                       forge_error *e) {
    llama_state *s = m->backend;
    *out_tokens = NULL;
    *out_count = 0;
    if (interrupted(cancel, user, deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Checkpoint cancelled before tokenization");
    int32_t count = 0;
    llama_token *tokens = tokenize(s, prompt, &count);
    if (!tokens)
        return fg_error(e, FORGE_ERR_MODEL,
                        "Cannot tokenize checkpoint prompt or apply chat template");
    forge_status status =
        prefill_tokens(m, tokens, (size_t)count, 0, stats, cancel, user, deadline, e);
    if (status == FORGE_OK && !checkpoint_positions(s, (size_t)count))
        status =
            fg_error(e, FORGE_ERR_UNSUPPORTED,
                     "Physical checkpoint requires a complete sequence starting at position 0");
    if (status != FORGE_OK) {
        free(tokens);
        clear_live_state(m);
        return status;
    }
    *out_tokens = tokens;
    *out_count = (size_t)count;
    return FORGE_OK;
}
static size_t checkpoint_state_size(forge_model *m) {
    llama_state *s = m->backend;
    llama_synchronize(s->ctx);
    return llama_state_seq_get_size(s->ctx, 0);
}
static size_t checkpoint_state_get(forge_model *m, uint8_t *bytes, size_t size) {
    llama_state *s = m->backend;
    /* Ordinary host bytes keep separate saves independent. ON_DEVICE explicitly
     * invalidates previous same-sequence saves in the pinned llama.cpp API. */
    return llama_state_seq_get_data(s->ctx, bytes, size, 0);
}
static size_t checkpoint_state_set(forge_model *m, const uint8_t *bytes, size_t size) {
    llama_state *s = m->backend;
    s->count = 0;
    return llama_state_seq_set_data(s->ctx, bytes, size, 0);
}
static bool checkpoint_accept_tokens(forge_model *m, const int32_t *tokens, size_t count) {
    llama_state *s = m->backend;
    if (!checkpoint_positions(s, count))
        return false;
    int32_t vocab_size = llama_vocab_n_tokens(s->vocab);
    for (size_t i = 0; i < count; i++)
        if (tokens[i] < 0 || tokens[i] >= vocab_size)
            return false;
    memcpy(s->tokens, tokens, count * sizeof(*tokens));
    s->count = count;
    return true;
}
static const fg_checkpoint_backend checkpoint_backend = {
    checkpoint_supported, checkpoint_prefill,       checkpoint_state_size, checkpoint_state_get,
    checkpoint_state_set, checkpoint_accept_tokens, clear_live_state};
static void llama_destroy(forge_model *m) {
    llama_state *s = m->backend;
    if (s) {
        if (s->ctx)
            llama_free(s->ctx);
        if (s->model)
            llama_model_free(s->model);
        free(s->tokens);
        free(s->template_name);
        free(s);
    }
}
bool fg_llama_init(forge_model *m, forge_error *e) {
    uint64_t start = fg_now_ms();
    llama_state *s = calloc(1, sizeof(*s));
    if (!s) {
        fg_error(e, FORGE_ERR_MEMORY, "Backend allocation failed");
        return false;
    }
    m->backend = s;
    m->destroy = llama_destroy;
    /* Backend registration is owned by llama.cpp. Forge has no mutable global session state. */
    ggml_backend_load_all();
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = m->config.gpu_layers;
    s->model = llama_model_load_from_file(m->config.model_path, mp);
    if (!s->model) {
        fg_error(e, FORGE_ERR_MODEL, "Failed to load GGUF model");
        return false;
    }
    s->vocab = llama_model_get_vocab(s->model);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)m->config.context_tokens;
    cp.n_batch = 512;
    cp.n_ubatch = 256;
    if (m->config.threads > 0) {
        cp.n_threads = m->config.threads;
        cp.n_threads_batch = m->config.threads;
    }
    s->ctx = llama_init_from_model(s->model, cp);
    if (!s->ctx) {
        fg_error(e, FORGE_ERR_MODEL,
                 "Failed to allocate inference context; reduce --context or --gpu-layers");
        return false;
    }
    s->capacity = llama_n_ctx(s->ctx);
    s->tokens = malloc(s->capacity * sizeof(*s->tokens));
    const char *tmpl = m->config.chat_template ? m->config.chat_template
                                               : llama_model_chat_template(s->model, NULL);
    s->template_name = fg_strdup(tmpl ? tmpl : "chatml");
    if (!s->tokens || !s->template_name) {
        fg_error(e, FORGE_ERR_MEMORY, "Inference state allocation failed");
        return false;
    }
    s->can_reuse = !llama_model_is_recurrent(s->model) && !llama_model_is_hybrid(s->model);
    if (llama_model_has_encoder(s->model) || !llama_model_has_decoder(s->model))
        s->checkpoint_unsupported = "encoder/non-decoder models";
    else if (llama_model_is_recurrent(s->model))
        s->checkpoint_unsupported = "recurrent models";
    else if (llama_model_is_hybrid(s->model))
        s->checkpoint_unsupported = "hybrid models";
    else if (llama_model_is_diffusion(s->model))
        s->checkpoint_unsupported = "diffusion models";
    else if (llama_model_n_swa(s->model) > 0)
        s->checkpoint_unsupported = "sliding-window attention models";
    else if (!llama_get_memory(s->ctx))
        s->checkpoint_unsupported = "models without sequence memory";
    s->load_ms = (double)(fg_now_ms() - start);
    m->count = llama_count;
    m->generate = llama_generate;
    m->checkpoint = &checkpoint_backend;
    return true;
}
