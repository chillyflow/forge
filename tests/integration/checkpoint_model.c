/* Optional real-model evidence. No downloads or scripted substitute. At most
 * one model is loaded at a time; GPU layers default to 0 and require an argument. */
#include "forge/checkpoint.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
} output;

static bool collect(const char *bytes, size_t length, void *userdata) {
    output *out = userdata;
    if (length > 16384 - out->length)
        return false;
    char *next = realloc(out->data, out->length + length + 1);
    if (!next)
        return false;
    out->data = next;
    memcpy(out->data + out->length, bytes, length);
    out->length += length;
    out->data[out->length] = 0;
    return true;
}

static bool integer(const char *text, long minimum, long maximum, long *value) {
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || !*text || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = parsed;
    return true;
}

#define OUTPUT_TOKENS 16u
#define SOURCE_PROMPT_BYTES 4096u

typedef struct {
    char *prompt;
    forge_checkpoint *checkpoint;
    forge_checkpoint_info info;
    forge_checkpoint_stats save, restore, repeated_restore;
    forge_metrics restored_metrics, repeated_metrics, cold_metrics;
    output restored, repeated, cold;
} sample;

/* Complete snippets let small contexts shed source at function boundaries.
 * The actual checkpoint token count, including the selected chat template,
 * decides whether a candidate fits; character counts are not token evidence. */
static const char *const source_blocks[] = {
    "\nfunc Clamp(n int) int {\n    if n < 0 { return 0 }\n    return n\n}\n",
    "\nfunc Max(a, b int) int {\n    if a > b { return a }\n    return b\n}\n",
    "\nfunc Sum(values []int) int {\n    total := 0\n    for _, n := range values { total += n }\n"
    "    return total\n}\n",
    "\nfunc Lookup(values map[string]int, key string) (int, bool) {\n"
    "    value, found := values[key]\n    return value, found\n}\n",
    "\nfunc Copy(values []int) []int {\n    result := make([]int, len(values))\n"
    "    copy(result, values)\n    return result\n}\n",
    "\nfunc First(values []int) (int, bool) {\n    if len(values) == 0 { return 0, false }\n"
    "    return values[0], true\n}\n",
    "\nfunc Contains(values []int, wanted int) bool {\n"
    "    for _, n := range values { if n == wanted { return true } }\n    return false\n}\n",
    "\nfunc CountPositive(values []int) int {\n    count := 0\n"
    "    for _, n := range values { if n > 0 { count++ } }\n    return count\n}\n",
    "\nfunc RemoveEmpty(values []string) []string {\n    result := []string{}\n"
    "    for _, s := range values { if s != \"\" { result = append(result, s) } }\n"
    "    return result\n}\n",
    "\nfunc Path(left, right string) string {\n    if left == \"\" { return right }\n"
    "    return left + \"/\" + right\n}\n",
    "\nfunc ValidRange(start, end, limit int) bool {\n"
    "    return start >= 0 && end >= start && end <= limit\n}\n",
    "\nfunc NextVersion(current int) int {\n    if current < Epoch { return Epoch }\n"
    "    return current + 1\n}\n"};

#define SOURCE_BLOCK_COUNT (sizeof(source_blocks) / sizeof(*source_blocks))

static char *copy_prompt(const char *prompt, forge_error *error) {
    size_t length = strlen(prompt);
    char *copy = malloc(length + 1);
    if (!copy) {
        error->code = FORGE_ERR_MEMORY;
        snprintf(error->message, sizeof(error->message), "Cannot allocate checkpoint test prompt");
        return NULL;
    }
    memcpy(copy, prompt, length + 1);
    return copy;
}

