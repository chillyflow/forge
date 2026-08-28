#ifndef FORGE_REPO_INTERNAL_H
#define FORGE_REPO_INTERNAL_H
#include "internal.h"
#include "forge/index.h"
#include "sqlite3.h"
#include "tree_sitter/api.h"

typedef struct fg_repo_tree {
    char *path, *source;
    size_t bytes, nodes;
    TSTree *tree;
    uint64_t used;
    bool touched, keep;
    struct fg_repo_tree *next;
} fg_repo_tree;

/* One explicit indexed SQLite snapshot. No live filesystem reads or processes.
 * Begin rejects nesting and installs bounded busy/progress callbacks. Consumers
 * finalize all statements before end; no borrowed SQLite values survive end.
 * write=true reserves the writer for cache publication, never source indexing.
 * max_vm_steps=0 disables the VM budget; deadline=0 disables the deadline.
 * Error/cancellation storage must outlive the scope. Not safe for concurrent use
 * of one handle. The same contract is shared by summaries and retrieval. */
typedef struct {
    forge_repo *repo;
    uint64_t generation;
    bool go_index_incomplete, filesystem_scan;
    void *internal;
} fg_repo_snapshot;
forge_status fg_repo_snapshot_begin(forge_repo *, fg_repo_snapshot *, bool write, uint64_t deadline,
                                    forge_cancel_fn, void *, uint64_t max_vm_steps, forge_error *);
bool fg_repo_snapshot_stopped(fg_repo_snapshot *);
/* commit=false rolls back; interruption/failure always rolls back. Restores the
 * prior busy timeout and index interruption fields; clears the scope. */
forge_status fg_repo_snapshot_end(fg_repo_snapshot *, bool commit, forge_error *);

struct forge_repo {
    char root[FG_PATH_MAX];
    sqlite3 *db;
    TSParser *parser;
    uint64_t generation, scan;
    size_t changed, files;
    bool go_index_incomplete, filesystem_scan;
    forge_error *error;
    forge_index_limits index_limits;
    forge_index_stats index_stats;
    fg_repo_tree *trees, *pending_trees;
    uint64_t cache_clock;
    uint64_t index_deadline;
    forge_cancel_fn index_cancelled;
    void *index_userdata;
    uint64_t index_busy_started;
    int index_busy_limit;
    bool index_scope_active, snapshot_active;
};
#endif
