#include "internal.h"
#include "chat_template.h"
#include "llama.h"
#include <ctype.h>
#include <math.h>
typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    llama_token *tokens;
    size_t count, capacity;
    char *template_name;
    fg_chat_templates *chat_templates;
    fg_chat_render *last_native_render;
    forge_thinking_mode thinking_mode;
    double load_ms;
    bool can_reuse;
    const char *checkpoint_unsupported;
    /* Tokens excluded while a routed reasoning prefix is being elicited. The
     * brace table holds every token whose piece contains '{' (any of them
     * could open the action object and fire the lazy trigger); the eog table
     * holds every end-of-generation token, banned until the action actually
     * begins so a routed generation cannot end actionless. Built once per
     * model on first routed generation. */
    llama_logit_bias *brace_ban, *eog_ban, *whitespace_ban;
    int32_t brace_ban_count, eog_ban_count, whitespace_ban_count;
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
    if (s->chat_templates) {
        size_t rendered_length = 0;
        char detail[256] = {0};
        char *rendered = fg_chat_templates_apply(s->chat_templates, prompt,
                                                 s->thinking_mode == FORGE_THINKING_ENABLED,
                                                 &rendered_length, detail, sizeof(detail));
        if (!rendered) {
            fg_error(error, FORGE_ERR_MODEL, "Cannot apply Jinja chat template: %s", detail);
            return NULL;
        }
        *allocated = rendered_length + 1;
        char *text = token_allocate(allocator, *allocated, error);
        if (!text) {
            free(rendered);
            return NULL;
        }
        memcpy(text, rendered, rendered_length + 1);
        free(rendered);
        return text;
    }
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
static llama_token *tokenize_text_allocated(llama_state *s, const char *text, int32_t *count,
                                            const fg_checkpoint_allocator *allocator,
                                            size_t *allocated, forge_error *error) {
    size_t len = strlen(text);
    if (len > INT32_MAX) {
        fg_error(error, FORGE_ERR_LIMIT, "Templated prompt is too large");
        return NULL;
    }
    int32_t n = llama_tokenize(s->vocab, text, (int32_t)len, NULL, 0, true, true);
    if (n >= 0 || n == INT32_MIN) {
        fg_error(error, FORGE_ERR_MODEL, "Cannot size prompt tokens");
        return NULL;
    }
    n = -n;
    if (n > 1048576) {
        fg_error(error, FORGE_ERR_LIMIT, "Prompt token count exceeds the runtime bound");
        return NULL;
    }
    *allocated = (size_t)n * sizeof(llama_token);
    llama_token *tokens = token_allocate(allocator, *allocated, error);
    if (!tokens)
        return NULL;
    *count = llama_tokenize(s->vocab, text, (int32_t)len, tokens, n, true, true);
    if (*count < 0 || *count > n) {
        token_release(allocator, tokens, *allocated);
        fg_error(error, FORGE_ERR_MODEL, "Cannot tokenize the complete prompt");
        return NULL;
    }
    return tokens;
}
static llama_token *tokenize_allocated(llama_state *s, const char *prompt, int32_t *count,
                                       const fg_checkpoint_allocator *allocator, size_t *allocated,
                                       forge_error *error) {
    size_t text_bytes = 0;
    char *text = format_prompt(s, prompt, allocator, &text_bytes, error);
    if (!text)
        return NULL;
    llama_token *tokens = tokenize_text_allocated(s, text, count, allocator, allocated, error);
    token_release(allocator, text, text_bytes);
    return tokens;
}
static llama_token *tokenize(llama_state *s, const char *prompt, int32_t *count) {
    size_t bytes = 0;
    return tokenize_allocated(s, prompt, count, NULL, &bytes, NULL);
}
static size_t llama_count(forge_model *m, const char *prompt) {
    llama_state *s = m->backend;
    int32_t count = 0;
    llama_token *tokens = tokenize(s, prompt, &count);
    free(tokens);
    return count > 0 ? (size_t)count : SIZE_MAX / 4;
}

static size_t llama_count_prompt(forge_model *m, const char *prompt) {
    if (m->config.prompt_protocol != FORGE_PROMPT_NATIVE)
        return llama_count(m, prompt);
    llama_state *s = m->backend;
    char detail[256] = {0};
    fg_chat_render *render = fg_chat_templates_apply_native(
        s->chat_templates, prompt, s->thinking_mode != FORGE_THINKING_DISABLED, detail,
        sizeof(detail));
    size_t length = 0, bytes = 0;
    const char *text = fg_chat_render_prompt(render, &length);
    (void)length;
    int32_t count = 0;
    llama_token *tokens =
        text ? tokenize_text_allocated(s, text, &count, NULL, &bytes, NULL) : NULL;
    free(tokens);
    fg_chat_render_destroy(render);
    return count > 0 ? (size_t)count : SIZE_MAX / 4;
}
static bool interrupted(forge_cancel_fn cancel, void *u, uint64_t deadline) {
    return (cancel && cancel(u)) || (deadline && fg_now_ms() >= deadline);
}

