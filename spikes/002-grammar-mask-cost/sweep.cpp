/* 002-grammar-mask-cost: what does a LIVE grammar cost per token, and does
 * masking a REDUCED candidate array give the same answer for far less?
 *
 * STRATEGIC QUESTION
 * The September loop-pilot runs decode at ~60-69 tok/s while the same weights
 * decode at ~230 tok/s unconstrained. A controlled A/B on one binary (arm A
 * native+temp0.6 vs arm B native+temp0) put 71% of arm A's decode time inside
 * the sampler. The suspected mechanism is structural, in the pinned llama.cpp:
 *
 *   llama_grammar_apply_impl() iterates EVERY entry of the candidate array,
 *   and per candidate calls decode_utf8(), which returns a std::vector<uint32_t>
 *   by value. With a 151k-entry array that is ~2 allocations per candidate per
 *   token. Forge's greedy "fast path" avoids it only because it hands the
 *   grammar a ONE-element array - and that fast path is switched off whenever
 *   temperature > 0 or a ban is armed, i.e. exactly when the grammar is live.
 *
 * If that mechanism is right, the cost is a function of the CANDIDATE ARRAY
 * SIZE at the moment the grammar is applied - and the fix is to reduce the
 * array first (top-K raw logits), apply the grammar to those K, and expand K
 * only if fewer than top_k candidates survive. That fix is distribution-
 * preserving whenever the surviving set is the same one the full-vocab path
 * would have chosen, which is what H3 below checks on real logits.
 *
 * THIS PROGRAM (one model load, all timings CPU-side, as Forge's sampler is)
 *   P1  generate N tokens with Forge's campaign chain on a live Forge grammar:
 *       per step, time llama_sampler_sample() itself (the "today" cost), then
 *       on the same logits time a full-vocab chain apply (reference) and a
 *       reduced top-K chain apply, and compare their allowed top-20 id lists.
 *   P2  fixed-state arms in three real grammar states (root, mid-action,
 *       post-action): grammar-only apply, full chain, reduced chain at
 *       K in {20,64,256}, and the same with a greedy tail.
 *   P3  the cost of selecting the raw top-K itself (nth_element over the vocab).
 *
 * Build: cmake target forge_sampler_attribution.
 * Run:   forge_sampler_attribution --model <gguf> --prompt <file> [--steps 64]
 */

#include "llama.h"

#include "chat_template.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

/* Forge internals, declared here rather than pulling src/internal.h into C++.
 * Definitions: src/tools/tools.c (fg_tool_grammar, fg_tool_native_schema). */
extern "C" {
char *fg_tool_grammar(bool thought, bool required, bool routed);
char *fg_tool_native_schema(void);
}

#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

struct hist {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const {
        if (v.empty())
            return 0.0;
        double s = 0;
        for (double x : v)
            s += x;
        return s / (double)v.size();
    }
    double median() const {
        if (v.empty())
            return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[s.size() / 2];
    }
    double p90() const {
        if (v.empty())
            return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[(size_t)(0.9 * (double)(s.size() - 1))];
    }
};

static char *read_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;
    *length = got;
    return text;
}

#define PREFILL_CHUNK 512

static bool prefill(struct llama_context *ctx, const llama_token *tokens, int32_t count,
                    int32_t *logits_index) {
    *logits_index = 0;
    for (int32_t start = 0; start < count; start += PREFILL_CHUNK) {
        int32_t take = count - start;
        if (take > PREFILL_CHUNK)
            take = PREFILL_CHUNK;
        struct llama_batch b = llama_batch_init(take, 0, 1);
        if (!b.token) {
            fprintf(stderr, "batch allocation failed\n");
            return false;
        }
        for (int32_t i = 0; i < take; i++) {
            b.token[i] = tokens[start + i];
            b.pos[i] = start + i;
            b.n_seq_id[i] = 1;
            b.seq_id[i][0] = 0;
            b.logits[i] = (start + i == count - 1) ? 1 : 0;
        }
        b.n_tokens = take;
        bool ok = llama_decode(ctx, b) == 0;
        llama_batch_free(b);
        if (!ok) {
            fprintf(stderr, "prefill decode failed at token %d of %d\n", start, count);
            return false;
        }
        *logits_index = take - 1;
    }
    return true;
}