static char *source_prompt(size_t blocks, size_t variant, forge_error *error) {
    char buffer[SOURCE_PROMPT_BYTES];
    const char *word = variant ? "beta" : "alpha";
    int count = snprintf(buffer, sizeof(buffer),
                         "Reply with exactly the word %s.\nSource context:\n```go\n"
                         "package cache\nconst Epoch = %u\n",
                         word, variant ? 9u : 7u);
    if (count < 0 || (size_t)count >= sizeof(buffer))
        return NULL;
    size_t length = (size_t)count;
    for (size_t i = 0; i < blocks; i++) {
        size_t bytes = strlen(source_blocks[i]);
        if (bytes > sizeof(buffer) - length - 1)
            return NULL;
        memcpy(buffer + length, source_blocks[i], bytes);
        length += bytes;
        buffer[length] = 0;
    }
    count = snprintf(buffer + length, sizeof(buffer) - length,
                     "\n```\nDo not explain the source; output only %s.\n", word);
    if (count < 0 || (size_t)count >= sizeof(buffer) - length)
        return NULL;
    return copy_prompt(buffer, error);
}

static void release_samples(sample samples[2]) {
    for (size_t i = 0; i < 2; i++) {
        forge_checkpoint_destroy(samples[i].checkpoint);
        free(samples[i].prompt);
        free(samples[i].restored.data);
        free(samples[i].repeated.data);
        free(samples[i].cold.data);
        samples[i] = (sample){0};
    }
}

/* Return 77 only for actual unsupported checkpoint backends. Capacity fitting
 * is bounded and uses no successful generation as a probing substitute. */
static int save_samples(forge_model *model, const forge_checkpoint_options *options,
                        const forge_model_config *config, bool source, size_t minimum_tokens,
                        sample samples[2], size_t *blocks_used, size_t *attempts,
                        forge_error *error) {
    size_t blocks = config->context_tokens >= 1024 ? 8 : config->context_tokens / 128;
    size_t maximum_tokens = config->context_tokens - OUTPUT_TOKENS;
    if (source && maximum_tokens > 500)
        maximum_tokens = 500;
    bool visited[SOURCE_BLOCK_COUNT + 1] = {false};
    const char *short_prompts[] = {"Reply with exactly the word alpha.",
                                   "Reply with exactly the word beta."};
    *attempts = 0;
    *blocks_used = 0;
    while (blocks <= SOURCE_BLOCK_COUNT && !visited[blocks]) {
        visited[blocks] = true;
        (*attempts)++;
        release_samples(samples);
        bool too_large = false;
        for (size_t i = 0; i < 2; i++) {
            *error = (forge_error){0};
            samples[i].prompt =
                source ? source_prompt(blocks, i, error) : copy_prompt(short_prompts[i], error);
            if (!samples[i].prompt) {
                if (error->code == FORGE_OK) {
                    error->code = FORGE_ERR_LIMIT;
                    snprintf(error->message, sizeof(error->message),
                             "Source-like prompt exceeds its fixed construction buffer");
                }
                return 1;
            }
            samples[i].checkpoint =
                forge_checkpoint_save(model, samples[i].prompt, options, &samples[i].save, error);
            if (!samples[i].checkpoint) {
                if (error->code == FORGE_ERR_UNSUPPORTED)
                    return 77;
                /* A context preflight failure has done no prefill. Do not hide
                 * state-copy caps or other later LIMIT errors by shrinking. */
                if (source && error->code == FORGE_ERR_LIMIT && !samples[i].save.prompt_tokens &&
                    !samples[i].save.prefill_tokens) {
                    too_large = true;
                    break;
                }
                return 1;
            }
            if (!forge_checkpoint_get_info(samples[i].checkpoint, &samples[i].info)) {
                error->code = FORGE_ERR_MODEL;
                snprintf(error->message, sizeof(error->message), "Saved checkpoint has no info");
                return 1;
            }
            if (samples[i].info.token_end > maximum_tokens) {
                too_large = true;
                break;
            }
        }
        if (!source && !too_large)
            return 0;
        if (!too_large && samples[0].info.token_end >= minimum_tokens &&
            samples[1].info.token_end >= minimum_tokens) {
            *blocks_used = blocks;
            return 0;
        }
        if (!source || (too_large && !blocks))
            break;
        if (too_large)
            blocks--;
        else
            blocks++;
    }
    error->code = FORGE_ERR_LIMIT;
    snprintf(error->message, sizeof(error->message),
             "Cannot fit %s prompts into %zu..%zu actual tokens with %u output tokens reserved",
             source ? "source-like" : "short", minimum_tokens, maximum_tokens, OUTPUT_TOKENS);
    return 1;
}

