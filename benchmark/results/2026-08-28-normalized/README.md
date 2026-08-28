# Local Go repair measurements

10 synthetic tasks, one run per configuration. All failures are retained.
These results do not establish general repository performance or statistical significance.

| Harness | Passed | Logical prompt tokens | Evaluated prompt tokens | Generated tokens |
| --- | ---: | ---: | ---: | ---: |
| Forge optimized | 10/10 | 71,990 | 31,739 | 2,825 |
| OpenCode | 10/10 | 396,185 | 73,136 | 5,323 |

## Conditions and limits

- Same GGUF hash, laptop GPU, 16,384-token context, greedy decoding, seed 42.
- OpenCode uses its normal tool protocol. Remote tools and subagents are disabled.
- OpenCode model stays loaded, but its slot KV is erased before each task; RAM prompt caching is disabled.
- OpenCode counters include all local inference requests during the task, including title generation.
- Forge uses a fresh process for each task. Its driver wall time includes model load and independent verification.
- OpenCode wall time excludes server/model startup and independent verification. Do not compare these wall times directly.
- Native Forge generation time includes grammar sampling and token event I/O; server generation time measures a different boundary.
- Test-file hashes must remain unchanged, and independent `go test -json ./...` must pass.
- See `environment.json` for exact model, hardware and revisions; numeric per-task records are included.

Both harnesses used the same prepared file hashes under utf8-lf-gofmt-v1. Initial formatting differs from older unnormalized runs; do not combine their totals.

## Provenance and preliminary failure

The Forge executable was built from `742848a341267393746e2df8f63fdee6011ddba2`.
The common fixture preparation and comparison guards are committed in `faf3d24`.
Every source file is written as UTF-8/LF and every Go file is formatted before
timing; test hashes are captured only after this preparation. This is applied
identically to both harnesses and is recorded in each numeric task entry.

Before normalization, an `average` smoke run repaired the function and passed
its Go tests but failed Forge's formatting gate three times. The model repeated
its final answer instead of resolving the formatting error. That run remains a
failure in [preliminary-unnormalized.json](preliminary-unnormalized.json); it is
not part of the normalized ten-task totals. Its test files were unchanged.
No improvement is attributed solely to the runtime across these different
starting inputs. This report contains no new ablations.

Forge's automatic checks ran 20 commands across the normalized suite with no
validation failures. An unknown changed-file scope selected formatting and
broad Go tests here; the other planner stages are not claimed to have executed.
The 56.6% reduction in evaluated prompt tokens is a single-run observation on
these small fixtures, not a general performance estimate. Raw local sessions
and OpenCode credentials/configuration are deliberately not published.
