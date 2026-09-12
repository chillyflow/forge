/* 001-parallel-decode: what does N-way batched decode actually cost?
 *
 * STRATEGIC QUESTION
 * Cloud harnesses are designed around token scarcity: every candidate costs
 * money, so candidates are generated one at a time and share a single token
 * budget. Forge's own --candidates does exactly this - it generates candidates
 * sequentially and divides the remaining budget by the number left, so more
 * candidates makes each candidate weaker.
 *
 * A laptop GPU is a different machine. Decode is memory-bandwidth-bound: the
 * model weights are read once per token regardless of how many sequences are
 * being served. If that holds, N sequences decoded in ONE batch should produce
 * N tokens for barely more than the cost of one, and reliability-through-
 * redundancy (generate N attempts, verify, keep a passer) becomes affordable.
 * If batched decode instead costs ~N x, the idea is dead and Forge should stop
 * chasing it.
 *
 * THIS PROGRAM
 * For N in {1,2,4,8}, produce N continuations two ways and time both:
 *
 *   sequential - N cold runs, each clearing the KV, prefilling alone, and
 *                decoding alone. This is what --candidates does today.
 *   batched    - one prefill into sequence 0, fork the KV N ways with
 *                llama_memory_seq_cp, then decode all N in one batch per step.
 *
 * Candidates are sampled with a per-sequence RNG seed so they DIVERGE. Greedy
 * decoding would keep all N identical forever, which would keep MoE expert
 * routing identical too and badly flatter the batched number.
 *
 * Sampling config mirrors Forge's campaign runs (top_k 20, top_p 0.8, temp 0.6)
 * and context params mirror src/inference/llama_backend.c so the numbers
 * transfer.
 *
 * Build: cmake target forge_parallel_sweep.
 * Run:   forge_parallel_sweep --model <gguf> --prompt <file> [--steps 128]
 */

#include "llama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define MAX_SEQUENCES 8

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
    char *text = malloc((size_t)size + 1);
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

/* A single llama_decode call may not exceed the context's n_batch, so the
 * prompt is submitted in chunks. Only the final token of the final chunk asks
 * for logits. */
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
        /* Logits are indexed within the most recent decode, not the whole
         * prompt, so the caller needs the final chunk's width. */
        *logits_index = take - 1;
    }
    return true;
}

/* One batched step: n tokens, one per sequence, sampled independently. */
static bool step_batched(struct llama_context *ctx, struct llama_sampler **samplers,
                         llama_token *tokens, llama_pos position, int32_t n) {
    struct llama_batch b = llama_batch_init(n, 0, n);
    if (!b.token) {
        fprintf(stderr, "batch allocation failed\n");
        return false;
    }
    for (int32_t i = 0; i < n; i++) {
        b.token[i] = tokens[i];
        b.pos[i] = position;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = i;
        b.logits[i] = 1;
    }
    b.n_tokens = n;
    bool ok = llama_decode(ctx, b) == 0;
    llama_batch_free(b);
    if (!ok)
        return false;
    for (int32_t i = 0; i < n; i++) {
        llama_token sampled = llama_sampler_sample(samplers[i], ctx, i);
        llama_sampler_accept(samplers[i], sampled);
        tokens[i] = sampled;
    }
    return true;
}

/* One single-sequence step, for the sequential baseline. */
static bool step_single(struct llama_context *ctx, struct llama_sampler *sampler,
                        llama_token *token, llama_pos position) {
    struct llama_batch b = llama_batch_init(1, 0, 1);
    if (!b.token) {
        fprintf(stderr, "batch allocation failed\n");
        return false;
    }
    b.token[0] = *token;
    b.pos[0] = position;
    b.n_seq_id[0] = 1;
    b.seq_id[0][0] = 0;
    b.logits[0] = 1;
    b.n_tokens = 1;
    bool ok = llama_decode(ctx, b) == 0;
    llama_batch_free(b);
    if (!ok)
        return false;
    llama_token sampled = llama_sampler_sample(sampler, ctx, 0);
    llama_sampler_accept(sampler, sampled);
    *token = sampled;
    return true;
}

static struct llama_sampler *make_sampler(uint32_t seed) {
    struct llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    struct llama_sampler *chain = llama_sampler_chain_init(sp);
    if (!chain)
        return NULL;
    llama_sampler_chain_add(chain, llama_sampler_init_top_k(20));
    llama_sampler_chain_add(chain, llama_sampler_init_top_p(0.8f, 1));
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.6f));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(seed));
    return chain;
}