static bool same_output(const output *left, const output *right) {
    return left->length > 0 && left->length == right->length &&
           !memcmp(left->data, right->data, left->length);
}

#define REQUIRE(condition, label)                                                                  \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s/%s: %s", case_name, variant_name, label);                          \
            if (error.code != FORGE_OK)                                                            \
                fprintf(stderr, ": %s (%s)", error.message, forge_status_string(error.code));      \
            fputc('\n', stderr);                                                                   \
            goto cleanup;                                                                          \
        }                                                                                          \
    } while (0)

static int run_case(const forge_model_config *config, bool source, size_t minimum_tokens,
                    size_t *largest_prompt) {
    const char *case_name = source ? "source" : "short", *variant_name = "setup";
    forge_checkpoint_options options = forge_default_checkpoint_options();
    options.repo_generation = 42;
    forge_error error = {0};
    forge_model *model = NULL;
    sample samples[2] = {0};
    size_t blocks = 0, attempts = 0;
    int result = 1;

    model = forge_model_load(config, &error);
    REQUIRE(model, "Load checkpoint model");
    result = save_samples(model, &options, config, source, minimum_tokens, samples, &blocks,
                          &attempts, &error);
    if (result == 77) {
        fprintf(stderr, "SKIP: %s: %s\n", case_name, error.message);
        goto cleanup;
    }
    bool saved = result == 0;
    result = 1;
    REQUIRE(saved, "Save independent A/B checkpoints within context");
    REQUIRE(samples[0].info.id != samples[1].info.id &&
                samples[0].info.context_hash != samples[1].info.context_hash &&
                samples[0].info.token_hash != samples[1].info.token_hash,
            "A/B checkpoints must have distinct identities and token sequences");
    if (!source)
        REQUIRE(samples[0].save.cached_tokens == 0 &&
                    samples[0].save.prefill_tokens == samples[0].info.token_end,
                "Short A starts with cold prefill accounting");

    /* Save B displaced A. Each round restores and generates A then B, so both
     * independent host copies are tested after another sequence used the KV. */
    for (size_t round = 0; round < 2; round++)
        for (size_t i = 0; i < 2; i++) {
            sample *current = &samples[i];
            variant_name = i ? "B" : "A";
            error = (forge_error){0};
            REQUIRE(current->info.valid && current->info.token_start == 0 &&
                        current->info.token_end > 1 && current->info.state_bytes > 0,
                    "Nonempty physical checkpoint");
            REQUIRE(current->save.prompt_tokens == current->info.token_end &&
                        current->save.cached_tokens + current->save.prefill_tokens ==
                            current->info.token_end,
                    "Save reports actual full-prompt token accounting");
            forge_checkpoint_stats *restore =
                round ? &current->repeated_restore : &current->restore;
            forge_metrics *metrics =
                round ? &current->repeated_metrics : &current->restored_metrics;
            output *out = round ? &current->repeated : &current->restored;
            REQUIRE(forge_checkpoint_restore(model, current->checkpoint, &options, restore,
                                             &error) == FORGE_OK,
                    "Restore after the other sequence occupied live state");
            REQUIRE(restore->restored_tokens == current->info.token_end &&
                        restore->state_bytes == current->info.state_bytes,
                    "Restore byte/token accounting");
            error = (forge_error){0};
            REQUIRE(forge_complete(model, current->prompt, OUTPUT_TOKENS, collect, out, metrics,
                                   &error) == FORGE_OK,
                    "Generate from restored checkpoint");
            REQUIRE(out->length > 0 && metrics->generated_tokens > 0 && !metrics->simulated,
                    "Actual model output");
            REQUIRE(metrics->prompt_tokens == current->info.token_end &&
                        metrics->cached_tokens == current->info.token_end - 1 &&
                        metrics->prefill_tokens == 1,
                    "Exact-hit final prompt token must be decoded again");
            if (round)
                REQUIRE(same_output(&current->restored, &current->repeated),
                        "Repeated restore output byte parity");
        }

    /* One model at a time; no overlapping RAM/VRAM allocations. Disabling
     * prefix reuse on the new instance makes BOTH cold generations cold even
     * though they share a template and use that same newly loaded instance. */
    forge_model_destroy(model);
    model = NULL;
    forge_model_config cold_config = *config;
    cold_config.reuse_prefix = false;
    error = (forge_error){0};
    model = forge_model_load(&cold_config, &error);
    REQUIRE(model, "Load cold model");
    for (size_t i = 0; i < 2; i++) {
        sample *current = &samples[i];
        variant_name = i ? "B" : "A";
        error = (forge_error){0};
        forge_checkpoint_stats rejected = {0};
        REQUIRE(forge_checkpoint_restore(model, current->checkpoint, &options, &rejected, &error) ==
                    FORGE_ERR_CONFLICT,
                "Reloaded instance must reject old checkpoints");
        REQUIRE(!rejected.restored_tokens && !rejected.state_bytes,
                "Rejected restore must not report successful state reuse");
        error = (forge_error){0};
        REQUIRE(forge_complete(model, current->prompt, OUTPUT_TOKENS, collect, &current->cold,
                               &current->cold_metrics, &error) == FORGE_OK,
                "Cold greedy generation");
        REQUIRE(!current->cold_metrics.simulated && !current->cold_metrics.cached_tokens &&
                    current->cold_metrics.prefill_tokens == current->info.token_end,
                "Both variants use actual cold prefill");
        REQUIRE(same_output(&current->restored, &current->cold),
                "Restored/cold greedy output byte parity");
    }

    /* Emit case evidence only after both variants passed every parity check.
     * Counts describe the accepted capture; fit_attempts exposes calibration
     * work so these diagnostics cannot be mistaken for an end-to-end benchmark. */
    for (size_t i = 0; i < 2; i++) {
        sample *current = &samples[i];
        if (current->info.token_end > *largest_prompt)
            *largest_prompt = current->info.token_end;
        printf(
            "{\"real_model_checkpoint\":true,\"matched\":true,\"case\":\"%s\",\"variant\":\"%s\","
            "\"gpu_layers\":%d,\"context_tokens\":%zu,\"prompt_bytes\":%zu,\"prompt_tokens\":%zu,"
            "\"source_blocks\":%zu,\"fit_attempts\":%zu,\"state_bytes\":%zu,"
            "\"save_cached_tokens\":%zu,\"save_prefill_tokens\":%zu,\"restored_tokens\":%zu,"
            "\"cached_tokens\":%zu,\"recomputed_tokens\":%zu,\"repeat_cached_tokens\":%zu,"
            "\"repeat_recomputed_tokens\":%zu,\"cold_prefill_tokens\":%zu,\"cold_cached_tokens\":%"
            "zu,"
            "\"generated_tokens\":%zu,\"output_bytes\":%zu,\"save_ms\":%.3f,\"restore_ms\":%.3f}\n",
            case_name, i ? "B" : "A", config->gpu_layers, config->context_tokens,
            strlen(current->prompt), current->info.token_end, blocks, attempts,
            current->info.state_bytes, current->save.cached_tokens, current->save.prefill_tokens,
            current->restore.restored_tokens, current->restored_metrics.cached_tokens,
            current->restored_metrics.prefill_tokens, current->repeated_metrics.cached_tokens,
            current->repeated_metrics.prefill_tokens, current->cold_metrics.prefill_tokens,
            current->cold_metrics.cached_tokens, current->restored_metrics.generated_tokens,
            current->restored.length, current->save.save_ms, current->restore.restore_ms);
    }
    result = 0;

cleanup:
    forge_model_destroy(model);
    release_samples(samples);
    return result;
}