static bool decode_one(struct llama_context *ctx, llama_token token, llama_pos position,
                       int32_t *logits_index) {
    struct llama_batch b = llama_batch_init(1, 0, 1);
    if (!b.token)
        return false;
    b.token[0] = token;
    b.pos[0] = position;
    b.n_seq_id[0] = 1;
    b.seq_id[0][0] = 0;
    b.logits[0] = 1;
    b.n_tokens = 1;
    bool ok = llama_decode(ctx, b) == 0;
    llama_batch_free(b);
    *logits_index = 0;
    return ok;
}

/* Forge's campaign chain: tool grammar, then top_k 20 / top_p 0.8 / temp 0.6. */
static struct llama_sampler *make_chain(const char *gbnf, const struct llama_vocab *vocab,
                                        bool with_grammar) {
    struct llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (!chain)
        return NULL;
    if (with_grammar) {
        struct llama_sampler *g = llama_sampler_init_grammar(vocab, gbnf, "root");
        if (!g) {
            llama_sampler_free(chain);
            return NULL;
        }
        llama_sampler_chain_add(chain, g);
    }
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(20));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.8f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.6f));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(42));
    return chain;
}

/* A chain with one member only, so the grammar's own cost is isolable. */
static struct llama_sampler *make_grammar_only(const char *gbnf, const struct llama_vocab *vocab) {
    struct llama_sampler *g = llama_sampler_init_grammar(vocab, gbnf, "root");
    return g;
}

static void fill_full(std::vector<llama_token_data> &buf, const float *logits, int32_t n_vocab) {
    buf.resize((size_t)n_vocab);
    for (int32_t i = 0; i < n_vocab; i++)
        buf[(size_t)i] = llama_token_data{i, logits[i], 0.0f};
}

static void fill_ids(std::vector<llama_token_data> &buf, const float *logits,
                     const std::vector<llama_token> &ids) {
    buf.resize(ids.size());
    for (size_t i = 0; i < ids.size(); i++)
        buf[i] = llama_token_data{ids[i], logits[ids[i]], 0.0f};
}

/* Raw top-k by logit, no grammar. scratch is reused across calls. */
static void topk_ids(const float *logits, int32_t n_vocab, int k, std::vector<llama_token> &out,
                     std::vector<llama_token> &scratch) {
    if (k > n_vocab)
        k = n_vocab;
    scratch.resize((size_t)n_vocab);
    for (int32_t i = 0; i < n_vocab; i++)
        scratch[(size_t)i] = i;
    const float *lg = logits;
    std::nth_element(scratch.begin(), scratch.begin() + k, scratch.end(),
                     [lg](llama_token a, llama_token b) { return lg[a] > lg[b]; });
    out.assign(scratch.begin(), scratch.begin() + k);
    std::sort(out.begin(), out.end(),
              [lg](llama_token a, llama_token b) { return lg[a] > lg[b]; });
}

/* Ordered ids of the surviving entries after a chain apply (top_k leaves the
 * array sorted by logit descending; masked entries are -INFINITY). */
static void allowed_ids(const llama_token_data_array &cur, std::vector<llama_token> &out,
                        int limit) {
    out.clear();
    for (size_t i = 0; i < cur.size && (int)out.size() < limit; i++) {
        if (cur.data[i].logit != -INFINITY)
            out.push_back(cur.data[i].id);
    }
}

static int count_allowed(const llama_token_data_array &cur) {
    int n = 0;
    for (size_t i = 0; i < cur.size; i++)
        if (cur.data[i].logit != -INFINITY)
            n++;
    return n;
}

/* ms for one apply over the given initial array. */
static double time_apply(struct llama_sampler *chain, std::vector<llama_token_data> &buf,
                         const float *logits, int32_t n_vocab, const std::vector<llama_token> *ids,
                         int iters, int *allowed_out, std::vector<llama_token> *ids_out) {
    hist h;
    int allowed = 0;
    std::vector<llama_token> out;
    for (int it = 0; it < iters; it++) {
        if (ids)
            fill_ids(buf, logits, *ids);
        else
            fill_full(buf, logits, n_vocab);
        llama_token_data_array cur = {buf.data(), buf.size(), -1, false};
        double t0 = now_ms();
        llama_sampler_apply(chain, &cur);
        h.add(now_ms() - t0);
        if (it == iters - 1) {
            allowed = count_allowed(cur);
            if (ids_out)
                allowed_ids(cur, out, 20);
        }
    }
    if (allowed_out)
        *allowed_out = allowed;
    if (ids_out)
        *ids_out = out;
    return h.median();
}