static forge_status native_grammar_prefill(llama_state *s, struct llama_sampler *grammar,
                                           const fg_chat_render *render, forge_error *error) {
    if (!grammar || fg_chat_render_grammar_lazy(render))
        return FORGE_OK;
    const char *prefix = fg_chat_render_generation_prompt(render);
    if (!prefix || !*prefix)
        return FORGE_OK;
    size_t length = strlen(prefix);
    if (length > INT32_MAX)
        return fg_error(error, FORGE_ERR_LIMIT,
                        "Native grammar generation prefix exceeds the runtime bound");
    int32_t count = llama_tokenize(s->vocab, prefix, (int32_t)length, NULL, 0, false, true);
    if (count >= 0 || count == INT32_MIN)
        return fg_error(error, FORGE_ERR_MODEL,
                        "Cannot size native grammar generation-prefix tokens");
    count = -count;
    llama_token *tokens = malloc((size_t)count * sizeof(*tokens));
    if (!tokens)
        return fg_error(error, FORGE_ERR_MEMORY,
                        "Cannot allocate native grammar generation-prefix tokens");
    int32_t got = llama_tokenize(s->vocab, prefix, (int32_t)length, tokens, count, false, true);
    if (got < 0 || got > count) {
        free(tokens);
        return fg_error(error, FORGE_ERR_MODEL, "Cannot tokenize native grammar generation prefix");
    }
    for (int32_t i = 0; i < got; i++) {
        bool skip = false;
        if (!i) {
            char small[128], *piece = small;
            int32_t piece_length =
                llama_token_to_piece(s->vocab, tokens[i], piece, (int32_t)sizeof(small), 0, true);
            if (piece_length < 0) {
                piece = malloc((size_t)-piece_length);
                if (!piece) {
                    free(tokens);
                    return fg_error(error, FORGE_ERR_MEMORY,
                                    "Cannot inspect native grammar prefix token");
                }
                piece_length =
                    llama_token_to_piece(s->vocab, tokens[i], piece, -piece_length, 0, true);
            }
            if (piece_length > 0)
                skip = isspace((unsigned char)piece[0]) && !isspace((unsigned char)prefix[0]);
            if (piece != small)
                free(piece);
            if (piece_length < 0) {
                free(tokens);
                return fg_error(error, FORGE_ERR_MODEL,
                                "Cannot inspect native grammar prefix token");
            }
        }
        if (!skip)
            llama_sampler_accept(grammar, tokens[i]);
    }
    free(tokens);
    return FORGE_OK;
}

static forge_status native_preserved_tokens(llama_state *s, const fg_chat_render *render,
                                            llama_token *tokens, size_t capacity, size_t *count,
                                            forge_error *error) {
    *count = 0;
    size_t strings = fg_chat_render_preserved_count(render);
    if (strings > capacity)
        return fg_error(error, FORGE_ERR_LIMIT,
                        "Native template returned too many preserved tokens");
    for (size_t i = 0; i < strings; i++) {
        const char *text = fg_chat_render_preserved(render, i);
        llama_token token = LLAMA_TOKEN_NULL;
        int32_t found =
            text ? llama_tokenize(s->vocab, text, (int32_t)strlen(text), &token, 1, false, true)
                 : 0;
        if (found != 1)
            continue; /* Matches llama.cpp: only single-token markers are preserved. */
        bool duplicate = false;
        for (size_t j = 0; j < *count; j++)
            duplicate |= tokens[j] == token;
        if (!duplicate)
            tokens[(*count)++] = token;
    }
    return FORGE_OK;
}