int main(int argc, char **argv) {
    const char *model_path = NULL;
    const char *prompt_path = NULL;
    int steps = 128;
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
    if (!model_path || !prompt_path || steps < 2) {
        fprintf(stderr,
                "usage: forge_parallel_sweep --model <gguf> --prompt <file> [--steps 128]\n");
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

    /* Mirror src/inference/llama_backend.c's runtime configuration. */
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 8192;
    cp.n_batch = 512;
    cp.n_ubatch = 256;
    cp.n_seq_max = MAX_SEQUENCES;
    cp.n_threads = 24;
    cp.kv_unified = true; /* candidates share a large prefix */
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        fprintf(stderr, "context creation failed\n");
        return 1;
    }
    llama_memory_t mem = llama_get_memory(ctx);

    int32_t n_prompt = -llama_tokenize(vocab, prompt, prompt_length, NULL, 0, true, true);
    if (n_prompt <= 0) {
        fprintf(stderr, "tokenization failed\n");
        return 1;
    }
    llama_token *tokens = malloc(sizeof(llama_token) * (size_t)n_prompt);
    if (!tokens ||
        llama_tokenize(vocab, prompt, prompt_length, tokens, n_prompt, true, true) < 0) {
        fprintf(stderr, "tokenization failed\n");
        return 1;
    }

    printf("prompt tokens : %d\n", n_prompt);
    printf("steps/candidate: %d\n", steps);
    printf("sampling      : top_k 20, top_p 0.8, temp 0.6, per-sequence seed\n");
    printf("ctx           : n_ctx 8192, n_batch 512, n_ubatch 256, n_seq_max %d, kv_unified\n\n",
           MAX_SEQUENCES);

    /* Prefill cost on its own, so the tables below can be read either way. */
    int32_t logits_index = 0;
    llama_memory_clear(mem, true);
    double prefill_start = now_ms();
    if (!prefill(ctx, tokens, n_prompt, &logits_index)) {
        fprintf(stderr, "prefill failed\n");
        return 1;
    }
    llama_synchronize(ctx);
    double prefill_ms = now_ms() - prefill_start;

    printf("shared prefill: %.0f ms for %d tokens (%.0f tok/s)\n\n", prefill_ms, n_prompt,
           (double)n_prompt / (prefill_ms / 1000.0));

    printf("%-3s %11s %11s %11s %11s %10s %11s\n", "N", "seq_prefill", "seq_decode",
           "bat_prefill", "bat_decode", "prefill_x", "decode_x");
    printf("%s\n", "-------------------------------------------------------------------------------"
                   "----------------");

    for (int n = 1; n <= MAX_SEQUENCES; n *= 2) {
        /* Sequential: N cold runs, each prefilling and decoding alone. Prefill
         * and decode are timed separately so the decode scaling is isolable -
         * at short step counts the prefill dominates and would flatter the
         * batched number. */
        double seq_prefill_ms = 0, seq_decode_ms = 0;
        for (int c = 0; c < n; c++) {
            llama_memory_clear(mem, true);
            double start = now_ms();
            if (!prefill(ctx, tokens, n_prompt, &logits_index)) {
                fprintf(stderr, "prefill failed\n");
                return 1;
            }
            llama_synchronize(ctx);
            seq_prefill_ms += now_ms() - start;

            struct llama_sampler *sampler = make_sampler(1000u + (uint32_t)c);
            llama_token current = llama_sampler_sample(sampler, ctx, logits_index);
            llama_sampler_accept(sampler, current);
            double decode_start = now_ms();
            for (int s = 0; s < steps - 1; s++)
                if (!step_single(ctx, sampler, &current, n_prompt + s)) {
                    fprintf(stderr, "sequential decode failed at step %d\n", s);
                    return 1;
                }
            llama_synchronize(ctx);
            seq_decode_ms += now_ms() - decode_start;
            llama_sampler_free(sampler);
        }

        /* Batched: one prefill, fork the KV N ways, decode all N per step. */
        llama_memory_clear(mem, true);
        double batched_prefill_start = now_ms();
        if (!prefill(ctx, tokens, n_prompt, &logits_index)) {
            fprintf(stderr, "prefill failed\n");
            return 1;
        }
        for (int i = 1; i < n; i++)
            llama_memory_seq_cp(mem, 0, i, -1, -1);
        llama_synchronize(ctx);
        double bat_prefill_ms = now_ms() - batched_prefill_start;

        struct llama_sampler *samplers[MAX_SEQUENCES] = {0};
        for (int i = 0; i < n; i++)
            samplers[i] = make_sampler(2000u + (uint32_t)i);

        llama_token current[MAX_SEQUENCES];
        llama_token first = llama_sampler_sample(samplers[0], ctx, logits_index);
        for (int i = 0; i < n; i++) {
            llama_sampler_accept(samplers[i], first);
            current[i] = first;
        }
        double batched_decode_start = now_ms();
        for (int s = 0; s < steps - 1; s++)
            if (!step_batched(ctx, samplers, current, n_prompt + s, n)) {
                fprintf(stderr, "batched decode failed at step %d (N=%d)\n", s, n);
                return 1;
            }
        llama_synchronize(ctx);
        double bat_decode_ms = now_ms() - batched_decode_start;
        for (int i = 0; i < n; i++)
            llama_sampler_free(samplers[i]);

        /* prefill_x: N prefills vs 1. decode_x: N single-stream generations
         * of (steps-1) tokens each vs one batched generation of N of them. */
        printf("%-3d %11.0f %11.0f %11.0f %11.0f %9.2fx %10.2fx\n", n, seq_prefill_ms,
               seq_decode_ms, bat_prefill_ms, bat_decode_ms,
               bat_prefill_ms > 0 ? seq_prefill_ms / bat_prefill_ms : 0.0,
               bat_decode_ms > 0 ? seq_decode_ms / bat_decode_ms : 0.0);
        fflush(stdout);
    }

    free(tokens);
    free(prompt);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
