# Tranche 2 native-protocol development campaign

Forge passed **83/87**, OpenCode **71/87**, and Aider **69/87** on the same
29 synthetic tasks, with three cold repetitions per task. The paired task-cluster
bootstrap estimates Forge minus OpenCode at **+13.79 percentage points**, with a
95% interval of **[+3.45, +27.59]**. This is development evidence; it does **not**
close the reliability plan's fresh-holdout promotion gate.

## Scope and provenance

- Original campaign: `.scratch/2026-09-01-tranche2-native-full-campaign2`.
- Protocol: `42f11da53bb8ba7da7643a9722429e14af13122397e2ef33ab80bc0ffbb4525c`.
- Freeze timestamp: `2026-09-02T00:47:59.533077+00:00`.
- Freeze base revision: `654418fb8ca118d5971eefce67416d9d93eac532`, with
  uncommitted changes recorded in the original lock. The checkpoint commit
  containing this report does not retroactively make that freeze clean.
- Measured Forge executable SHA-256:
  `b4c5654861a8e440a3252ed490fec10e238fb257181403b2daa40765d6b153eb`.
- Qwen3-Coder-30B-A3B-Instruct Q4_K_M; model SHA-256
  `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad`.
- NVIDIA RTX 5090 Laptop GPU, driver 616.56, Windows; llama.cpp b10566.
- OpenCode 1.18.25; Aider 0.86.2 using its single-message whole-edit adapter.
- Context 16,384; output reserve 2,048; action limit 16; temperature 0;
  seed 42; order seed 20260831. Forge uses the native prompt protocol.

All source files listed by the original lock and the measured Forge executable
matched their hashes at audit time, before documentation changes. All 261
aggregate records matched their per-run `result.json`. Recorded configuration,
runtime identities, model identity, prepared fixture bytes, and protected-file
hashes matched the lock. The existing reporter accepted every record, including
nonzero token counts, timing/resource measurements, seeded schedule metadata,
unchanged protected files, and retained failed workspaces. See [audit.json](audit.json).

The lock was created from a dirty tree, contrary to [ANALYSIS.md](../../ANALYSIS.md).
The suite was also used during development: the candidate includes specific
repair guidance triggered by `order=[`, `success=map[`, `ValueError not raised`,
and `capped=map[` diagnostics. A positive interval on these tasks cannot establish
performance on an untouched holdout or isolate the effect of native roles alone.

OpenCode's retained results span September 1 and 2. The local recovery helper
documents replacement of Forge runs mistakenly placed in the OpenCode leg.
The final dataset contains 87 genuine OpenCode records and no Forge records in
that leg. Sorted schedule metadata does not independently establish uninterrupted
execution order or attest runtime identity for each resumed run. The recovery
helpers were not part of the original freeze. Preserve this limitation when
citing the results.

## Timing and token evidence

All-run timing includes failures. Intervals use the unchanged reporter's 20,000
task-cluster percentile samples and base seed `20260901`, with its existing
label-derived seeds recorded in [analysis/summary.json](analysis/summary.json).