static bool native_token_is_preserved(llama_token token, const llama_token *preserved,
                                      size_t count) {
    for (size_t i = 0; i < count; i++)
        if (preserved[i] == token)
            return true;
    return false;
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
    if (s->brace_ban && s->eog_ban && s->whitespace_ban)
        return true;
    int32_t count = llama_vocab_n_tokens(s->vocab);
    llama_logit_bias *braces = count > 0 ? malloc((size_t)count * sizeof(*braces)) : NULL;
    llama_logit_bias *ends = count > 0 ? malloc((size_t)count * sizeof(*ends)) : NULL;
    llama_logit_bias *whitespace = count > 0 ? malloc((size_t)count * sizeof(*whitespace)) : NULL;
    if (!braces || !ends || !whitespace) {
        free(braces);
        free(ends);
        free(whitespace);
        fg_error(e, FORGE_ERR_MEMORY, "Routed prefix suppression table allocation failed");
        return false;
    }
    int32_t brace_count = 0, eog_count = 0, whitespace_count = 0;
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
                free(whitespace);
                fg_error(e, FORGE_ERR_MEMORY, "Routed prefix suppression table allocation failed");
                return false;
            }
            length = llama_token_to_piece(s->vocab, token, piece, -length, 0, true);
        }
        bool ban = length > 0 && memchr(piece, '{', (size_t)length) != NULL;
        bool whitespace_only = length > 0 && fg_json_whitespace_only(piece, (size_t)length);
        if (piece != small)
            free(piece);
        if (ban) {
            braces[brace_count].token = token;
            braces[brace_count].bias = -INFINITY;
            brace_count++;
        }
        if (whitespace_only) {
            whitespace[whitespace_count].token = token;
            whitespace[whitespace_count].bias = -INFINITY;
            whitespace_count++;
        }
    }
    s->brace_ban = braces;
    s->brace_ban_count = brace_count;
    s->eog_ban = ends;
    s->eog_ban_count = eog_count;
    s->whitespace_ban = whitespace;
    s->whitespace_ban_count = whitespace_count;
    return true;
}
/* Think budget expired without an action: replace the never-triggered lazy
 * grammar at slot 0 (after conditional ban removal) with an eager grammar over
 * the same GBNF, so the action must open under full constraint — tool
 * constraints retained, all three envelope alternatives open. The end ban is
 * dropped here because a fresh root grammar masks end-of-generation until the
 * action object closes. Selector samplers are re-added in their original
 * order behind the new grammar. The lazy sampler's un-fired trigger buffer is
 * host text the caller already holds, so freeing it loses nothing. */
