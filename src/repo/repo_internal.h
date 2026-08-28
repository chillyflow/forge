#ifndef FORGE_REPO_INTERNAL_H
#define FORGE_REPO_INTERNAL_H
#include "internal.h"
#include "sqlite3.h"
#include "tree_sitter/api.h"

struct forge_repo {
    char root[FG_PATH_MAX];
    sqlite3 *db;
    TSParser *parser;
    uint64_t generation, scan;
    size_t changed, files;
    bool go_index_incomplete, filesystem_scan;
    forge_error *error;
};
#endif