| Harness | Passed | E2E median, 95% CI (s) | E2E p90 (s) | E2E mean (s) | E2E stdev (s) | Agent median (s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Forge | 83/87 | 20.83 [19.34, 22.84] | 33.10 | 23.84 | 9.07 | 12.22 |
| OpenCode | 71/87 | 27.59 [25.24, 30.24] | 45.36 | 32.03 | 14.23 | 16.98 |
| Aider | 69/87 | 14.86 [14.70, 15.44] | 17.78 | 15.59 | 1.72 | 4.48 |

On the 70 task/repetition pairs where Forge and OpenCode both passed, the median
paired E2E difference (Forge minus OpenCode) is **-5.46 seconds**, 95% interval
**[-6.62, -4.04]**. Forge's all-run median is 24.52% lower. Aider's adapter has
different interaction semantics; its timing describes that adapter configuration.

| Harness | Prompt tokens | Evaluated prefill tokens | Cached tokens | Generated tokens | Cached / prompt |
| --- | ---: | ---: | ---: | ---: | ---: |
| Forge | 2,734,236 | 980,218 | 1,754,018 | 87,053 | 64.15% |
| OpenCode | 6,341,217 | 779,407 | 5,561,810 | 129,238 | 87.71% |
| Aider | 114,666 | 104,064 | 10,602 | 27,933 | 9.25% |

These counters use the same model tokenizer but measure different harness
protocols. In particular, fewer total prompt tokens do not establish less
evaluated prefill work: Forge evaluated more prefill tokens than OpenCode here.
Resource distributions and language/category slices are in the generated report.
[runs.csv](runs.csv) contains all 261 outcomes and timing/token records; blank
external-harness loop/validation counters mean unmeasured, not zero.

## Promotion gates and remaining failures

| Gate | Evidence and status |
| --- | --- |
| Four regression tasks, 12/12 | The separate same-binary gate 11 passed 12/12 with zero loop warnings. Campaign2 reproduced only 11/12: quota allocation failed once. |
| Invariant set, 60/60 | Separate same-binary invariant gate 3 passed 60/60 with zero loop warnings and no protected-file changes. |
| Hard holdout clusters | Open. Atomic transfers: Forge 0/3 vs OpenCode 1/3; dependency order: 3/3 vs 0/3; event replay: 3/3 vs 1/3. The aggregate advantage does not erase the atomic-transfer loss. |
| At least 80/87 | Campaign2 achieved 83/87. |
| New frozen holdout, lower bound above zero | Numerical condition met on this dataset; fresh-holdout and clean-freeze requirements unmet. Gate remains open. |
| Latency and complete measurements | Reporter checks passed; all-run and matched timing favor Forge. Resume provenance limits remain as described above. |

All three atomic-transfer runs failed independent verification, recorded one loop
warning each, and exhausted the rendered prompt budget. Their final workspaces
still read original balances when validating dependent transfers. The quota run
repaired the ID comparator and passed independent verification, but Forge exited
with `Rendered prompt exceeds context budget` before completion. Its nonzero
exit remains a failure under the original scoring rule. Forge therefore has
both an atomic-transfer correctness defect and a completion-budget failure.

[Task outcomes](task-outcomes.md) retain every cluster. Gate inputs are stored
under [gates](gates). Next promotion work must address the remaining failures
and use a clean revision with a genuinely new frozen evaluation set, without
tuning against that set's results.

## Reproduction and retained artifacts

Run from the repository root:

```sh
python benchmark/report.py --run OpenCode=benchmark/results/2026-09-02-tranche2-native/opencode --run Forge=benchmark/results/2026-09-02-tranche2-native/forge --run Aider=benchmark/results/2026-09-02-tranche2-native/aider --output /tmp/forge-tranche2-report
```

The exported inputs reproduce the original `summary.json` exactly. Harness
labels and their order are intentional because they determine contrast direction
and the reporter's label-derived bootstrap seeds.

The bundle retains original numeric result/environment files, the unchanged
freeze lock, source files for all 38 failed workspaces, and verifier output/diffs
in each leg's `failure-evidence.json`. Generated caches and private harness
state/history are omitted. Full original sessions remain in the source campaign
directory; this export does not replace them.

## Checkpoint validation

- Rebuilt the Release core configuration with `FORGE_WITH_LLAMA=OFF`.
- CTest: 23 groups passed; `checkpoint_model` skipped because this invocation
  did not supply its model fixture.
- Reran the CLI integration and benchmark-fixture groups with the bundled Go
  toolchain on PATH: 51 integration tests (two platform skips), 22 benchmark
  tests, no failures. This covers the Go tests skipped by the first invocation.
- The existing measured GPU build passed its chat-template unit test. Its
  executable hash still matches the original lock; no model campaign was rerun.
- Recomputed the report from the exported inputs in a temporary directory and
  compared parsed `summary.json` values: exact match to the original-source run.