static forge_status force_action(llama_state *s, struct llama_sampler *chain,
                                 struct llama_sampler **grammar_sampler, const char *grammar,
                                 bool eog_banned, bool *progress_armed, forge_error *e) {
    if (eog_banned)
        llama_sampler_free(llama_sampler_chain_remove(chain, 0));
    llama_sampler_free(llama_sampler_chain_remove(chain, 0));
    *grammar_sampler = NULL;
    struct llama_sampler *eager = llama_sampler_init_grammar(s->vocab, grammar, "root");
    struct llama_sampler *progress =
        eager ? llama_sampler_init_logit_bias(llama_vocab_n_tokens(s->vocab),
                                              s->whitespace_ban_count, s->whitespace_ban)
              : NULL;
    if (!eager || !progress) {
        if (eager)
            llama_sampler_free(eager);
        return fg_error(e, FORGE_ERR_PARSE, "Action grammar was rejected while forcing the action");
    }
    int selectors = llama_sampler_chain_n(chain);
    struct llama_sampler *tail[4];
    if (selectors < 0 || (size_t)selectors > sizeof(tail) / sizeof(*tail)) {
        llama_sampler_free(eager);
        llama_sampler_free(progress);
        return fg_error(e, FORGE_ERR_ARGUMENT, "Unexpected sampler chain while forcing the action");
    }
    for (int i = selectors - 1; i >= 0; i--)
        tail[i] = llama_sampler_chain_remove(chain, i);
    /* A fresh root permits ws*.  Keep every whitespace-only token masked until
     * the first token that advances into the object.  Tokens containing both
     * whitespace and '{' remain legal, preserving tokenizer portability. */
    llama_sampler_chain_add(chain, progress);
    llama_sampler_chain_add(chain, eager);
    for (int i = 0; i < selectors; i++)
        llama_sampler_chain_add(chain, tail[i]);
    *grammar_sampler = eager;
    *progress_armed = true;
    return FORGE_OK;
}
static forge_status llama_generate(forge_model *m, const char *prompt, const char *grammar,
                                   const fg_decode_policy *policy, size_t max_tokens,
                                   forge_token_fn cb, void *u, char **output, forge_metrics *stats,
                                   forge_cancel_fn cancel, void *cu, uint64_t deadline,
                                   forge_error *e) {
    llama_state *s = m->backend;
    bool native = m->config.prompt_protocol == FORGE_PROMPT_NATIVE;
    fg_chat_render *native_render = NULL;
    const char *prepared_prompt = prompt;
    const char *active_grammar = grammar;
    int32_t n = 0;
    llama_token *tokens = NULL;
    fg_chat_render_destroy(s->last_native_render);
    s->last_native_render = NULL;
    if (native) {
        if (policy)
            return fg_error(e, FORGE_ERR_ARGUMENT,
                            "Native prompt protocol cannot use routed JSON decoding");
        char detail[256] = {0};
        native_render = fg_chat_templates_apply_native(s->chat_templates, prompt,
                                                       s->thinking_mode != FORGE_THINKING_DISABLED,
                                                       detail, sizeof(detail));
        size_t prepared_length = 0;
        prepared_prompt = fg_chat_render_prompt(native_render, &prepared_length);
        (void)prepared_length;
        active_grammar = fg_chat_render_grammar(native_render);
        size_t token_bytes = 0;
        if (prepared_prompt)
            tokens = tokenize_text_allocated(s, prepared_prompt, &n, NULL, &token_bytes, e);
        if (!native_render || !prepared_prompt || !active_grammar || !tokens) {
            fg_chat_render_destroy(native_render);
            if (!e || !e->code)
                fg_error(e, FORGE_ERR_UNSUPPORTED, "Cannot prepare native roles/tools: %s",
                         *detail ? detail : "unsupported chat template");
            return e && e->code ? e->code : FORGE_ERR_MODEL;
        }
    } else {
        tokens = tokenize(s, prompt, &n);
        if (!tokens)
            return fg_error(
                e, FORGE_ERR_MODEL,
                "Cannot tokenize prompt or apply chat template; use --chat-template chatml "
                "for a compatible model");
    }
    if (n <= 0 || (size_t)n > s->capacity || max_tokens > s->capacity - (size_t)n) {
        free(tokens);
        fg_chat_render_destroy(native_render);
        return fg_error(e, FORGE_ERR_LIMIT, "Prompt plus output reserve exceeds model context");
    }
    fg_checkpoint_cache_operation cache = {0};
    forge_checkpoint_cache_request native_request = {0};
    const forge_checkpoint_cache_request *logical_request = m->cache_request;
    size_t native_anchor = 0;
    if (native && logical_request) {
        native_request = *logical_request;
        if (logical_request->anchor_count)
            native_anchor = fg_chat_render_cache_anchor(native_render);
        native_request.anchor_ends = &native_anchor;
        native_request.anchor_count = native_anchor ? 1 : 0;
        m->cache_request = &native_request;
    }
    forge_status status = fg_checkpoint_cache_begin(m, prepared_prompt, tokens, (size_t)n, cancel,
                                                    cu, deadline, &cache, e);
    m->cache_request = logical_request;
    if (status == FORGE_OK)
        status = prefill_tokens(m, tokens, (size_t)n, max_tokens, stats, cancel, cu, deadline,
                                &cache, e);
    free(tokens);
    if (status != FORGE_OK) {
        fg_chat_render_destroy(native_render);
        return status;
    }
    struct llama_sampler *sampler = NULL;
    struct llama_sampler *grammar_sampler = NULL;
    llama_token preserved_tokens[64] = {0};
    size_t preserved_count = 0;
    fg_buf out = {0};
    size_t native_emitted = 0;
    sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!sampler) {
        status = fg_error(e, FORGE_ERR_MEMORY, "Sampler allocation failed");
        goto finish;
    }
    /* §32 per-state routing. A lazy trigger alone never elicits reasoning: the
     * model may open the action object on its first token and the grammar arms
     * immediately. While the suppress window lasts, action-opening tokens are
     * excluded, so decoding can only produce plain text; end-of-generation
     * tokens stay excluded until the action actually begins, so a routed
     * generation cannot end actionless. Both bans sit ahead of the samplers:
     * end ban at slot 0 until the action begins, brace ban at slot 1 until the
     * window is spent. When the think cap expires without an action, the lazy
     * grammar is swapped for an eager one so the action must open constrained.
     * Once the action begins, generation ends at the first token that
     * completes the object — the grammar would otherwise keep whitespace
     * padding legal until an end token or the budget (root ends in ws). */
    size_t min_think = 0, think_cap = 0;
    const char *cue = NULL;
    if (policy) {
        fg_think_bounds(policy, max_tokens, &min_think, &think_cap);
        cue = policy->native_thinking ? "" : (policy->cue ? policy->cue : FG_THOUGHT_CUE);
    }
    size_t suppress = min_think;
    bool awaiting_action = false, braces_armed = false, progress_armed = false;
    if (policy && !policy->native_thinking) {
        if (!action_ban_ready(s, e)) {
            status = e && e->code ? e->code : FORGE_ERR_MEMORY;
            goto finish;
        }
        struct llama_sampler *ends = llama_sampler_init_logit_bias(llama_vocab_n_tokens(s->vocab),
                                                                   s->eog_ban_count, s->eog_ban);
        struct llama_sampler *braces =
            ends && suppress ? llama_sampler_init_logit_bias(llama_vocab_n_tokens(s->vocab),
                                                             s->brace_ban_count, s->brace_ban)
                             : NULL;
        if (!ends || (suppress && !braces)) {
            if (ends)
                llama_sampler_free(ends);
            status = fg_error(e, FORGE_ERR_MEMORY, "Routed prefix sampler allocation failed");
            goto finish;
        }
        llama_sampler_chain_add(sampler, ends);
        awaiting_action = true;
        if (braces) {
            llama_sampler_chain_add(sampler, braces);
            braces_armed = true;
        }
    }
    if (active_grammar) {
        if (native) {
            const char *patterns[64] = {0};
            llama_token trigger_tokens[64] = {0};
            size_t pattern_count = fg_chat_render_trigger_pattern_count(native_render);
            size_t trigger_count = fg_chat_render_trigger_token_count(native_render);
            if (pattern_count > 64 || trigger_count > 64) {
                status = fg_error(e, FORGE_ERR_LIMIT,
                                  "Native template returned too many grammar triggers");
                goto finish;
            }
            for (size_t i = 0; i < pattern_count; i++)
                patterns[i] = fg_chat_render_trigger_pattern(native_render, i);
            for (size_t i = 0; i < trigger_count; i++)
                trigger_tokens[i] = fg_chat_render_trigger_token(native_render, i);
            grammar_sampler = fg_chat_render_grammar_lazy(native_render)
                                  ? llama_sampler_init_grammar_lazy_patterns(
                                        s->vocab, active_grammar, "root", patterns, pattern_count,
                                        trigger_tokens, trigger_count)
                                  : llama_sampler_init_grammar(s->vocab, active_grammar, "root");
        } else if (policy) {
            const char *patterns[] = {policy->trigger};
            grammar_sampler = llama_sampler_init_grammar_lazy_patterns(
                s->vocab, active_grammar, "root", patterns, 1, NULL, 0);
        } else
            grammar_sampler = llama_sampler_init_grammar(s->vocab, active_grammar, "root");
        if (!grammar_sampler) {
            status =
                fg_error(e, FORGE_ERR_PARSE,
                         native ? "Native template grammar or trigger was rejected by llama.cpp"
                                : "Generated tool grammar or trigger was rejected by llama.cpp");
            goto finish;
        }
        if (native) {
            status = native_grammar_prefill(s, grammar_sampler, native_render, e);
            if (status == FORGE_OK)
                status = native_preserved_tokens(
                    s, native_render, preserved_tokens,
                    sizeof(preserved_tokens) / sizeof(*preserved_tokens), &preserved_count, e);
            if (status != FORGE_OK) {
                llama_sampler_free(grammar_sampler);
                grammar_sampler = NULL;
                goto finish;
            }
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
    size_t sample_budget = max_tokens;
    /* Force-decode the reasoning cue so greedy decoding continues a prose
     * sentence instead of echoing the prompt (the measured failure of a bare
     * '{' ban). The cue streams and enters the raw response like sampled
     * text, and every sampler accepts it so the lazy trigger buffer stays
     * consistent with the output. A cue that cannot be force-decoded fails
     * loudly here: proceeding with the bans armed but no steering text is the
     * measured 10/10-death configuration, never a silent fallback. */
    if (policy && *cue) {
        llama_token cue_tokens[16];
        int32_t cue_count =
            llama_tokenize(s->vocab, cue, (int32_t)strlen(cue), cue_tokens,
                           (int32_t)(sizeof(cue_tokens) / sizeof(*cue_tokens)), false, false);
        if (cue_count <= 0) {
            status = fg_error(e, FORGE_ERR_MODEL,
                              "Reasoning cue cannot be tokenized within its 16-token bound");
            goto finish;
        }
        if ((size_t)cue_count >= max_tokens) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Turn token budget cannot fit the reasoning cue");
            goto finish;
        }
        /* Screen rendered pieces, not configured bytes: tokenizer
         * normalization can map other code points onto '{', and a cue that
         * opens the action object would fire the trigger mid-scaffold. The
         * trigger buffer sees special=true renderings, so judge those. */
        for (int32_t c = 0; c < cue_count; c++) {
            char small[128];
            int32_t special = llama_token_to_piece(s->vocab, cue_tokens[c], small,
                                                   (int32_t)sizeof(small), 0, true);
            if (special < 0 || memchr(small, '{', (size_t)special)) {
                status = fg_error(e, FORGE_ERR_ARGUMENT,
                                  "Reasoning cue renders an action-opening token");
                goto finish;
            }
        }
        for (int32_t c = 0; c < cue_count && status == FORGE_OK; c++) {
            llama_token token = cue_tokens[c];
            char small[128];
            int32_t length =
                llama_token_to_piece(s->vocab, token, small, (int32_t)sizeof(small), 0, false);
            if (length < 0 || !fg_buf_add(&out, small, (size_t)FG_MAX(length, 0))) {
                status = fg_error(e, FORGE_ERR_MEMORY, "Token output failed");
                break;
            }
            llama_sampler_accept(sampler, token);
            stats->generated_tokens++;
            if (cb && !cb(small, (size_t)length, u)) {
                status = fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
                break;
            }
            status = decode_batch(s, &token, 1, s->count, e);
            if (status == FORGE_OK)
                s->tokens[s->count++] = token;
        }
        sample_budget = max_tokens - (size_t)cue_count;
        if (status != FORGE_OK) {
            stats->decode_ms += (double)(fg_now_ms() - begin);
            goto finish;
        }
    }
    /* Eager grammar constrains the action from the first token; routed
     * generation reaches the action either at the lazy trigger or at the
     * forced swap. action_offset marks where the constrained action region
     * begins — the completeness check must never scan reasoning prose, which
     * can legally contain complete JSON objects that never armed the grammar. */
    bool action_open = !native && !policy && active_grammar;
    size_t action_offset = 0;
    for (size_t i = 0; i < sample_budget; i++) {
        /* The action cannot begin while '{' is banned, so while the window
         * lasts the brace ban is still at slot 1 behind the end ban. */
        if (braces_armed && i == suppress) {
            llama_sampler_free(llama_sampler_chain_remove(sampler, 1));
            braces_armed = false;
        }
        if (policy && !policy->native_thinking && !action_open && !policy->think_unbounded &&
            i == think_cap) {
            status = force_action(s, sampler, &grammar_sampler, active_grammar, awaiting_action,
                                  &progress_armed, e);
            if (status != FORGE_OK)
                break;
            awaiting_action = false;
            action_open = true;
            action_offset = out.len;
            stats->forced_actions++;
        }
        if (interrupted(cancel, cu, deadline)) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Inference cancelled or deadline reached");
            break;
        }
        llama_synchronize(s->ctx);
        uint64_t sampling_start = fg_now_ms();
        /* The greedy fast path reads raw logits and would bypass the bans. */
        fg_action_phase phase =
            action_open ? fg_action_decode_phase(out.data ? out.data + action_offset : "")
                        : FG_ACTION_SELECT;
        bool structured =
            native || (action_open && (phase == FG_ACTION_SELECT || phase == FG_ACTION_ARGUMENTS));
        llama_token token = sample_token(
            s, sampler, grammar_sampler,
            !braces_armed && !awaiting_action && !progress_armed && m->config.grammar_fast_path &&
                (m->config.temperature <= 0 || (!native && structured)),
            stats);
        stats->sampling_ms += (double)(fg_now_ms() - sampling_start);
        if (llama_vocab_is_eog(s->vocab, token)) {
            ended = true;
            break;
        }
        int32_t cap = 128;
        char small[128], *piece = small;
        bool render_special =
            native && native_token_is_preserved(token, preserved_tokens, preserved_count);
        int32_t length = llama_token_to_piece(s->vocab, token, piece, cap, 0, render_special);
        if (length < 0) {
            cap = -length;
            piece = malloc((size_t)cap);
            if (!piece) {
                status = FORGE_ERR_MEMORY;
                break;
            }
            length = llama_token_to_piece(s->vocab, token, piece, cap, 0, render_special);
        }
        if (length < 0 || !fg_buf_add(&out, piece, (size_t)FG_MAX(length, 0))) {
            if (piece != small)
                free(piece);
            status = fg_error(e, FORGE_ERR_MEMORY, "Token output failed");
            break;
        }
        stats->generated_tokens++;
        if (!native && action_open) {
            if (phase == FG_ACTION_SELECT)
                stats->action_select_tokens++;
            else if (phase == FG_ACTION_ARGUMENTS)
                stats->action_argument_tokens++;
            else if (phase == FG_ACTION_PATCH)
                stats->patch_tokens++;
            else if (phase == FG_ACTION_FINAL)
                stats->final_tokens++;
            else if (phase == FG_ACTION_MEMORY)
                stats->memory_tokens++;
        }
        if (progress_armed) {
            stats->forced_action_progress_tokens++;
            if (!fg_json_whitespace_only(piece, (size_t)FG_MAX(length, 0))) {
                llama_sampler_free(llama_sampler_chain_remove(sampler, 0));
                progress_armed = false;
            }
        }
        bool closing = length > 0 && memchr(piece, '}', (size_t)length) != NULL;
        if (policy && !action_open) {
            const char *opened = fg_action_begin(out.data ? out.data : "");
            if (opened) {
                if (awaiting_action) {
                    llama_sampler_free(llama_sampler_chain_remove(sampler, 0));
                    awaiting_action = false;
                }
                action_open = true;
                action_offset = (size_t)(opened - out.data);
                stats->action_select_tokens++;
            } else
                stats->think_tokens++;
        }
        if (native) {
            size_t safe_length = out.len;
            bool stopped = false;
            fg_chat_render_scan_stop(native_render, out.data, out.len, native_emitted, &safe_length,
                                     &stopped);
            if (safe_length > native_emitted && cb &&
                !cb(out.data + native_emitted, safe_length - native_emitted, u)) {
                if (piece != small)
                    free(piece);
                status = fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
                break;
            }
            native_emitted = safe_length;
            if (stopped) {
                out.len = safe_length;
                out.data[out.len] = 0;
                if (piece != small)
                    free(piece);
                ended = true;
                break;
            }
        } else if (cb && !cb(piece, (size_t)length, u)) {
            if (piece != small)
                free(piece);
            status = fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
            break;
        }
        if (piece != small)
            free(piece);
        /* End at completion: after the action object closes, the grammar
         * still keeps whitespace legal (root ends in ws), so waiting for an
         * end token can burn the rest of the budget. Objects can only close
         * on a '}' piece, and the scan starts at the constrained region. */
        if (!native && active_grammar && action_open && closing &&
            fg_action_complete(out.data + action_offset)) {
            stats->action_stops++;
            ended = true;
            break;
        }
        status = decode_batch(s, &token, 1, s->count, e);
        if (status != FORGE_OK)
            break;
        s->tokens[s->count++] = token;
    }
    if (native && status == FORGE_OK && native_emitted < out.len) {
        if (cb && !cb(out.data + native_emitted, out.len - native_emitted, u))
            status = fg_error(e, FORGE_ERR_CANCELLED, "Token callback cancelled");
        else
            native_emitted = out.len;
    }
    stats->decode_ms += (double)(fg_now_ms() - begin);
    if (status == FORGE_OK && active_grammar && !ended) {
        if (native) {
            char detail[256] = {0};
            char *parsed = fg_chat_render_parse(native_render, out.data ? out.data : "", detail,
                                                sizeof(detail));
            if (!parsed)
                status = fg_error(e, FORGE_ERR_LIMIT,
                                  "Generation limit reached before one complete native call: %s",
                                  *detail ? detail : "native parser rejected the output");
            free(parsed);
        } else {
            bool complete = policy && action_open &&
                            fg_action_complete(out.data ? out.data + action_offset : "");
            yyjson_doc *d = policy ? NULL : yyjson_read(out.data ? out.data : "", out.len, 0);
            if (!complete && !d)
                status = fg_error(e, FORGE_ERR_LIMIT,
                                  "Generation limit reached before a complete action");
            yyjson_doc_free(d);
        }
    }
