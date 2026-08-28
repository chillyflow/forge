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

/* Automatic cache is disabled until explicitly configured. These bounds cover
 * manager-owned requested allocations, including transient probe/capture work,
 * not allocator overhead, inference buffers or caller-owned explicit handles. */
#define FORGE_CHECKPOINT_CACHE_MAX_BYTES FORGE_CHECKPOINT_MAX_STATE_BYTES
#define FORGE_CHECKPOINT_CACHE_MAX_ENTRIES 64u
#define FORGE_CHECKPOINT_CACHE_MAX_ANCHORS 4u
#define FORGE_CHECKPOINT_CACHE_MAX_WORKSPACE_BYTES 4095u
#define FORGE_CHECKPOINT_CACHE_MAX_CONTEXT_BYTES 256u

typedef struct {
    size_t max_bytes, max_entries, min_prefix_tokens, max_captures_per_prompt;
} forge_checkpoint_cache_options;

typedef struct {
    /* Exact nonempty UTF-8 namespaces, not filesystem paths checked by this API.
     * The host supplies a canonical workspace and a unique logical-context ID.
     * A change to either namespace or generation invalidates all cache entries. */
    const char *workspace, *context_id;
    uint64_t repo_generation;
    /* Strictly increasing UTF-8 byte boundaries in the complete raw prompt.
     * Each nominates an eligible prefix; tokenization determines the actual cut.
     * Zero anchors bypass lookup/capture. Arrays/strings are borrowed only for
     * the synchronous call. Never derive these offsets by adding token counts. */
    const size_t *anchor_ends;
    size_t anchor_count;
} forge_checkpoint_cache_request;

typedef struct {
    bool enabled;
    size_t max_bytes, resident_bytes, pending_bytes, peak_bytes, entries;
    uint64_t requests, lookups, hits, misses, captures, evictions, invalidations;
    uint64_t skipped_no_request, skipped_ablation, skipped_no_anchor, skipped_live_prefix;
    uint64_t skipped_budget, skipped_unsupported, probe_failures, capture_failures;
    uint64_t restore_failures, cancellations;
    /* Successful physical restores versus tokens subsequently matched by actual
     * prefill. additional_matched_tokens subtracts the previously usable live
     * prefix; it is not an elapsed-time or end-to-end performance claim. */
    uint64_t tokens_restored, restored_tokens_reused, additional_matched_tokens;
    double probe_ms, capture_ms, restore_ms;
} forge_checkpoint_cache_stats;

/* Defaults: 256 MiB aggregate, eight entries, 128-token minimum, two captures.
 * NULL disables/frees the manager. Valid reconfiguration discards the old cache
 * and counters before allocating the replacement; allocation failure leaves it
 * disabled. Invalid options/unsupported backends leave the old cache unchanged. */
forge_checkpoint_cache_options forge_default_checkpoint_cache_options(void);
forge_status forge_checkpoint_cache_configure(forge_model *, const forge_checkpoint_cache_options *,
                                              forge_error *);
/* Clears retained entries/namespaces, preserving options/cumulative counters
 * and the model's ordinary live prefix. All mutations reject active operations. */
forge_status forge_checkpoint_cache_clear(forge_model *, forge_error *);
/* A valid disabled model returns a zeroed stats object with enabled=false. */
bool forge_checkpoint_cache_get_stats(const forge_model *, forge_checkpoint_cache_stats *);

/* Additive complete API. The request cannot grant permissions or establish
 * repository freshness. Without a configured manager it generates normally;
 * existing forge_complete() never consults/captures automatic checkpoints.
 * reuse_prefix=false bypasses this cache as well as ordinary prefix reuse.
 * Cancellation/deadlines for agent calls use the existing guarded internal
 * generation path; this convenience call, like forge_complete, has no timeout. */
forge_status forge_complete_with_cache(forge_model *, const char *prompt,
                                       const forge_checkpoint_cache_request *, size_t max_tokens,
                                       forge_token_fn, void *, forge_metrics *, forge_error *);

/* One operation per model, no concurrent use or destruction during callbacks.
 * Cancellation/deadlines are checked around each stage and prefill batch;
 * individual llama.cpp decode/state-copy calls may overrun the deadline. */

#ifdef __cplusplus
}
#endif
#endif
