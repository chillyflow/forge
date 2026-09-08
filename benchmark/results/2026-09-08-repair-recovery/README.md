# Failed repair recovery development — September 8, 2026

The retained recovery candidate passed **7/10**, but lost the baseline's
paraphrased retraction pass. It does not pass the preservation gate. A later
traceback-locals experiment regressed to **5/10** and has been reverted; current
source matches the earlier `7660bf1` candidate byte for byte. The subsequent
regression gate finished **10/12** and the invariant gate **59/60**. This candidate
is unqualified; the broader recovery objective remains unfinished.
Every run is retained; focused-run successes are not substituted into the full
matrix. No comparative holdout or promotion claim has been made.

## Changes and verification

The benchmark test now resolves its expected holdout path, matching campaign
normalization of Windows short names and macOS directory aliases. Strict scripted
edit fixtures use the existing deterministic watcher fallback. Native watcher
delivery tests use outer deadlines so legitimate per-poll deadline rescans are
not confused with unrelated exclusion or event-delivery assertions. Production
watcher behavior and the separate timeout/cancellation tests are retained.

All five CI jobs passed on the restored source at `b5d3713`:
[CI run](https://github.com/chillyflow/forge/actions/runs/34275274055).
The full local GPU suite and separate Qwen3-Coder GPU stability probe passed for
the recovery candidates. CTest ran 24 tests successfully and skipped only the
opt-in checkpoint-model test; the separate chat-template probe exercised forced
opening, cancellation, budget exhaustion, recovery, cached tool-call equivalence,
and generation without a callback. Logs are retained in the evidence archives.

Recovery now retains up to eight failed input snapshots, validation commands,
and bounded diagnostics. Equality requires complete, unchanged before/after
workspace inputs and compares contents across edit tools. Changed dependencies
or test inputs prevent a match. Returning to a failed state produces an advisory
recovery event; the edit remains applied so legitimate intermediate reverts are
possible. Failures from unstable inputs retain diagnostics without asserting
snapshot equality. Deterministic regressions cover these cases.

The existing validation planner supplies real test-runner commands before the
first action. Historical assertions remain available across reads and rejected
edits. The second candidate also checks applied edits immediately after an
observed failure and reports the current file bounds when rejecting an invalid
hunk. These checks use the existing permissions, deadlines and model budgets.

## Retained development measurements

| Candidate | Clean source | Previously failing three | All ten | Baseline passes preserved |
| --- | --- | --- | --- | --- |
| v2: failed states and early runner guidance | `c821b5e` | 1/3 | 7/10 | No: lost paraphrased; gained renamed |
| v3: immediate revalidation and hunk bounds | `7660bf1` | 0/3 | 7/10 | No: lost paraphrased; gained distractor |
| v4: unittest traceback locals; reverted | `c5c3f13` | 1/3 | 5/10 | No: lost contrast and paraphrased |

The unmeasured v1 bundle was staged before the stable-input diagnostic fix; no
model runs were executed with it. The three measured candidates contain all 39
scheduled runs. All retain the five window passes; v2 and v3 also retain the
contrast retraction pass. All failures exhausted the 16-turn limit. No process
crashes, task timeouts, or protected-file mutations occurred. All five retraction
variants selected a real unittest runner on their first command attempt.

The full-matrix median end-to-end times were 37.281 seconds for v2 and 36.1875
seconds for v3, versus 41.875 seconds for the rejected v4. These development
measurements do not establish a latency or correctness advantage over another
agent. The v2 and v3 full matrices each recorded one loop warning; v4 recorded
seven. Inconsistent results between focused and full runs remain visible.

All candidates used the unchanged ten manifests from
[repair-validation-v1](../2026-09-08-repair-validation-v1/README.md), the same
Qwen3-Coder-30B-A3B-Instruct-Q4_K_M model, GPU layers -1, native protocol,
embedded template, context 16384, output reserve 2048, temperature 0, seed 42,
16 turns, 600-second task limit, cold lifecycle, one repetition, and task order
seed 20260831. Each candidate froze a clean source commit and a separate
executable plus adjacent runtime libraries. Post-run audits verified unchanged
source, runtime and task identities. No benchmark hint or protected test changed.

## Evidence and next gate

Each version directory contains source identity, command records, fixture
preflight, comparison, per-run analysis, aggregate results, environment and audit
JSON. `complete-evidence.zip` retains every file from the original evaluation
directory, including session contexts, tool traces, validation output, diffs,
local check logs, and every failed workspace. `evidence-inventory.json` records
every file hash and the archive hash. The archives were reopened and every
member verified against that inventory. Immutable runtime bundles remain in
`.scratch/recovery-runtime-v2`, `-v3` and `-v4` locally.

- [v2 audit](v2/audit.json), [comparison](v2/comparison.json),
  [complete evidence](v2/complete-evidence.zip).
- [v3 audit](v3/audit.json), [comparison](v3/comparison.json),
  [complete evidence](v3/complete-evidence.zip).
- [v4 audit](v4/audit.json), [comparison](v4/comparison.json),
  [complete evidence](v4/complete-evidence.zip).
- [Retained v3 gate audit](v3-gates/audit.json),
  [regression results](v3-gates/regression/results.json),
  [invariant results](v3-gates/invariants/results.json),
  [complete gate evidence](v3-gates/complete-evidence.zip).

The v4 experiment exposed the actual failing local input without modifying the
test. Its deterministic regression, full local suite and GPU probe passed, but
the model still described the right failing order and generated ineffective
code. Its full matrix lost both prior retraction passes, so the planner change
and corresponding feature test were reverted. The rejected implementation and
test remain in commit `c5c3f13`, and every measured outcome remains in v4 evidence.

## Broader gates on the retained candidate

The original frozen v3 runtime completed all 72 scheduled runs with the original
four regression tasks and 20 invariant tasks, three repetitions each. All 29
development fixtures passed preflight, including all 19 supplied oracles. Model
settings, per-run budgets, runtime bundle and task identities were unchanged.
The restored source also passed the full local suite again in 80.87 seconds.

| Gate | Result | Loop warnings | Failure |
| --- | --- | --- | --- |
| Regression | 10/12 — failed | 2 | `go_multifile_transfer`, repetitions 2 and 3 |
| Invariants | 59/60 — failed | 1 | `go_api_pagination`, repetition 3 |
| Protected-file integrity | 72/72 unchanged | — | None |
| Run population and frozen identities | Passed | — | None |

All three failures exhausted 16 turns. Transfer repetition 3 emitted a
`failed_workspace_state` event when the model returned to known failing code,
but subsequent reads did not produce a correct repair. Pagination failed without
a loop warning. The invariant warning occurred in a different, passing run;
warning counts are not a complete measure of stalled repair behavior.

The three candidate trials plus these gates contain **111 executions**, all
retained, with no crashes, task timeouts or protected-file mutations. The
`v3-gates` archive includes the earlier 13 v3 runs alongside the 72 new runs;
those copied records are not additional executions or replacements. The earlier
v3 archive and audit remain unchanged. The extended audit verifies exact task,
variant and repetition populations, real inference, unchanged inputs, and fixed
settings. It also retains the analysis scripts, restored-build checks and CI
record used for this audit.

Failed-state recognition and available assertions have not established reliable
repair. Recovery remains advisory and can leave its episode after a different
allowed action, even when the implementation still fails. The next investigation
is how recovery directs a substantive repair after repeated failing evidence,
while preserving legitimate reverts and changes across files.

The repair/preservation, 12/12 regression and 60/60 invariant gates remain open.
The remaining 15 development executions needed to assess 80/87 were not run.
No full campaign, new holdout, correctness-confidence claim or latency-advantage
claim was made. The existing requirement for a fresh clean-frozen comparison of
Forge, OpenCode and Aider, a positive task-cluster bootstrap lower correctness
bound, and preserved latency advantage is unchanged.