finish:
    if (sampler)
        llama_sampler_free(sampler);
    if (status != FORGE_OK) {
        clear_live_state(m);
        fg_buf_clear(&out);
        fg_chat_render_destroy(native_render);
        return status;
    }
    *output = fg_buf_take(&out);
    if (!*output) {
        fg_chat_render_destroy(native_render);
        return fg_error(e, FORGE_ERR_MEMORY, "Output allocation failed");
    }
    if (native) {
        s->last_native_render = native_render;
        native_render = NULL;
    }
    return FORGE_OK;
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
    fg_chat_render *native_render = NULL;
    const char *prepared_prompt = prompt;
    *out_tokens = NULL;
    *out_count = 0;
    if (interrupted(cancel, user, deadline))
        return fg_error(e, FORGE_ERR_CANCELLED, "Checkpoint cancelled before tokenization");
    int32_t count = 0;
    llama_token *tokens = NULL;
    if (m->config.prompt_protocol == FORGE_PROMPT_NATIVE) {
        char detail[256] = {0};
        native_render = fg_chat_templates_apply_native(s->chat_templates, prompt,
                                                       s->thinking_mode != FORGE_THINKING_DISABLED,
                                                       detail, sizeof(detail));
        size_t length = 0;
        prepared_prompt = fg_chat_render_prompt(native_render, &length);
        (void)length;
        size_t allocated = 0;
        if (prepared_prompt)
            tokens = tokenize_text_allocated(s, prepared_prompt, &count, NULL, &allocated, e);
        if (!native_render || !prepared_prompt || !tokens) {
            fg_chat_render_destroy(native_render);
            if (!e || !e->code)
                fg_error(e, FORGE_ERR_UNSUPPORTED, "Cannot prepare native checkpoint prompt: %s",
                         *detail ? detail : "unsupported chat template");
            return e && e->code ? e->code : FORGE_ERR_MODEL;
        }
    } else
        tokens = tokenize(s, prompt, &count);
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
        fg_chat_render_destroy(native_render);
        clear_live_state(m);
        return status;
    }
    fg_chat_render_destroy(native_render);
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
        m->config.prompt_protocol == FORGE_PROMPT_NATIVE
            ? tokenize_text_allocated(m->backend, prefix, &count, allocator, &token_bytes, error)
            : tokenize_allocated(m->backend, prefix, &count, allocator, &token_bytes, error);
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
        fg_chat_render_destroy(s->last_native_render);
        fg_chat_templates_destroy(s->chat_templates);
        free(s->brace_ban);
        free(s->eog_ban);
        free(s->whitespace_ban);
        free(s);
    }
}

