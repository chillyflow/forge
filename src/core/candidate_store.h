#ifndef FG_CANDIDATE_STORE_H
#define FG_CANDIDATE_STORE_H
#include "internal.h"
#include "input_snapshot.h"
typedef struct fg_candidate_store fg_candidate_store;
/* Bounded exact contents, including initially dirty/untracked files. */
fg_candidate_store *fg_candidate_store_take(const char *root, forge_cancel_fn, void *, uint64_t,
                                            forge_error *);
void fg_candidate_store_destroy(fg_candidate_store *);
bool fg_candidate_store_equal(const fg_candidate_store *, const fg_candidate_store *);
size_t fg_candidate_store_cost(const fg_candidate_store *, const fg_candidate_store *);
bool fg_candidate_store_materialize(const fg_candidate_store *, const char *empty_root,
                                    forge_error *);
bool fg_candidate_store_materialize_until(const fg_candidate_store *, const char *empty_root,
                                          forge_cancel_fn, void *, uint64_t, forge_error *);
/* Refuses stale inputs before writing. Each changed file has before/after
 * content and intent/outcome evidence, and an atomic replacement or deletion.
 * Root must equal context.root. Does not restore process side effects elsewhere. */
bool fg_candidate_store_apply(const fg_candidate_store *expected, const fg_candidate_store *desired,
                              fg_tool_context *, forge_error *);
/* Preflight also leaves journal capacity for desired -> expected. The reserve
 * is checked before mutation; actual journal bytes are charged as written. */
bool fg_candidate_store_apply_reserving_restore(const fg_candidate_store *expected,
                                                const fg_candidate_store *desired,
                                                fg_tool_context *, forge_error *);
/* Recovery after an unsuccessful application. Uses the cleanup deadline and
 * ignores cancellation only for restoring known complete contents. An unchanged
 * baseline is success; a complete attempted candidate may be restored; any
 * partial/unexpected workspace is refused. All writes still require policy.
 * Success leaves the original error untouched so the failed operation remains
 * failed. Failure reports why automatic recovery was refused or unsuccessful. */
bool fg_candidate_store_recover(const fg_candidate_store *baseline,
                                const fg_candidate_store *attempted, fg_tool_context *,
                                uint64_t cleanup_deadline, forge_error *);
forge_status fg_candidate_search(const forge_agent_config *, const char *, fg_session *,
                                 forge_metrics *, forge_event_fn, void *, forge_error *);
#endif
