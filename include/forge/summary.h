#ifndef FORGE_SUMMARY_H
#define FORGE_SUMMARY_H
#include "context.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_SUMMARY_MAX_INPUT_BYTES ((size_t)1024 * 1024)
#define FORGE_SUMMARY_MAX_TEXT_BYTES ((size_t)64 * 1024)
#define FORGE_SUMMARY_MAX_DEPENDENCIES ((size_t)100000)
#define FORGE_SUMMARY_MAX_MANIFEST_BYTES ((size_t)8 * 1024 * 1024)
#define FORGE_SUMMARY_MAX_SOURCE_BYTES ((size_t)256 * 1024 * 1024)
#define FORGE_SUMMARY_MAX_CACHE_ENTRIES ((size_t)4096)
#define FORGE_SUMMARY_MAX_CACHE_BYTES ((size_t)64 * 1024 * 1024)

typedef enum {
    FORGE_SUMMARY_REPOSITORY,
    FORGE_SUMMARY_MODULE,
    FORGE_SUMMARY_PACKAGE,
    FORGE_SUMMARY_FILE,
    FORGE_SUMMARY_SYMBOL
} forge_summary_scope;
typedef enum { FORGE_SUMMARY_OUTLINE, FORGE_SUMMARY_FULL_SOURCE } forge_summary_evidence;
typedef enum {
    FORGE_SUMMARY_MISS,
    FORGE_SUMMARY_HIT,
    FORGE_SUMMARY_CORRUPT
} forge_summary_cache_status;

typedef struct {
    forge_summary_scope scope;
    /* Workspace-relative directory for module/package, file for file/symbol.
     * Repository path is NULL or ".". No live filesystem lookup is performed. */
    const char *path;
    const char *symbol; /* Required only for symbol scope. Exact, case-sensitive. */
    const char *kind;   /* Optional syntactic declaration kind, not a resolved type. */
    bool has_start_byte;
    size_t start_byte; /* Optional current indexed locator to disambiguate names. */
} forge_summary_target;

typedef struct {
    /* Caller must include model/profile/template/instruction versions in these
     * identities when they affect generation. The cache cannot discover them. */
    const char *recipe_id, *producer_id, *instructions;
    /* File/symbol always supply their complete source/span. Aggregate OUTLINE
     * supplies complete member inventories, imports and declaration signatures;
     * it does not imply access to bodies or resolved semantics. FULL_SOURCE also
     * includes each member's source, or returns LIMIT without clipping. */
    forge_summary_evidence evidence;
    size_t max_input_bytes, max_input_tokens, max_summary_bytes, max_summary_tokens;
    size_t max_dependencies, max_manifest_bytes, max_source_bytes;
    size_t max_cache_entries, max_cache_bytes; /* Manifest + text payload, not SQLite RSS. */
    uint64_t timeout_ms, deadline_ms, max_vm_steps;
    forge_count_tokens_fn count_tokens;
    void *count_userdata;
    forge_cancel_fn cancelled;
    void *userdata;
} forge_summary_options;

typedef struct forge_summary_input forge_summary_input;
typedef struct {
    forge_summary_scope scope;
    forge_summary_evidence evidence;
    forge_summary_cache_status cache_status;
    uint64_t generation, created_generation, validated_generation;
    const char *path, *symbol, *kind, *recipe_id, *producer_id;
    const char *recipe_hash, *dependency_hash, *cache_key;
    const char *prompt, *manifest_json, *text;
    size_t dependencies, source_bytes, input_bytes, input_tokens, text_bytes, text_tokens;
    size_t start_byte, end_byte, line; /* Current symbol locator; zero for other scopes. */
    bool tokens_known, go_index_incomplete, filesystem_scan;
} forge_summary_view;
typedef struct {
    uint64_t generation;
    size_t evicted_entries;
    bool reused, repaired_corruption;
} forge_summary_store_result;

forge_summary_options forge_default_summary_options(void);
/* Uses only a committed indexed snapshot. Never refreshes the index, invokes a
 * process/model, or reads live files. Missing SHA metadata requires reindexing.
 * No dependency or source text is silently clipped. A corrupt cache row is a
 * CORRUPT miss with text=NULL; malformed indexed evidence is an error.
 *
 * Result owns copied strings; callbacks/userdata remain borrowed until destroy.
 * Token counts require count_tokens; nonzero token budgets without it fail.
 * Generation records observed indexed state, not unobserved filesystem edits. */
forge_summary_input *forge_repo_summary_prepare(forge_repo *, const forge_summary_target *,
                                                const forge_summary_options *, forge_error *);
bool forge_summary_input_get(const forge_summary_input *, forge_summary_view *);
char *forge_summary_input_json(const forge_summary_input *, forge_error *);
void forge_summary_input_destroy(forge_summary_input *);
/* Caller generates text explicitly between prepare and store. Revalidates all
 * inputs in a writer transaction. Unrelated generation changes can succeed;
 * changed/deleted dependencies return CONFLICT. A valid first writer wins for
 * one cache key, even if another producer returns different text. Cache writes
 * never advance source generation. UTF-8 text must have no embedded NUL.
 * Input remains immutable and reusable. No inference or quality proof is made. */
forge_status forge_repo_summary_store(forge_repo *, const forge_summary_input *, const char *text,
                                      size_t length, forge_summary_store_result *, forge_error *);

#ifdef __cplusplus
}
#endif
#endif