static bool native_protocol_probe(forge_model *m, llama_state *s, forge_error *error) {
    char *tools = fg_tool_native_schema();
    fg_buf request = {0};
    bool built = tools && fg_buf_printf(&request,
                                        "{\"protocol\":\"forge-native-v1\",\"tools\":%s,"
                                        "\"anchor_message_count\":1,\"messages\":["
                                        "{\"role\":\"system\",\"content\":\"Forge\"},"
                                        "{\"role\":\"user\",\"content\":\"Probe\"}]}",
                                        tools);
    free(tools);
    if (!built) {
        fg_buf_clear(&request);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate native protocol probe");
        return false;
    }
    char detail[256] = {0};
    fg_chat_render *render = fg_chat_templates_apply_native(
        s->chat_templates, request.data, m->config.thinking != FORGE_THINKING_DISABLED, detail,
        sizeof(detail));
    fg_buf_clear(&request);
    if (!render) {
        fg_error(error, FORGE_ERR_UNSUPPORTED,
                 "Native prompt protocol is unsupported by the selected chat template: %s",
                 *detail ? detail : "roles, tools, grammar, or parser unavailable");
        return false;
    }
    fg_chat_render_destroy(render);
    return true;
}

static forge_status llama_parse_native(forge_model *m, const char *response, char **message,
                                       forge_error *error) {
    if (!m || !response || !message)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing native response input");
    llama_state *s = m->backend;
    *message = NULL;
    if (!s || !s->last_native_render)
        return fg_error(error, FORGE_ERR_CONFLICT,
                        "Native response parser has no matching rendered prompt");
    char detail[256] = {0};
    *message = fg_chat_render_parse(s->last_native_render, response, detail, sizeof(detail));
    if (!*message)
        return fg_error(error, FORGE_ERR_PARSE, "Cannot parse native tool call: %s",
                        *detail ? detail : "chat-template parser rejected the response");
    return FORGE_OK;
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
    s->thinking_mode = m->config.thinking;
    if (m->config.prompt_protocol == FORGE_PROMPT_NATIVE ||
        m->config.thinking != FORGE_THINKING_AUTO) {
        char detail[256] = {0};
        s->chat_templates =
            fg_chat_templates_create(s->model, m->config.chat_template, detail, sizeof(detail));
        if (!s->chat_templates) {
            fg_error(e, FORGE_ERR_MODEL, "Cannot initialize Jinja chat template: %s", detail);
            return false;
        }
        if (m->config.thinking != FORGE_THINKING_AUTO &&
            !fg_chat_templates_support_thinking(s->chat_templates)) {
            fg_error(e, FORGE_ERR_MODEL,
                     "The selected chat template does not support enable_thinking");
            return false;
        }
        if (m->config.prompt_protocol == FORGE_PROMPT_NATIVE && !native_protocol_probe(m, s, e))
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
    m->count_prompt = llama_count_prompt;
    m->generate = llama_generate;
    m->parse_native = llama_parse_native;
    m->checkpoint = &checkpoint_backend;
    return true;
}
