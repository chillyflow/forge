#ifndef FORGE_RETRIEVAL_H
#define FORGE_RETRIEVAL_H
#include "context.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_RETRIEVAL_MAX_QUERY_BYTES ((size_t)1024)
#define FORGE_RETRIEVAL_MAX_RESULTS ((size_t)256)
#define FORGE_RETRIEVAL_MAX_OUTPUT_BYTES ((size_t)1024 * 1024)
#define FORGE_RETRIEVAL_MAX_SNIPPET_BYTES ((size_t)8192)
#define FORGE_RETRIEVAL_MAX_CANDIDATES ((size_t)4096)
#define FORGE_RETRIEVAL_MAX_SOURCE_BYTES ((size_t)256 * 1024 * 1024)

typedef struct {
    /* Optional indexed workspace-relative file for graph seeding. No live
     * filesystem lookup; a missing file is NOT_FOUND. Exact symbol hits also
     * seed their packages. The graph is the syntactic Go import union. */
    const char *seed_file;
    size_t graph_depth;      /* 0..8; zero disables the graph stage. */
    bool include_dependents; /* Traverse incoming as well as outgoing imports. */
    size_t max_results, max_output_bytes, max_output_tokens, max_snippet_bytes;
    size_t max_candidates, max_source_bytes;
    uint64_t timeout_ms, deadline_ms, max_vm_steps;
    forge_count_tokens_fn count_tokens;
    void *count_userdata;
    forge_cancel_fn cancelled;
    void *userdata;
} forge_retrieval_options;

typedef struct {
    uint64_t generation;
    size_t results, output_bytes, output_tokens, candidates, source_bytes;
    bool tokens_known, truncated;
} forge_retrieval_stats;

/* Defaults: 16 results, 64 KiB complete JSON, 2 KiB excerpts, 256 candidate
 * rows, 16 MiB inspected source, one graph hop in both directions, 5 seconds
 * and 50 million SQLite VM steps. Token budgets require a counter. */
forge_retrieval_options forge_default_retrieval_options(void);

/* Stages: exact case-sensitive symbol, Go package import neighborhood, literal
 * case-sensitive text, then FTS5 with quoted query terms (never raw MATCH
 * syntax). Earlier stages win deterministic file/span deduplication. All reads
 * share one indexed SQLite snapshot; no process, model or live source reads.
 * Returned UTF-8 JSON is caller-owned (forge_free). It contains source digests,
 * excerpt provenance, graph limitations and an explicit stage/budget trace.
 * Bounds apply to the complete serialized JSON; output tokens are counted on
 * that exact JSON, not added per result. Low-priority tail results are omitted
 * until budgets fit. A budget too small for metadata returns LIMIT. Excerpts
 * and limited stages are marked; candidate counts are not total-match counts.
 * Cancellation/SQL/corrupt observed metadata failures return no partial output.
 * Stats are zero on failure. JSON contains no self-referential output count.
 * Memory limits describe owned payload/work, not SQLite or process RSS. */
char *forge_repo_retrieve(forge_repo *, const char *query, const forge_retrieval_options *,
                          forge_retrieval_stats *, forge_error *);

#ifdef __cplusplus
}
#endif
#endif
