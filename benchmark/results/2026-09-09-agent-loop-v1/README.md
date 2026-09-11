# Remaining agent-loop diagnostic, September 9, 2026

**No added policy beat minimal in this frozen diagnostic.** Minimal passed 4/6;
semantic, reflection and combined passed 3/6; checkpoint-only, best-of-two and
impact passed 2/6. The implementations remain opt-in. These six examined fixtures
and one repetition do not establish superiority, preservation or a fresh holdout.

All 42 scheduled runs are retained, with no model-run retries or result-based
exclusions. The frozen-input and population audit passed. All protected files
remained unchanged, all runs used real inference, and all shared task budgets
passed the evidence audit. No failed root or failed child trial passed independent
secondary verification, so this pilot found no passing terminal solution lost
to missing completion.

## Primary results

| Arm | Passes / scheduled | Median cold end-to-end s | Total cold end-to-end s |
| --- | ---: | ---: | ---: |
| minimal | 4/6 | 66.117 | 461.625 |
| candidate | 2/6 | 147.360 | 711.483 |
| best-of-2 | 2/6 | 120.664 | 646.359 |
| semantic | 3/6 | 119.133 | 621.468 |
| impact | 2/6 | 152.727 | 701.704 |
| reflection | 3/6 | 78.383 | 543.251 |
| combined | 3/6 | 137.453 | 735.266 |

Primary success requires a successful agent exit, passing independent verification
and unchanged protected files. Cold end-to-end time includes the Forge process
and independent verifier. Controller setup and hashing are excluded. Medians and
totals include both successes and failures; all 42 timing observations are present.

| Fixture | minimal | candidate | best-of-2 | semantic | impact | reflection | combined |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Python retractions: distractor | PASS | FAIL | FAIL | PASS | FAIL | FAIL | FAIL |
| Python retractions: original | PASS | FAIL | FAIL | FAIL | FAIL | PASS | FAIL |
| Python retractions: paraphrased | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL |
| Python retractions: renamed | FAIL | FAIL | FAIL | FAIL | FAIL | FAIL | PASS |
| Go pagination | PASS | PASS | PASS | PASS | PASS | PASS | PASS |
| Go multi-file transfer | PASS | PASS | PASS | PASS | PASS | PASS | PASS |

Both Go fixtures passed in every arm. Combined solved renamed retractions, which
minimal missed, but lost the original and distractor variants that minimal solved.
No policy solved the paraphrased variant. Four fixtures are variants of the same
Python problem; these are not six independent problem classes. Differences here
do not identify causal effects or justify a confidence interval or default change.

## Work and mechanism evidence

| Arm | Total actions | Generated tokens | Cumulative input tokens | Automatic validation s | Automatic validation commands |
| --- | ---: | ---: | ---: | ---: | ---: |
| minimal | 94 | 19,009 | 326,365 | 0.000 | 0 |
| candidate | 131 | 33,384 | 729,756 | 5.751 | 29 |
| best-of-2 | 163 | 23,653 | 553,126 | 24.019 | 106 |
| semantic | 123 | 28,612 | 661,212 | 6.249 | 37 |
| impact | 133 | 29,699 | 767,871 | 6.798 | 39 |
| reflection | 111 | 22,370 | 543,239 | 5.736 | 31 |
| combined | 165 | 26,970 | 562,153 | 24.860 | 108 |

Automatic validation time is executor time only. Minimal's zero automatic commands
does not mean it was untested: model-issued commands are accounted in tool time,
and every run has an independent verifier. Snapshotting, indexing and selection
are distinct costs; the full timing/token breakdown remains in [analysis.json](analysis.json).

- **Completion:** all 23 unsuccessful root workspaces were verified again on
  separate copies with unchanged manifest commands. None passed; none was unknown.
  Minimal emitted six final actions, including two whose terminal inputs failed
  the independent verifier. Completion alone is not repair success.
- **Best-of-two:** both multi-candidate arms generated all 12 planned children.
  Best-of-two had four completed children and eight failed trials; combined had
  five completed children and seven failed trials. Every completed child passed
  real-workspace selection validation. Independently verified copies of all 15
  failed trials also failed: no discarded passing child or missing-final success
  was found. Best-of-two selected two root winners; combined selected three.
  Journal evidence recorded 15 and 24 applied file operations respectively,
  with matching preparation/outcome records and no failed or unfinished operation.
  These are file-operation counts, not counts of whole-workspace transactions.
- **Semantic detection:** one repeated failed state was recorded in the semantic
  arm and five in combined. Canonical diagnostics and repository evidence are
  present; activation does not demonstrate a net accuracy gain.
- **Reflection:** the reflection arm offered five bounded diagnostic turns and
  completed four; combined offered and completed nine. The uncompleted offer was
  the renamed-reflection run, whose generation exhausted its bounded response
  before a complete native call. Offers and completed reflection events are
  counted separately; these actions do not grant extra total tokens or turns.
