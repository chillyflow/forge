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
    /* Tokens excluded while a routed reasoning prefix is being elicited. The
     * brace table holds every token whose piece contains '{' (any of them
     * could open the action object and fire the lazy trigger); the eog table
     * holds every end-of-generation token, banned until the action actually
     * begins so a routed generation cannot end actionless. Built once per
     * model on first routed generation. */
    llama_logit_bias *brace_ban, *eog_ban;
    int32_t brace_ban_count, eog_ban_count;
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
static void *token_allocate(const fg_checkpoint_allocator *allocator, size_t bytes,
                            forge_error *error) {
    if (allocator)
        return allocator->allocate(allocator->userdata, bytes, error);
    void *memory = malloc(bytes);
    if (!memory)
        fg_error(error, FORGE_ERR_MEMORY, "Tokenization allocation failed");
    return memory;
}
static void token_release(const fg_checkpoint_allocator *allocator, void *memory, size_t bytes) {
    if (allocator)
        allocator->release(allocator->userdata, memory, bytes);
    else
        free(memory);
}
static char *format_prompt(llama_state *s, const char *prompt,
                           const fg_checkpoint_allocator *allocator, size_t *allocated,
                           forge_error *error) {
    struct llama_chat_message message = {"user", prompt};
    int32_t n = llama_chat_apply_template(s->template_name, &message, 1, true, NULL, 0);
    if (n < 0 || n > 16 * 1024 * 1024) {
        fg_error(error, n < 0 ? FORGE_ERR_MODEL : FORGE_ERR_LIMIT,
                 "Cannot format bounded chat prompt");
        return NULL;
    }
    *allocated = (size_t)n + 1;
    char *text = token_allocate(allocator, *allocated, error);
    if (!text)
        return NULL;
    int32_t got = llama_chat_apply_template(s->template_name, &message, 1, true, text, n + 1);
    if (got < 0 || got > n) {
        token_release(allocator, text, *allocated);
        fg_error(error, FORGE_ERR_MODEL, "Chat template returned inconsistent size");
        return NULL;
    }
    text[got] = 0;
    return text;
}
static llama_token *tokenize_allocated(llama_state *s, const char *prompt, int32_t *count,
                                       const fg_checkpoint_allocator *allocator, size_t *allocated,
                                       forge_error *error) {
    size_t text_bytes = 0;
    char *text = format_prompt(s, prompt, allocator, &text_bytes, error);
    if (!text)
        return NULL;
    size_t len = strlen(text);
    if (len > INT32_MAX) {
        token_release(allocator, text, text_bytes);
        fg_error(error, FORGE_ERR_LIMIT, "Templated prompt is too large");
        return NULL;
    }
    int32_t n = llama_tokenize(s->vocab, text, (int32_t)len, NULL, 0, true, true);
    if (n >= 0 || n == INT32_MIN) {
        token_release(allocator, text, text_bytes);
        fg_error(error, FORGE_ERR_MODEL, "Cannot size prompt tokens");
        return NULL;
    }
    n = -n;
    if (n > 1048576) {
        token_release(allocator, text, text_bytes);
        fg_error(error, FORGE_ERR_LIMIT, "Prompt token count exceeds the runtime bound");
        return NULL;
    }
    *allocated = (size_t)n * sizeof(llama_token);
    llama_token *tokens = token_allocate(allocator, *allocated, error);
    if (!tokens) {
        token_release(allocator, text, text_bytes);
        return NULL;
    }
    *count = llama_tokenize(s->vocab, text, (int32_t)len, tokens, n, true, true);
    token_release(allocator, text, text_bytes);
    if (*count < 0 || *count > n) {
        token_release(allocator, tokens, *allocated);
        fg_error(error, FORGE_ERR_MODEL, "Cannot tokenize the complete prompt");
        return NULL;
    }
    return tokens;
}
static llama_token *tokenize(llama_state *s, const char *prompt, int32_t *count) {
    size_t bytes = 0;
    return tokenize_allocated(s, prompt, count, NULL, &bytes, NULL);
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
static size_t live_prefix(forge_model *m, const int32_t *tokens, size_t count) {
    llama_state *s = m->backend;
    size_t prefix = 0;
    if (m->config.reuse_prefix && s->can_reuse)
        while (prefix < count && prefix < s->count && tokens[prefix] == s->tokens[prefix])
            prefix++;
    llama_memory_t memory = llama_get_memory(s->ctx);
    if (prefix && (!memory || llama_memory_seq_pos_min(memory, 0) != 0 ||
                   llama_memory_seq_pos_max(memory, 0) < (llama_pos)(prefix - 1)))
        prefix = 0;
    return prefix;
}
/* Both generation and explicit checkpoint capture use this exact prefill path.
 * It never samples and only records a token suffix after successful decoding. */
static forge_status prefill_tokens(forge_model *m, const llama_token *tokens, size_t count,
                                   size_t output_reserve, forge_metrics *stats,
                                   forge_cancel_fn cancel, void *cu, uint64_t deadline,
                                   fg_checkpoint_cache_operation *cache, forge_error *e) {
    llama_state *s = m->backend;
    if (!count || count > s->capacity || output_reserve > s->capacity - count)
        return fg_error(e, FORGE_ERR_LIMIT, "Prompt plus output reserve exceeds model context");
    if (interrupted(cancel, cu, deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled before prefill");
    uint64_t begin = fg_now_ms();
    size_t prefix = live_prefix(m, tokens, count);
    /* Re-evaluate the final prompt token to obtain valid logits, even on an exact hit. */
    if (prefix == count && prefix)
        prefix--;
    llama_memory_t memory = llama_get_memory(s->ctx);
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
    fg_checkpoint_cache_note_reuse(cache, prefix);
    if (prefix && (status = fg_checkpoint_cache_capture(m, cache, prefix, e)) != FORGE_OK)
        goto done;
    for (size_t pos = prefix; pos < count;) {
        if (interrupted(cancel, cu, deadline)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled or deadline reached");
            goto done;
        }
        size_t end = fg_checkpoint_cache_next(cache, pos, pos + FG_MIN(count - pos, 512));
        size_t take = end - pos;
        status = decode_batch(s, tokens + pos, take, pos, e);
        if (status != FORGE_OK)
            goto done;
        memcpy(s->tokens + pos, tokens + pos, take * sizeof(*tokens));
        pos = end;
        s->count = pos;
        stats->prefill_tokens += take;
        status = fg_checkpoint_cache_capture(m, cache, pos, e);
        if (status != FORGE_OK)
            goto done;
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
static bool action_ban_ready(llama_state *s, forge_error *e) {
    if (s->brace_ban && s->eog_ban)
        return true;
    int32_t count = llama_vocab_n_tokens(s->vocab);
    llama_logit_bias *braces = count > 0 ? malloc((size_t)count * sizeof(*braces)) : NULL;
    llama_logit_bias *ends = count > 0 ? malloc((size_t)count * sizeof(*ends)) : NULL;
    if (!braces || !ends) {
        free(braces);
        free(ends);
        fg_error(e, FORGE_ERR_MEMORY, "Routed prefix suppression table allocation failed");
        return false;
    }
    int32_t brace_count = 0, eog_count = 0;
    for (llama_token token = 0; token < count; token++) {
        if (llama_vocab_is_eog(s->vocab, token)) {
            ends[eog_count].token = token;
            ends[eog_count].bias = -INFINITY;
            eog_count++;
            continue;
        }
        /* Render with special=true: the lazy trigger buffer accumulates the
         * grammar's own piece rendering, which includes special tokens, so
         * the ban must judge the same text the trigger will see. */
        char small[128], *piece = small;
        int32_t length =
            llama_token_to_piece(s->vocab, token, piece, (int32_t)sizeof(small), 0, true);
        if (length < 0) {
            piece = malloc((size_t)-length);
            if (!piece) {
                free(braces);
                free(ends);
                fg_error(e, FORGE_ERR_MEMORY, "Routed prefix suppression table allocation failed");
                return false;
            }
            length = llama_token_to_piece(s->vocab, token, piece, -length, 0, true);
        }
        bool ban = length > 0 && memchr(piece, '{', (size_t)length) != NULL;
        if (piece != small)
            free(piece);
        if (ban) {
            braces[brace_count].token = token;
            braces[brace_count].bias = -INFINITY;
            brace_count++;
        }
    }
    s->brace_ban = braces;
    s->brace_ban_count = brace_count;
    s->eog_ban = ends;
    s->eog_ban_count = eog_count;
    return true;
}
/* True once the output contains the start of an action object: '{', optional
 * JSON whitespace, a quoted tool/memory/final key, optional whitespace, ':'.
 * Mirrors FG_ACTION_TRIGGER_PATTERN so the host can tell when the lazy
 * grammar has armed and end-of-generation may be allowed again. */
static bool action_begun(const char *text) {
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
                return true;
        }
    }
    return false;
}
static bool complete_action_suffix(const char *text) {
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
static forge_status llama_generate(forge_model *m, const char *prompt, const char *grammar,
                                   const char *grammar_trigger, size_t max_tokens,
                                   forge_token_fn cb, void *u, char **output, forge_metrics *stats,
                                   forge_cancel_fn cancel, void *cu, uint64_t deadline,
                                   forge_error *e) {
    llama_state *s = m->backend;
    int32_t n = 0;
    llama_token *tokens = tokenize(s, prompt, &n);
    if (!tokens)
        return fg_error(e, FORGE_ERR_MODEL,
                        "Cannot tokenize prompt or apply chat template; use --chat-template chatml "
                        "for a compatible model");
    if (n <= 0 || (size_t)n > s->capacity || max_tokens > s->capacity - (size_t)n) {
        free(tokens);
        return fg_error(e, FORGE_ERR_LIMIT, "Prompt plus output reserve exceeds model context");
    }
    fg_checkpoint_cache_operation cache = {0};
    forge_status status =
        fg_checkpoint_cache_begin(m, prompt, tokens, (size_t)n, cancel, cu, deadline, &cache, e);
    if (status == FORGE_OK)
        status = prefill_tokens(m, tokens, (size_t)n, max_tokens, stats, cancel, cu, deadline,
                                &cache, e);
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
    /* §32 elicitation. A lazy trigger alone never elicits reasoning: the model
     * may open the action object on its first token and the grammar arms
     * immediately. While the budget lasts, action-opening tokens are excluded,
     * so decoding can only produce plain text; end-of-generation tokens stay
     * excluded until the action actually begins, so a routed generation cannot
     * end actionless after reasoning (the token budget remains the backstop).
     * Both bans sit ahead of the samplers: end ban at slot 0 until the action
     * begins, brace ban at slot 1 until the budget is spent. */
    size_t suppress = grammar_trigger ? FG_MIN(FG_THOUGHT_MIN_PREFIX_TOKENS, max_tokens / 4) : 0;
    bool awaiting_action = false;
    if (suppress) {
        if (!action_ban_ready(s, e)) {
            status = e && e->code ? e->code : FORGE_ERR_MEMORY;
            goto finish;
        }
        struct llama_sampler *ends = llama_sampler_init_logit_bias(
            llama_vocab_n_tokens(s->vocab), s->eog_ban_count, s->eog_ban);
        struct llama_sampler *braces =
            ends ? llama_sampler_init_logit_bias(llama_vocab_n_tokens(s->vocab),
                                                 s->brace_ban_count, s->brace_ban)
                 : NULL;
        if (!braces) {
            if (ends)
                llama_sampler_free(ends);
            status = fg_error(e, FORGE_ERR_MEMORY, "Routed prefix sampler allocation failed");
            goto finish;
        }
        llama_sampler_chain_add(sampler, ends);
        llama_sampler_chain_add(sampler, braces);
        awaiting_action = true;
    }
    if (grammar) {
        if (grammar_trigger) {
            const char *patterns[] = {grammar_trigger};
            grammar_sampler = llama_sampler_init_grammar_lazy_patterns(s->vocab, grammar, "root",
                                                                       patterns, 1, NULL, 0);
        } else
            grammar_sampler = llama_sampler_init_grammar(s->vocab, grammar, "root");
        if (!grammar_sampler) {
            status = fg_error(e, FORGE_ERR_PARSE,
                              "Generated tool grammar or trigger was rejected by llama.cpp");
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
        /* The action cannot begin while '{' is banned, so when the budget ends
         * the brace ban is still at slot 1 behind the end ban. */
        if (suppress && i == suppress) {
            llama_sampler_free(llama_sampler_chain_remove(sampler, 1));
            suppress = 0;
        }
        if (interrupted(cancel, cu, deadline)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled or deadline reached");
            break;
        }
        llama_synchronize(s->ctx);
        uint64_t sampling_start = fg_now_ms();
        /* The greedy fast path reads raw logits and would bypass the bans. */
        llama_token token = sample_token(s, sampler, grammar_sampler,
                                         !suppress && !awaiting_action &&
                                             m->config.grammar_fast_path &&
                                             m->config.temperature <= 0,
                                         stats);
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
        if (awaiting_action && !suppress && action_begun(out.data ? out.data : "")) {
            llama_sampler_free(llama_sampler_chain_remove(sampler, 0));
            awaiting_action = false;
        }
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
        bool complete = grammar_trigger ? complete_action_suffix(out.data ? out.data : "") : false;
        yyjson_doc *d = grammar_trigger ? NULL : yyjson_read(out.data ? out.data : "", out.len, 0);
        if (!complete && !d)
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
        prefill_tokens(m, tokens, (size_t)count, 0, stats, cancel, user, deadline, NULL, e);
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
static bool checkpoint_live_matches(forge_model *m, const int32_t *tokens, size_t count) {
    llama_state *s = m->backend;
    return s->count == count && checkpoint_positions(s, count) &&
           !memcmp(s->tokens, tokens, count * sizeof(*tokens));
}
static forge_status
checkpoint_probe_prefix(forge_model *m, const char *prompt, size_t end, const int32_t *full_tokens,
                        size_t full_count, size_t *common, const fg_checkpoint_allocator *allocator,
                        forge_cancel_fn cancel, void *user, uint64_t deadline, forge_error *error) {
    *common = 0;
    if (interrupted(cancel, user, deadline))
        return fg_error(error, FORGE_ERR_CANCELLED,
                        "Checkpoint probe cancelled before tokenization");
    char *prefix = token_allocate(allocator, end + 1, error);
    if (!prefix)
        return error && error->code ? error->code : FORGE_ERR_MEMORY;
    memcpy(prefix, prompt, end);
    prefix[end] = 0;
    int32_t count = 0;
    size_t token_bytes = 0;
    llama_token *tokens =
        tokenize_allocated(m->backend, prefix, &count, allocator, &token_bytes, error);
    token_release(allocator, prefix, end + 1);
    if (!tokens)
        return error && error->code ? error->code : FORGE_ERR_MODEL;
    for (size_t i = 0; i < (size_t)count && i < full_count && tokens[i] == full_tokens[i]; i++)
        (*common)++;
    token_release(allocator, tokens, token_bytes);
    if (interrupted(cancel, user, deadline))
        return fg_error(error, FORGE_ERR_CANCELLED,
                        "Checkpoint probe cancelled after tokenization");
    return FORGE_OK;
}
static const fg_checkpoint_backend checkpoint_backend = {
    checkpoint_supported, checkpoint_prefill,       checkpoint_state_size, checkpoint_state_get,
    checkpoint_state_set, checkpoint_accept_tokens, clear_live_state,      checkpoint_probe_prefix,
    live_prefix,          checkpoint_live_matches};
static void llama_destroy(forge_model *m) {
    llama_state *s = m->backend;
    if (s) {
        if (s->ctx)
            llama_free(s->ctx);
        if (s->model)
            llama_model_free(s->model);
        free(s->tokens);
        free(s->template_name);
        free(s->brace_ban);
        free(s->eog_ban);
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