static int run_automatic(const forge_model_config *config) {
    const char *case_name = "automatic-source", *variant_name = "setup";
    forge_error error = {0};
    forge_model *model = NULL;
    sample samples[2] = {0};
    output invalidated = {0};
    int result = 1;
    REQUIRE(config->context_tokens >= 512,
            "Automatic fixture requires at least 512 context tokens");
    forge_model_config cold_config = *config;
    cold_config.reuse_prefix = false;
    model = forge_model_load(&cold_config, &error);
    REQUIRE(model, "Load uncached reference model");
    for (size_t i = 0; i < 2; i++) {
        variant_name = i ? "B" : "A";
        samples[i].prompt = source_prompt(4, i, &error);
        REQUIRE(samples[i].prompt, "Create bounded source fixture");
        REQUIRE(forge_complete(model, samples[i].prompt, OUTPUT_TOKENS, collect, &samples[i].cold,
                               &samples[i].cold_metrics, &error) == FORGE_OK,
                "Cold reference generation");
        REQUIRE(!samples[i].cold_metrics.simulated && !samples[i].cold_metrics.cached_tokens &&
                    samples[i].cold_metrics.prefill_tokens == samples[i].cold_metrics.prompt_tokens,
                "Reference must decode the complete actual prompt");
    }
    forge_model_destroy(model);
    model = forge_model_load(config, &error);
    REQUIRE(model, "Load automatic-cache model");
    forge_checkpoint_cache_options options = forge_default_checkpoint_cache_options();
    options.max_entries = 2;
    options.min_prefix_tokens = 16;
    options.max_captures_per_prompt = 1;
    REQUIRE(forge_checkpoint_cache_configure(model, &options, &error) == FORGE_OK,
            "Enable supported physical cache");
    forge_checkpoint_cache_request request = {"checkpoint-model-fixture", "automatic-a-b-a", 7,
                                              NULL, 1};
    for (size_t round = 0; round < 2; round++)
        for (size_t i = 0; i < 2; i++) {
            sample *current = &samples[i];
            variant_name = i ? "B" : "A";
            size_t anchor = strlen(current->prompt);
            request.anchor_ends = &anchor;
            output *out = round ? &current->repeated : &current->restored;
            forge_metrics *metrics =
                round ? &current->repeated_metrics : &current->restored_metrics;
            REQUIRE(forge_complete_with_cache(model, current->prompt, &request, OUTPUT_TOKENS,
                                              collect, out, metrics, &error) == FORGE_OK,
                    "Automatic A/B/A/B generation");
            REQUIRE(!metrics->simulated && same_output(out, &current->cold),
                    "Automatic and cold output byte parity");
            REQUIRE(metrics->cached_tokens + metrics->prefill_tokens == metrics->prompt_tokens,
                    "Actual prompt accounting");
            if (round)
                REQUIRE(metrics->checkpoint_hits == 1 && metrics->prefill_tokens == 1 &&
                            metrics->checkpoint_restored_tokens == metrics->prompt_tokens &&
                            metrics->checkpoint_reused_tokens == metrics->prompt_tokens - 1 &&
                            metrics->checkpoint_additional_tokens > 0,
                        "A displaced prefix must be restored and reused with one token recomputed");
            else
                REQUIRE(metrics->checkpoint_captures == 1 && !metrics->checkpoint_hits,
                        "Initial prefixes must be captured without an explicit save call");
        }
    forge_checkpoint_cache_stats stats = {0};
    REQUIRE(forge_checkpoint_cache_get_stats(model, &stats) && stats.entries == 2 &&
                !stats.pending_bytes && stats.peak_bytes <= options.max_bytes,
            "Aggregate manager accounting remains within the configured cap");
    size_t anchor = strlen(samples[0].prompt);
    request.anchor_ends = &anchor;
    request.repo_generation++;
    forge_metrics changed = {0};
    REQUIRE(forge_complete_with_cache(model, samples[0].prompt, &request, OUTPUT_TOKENS, collect,
                                      &invalidated, &changed, &error) == FORGE_OK,
            "Generate after source-generation change");
    REQUIRE(!changed.checkpoint_hits && same_output(&invalidated, &samples[0].cold),
            "Source-generation change rejects old physical entries");
    forge_checkpoint_cache_stats after = {0};
    REQUIRE(forge_checkpoint_cache_get_stats(model, &after) &&
                after.invalidations > stats.invalidations,
            "Generation invalidation is observable");
    for (size_t i = 0; i < 2; i++) {
        const forge_metrics *m = &samples[i].repeated_metrics;
        printf("{\"real_model_automatic_checkpoint\":true,\"matched\":true,\"variant\":\"%s\","
               "\"gpu_layers\":%d,\"context_tokens\":%zu,\"prompt_tokens\":%zu,"
               "\"cold_prefill_tokens\":%zu,\"cached_tokens\":%zu,\"prefill_tokens\":%zu,"
               "\"generated_tokens\":%zu,\"cold_generated_tokens\":%zu,"
               "\"restored_tokens\":%llu,\"additional_matched_tokens\":%llu,"
               "\"peak_bytes\":%zu,\"max_bytes\":%zu,\"probe_ms\":%.3f,"
               "\"capture_ms\":%.3f,\"restore_ms\":%.3f,\"generation_invalidated\":true}\n",
               i ? "B" : "A", config->gpu_layers, config->context_tokens, m->prompt_tokens,
               samples[i].cold_metrics.prefill_tokens, m->cached_tokens, m->prefill_tokens,
               m->generated_tokens, samples[i].cold_metrics.generated_tokens,
               (unsigned long long)m->checkpoint_restored_tokens,
               (unsigned long long)m->checkpoint_additional_tokens, stats.peak_bytes,
               options.max_bytes, m->checkpoint_probe_ms,
               samples[i].restored_metrics.checkpoint_capture_ms, m->checkpoint_restore_ms);
    }
    result = 0;
cleanup:
    forge_model_destroy(model);
    free(invalidated.data);
    release_samples(samples);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "SKIP: supply a local GGUF: checkpoint_model [--automatic] MODEL "
                        "[GPU_LAYERS] [CONTEXT] "
                        "[CHAT_TEMPLATE]\n");
        return 77;
    }
    bool automatic = !strcmp(argv[1], "--automatic");
    if (automatic) {
        argv++;
        argc--;
    }
    long gpu_layers = 0, context = 1024;
    if (argc < 2 || argc > 5 || (argc > 2 && !integer(argv[2], -1, INT_MAX, &gpu_layers)) ||
        (argc > 3 && !integer(argv[3], 128, 1048576, &context))) {
        fprintf(stderr, "Invalid checkpoint-model test arguments\n");
        return 1;
    }
    forge_model_config config = forge_default_model_config();
    config.model_path = argv[1];
    config.gpu_layers = (int)gpu_layers;
    config.context_tokens = (size_t)context;
    config.temperature = 0;
    config.threads = 1;
    config.reuse_prefix = true;
    config.chat_template = argc > 4 ? argv[4] : NULL;
    if (automatic)
        return run_automatic(&config);
    size_t short_tokens = 0, source_tokens = 0;
    int result = run_case(&config, false, 2, &short_tokens);
    if (result)
        return result;
    size_t minimum_source_tokens = short_tokens + 1;
    if (config.context_tokens >= 1024 && minimum_source_tokens < 200)
        minimum_source_tokens = 200;
    return run_case(&config, true, minimum_source_tokens, &source_tokens);
}
