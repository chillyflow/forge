#ifndef FORGE_CONTEXT_H
#define FORGE_CONTEXT_H
#include "forge.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    FORGE_SEG_SYSTEM,
    FORGE_SEG_TOOLS,
    FORGE_SEG_REPO,
    FORGE_SEG_TASK,
    FORGE_SEG_MEMORY,
    FORGE_SEG_SOURCE,
    FORGE_SEG_ACTION,
    FORGE_SEG_RESULT
} forge_segment_kind;
typedef struct forge_context forge_context;
typedef struct {
    uint64_t id, content_hash, version, generation, dependency;
    forge_segment_kind kind;
    size_t tokens;
    int priority;
    bool pinned, selected;
    const char *text;
    /* Appended metadata. dependency remains the first (oldest) dependency for
     * source compatibility; use get_dependency for the complete DAG. */
    bool immutable, cacheable, stale;
    uint64_t source_hash;
    size_t dependency_count;
} forge_segment_view;
typedef size_t (*forge_count_tokens_fn)(const char *, void *);
forge_context *forge_context_create(size_t capacity, size_t reserve, forge_count_tokens_fn, void *);
uint64_t forge_context_add(forge_context *, forge_segment_kind, const char *, int priority,
                           bool pinned, uint64_t dependency, uint64_t generation);
forge_status forge_context_update(forge_context *, uint64_t id, const char *, uint64_t generation);
/* Dependencies must name existing older IDs. Duplicate additions are no-ops. */
forge_status forge_context_add_dependency(forge_context *, uint64_t id, uint64_t dependency);
size_t forge_context_dependency_count(const forge_context *, uint64_t id);
bool forge_context_get_dependency(const forge_context *, uint64_t id, size_t index,
                                  uint64_t *dependency);
/* Sealing is irreversible. Immutable text, dependencies and source bindings
 * cannot be changed; pinning/selection/staleness remain planner metadata. */
forge_status forge_context_set_flags(forge_context *, uint64_t id, bool immutable, bool cacheable);
void forge_context_invalidate(forge_context *, uint64_t dependency, uint64_t generation);
/* source is an opaque source identity hash, not the segment content hash.
 * UINT64_MAX denotes repository-wide source data. Immutable bindings are ignored. */
void forge_context_bind_source(forge_context *, uint64_t id, uint64_t source);
void forge_context_pin(forge_context *, uint64_t id, bool pinned);
char *forge_context_plan(forge_context *, size_t *tokens, size_t *evicted, forge_error *);
size_t forge_context_size(const forge_context *);
bool forge_context_get(const forge_context *, size_t index, forge_segment_view *);
/* Versioned, caller-owned JSON snapshots, not inference/KV checkpoints.
 * Import validates text hashes, dependency closure and token counts using the
 * caller-supplied counter. Use the same tokenizer to reconstruct a plan exactly. */
char *forge_context_export(const forge_context *, forge_error *);
forge_context *forge_context_import(const char *json, forge_count_tokens_fn, void *, forge_error *);
void forge_context_destroy(forge_context *);
#ifdef __cplusplus
}
#endif
#endif
