#ifndef FORGE_STATE_H
#define FORGE_STATE_H
#include "forge.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_STATE_SCHEMA_VERSION 1
#define FORGE_STATE_MAX_GOAL_BYTES 8192u
#define FORGE_STATE_MAX_MODEL_BYTES 8192u
#define FORGE_STATE_MAX_ITEMS 32u
#define FORGE_STATE_MAX_ITEM_BYTES 512u
#define FORGE_STATE_MAX_CHANGED_PATHS 1024u
#define FORGE_STATE_MAX_PATH_BYTES 4095u
#define FORGE_STATE_MAX_RECENT_OUTCOMES 32u
#define FORGE_STATE_MAX_DETAIL_BYTES 512u
#define FORGE_STATE_MAX_TOOL_NAME_BYTES 128u

typedef struct forge_working_state forge_working_state;

typedef enum {
    FORGE_STATE_UNVERIFIED = 0,
    FORGE_STATE_PASSED,
    FORGE_STATE_FAILED,
    FORGE_STATE_DENIED,
    FORGE_STATE_NOT_APPLICABLE
} forge_state_validation_status;

typedef struct {
    uint64_t tool_call_id; /* Zero is allowed for synthetic host observations. */
    const char *tool_name;
    const char *path;    /* Optional workspace-relative path; required if changed. */
    forge_status result; /* Host-observed result; never promotes validation status. */
    const char *detail;  /* Optional UTF-8; retained prefix is capped at 512 bytes. */
    uint64_t generation;
    bool changed;
} forge_state_observation;

/* State owns copies of all inputs. A state is not safe for concurrent mutation.
 * The immutable nonempty goal is limited to FORGE_STATE_MAX_GOAL_BYTES.
 * Host text inputs must be NUL-terminated valid UTF-8; JSON strings must not
 * contain embedded NUL. Paths are normalized to forward slashes and must be
 * relative, without empty, "." or ".." components or a drive prefix.
 */
forge_working_state *forge_working_state_create(const char *goal, forge_error *);
void forge_working_state_destroy(forge_working_state *);

/* Transactionally update model claims only. Accept a legacy JSON notes string
 * or an object containing exactly these five arrays of strings:
 * facts, hypotheses, decisions, relevant_files, remaining.
 *
 * A string changes notes without clearing typed fields. An object replaces all
 * typed fields without clearing legacy notes. The cumulative decoded UTF-8
 * payload (notes plus all array strings) is capped at 8192 bytes. Each array
 * holds at most 32 items, each at most 512 bytes; serialized input <= 64 KiB.
 * Duplicate/unknown/missing fields, NUL, invalid UTF-8, and limits are rejected
 * without changing state. The model cannot alter the goal or observed evidence.
 */
forge_status forge_working_state_update_json(forge_working_state *, const char *memory_value_json,
                                             forge_error *);

/* Host evidence only. Older generations are rejected. An increase invalidates
 * validation and marks unchanged model claims stale, without deleting them.
 * Successful tool results do not mark validation passed.
 *
 * Keep 32 recent outcomes and count evictions/detail-byte omissions. Distinct
 * normalized changed paths are retained up to 1024. A new path beyond this cap
 * returns FORGE_ERR_LIMIT, counts the rejected observation, marks evidence
 * incomplete, and invalidates validation. This fail-closed metadata change is
 * intentional; no later PASSED status is allowed for an incomplete state.
 */
forge_status forge_working_state_observe(forge_working_state *, const forge_state_observation *,
                                         forge_error *);

/* Only host code may set validation. It applies to the supplied generation.
 * A newer generation first invalidates old state; an older one is rejected.
 * Details are bounded UTF-8 prefixes with explicit omitted-byte accounting.
 */
forge_status forge_working_state_set_validation(forge_working_state *, uint64_t generation,
                                                forge_state_validation_status, const char *detail,
                                                forge_error *);

/* Deterministic schema-versioned JSON, including model claims, observed
 * changes/outcomes, validation generation, staleness, and overflow counters.
 * The caller owns the returned string and releases it with forge_free.
 */
char *forge_working_state_json(const forge_working_state *, forge_error *);

/* Prompt view with an exact serialized-byte cap. Never drops the immutable
 * goal, model notes/fields, current validation, or overflow/staleness metadata.
 * Include recent host evidence as space permits, with context_omitted counts
 * and full_state_artifact pointing to working_state.json. Return FORGE_ERR_LIMIT
 * if the mandatory core cannot fit. Does not mutate the full state.
 */
char *forge_working_state_context_json(const forge_working_state *, size_t max_bytes,
                                       forge_error *);

/* Smallest prompt view: preserve the complete mandatory core above and omit
 * all optional host changes/outcomes with explicit omission counts. Use this
 * when fitting context with an actual tokenizer; it is still subject to the
 * caller's overall context limit and must never be silently truncated. */
char *forge_working_state_context_core_json(const forge_working_state *, forge_error *);

#ifdef __cplusplus
}
#endif
#endif
