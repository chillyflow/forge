#ifndef FORGE_GO_GRAPH_H
#define FORGE_GO_GRAPH_H
#include "repo_internal.h"

#define FG_GO_GRAPH_MAX_MODULES ((size_t)256)
#define FG_GO_GRAPH_MAX_PACKAGES ((size_t)4096)
#define FG_GO_GRAPH_MAX_EDGES ((size_t)65536)
#define FG_GO_GRAPH_MAX_EXTRA_FILES ((size_t)1024)
#define FG_GO_GRAPH_NONE SIZE_MAX

typedef struct fg_go_graph fg_go_graph;

/* All strings and arrays are graph-owned, immutable, and valid until destroy.
 * A NULL path is an unresolved import identity, never a guessed directory alias.
 * Directories are canonical workspace-relative paths; "." denotes the root. */
typedef struct {
    const char *directory, *path;
    bool synthetic;
} fg_go_module;
typedef struct {
    const char *directory, *path, *name;
    size_t module, from_head, to_head;
    bool present, tests;
} fg_go_package;
typedef struct {
    size_t from, to, next_from, next_to;
    bool test_only;
} fg_go_edge;
typedef struct {
    const char *code, *path, *detail;
} fg_go_reason;

/* Consume one caller-owned active indexed SQLite snapshot. The loader never
 * starts/ends a transaction, refreshes the index, reads live source, or launches
 * a process. It checks the scope's cancellation/deadline/VM budget. On failure
 * no partial graph is returned; the caller must end its snapshot.
 *
 * Validation may provide up to 1024 canonical changed file paths to retain
 * otherwise absent Go package directories as present=false deletion nodes.
 * Other consumers pass NULL, 0. These extra paths do not mark affected nodes.
 *
 * Modules/packages are sorted by directory, edges by from/to, reasons by code.
 * Repeated edges merge test_only by AND. Limits apply before edge deduplication.
 * Adjacency heads/next links use FG_GO_GRAPH_NONE. This is a syntactic union of
 * indexed package imports, not type/call resolution or a sound build graph.
 */
fg_go_graph *fg_go_graph_load(fg_repo_snapshot *, const char *const *extra_package_files,
                              size_t extra_file_count, forge_error *);
void fg_go_graph_destroy(fg_go_graph *);
uint64_t fg_go_graph_generation(const fg_go_graph *);
bool fg_go_graph_applicable(const fg_go_graph *);
const fg_go_module *fg_go_graph_modules(const fg_go_graph *, size_t *count);
const fg_go_package *fg_go_graph_packages(const fg_go_graph *, size_t *count);
const fg_go_edge *fg_go_graph_edges(const fg_go_graph *, size_t *count);
const fg_go_reason *fg_go_graph_reasons(const fg_go_graph *, size_t *count);
size_t fg_go_graph_find_package(const fg_go_graph *, const char *directory);
size_t fg_go_graph_module_for(const fg_go_graph *, const char *directory);
bool fg_go_graph_excluded_path(const char *canonical_relative_file);

#endif
