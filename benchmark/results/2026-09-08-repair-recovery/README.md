# Failed repair recovery development — September 8, 2026

The first two measured recovery candidates each passed **7/10**, but both lost
the baseline's paraphrased retraction pass. Neither passes the preservation gate.
Every run is retained; focused-run successes are not substituted into the full
matrix. No comparative holdout or promotion claim has been made.

## Changes and verification

The benchmark test now resolves its expected holdout path, matching campaign
normalization of Windows short names and macOS directory aliases. Strict scripted
edit fixtures use the existing deterministic watcher fallback. Native watcher
delivery tests use outer deadlines so legitimate per-poll deadline rescans are
not confused with unrelated exclusion or event-delivery assertions. Production
watcher behavior and the separate timeout/cancellation tests are retained.

All five CI jobs passed at `7e587c1`:
[CI run](https://github.com/chillyflow/forge/actions/runs/34270369517).
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

The unmeasured v1 bundle was staged before the stable-input diagnostic fix; no
model runs were executed with it. The two measured candidates contain all 26
scheduled runs. Both full matrices retain all five window passes and the
contrast retraction pass. All failures exhausted the 16-turn limit. No process
crashes, task timeouts, or protected-file mutations occurred. All five retraction
variants selected a real unittest runner on their first command attempt.

The full-matrix median end-to-end times were 37.281 seconds for v2 and 36.1875
seconds for v3. These development measurements do not establish a latency or
correctness advantage over another agent. Each full matrix recorded one loop
warning. Inconsistent results between focused and full runs remain visible.

Both candidates used the unchanged ten manifests from
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
`.scratch/recovery-runtime-v2` and `.scratch/recovery-runtime-v3` locally.

- [v2 audit](v2/audit.json), [comparison](v2/comparison.json),
  [complete evidence](v2/complete-evidence.zip).
- [v3 audit](v3/audit.json), [comparison](v3/comparison.json),
  [complete evidence](v3/complete-evidence.zip).

The traces show an additional diagnostic gap: assertions inside loops report
the unexpected result but omit the particular failing local input. The next
bounded candidate enables unittest traceback locals through the existing
planner, with a deterministic regression that identifies the failed loop input
without modifying tests. It will use a new clean freeze and the same settings.

The 12/12 regression and 60/60 invariant gates have not been rerun for these
candidates because the cheaper preservation gate failed. Those gates, the
existing 80/87 development threshold, protected-file integrity, a fresh untouched
comparative holdout, a positive task-cluster bootstrap lower correctness bound,
and preserved latency advantage remain open. No earlier gate result is claimed
for a new binary.
