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
    bool index_scope_active;
};
#endif