static void accept_tokens(struct llama_sampler *chain, const std::vector<llama_token> &tokens,
                          size_t count) {
    for (size_t i = 0; i < count && i < tokens.size(); i++)
        llama_sampler_accept(chain, tokens[i]);
}

/* So is the raw top-K a safe place to apply the grammar? For a state, compare the
 * allowed top-20 of the FULL array against membership in the raw top-K sets. The
 * count of allowed tokens OUTSIDE raw top-K is the truncation error a reordered
 * chain ([top_k K][grammar][top_k 20]) would commit; zero means the sampled set
 * is identical to the full-vocab one. */
static void topk_coverage(const char * label, const char * gbnf, const struct llama_vocab * vocab,
                          const std::vector<llama_token> & accepted, const float * logits,
                          int32_t n_vocab, std::vector<llama_token_data> & buf_full,
                          std::vector<llama_token_data> & buf_red, std::vector<llama_token> & ids,
                          std::vector<llama_token> & ks) {
    /* The set the current Forge chain samples from is the allowed tokens ordered
     * by logit and cut to top_k, so the coverage arm needs [grammar][top_k 20]:
     * a grammar-only sampler leaves the array in token-id order and would report
     * an id-ordered "allowed" list instead. */
    struct llama_sampler * chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    struct llama_sampler * g = llama_sampler_init_grammar(vocab, gbnf, "root");
    if (!chain || !g) {
        return;
    }
    llama_sampler_chain_add(chain, g);
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(20));
    accept_tokens(chain, accepted, accepted.size());
    std::vector<llama_token> allowed;
    int n_allowed = 0;
    time_apply(chain, buf_full, logits, n_vocab, NULL, 1, &n_allowed, &allowed);
    printf("P4 %-12s sampled-set=%2zu", label, allowed.size());
    for (int K : {20, 64, 256}) {
        topk_ids(logits, n_vocab, K, ks, ids);
        size_t outside = 0;
        for (llama_token t : allowed) {
            if (std::find(ks.begin(), ks.end(), t) == ks.end()) {
                outside++;
            }
        }
        printf(" | K=%-3d outside=%zu", K, outside);
    }
    printf("\n");
    llama_sampler_free(chain);
}

