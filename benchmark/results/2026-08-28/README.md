# Local Go repair measurements

Ten tiny synthetic tasks, one run per configuration. All failures are retained.
These results do not establish general repository performance or statistical significance.

| Harness | Passed | Logical prompt tokens | Evaluated prompt tokens | Generated tokens |
| --- | ---: | ---: | ---: | ---: |
| Forge no-kv | 9/10 | 52,295 | 52,295 | 2,863 |
| Forge optimized | 9/10 | 49,444 | 12,865 | 3,112 |
| OpenCode | 10/10 | 451,073 | 74,616 | 5,870 |

Forge evaluated 82.8% fewer prompt tokens across this suite, but solved one fewer
task. This is evidence of lower prompt overhead on these fixtures, not equivalent
accuracy or an overall performance win.

On the nine tasks both harnesses solved, Forge used 37,985 logical / 10,858
evaluated prompt tokens, versus OpenCode's 396,850 / 66,786. The full ten-task
records above remain the primary result; no failure exclusion is hidden.

## Failure and ablations

Both Forge variants failed `average`: the model inserted an early return without
a Go statement separator, received the compiler error, then repeatedly requested
an identical replacement. The loop detector stopped the run after thirteen
turns. OpenCode repaired the same fixture and passed the verifier.

The current no-KV ablation evaluated 52,295 tokens versus 12,865 with reuse, at
the same 9/10 success count. The independent runs did not always produce identical
action traces, so this is an end-to-end comparison rather than fixed-token timing.

`grammar-ablation.json` retains the earlier paired run at `cb3254b`. Both action
traces matched across modes, including the failed task. For `add`, generation
time was 1,203 ms with the fast path versus 4,859 ms with full masking. For
`average`, it was 5,672 versus 27,565 ms. These timings include sampling and event
output. That revision's `sampling_ms` includes GPU waits; it is not isolated CPU
sampling time. `initial-kv-ablation.json` preserves the earlier pre-optimization
suite at `f791ef2`, including its failures.

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

The tested native executable was built from `73c200e`, using llama.cpp
`bb4caa7540188872173c44d161602d9271386413` (release `b10566`, v0.2.0), CUDA 13.3
DLLs, and MSVC 19.44. The baseline used the same release's `llama-server` with
OpenCode 1.18.25. Both used batch 512, microbatch 256 and four inference threads.
Forge had a 16-turn limit, 2,048-token response reserve and 600-second task
deadline. OpenCode had a 16-step limit and the same response/context limits.

All measurements were made sequentially on the same laptop on 2026-08-28, with
a warm OS file cache and Go build cache. No model processes ran concurrently.
The benchmark runner's temporary checkouts were discarded after recording
artifacts. Raw session prompts and outputs remain private; published files
contain only numeric records, environment metadata and this analysis.