- **Impact:** all 17 direct-impact plans and all 32 combined plans fell back.
  There were zero narrowed test plans. Direct Go pagination encountered
  `go_dynamic_or_generated_behavior`; multi-file transfer encountered
  `non_function_or_method_change`. Python and independent filesystem trial
  indexing also retain conservative fallback. Compiler-only `-run ^$` entries
  are not targeted test execution. Plan counts describe selections, not executed
  tests. This diagnostic therefore demonstrates no test-targeting latency benefit.
- **Failure boundaries:** checkpoint-only's four failures and semantic's three
  failures reached the pinned-context limit. Impact had three context-limit
  failures and one action-cap failure. Reflection had one context-limit, one
  action-cap and one incomplete bounded native-call failure. Best-of-two and
  combined failed when no child qualified. No failure is excluded from the totals.
- **Interactive partnership:** persistent conversations and questions have separate
  scripted contract coverage. This unattended pilot does not measure interactive
  effectiveness. Questions currently require one candidate so a clarification
  cannot disappear with a discarded trajectory.

## Frozen method and provenance

The [preregistered protocol](../../LOOP_COMPLETION.md) and machine-readable
[protocol.json](protocol.json) define seven arms using the same executable,
native embedded chat template and thought history. All except minimal enable
candidate checkpoints; best-of-two and combined use two trajectories. The six
unchanged manifests come from the earlier repair-control diagnostic. Preflight
verified broken inputs fail and supplied oracle inputs pass before inference.

Limits per task: 32 actions, 32768 generated tokens, 262144 cumulative input
tokens, context 16384 with 2048 output reserve, 600 seconds, and a separate
120-second independent verifier. Temperature is 0.6, base seed 42 and order seed
20260831. Candidate seeds are 42 and 2654435811. All candidates share the original
total budgets; no candidate receives a fresh task budget. All 42 runs are cold.

Hardware was an NVIDIA GeForce RTX 5090 Laptop GPU (24463 MiB), driver 616.56,
on Windows-10-10.0.26200-SP0, with GPU layers -1. Toolchains were Go 1.27.0,
Python 3.11.9 and Node 24.18.0. Model: Qwen3-Coder-30B-A3B-Instruct-Q4_K_M,
18,556,689,568 bytes, SHA-256
`fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad`.
Executable SHA-256:
`b7e212caf8c485062153cb2c989adbb1228e150705ca66dc96235c49489334a5`.

The runtime was copied to `.forge/loop-runtime-20260909-v2`; all adjacent DLL
identities are in the protocol. The retained [source snapshot](minimal-source.zip)
and [working-tree diff](minimal-source.diff) identify the tested uncommitted
implementation. This is a frozen working-tree diagnostic, not a clean-commit
release. The [final audit](audit.json) verifies unchanged identities and a complete
42/42 population. Later README/roadmap/report edits do not change the frozen code.
The [post-report source check](source-post-report-audit.json) confirms that all
128 frozen source files still match. Final documentation copies are retained
separately under `validation/*-final.md`.
Runtime libraries and model weights are identified by hash rather than bundled.

The earlier [checkpoint V3 experiment](../2026-09-08-candidate-checkpoint-v3/README.md)
used different temperature and action limits and is not pooled here. Its results,
earlier failures and archives remain unchanged. A separate
[pre-review combined add smoke](../2026-09-09-agent-loop-smoke/README.md) passed on
an earlier executable; it is integration evidence only, outside these 42 runs.

## Verification and retained evidence

The final Windows GPU build passed **31 CTest checks with one opt-in model test
skipped**, in 87.71 seconds. A separate local Qwen native-template probe passed.
[Validation notes](validation/README.md) identify final logs, intermediate failures,
review corrections and successful Serena compilation-database maintenance.
The [current option guide](../../../docs/AGENT_LOOP.md) documents implementation
bounds, conservative structural mapping and interactive limitations.

The root `cells/` directories retain commands, fixtures, prompts, outputs, root
and child sessions, terminal workspaces, journals, plans, diagnostics and independent
verification. [outcomes.json](outcomes.json), [summary.json](summary.json) and
[SUMMARY.md](SUMMARY.md) are the controller outputs; [analysis.json](analysis.json)
adds completion, shared-budget and mechanism audits. Secondary root/child
verification is separate in `terminal-verification/` and
`child-terminal-verification/`; it never rewrites primary success or timing.

[analysis.py](analysis.py) is the exact analysis script identified by hash in
`analysis.json`. [analysis-execution.txt](analysis-execution.txt) retains an initial
analysis-only hash-check error and its correction: the controller hashes canonical
protocol JSON, not its file serialization. The corrected analysis completed with
all evidence checks passing and unchanged primary evidence. No model run was
repeated. [execution.txt](execution.txt) retains the full matrix controller output.

[complete-evidence.zip](complete-evidence.zip) contains this report and retained
evidence with [evidence-inventory.json](evidence-inventory.json). Every archive
member is reopened and checked against its recorded SHA-256 after creation.
The separate smoke archive is retained in its own directory. No fresh holdout,
cross-platform coding evidence or design release gate is claimed by this package.

The verified smoke archive contains 174 files and is 370853 bytes; its SHA-256 is
`dd6748fe9826b75ec0f35c9a71642bbacad6c4a7e0a7c5b2d8805156ea12742c`.