int main(int argc, char **argv) {
    const char *model_path = NULL;
    const char *prompt_path = NULL;
    int steps = 64;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc)
            model_path = argv[++i];
        else if (!strcmp(argv[i], "--prompt") && i + 1 < argc)
            prompt_path = argv[++i];
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc)
            steps = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!model_path || !prompt_path || steps < 8) {
        fprintf(stderr, "usage: forge_sampler_attribution --model <gguf> --prompt <file> "
                        "[--steps 64]\n");
        return 2;
    }

    size_t prompt_length = 0;
    char *prompt = read_file(prompt_path, &prompt_length);
    if (!prompt) {
        fprintf(stderr, "cannot read prompt: %s\n", prompt_path);
        return 1;
    }

    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 999;
    struct llama_model *model = llama_model_load_from_file(model_path, mp);
    if (!model) {
        fprintf(stderr, "model load failed: %s\n", model_path);
        return 1;
    }
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    const int32_t n_vocab = llama_vocab_n_tokens(vocab);

    /* Mirror src/inference/llama_backend.c. */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 16384;
    cp.n_batch = 512;
    cp.n_ubatch = 256;
    cp.n_threads = 24;
    cp.n_threads_batch = 24;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        fprintf(stderr, "context creation failed\n");
        return 1;
    }

    /* --- the grammar: Forge's own, native template render preferred --- */
    std::string gbnf;
    const char *grammar_source = "none";
    int render_lazy = 0;
    fg_chat_templates *templates = NULL;
    {
        char tmpl_err[256] = {0};
        templates = fg_chat_templates_create(model, NULL, tmpl_err, sizeof(tmpl_err));
        if (!templates)
            fprintf(stderr, "template init failed (%s); using flat grammar\n",
                    tmpl_err[0] ? tmpl_err : "no detail");
    }
    if (templates) {
        char *tools = fg_tool_native_schema();
        if (tools) {
            std::string request = std::string("{\"protocol\":\"forge-native-v1\",\"tools\":") +
                                  tools +
                                  ",\"anchor_message_count\":1,\"messages\":["
                                  "{\"role\":\"system\",\"content\":\"Forge\"},"
                                  "{\"role\":\"user\",\"content\":\"Probe\"}]}";
            char err[256] = {0};
            fg_chat_render *render =
                fg_chat_templates_apply_native(templates, request.c_str(), false, err, sizeof(err));
            if (render) {
                const char *g = fg_chat_render_grammar(render);
                if (g && *g) {
                    gbnf = g;
                    grammar_source = "native-template";
                    render_lazy = fg_chat_render_grammar_lazy(render) ? 1 : 0;
                }
                fg_chat_render_destroy(render);
            } else {
                fprintf(stderr, "native render unavailable (%s); falling back to flat grammar\n",
                        err[0] ? err : "no detail");
            }
            free(tools);
        }
        fg_chat_templates_destroy(templates);
    }
    if (gbnf.empty()) {
        char *flat = fg_tool_grammar(false, false, false);
        if (flat) {
            gbnf = flat;
            grammar_source = "flat-tool-grammar";
            free(flat);
        }
    }
    if (gbnf.empty()) {
        fprintf(stderr, "no grammar available\n");
        return 1;
    }

    int32_t n_prompt = -llama_tokenize(vocab, prompt, prompt_length, NULL, 0, true, true);
    if (n_prompt <= 0) {
        fprintf(stderr, "tokenization failed\n");
        return 1;
    }
    std::vector<llama_token> prompt_tokens((size_t)n_prompt);
    if (llama_tokenize(vocab, prompt, prompt_length, prompt_tokens.data(), n_prompt, true, true) < 0) {
        fprintf(stderr, "tokenization failed\n");
        return 1;
    }

    printf("model          : %s\n", model_path);
    printf("vocab          : %d tokens\n", (int)n_vocab);
    printf("grammar source : %s (%zu bytes, render_lazy=%d)\n", grammar_source, gbnf.size(),
           render_lazy);
    printf("prompt tokens  : %d\n", (int)n_prompt);
    printf("steps          : %d\n", steps);
    printf("chain          : [grammar] top_k 20, top_p 0.8, temp 0.6, dist(42)\n\n");

    int32_t logits_index = 0;
    double t0 = now_ms();
    if (!prefill(ctx, prompt_tokens.data(), n_prompt, &logits_index)) {
        fprintf(stderr, "prefill failed\n");
        return 1;
    }
    llama_synchronize(ctx);
    printf("shared prefill : %.0f ms for %d tokens (%.0f tok/s)\n\n", now_ms() - t0, (int)n_prompt,
           (double)n_prompt / ((now_ms() - t0) / 1000.0));

    std::vector<float> snap((size_t)n_vocab);
    std::vector<llama_token_data> buf_full;
    std::vector<llama_token_data> buf_red;
    std::vector<llama_token> ks, ids, ids_full, ids_red;

    /* ---------- P1: real generation steps, paired cost + equivalence ---------- */
    struct llama_sampler *gen = make_chain(gbnf.c_str(), vocab, true);
    if (!gen) {
        fprintf(stderr, "grammar rejected by llama.cpp\n");
        return 1;
    }
    std::vector<llama_token> generated;

    /* P5 reference/proposed comparison: greedy, so agreement is a clean predicate.
     * The proposed scheme (Forge-side, works with the shipped prebuilt DLL) masks a
     * reduced raw top-K array and expands K x4 while the mask leaves no allowed
     * token; at K = n_vocab it is the current behaviour by construction. */
    int p5_agree = 0, p5_steps = 0, p5_reduced_only = 0;
    double p5_ref_ms = 0, p5_prop_ms = 0;
    long long p5_k_sum = 0;
    hist p5_k_reached;

    hist today_ms, full_apply_ms, red_apply_ms, select_ms;
    int matches20 = 0, matches5 = 0, steps_ge20 = 0;
    int expanded_steps = 0;
    int top1_match = 0, top1_steps = 0;
    long long outside64 = 0;

    printf("P1 real steps (gen chain = today's cost; full/reduced apply on identical logits)\n");
    printf("%5s %10s %11s %11s %8s %9s %7s\n", "step", "today_ms", "full_ms", "red64_ms",
           "allowed", "match20", "redK");
    printf("---------------------------------------------------------------------------\n");
    for (int step = 0; step < steps; step++) {
        const float *logits = llama_get_logits_ith(ctx, logits_index);
        if (!logits) {
            fprintf(stderr, "no logits at step %d\n", step);
            return 1;
        }
        memcpy(snap.data(), logits, sizeof(float) * (size_t)n_vocab);

        /* today: the exact call Forge makes */
        double tt0 = now_ms();
        llama_token sampled = llama_sampler_sample(gen, ctx, logits_index);
        today_ms.add(now_ms() - tt0);

        /* reference: full-vocab apply of the same chain on the same logits */
        int allowed_full = 0;
        double tf = time_apply(gen, buf_full, snap.data(), n_vocab, NULL, 1, &allowed_full,
                               &ids_full);

        /* reduced: raw top-K, expand until at least top_k survive */
        int k = 64;
        int allowed_red = 0;
        double tr = 0;
        int used_k = k;
        for (;;) {
            double ts0 = now_ms();
            topk_ids(snap.data(), n_vocab, k, ks, ids);
            select_ms.add(now_ms() - ts0);
            used_k = (k > n_vocab) ? n_vocab : k;
            tr = time_apply(gen, buf_red, snap.data(), n_vocab, &ks, 1, &allowed_red, &ids_red);
            if (allowed_red >= 20 || k >= n_vocab)
                break;
            k *= 4;
            expanded_steps++;
        }
        full_apply_ms.add(tf);
        red_apply_ms.add(tr);

        /* ---- P5: same state, same logits, greedy reference vs reduced+expanded ----
         * Both sides read the token a greedy sampler would actually draw (the
         * highest-logit survivor of the applied array). Reading an "allowed" list
         * instead would compare an id-ordered list with a rank-ordered one. */
        {
            struct llama_sampler * gclone = llama_sampler_clone(gen);
            if (gclone) {
                /* keep the grammar (index 0), drop the stochastic tail, go greedy */
                while (llama_sampler_chain_get(gclone, 1) != NULL) {
                    struct llama_sampler * removed = llama_sampler_chain_remove(gclone, 1);
                    if (removed) {
                        llama_sampler_free(removed);
                    } else {
                        break;
                    }
                }
                llama_sampler_chain_add(gclone, llama_sampler_init_greedy());

                fill_full(buf_full, snap.data(), n_vocab);
                llama_token_data_array cur_ref = {buf_full.data(), buf_full.size(), -1, false};
                double t_ref0 = now_ms();
                llama_sampler_apply(gclone, &cur_ref);
                p5_ref_ms += now_ms() - t_ref0;
                llama_token ref_token = -1;
                /* plain argmax over the survivors of the applied array */
                {
                    float best = -INFINITY;
                    for (size_t i = 0; i < cur_ref.size; i++) {
                        if (cur_ref.data[i].logit > best) {
                            best = cur_ref.data[i].logit;
                            ref_token = cur_ref.data[i].id;
                        }
                    }
                }

                int kk = 20;
                llama_token prop_token = -1;
                double t_prop = 0;
                for (;;) {
                    topk_ids(snap.data(), n_vocab, kk, ks, ids);
                    fill_ids(buf_red, snap.data(), ks);
                    llama_token_data_array cur_prop = {buf_red.data(), buf_red.size(), -1, false};
                    double t0 = now_ms();
                    llama_sampler_apply(gclone, &cur_prop);
                    t_prop += now_ms() - t0;
                    prop_token = -1;
                    {
                        float best = -INFINITY;
                        for (size_t i = 0; i < cur_prop.size; i++) {
                            if (cur_prop.data[i].logit > best) {
                                best = cur_prop.data[i].logit;
                                prop_token = cur_prop.data[i].id;
                            }
                        }
                    }
                    if (prop_token >= 0 || kk >= n_vocab) {
                        break;
                    }
                    kk *= 4;
                }
                p5_prop_ms += t_prop;
                p5_k_sum += (kk > n_vocab ? n_vocab : kk);
                p5_k_reached.add((double)(kk > n_vocab ? n_vocab : kk));
                if (kk < n_vocab) {
                    p5_reduced_only++;
                }
                if (ref_token >= 0 && prop_token >= 0) {
                    p5_steps++;
                    if (ref_token == prop_token) {
                        p5_agree++;
                    }
                }
                llama_sampler_free(gclone);
            }
        }

        /* K=64 with NO expansion — the fast case the fix would actually use.
         * Agreement with the reference decides whether truncation is safe in
         * this state; outside64 counts reference-allowed tokens the raw top-64
         * never contained (the truncation error the fix would commit). */
        std::vector<llama_token> ids_noexp;
        int allowed_noexp = 0;
        {
            std::vector<llama_token> k64;
            topk_ids(snap.data(), n_vocab, 64, ks, k64);
            time_apply(gen, buf_red, snap.data(), n_vocab, &ks, 1, &allowed_noexp, &ids_noexp);
            for (llama_token id : ids_full)
                if (std::find(k64.begin(), k64.end(), id) == k64.end())
                    outside64++;
        }
        if (!ids_full.empty() && !ids_noexp.empty()) {
            top1_steps++;
            if (ids_full[0] == ids_noexp[0])
                top1_match++;
        }

        size_t n_cmp20 = std::min(ids_full.size(), ids_red.size());
        size_t n_cmp5 = std::min<size_t>(n_cmp20, 5);
        if (n_cmp20 >= 5) {
            bool ok5 = std::equal(ids_full.begin(), ids_full.begin() + n_cmp5, ids_red.begin());
            if (ok5)
                matches5++;
        } else if (ids_full.size() == ids_red.size()) {
            matches5++;
        }
        bool ok20 = ids_full.size() == ids_red.size() &&
                    std::equal(ids_full.begin(), ids_full.end(), ids_red.begin());
        if (ok20)
            matches20++;
        if (ids_full.size() >= 20)
            steps_ge20++;

        printf("%5d %10.2f %11.2f %11.3f %8d %9s %7d\n", step, today_ms.v.back(), tf, tr,
               allowed_full, ok20 ? "yes" : "NO", used_k);
        fflush(stdout);

        generated.push_back(sampled);
        if (!decode_one(ctx, sampled, (llama_pos)(n_prompt + step), &logits_index)) {
            fprintf(stderr, "decode failed at step %d\n", step);
            return 1;
        }
    }
    printf("---------------------------------------------------------------------------\n");
    printf("P1 medians    : today %.2f ms | full apply %.2f ms | reduced(64) %.2f ms | "
           "top-k select %.3f ms\n",
           today_ms.median(), full_apply_ms.median(), red_apply_ms.median(), select_ms.median());
    printf("P1 equivalence: top-20 identical %d/%d steps (steps with 20 allowed: %d); "
           "top-5 identical %d/%d; expansions %d\n",
           matches20, steps, steps_ge20, matches5, steps, expanded_steps);
    printf("P1 truncation : K=64-no-expansion top-1 agreement %d/%d steps; reference-allowed "
           "tokens outside the raw top-64: %lld total\n",
           top1_match, top1_steps, outside64);
    {
        /* End-to-end determinism check: a fixed seed must produce the same
         * token sequence before and after any sampler change. */
        uint64_t h = 1469598103934665603ULL;
        for (llama_token t : generated) {
            h ^= (uint64_t)(uint32_t)t;
            h *= 1099511628211ULL;
        }
        printf("P1 token hash : %016llx over %d tokens; first ids:", (unsigned long long)h,
               (int)generated.size());
        for (size_t i = 0; i < generated.size() && i < 8; i++)
            printf(" %d", (int)generated[i]);
        printf("\n\n");
    }

    if (p5_steps > 0) {
        printf("P5 reduced+expanded chain (greedy, same state and logits at every step)\n");
        printf("   same token as the full-array reference : %d/%d steps\n", p5_agree, p5_steps);
        printf("   answered without the full vocabulary  : %d/%d steps (median K %.0f)\n",
               p5_reduced_only, p5_steps, p5_k_reached.median());
        printf("   cost per step: reference %.3f ms | proposed %.3f ms\n\n",
               p5_ref_ms / (double)p5_steps, p5_prop_ms / (double)p5_steps);
    }

    /* ---------- P2: fixed-state arms in three real grammar states ---------- */
    struct state_arm {
        const char *name;
        size_t accepts;
    };
    size_t accept_counts[3] = {0, generated.size() / 4, generated.size()};
    const char *state_names[3] = {"root", "mid-action", "post-action"};

    printf("P2 fixed-state apply cost (median ms over 10 iterations), grammar state reached by\n"
           "   accepting the first N tokens of the P1 generation\n");
    printf("%-12s %10s %12s %12s %12s %12s %12s\n", "state", "gram-only", "chain-full",
           "K=20", "K=64", "K=256", "no-grammar");
    printf("-----------------------------------------------------------------------------------"
           "----\n");

    double json_arms[6] = {0};
    for (int s = 0; s < 3; s++) {
        /* One grammar sampler per state; the chain and the bare sampler reach
         * the same state because both accept the same tokens. */
        struct llama_sampler *g_only = make_grammar_only(gbnf.c_str(), vocab);
        struct llama_sampler *g_chain = make_chain(gbnf.c_str(), vocab, true);
        struct llama_sampler *plain = make_chain(gbnf.c_str(), vocab, false);
        if (!g_only || !g_chain || !plain) {
            fprintf(stderr, "sampler construction failed\n");
            return 1;
        }
        accept_tokens(g_only, generated, accept_counts[s]);
        accept_tokens(g_chain, generated, accept_counts[s]);

        int a = 0;
        double t_gram = time_apply(g_only, buf_full, snap.data(), n_vocab, NULL, 10, &a, NULL);
        double t_full = time_apply(g_chain, buf_full, snap.data(), n_vocab, NULL, 10, &a, NULL);
        double t_nogram = time_apply(plain, buf_full, snap.data(), n_vocab, NULL, 10, &a, NULL);
        double t20 = 0, t64 = 0, t256 = 0;
        for (int k : {20, 64, 256}) {
            topk_ids(snap.data(), n_vocab, k, ks, ids);
            double t = time_apply(g_chain, buf_red, snap.data(), n_vocab, &ks, 10, &a, NULL);
            if (k == 20)
                t20 = t;
            else if (k == 64)
                t64 = t;
            else
                t256 = t;
        }
        printf("%-12s %10.2f %12.2f %12.3f %12.3f %12.3f %12.2f\n", state_names[s], t_gram,
               t_full, t20, t64, t256, t_nogram);
        if (s == 1) {
            json_arms[0] = t_gram;
            json_arms[1] = t_full;
            json_arms[2] = t20;
            json_arms[3] = t64;
            json_arms[4] = t256;
            json_arms[5] = t_nogram;
        }
        llama_sampler_free(g_only);
        llama_sampler_free(g_chain);
        llama_sampler_free(plain);
    }
    printf("\n");

    /* ---------- P2b: permissive grammar state, the case reduction targets ---------- */
    double perm_full = 0, perm_k64 = 0, perm_nogram = 0;
    int perm_allowed_full = 0, perm_allowed_k64 = 0, perm_equiv = 0;
    {
        const char *permissive = "root ::= c*\nc ::= [\\t\\n] | [ -~]\n";
        struct llama_sampler *p_chain = make_chain(permissive, vocab, true);
        struct llama_sampler *p_plain = make_chain(permissive, vocab, false);
        if (p_chain && p_plain) {
            std::vector<llama_token> p_full, p_red;
            int a = 0;
            perm_full = time_apply(p_chain, buf_full, snap.data(), n_vocab, NULL, 10, &a, &p_full);
            perm_allowed_full = a;
            topk_ids(snap.data(), n_vocab, 64, ks, ids);
            perm_k64 = time_apply(p_chain, buf_red, snap.data(), n_vocab, &ks, 10, &a, &p_red);
            perm_allowed_k64 = a;
            perm_nogram = time_apply(p_plain, buf_full, snap.data(), n_vocab, NULL, 10, &a, NULL);
            perm_equiv = (p_full.size() == p_red.size() &&
                          std::equal(p_full.begin(), p_full.end(), p_red.begin()))
                             ? 1
                             : 0;
            printf("P2b permissive grammar (root ::= [printable-ascii]*), same logits:\n");
            printf("   chain on full array %8.2f ms | chain on K=64 %8.3f ms | no grammar %8.3f ms\n",
                   perm_full, perm_k64, perm_nogram);
            printf("   allowed: full array %d, K=64 array %d | allowed top-20 lists identical: %s\n\n",
                   perm_allowed_full, perm_allowed_k64, perm_equiv ? "yes" : "NO");
        }
        if (p_chain)
            llama_sampler_free(p_chain);
        if (p_plain)
            llama_sampler_free(p_plain);
    }

        /* ---------- P4: is the raw top-K sufficient, per state? ---------- */
    {
        const char * permissive = "root ::= c*\nc ::= [\\t\\n] | [ -~]\n";
        std::vector<llama_token> none;
        printf("P4 truncation risk of applying the grammar to a reduced array\n");
        topk_coverage("root", gbnf.c_str(), vocab, none, snap.data(), n_vocab, buf_full, buf_red,
                      ids, ks);
        std::vector<llama_token> quarter(generated.begin(),
                                         generated.begin() + (long)generated.size() / 4);
        topk_coverage("mid-action", gbnf.c_str(), vocab, quarter, snap.data(), n_vocab, buf_full,
                      buf_red, ids, ks);
        topk_coverage("post-action", gbnf.c_str(), vocab, generated, snap.data(), n_vocab, buf_full,
                      buf_red, ids, ks);
        topk_coverage("permissive", permissive, vocab, none, snap.data(), n_vocab, buf_full,
                      buf_red, ids, ks);
        printf("\n");
    }

