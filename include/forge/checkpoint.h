#ifndef FORGE_CHECKPOINT_H
#define FORGE_CHECKPOINT_H
#include "forge.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct forge_checkpoint forge_checkpoint;

/* Limits cover the host state copy, not the already allocated model/KV memory. */
#define FORGE_CHECKPOINT_MAX_STATE_BYTES ((size_t)1024 * 1024 * 1024)
#define FORGE_CHECKPOINT_MAX_TIMEOUT_MS UINT64_C(3600000)

typedef struct {
    size_t max_state_bytes;
    uint64_t repo_generation;
    uint64_t timeout_ms;
    forge_cancel_fn cancelled;
    void *userdata;
    /* Save only: same instance and an exact token prefix are required. The child
     * owns its complete state; destroying the parent does not invalidate it. */
    const forge_checkpoint *parent;
} forge_checkpoint_options;

typedef struct {
    uint64_t id, parent, repo_generation, context_hash, token_hash;
    size_t token_start, token_end, state_bytes;
    /* Means a complete physical copy was captured, not that source files remain
     * current or that this handle belongs to a subsequently loaded model. */
    bool valid;
} forge_checkpoint_info;

typedef struct {
    size_t prompt_tokens, cached_tokens, prefill_tokens, restored_tokens, state_bytes;
    double prefill_ms, save_ms, restore_ms, duration_ms;
} forge_checkpoint_stats;

/* Defaults: 256 MiB host state cap, 120 s cooperative timeout, generation 0. */
forge_checkpoint_options forge_default_checkpoint_options(void);

/* Explicitly prefill prefix_prompt without sampling, then save sequence 0.
 * This replaces the model's live prompt state; even a later size/parent/capture
 * failure may leave that successfully prefilled prompt active. Returned handles
 * own their tokens and host bytes and may outlive the model. No disk I/O occurs.
 * Scripted and unsupported model architectures return FORGE_ERR_UNSUPPORTED. */
forge_checkpoint *forge_checkpoint_save(forge_model *, const char *prefix_prompt,
                                        const forge_checkpoint_options *, forge_checkpoint_stats *,
                                        forge_error *);

/* Restore only to the original model instance, with matching repo_generation.
 * options.parent is ignored. Any failure after beginning the state write clears
 * the live KV/token cache; failed preflight checks leave it unchanged.
 * Next generation still compares actual token IDs and re-decodes its final
 * prompt token. No sampler/RNG state or mid-generation continuation is saved. */
forge_status forge_checkpoint_restore(forge_model *, const forge_checkpoint *,
                                      const forge_checkpoint_options *, forge_checkpoint_stats *,
                                      forge_error *);

bool forge_checkpoint_get_info(const forge_checkpoint *, forge_checkpoint_info *);
void forge_checkpoint_destroy(forge_checkpoint *);

/* One operation per model, no concurrent use or destruction during callbacks.
 * Cancellation/deadlines are checked around each stage and prefill batch;
 * individual llama.cpp decode/state-copy calls may overrun the deadline. */

#ifdef __cplusplus
}
#endif
#endif
