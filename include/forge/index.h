#ifndef FORGE_INDEX_H
#define FORGE_INDEX_H
#include "forge.h"
#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_INDEX_MAX_FILE_BYTES ((size_t)2 * 1024 * 1024)
#define FORGE_INDEX_MAX_CACHE_FILES ((size_t)4096)
#define FORGE_INDEX_MAX_CACHE_SOURCE_BYTES ((size_t)256 * 1024 * 1024)
#define FORGE_INDEX_MAX_CACHE_NODES ((size_t)8000000)
#define FORGE_INDEX_SYMBOL_LIMIT ((size_t)4096)

/* Retention limits, not parser/RSS limits. Any zero disables the cache.
 * Source bytes count payload only; nodes count the exposed syntax tree,
 * including anonymous nodes. Tree-sitter's allocation sizes are opaque. */
typedef struct {
    size_t max_cached_files;
    size_t max_cached_source_bytes;
    size_t max_cached_nodes;
} forge_index_limits;

/* Per-handle, saturating work counters, including work later rolled back.
 * Incremental parses mean an edited old tree was supplied to the parser;
 * they do not measure how many internal nodes Tree-sitter actually reused.
 * Current/peak retained counts include bounded transaction staging. */
typedef struct {
    uint64_t full_attempts, delta_attempts, commits, rollbacks;
    uint64_t files_read, source_bytes_read, unchanged_files, files_indexed, files_removed;
    uint64_t cold_parses, incremental_parses, cache_hits, cache_misses;
    uint64_t cache_evictions, cache_invalidations, cache_skips, observed_change_bumps;
    size_t cached_files, cached_source_bytes, cached_nodes;
    size_t peak_cached_files, peak_cached_source_bytes, peak_cached_nodes;
} forge_index_stats;

forge_index_limits forge_default_index_limits(void);
/* Limits are local to the open handle. Shrinking them evicts entries immediately.
 * NULL limits restore the defaults. These APIs, like indexing, are not thread safe. */
forge_status forge_repo_set_index_limits(forge_repo *, const forge_index_limits *, forge_error *);
bool forge_repo_get_index_stats(const forge_repo *, forge_index_stats *);

/* Caller-owned schema-1 JSON for an indexed path: source hash, exposed AST hash,
 * ordered declaration hash, and up to FORGE_INDEX_SYMBOL_LIMIT per-symbol source
 * hashes. Hashes are labeled noncryptographic FNV-1a-64, not semantic identities.
 * Uses committed index bytes, not the current filesystem; reindex for freshness.
 * Old databases acquire AST/symbol metadata lazily on their next index pass. */
char *forge_repo_index_describe(forge_repo *, const char *relative_path, forge_error *);

#ifdef __cplusplus
}
#endif
#endif
