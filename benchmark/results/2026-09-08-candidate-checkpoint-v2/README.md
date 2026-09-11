# Candidate checkpoint diagnostic — version 2

Both arms completed **7/18 tasks**, and both left **7/18 test-passing terminal
workspaces**. No passing terminal workspace was lost to missing completion in
this matrix. Candidate checkpoints did not improve the primary outcome and
increased median cold end-to-end time to **43.375 s**, versus **27.869 s** for the
minimal control.

| Measure | Candidate | Minimal |
| --- | ---: | ---: |
| Completed / scheduled | 7/18 | 7/18 |
| Terminal workspace tests pass | 7/18 | 7/18 |
| Median model actions | 16 | 16 |
| Total generated tokens | 45,713 | 46,001 |
| Total prompt prefill tokens | 761,458 | 59,120 |
| Total prefill time | 276.527 s | 28.628 s |
| Host validation executor time | 16.457 s | 0 |
| Median cold end-to-end | 43.375 s | 27.869 s |

All 36 scheduled outcomes are retained, with real inference and unchanged
protected files. The source, model, runtime, harness and fixtures remained
frozen. All six broken fixtures failed preflight and all six oracles passed.
The 22 primary failures were independently checked again on copies after the
matrix; none had passing terminal tests. Secondary checks did not call a model,
repair code, change primary results, or enter the original timing measurements.

The candidate passed the original retraction once; minimal passed the paraphrased
retraction once. Both passed Go pagination and multi-file transfer 3/3, and both
failed all distractor and renamed retractions. One minimal run accepted a final
whose independent tests failed. Candidate accepted finals only after host checks
passed on changed, stable inputs. All 11 candidate failures reached the action
cap after failing validation; their attempted finals were rejected.

## Implementation finding and follow-up

Version 2 appended changing `CANDIDATE_STATE` metadata as a system segment. The
context renderer places system segments before the conversation, changing the
physical prefix every action. That defeats most sequential KV reuse. Generated
tokens and load/decode times were similar between arms, while prefill work grew
12.88x. The candidate's validation executor consumed only 16.457 seconds across
all 18 runs; it does not explain the 263.967-second aggregate end-to-end gap.

This is an implementation latency defect. A separately frozen version 3 moves
the control metadata into appended user messages, with a transcript-prefix
regression. It changes neither the candidate rules nor the six-task population.
Version 2 is retained in full and must not be replaced by version 3's outcomes.
Neither version is a fresh holdout or a release/preservation gate.

## Protocol and verification

The comparison uses the same binary in both arms on an RTX 5090 Laptop GPU,
Qwen3-Coder-30B-A3B-Instruct-Q4_K_M, GPU layers -1, native embedded template,
temperature 0, seed 42, context 16384, output reserve 2048, 16 actions, cumulative
generated/input limits 32768/262144, 600 seconds per task, and three repetitions
of six examined diagnostic fixtures. Arm order is interleaved with seed
20260831. Source is an uncommitted snapshot on `07c178ac7707e03b175d3e87b2eb657d19d907d5`.

The GPU Release build passed. CTest passed 26 tests and skipped the opt-in
checkpoint-model test (81.70 seconds total); this includes 22 scripted agent
cases and native checkpoint schema/parser tests. A real Qwen smoke repaired the
add fixture, passed five host checks, and finalized against the same snapshot.
The earlier version 1 stopped after a pre-inference schema-registry integration
failure; its [failed/interrupted outcomes](aborted-v1/README.md) remain separately recorded.

- [Frozen protocol](protocol.json), [identity/population audit](audit.json),
  [all primary outcomes](outcomes.json), and [detailed analysis](analysis.json).
- [Analysis and secondary-check script](analyze.py), [source snapshot](minimal-source.zip),
  [source diff](minimal-source.diff), and [build/test evidence](validation/ctest.txt).
- Full raw model output, prompts, tool results, validation reports, edit journals,
  and failed workspaces remain under `cells/`.
- [Full traces and source archive](complete-evidence.zip) and
  [verified file inventory](evidence-inventory.json).
