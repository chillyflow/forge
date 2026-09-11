#ifndef FG_REPO_IMPACT_H
#define FG_REPO_IMPACT_H
#include "forge/forge.h"

typedef struct fg_impact_snapshot fg_impact_snapshot;

/* Capture immutable indexed bytes and Go declarations before a repair. No live
 * reads or subprocesses. The caller must index first and owns the snapshot.
 * Limits: 4096 files, 64 MiB source, 16384 declarations. Incomplete captures fail
 * explicitly. Python definition ranges are lexical diagnostic candidates. */
fg_impact_snapshot *fg_impact_snapshot_take(forge_repo *, uint64_t deadline, forge_cancel_fn,
                                            void *, forge_error *);
void fg_impact_snapshot_destroy(fg_impact_snapshot *);

/* Compare with the current indexed workspace, returning an owned JSON report.
 * Go declaration changes are exact byte-range comparisons. Caller candidates
 * are syntactic identifier occurrences, never resolved calls or test coverage.
 * Unknown, deleted, ambiguous, dynamic and unsupported changes force fallback.
 * All preliminary selections still require the existing broad final tests. */
char *fg_impact_analyze(forge_repo *, const fg_impact_snapshot *, uint64_t deadline,
                        forge_cancel_fn, void *, forge_error *);

/* NULL baseline is exactly the ordinary broad planner. With a baseline, add
 * impact evidence and replace preliminary test stages when analysis permits it.
 * The broad_tests stage is preserved; targeted success is never final evidence. */
char *fg_repo_validation_plan_impact(forge_repo *, const fg_impact_snapshot *, uint64_t deadline,
                                     forge_cancel_fn, void *, forge_error *);
#endif
