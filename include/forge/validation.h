#ifndef FORGE_VALIDATION_H
#define FORGE_VALIDATION_H
#include "forge/forge.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Return a deterministic JSON plan for workspace-relative changed paths.
 * The repository must have been indexed with forge_repo_index after the changes.
 * This function does not refresh the index or execute any command. The caller
 * owns the result and releases it with forge_free.
 *
 * Commands contain argv arrays and workspace-relative cwd values. Execute
 * stages in order, stopping on failure, policy denial, cancellation or budget
 * exhaustion. A command with require_empty_stdout also fails on nonempty
 * stdout (gofmt -l). See docs/VALIDATION.md for the versioned JSON contract and
 * limitations of the syntactic Go import graph.
 *
 * An empty change list requests broad verification. Maximum 1024 paths;
 * invalid paths or oversized graphs fail explicitly, never truncate a plan.
 */
char *forge_repo_validation_plan(forge_repo *, const char *const *changed_paths, size_t path_count,
                                 forge_error *);

#ifdef __cplusplus
}
#endif
#endif