/* ---------- P3: cost vs candidate-array size (stock grammar) ---------- */
    /* If the cost is per-candidate dispatch, it is linear in the array size at a
     * constant ns/candidate. If it were the recursion's vector churn, a shallow
     * grammar would be much cheaper than a deep one at the same size. Candidates
     * are a uniform random sample of the vocabulary, so the mix of tokens that
     * die on the first character class and tokens that walk deeper is
     * representative rather than flattering. */
    {
        struct llama_sampler *g_only = make_grammar_only(gbnf.c_str(), vocab);
        if (g_only) {
            accept_tokens(g_only, generated, generated.size() / 4);
            std::vector<llama_token> sample((size_t)n_vocab);
            for (int32_t i = 0; i < n_vocab; i++)
                sample[(size_t)i] = i;
            std::mt19937 rng(7);
            std::shuffle(sample.begin(), sample.end(), rng);
            printf("P3 cost vs candidate-array size (stock grammar, mid-action state, 10 iterations)\n");
            for (int n : {1024, 4096, 16384, 65536, n_vocab}) {
                std::vector<llama_token> ids_n(sample.begin(), sample.begin() + (n < n_vocab ? n : n_vocab));
                int a = 0;
                double t = time_apply(g_only, buf_red, snap.data(), n_vocab, &ids_n, 10, &a, NULL);
                printf("   %7d candidates: %9.3f ms  %8.1f ns/candidate\n", n, t, 1e6 * t / (double)n);
            }
            printf("\n");
            llama_sampler_free(g_only);
        }
    }
    /* ---------- machine-readable evidence ---------- */
    double ratio = red_apply_ms.median() > 0 ? full_apply_ms.median() / red_apply_ms.median() : 0.0;
    printf("{\n");
    printf("  \"spike\": \"002-grammar-mask-cost\",\n");
    printf("  \"vocab\": %d,\n", (int)n_vocab);
    printf("  \"grammar_source\": \"%s\",\n", grammar_source);
    printf("  \"render_lazy\": %d,\n", render_lazy);
    printf("  \"prompt_tokens\": %d,\n", (int)n_prompt);
    printf("  \"steps\": %d,\n", steps);
    printf("  \"p1\": {\"today_median_ms\": %.3f, \"full_apply_median_ms\": %.3f, "
           "\"reduced_median_ms\": %.3f, \"select_median_ms\": %.3f, \"full_over_reduced\": %.1f, "
           "\"match20\": %d, \"steps_with_20_allowed\": %d, \"match5\": %d, \"expansions\": %d, "
           "\"top1_match_k64\": %d, \"top1_steps\": %d, \"outside64\": %lld},\n",
           today_ms.median(), full_apply_ms.median(), red_apply_ms.median(), select_ms.median(),
           ratio, matches20, steps_ge20, matches5, expanded_steps, top1_match, top1_steps,
           outside64);
    printf("  \"p2_mid_action\": {\"grammar_only_ms\": %.3f, \"chain_full_ms\": %.3f, "
           "\"k20_ms\": %.3f, \"k64_ms\": %.3f, \"k256_ms\": %.3f, \"no_grammar_ms\": %.3f},\n",
           json_arms[0], json_arms[1], json_arms[2], json_arms[3], json_arms[4], json_arms[5]);
    printf("  \"p2_permissive\": {\"chain_full_ms\": %.3f, \"k64_ms\": %.3f, "
           "\"no_grammar_ms\": %.3f, \"allowed_full\": %d, \"allowed_k64\": %d, \"equiv_top20\": %d}\n",
           perm_full, perm_k64, perm_nogram, perm_allowed_full, perm_allowed_k64, perm_equiv);
    printf("}\n");

    llama_sampler_free(gen);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    free(prompt);
    return 0;
}
