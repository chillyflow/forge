#ifndef FG_INPUT_SNAPSHOT_H
#define FG_INPUT_SNAPSHOT_H
#include "forge/forge.h"

/* Internal, bounded input evidence. Directory depth is relative to root; paths
 * (including root and separators) must fit in FG_PATH_MAX in the implementation.
 * Only the root .git/ and .forge/ directories are excluded. */
#define FG_INPUT_SNAPSHOT_MAX_DEPTH 64u

typedef struct fg_input_snapshot fg_input_snapshot;

/* Both limits must be nonzero. max_bytes counts file contents, not path storage.
 * deadline is an absolute fg_now_ms() value; zero disables the deadline.
 * Returns NULL on any incomplete scan, unsafe entry, limit, read error, detected
 * concurrent mutation, or cancellation. No partial snapshot is returned.
 * The caller owns the result. No subprocesses, model, or GPU are involved. */
fg_input_snapshot *fg_input_snapshot_take(const char *root, size_t max_files, uint64_t max_bytes,
                                          forge_cancel_fn cancel, void *user,
                                          uint64_t absolute_deadline, forge_error *error);

/* NULL is never valid evidence: equal(NULL, NULL) is false and hash(NULL) is 0.
 * Equality compares sorted paths, lengths and per-file noncryptographic hashes;
 * the aggregate hash alone is not used as an equality proof. */
bool fg_input_snapshot_equal(const fg_input_snapshot *left, const fg_input_snapshot *right);
uint64_t fg_input_snapshot_hash(const fg_input_snapshot *snapshot);
void fg_input_snapshot_destroy(fg_input_snapshot *snapshot);
#endif
