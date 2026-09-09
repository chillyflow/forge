# Tranche 2 repair and fresh holdout: not accepted

The identified atomic-transfer and quota-completion failures are repaired in
the development gates. **Tranche 2 remains unaccepted.** The new clean-frozen
holdout exposed a replay failure and was rejected for a protected-test
mutation by OpenCode. The earlier positive development bootstrap does not
substitute for this gate.

Forge remains a **development preview**. Durable session resume, deeper repository
semantics, speculative decoding, broader language/platform evidence, isolation,
observability, and a stable packaged library API remain unfinished product work.

## Implementation changes

- Native repair instructions now require baseline tests and tracing the failing
  input to the first incorrect expression before editing. The surrounding
  algorithm is preserved. Atomic transfers passed 3/3 in the final development
  matrix, and both new atomic holdout tasks passed 3/3.
- Context planning now checks the fully rendered prompt and removes optional
  dependency bundles in reverse admission order until it fits. Pinned context,
  shared dependencies, and native call/result pairs remain intact. A truly
  oversized pinned prompt still fails.
- The native tool-schema validator now accepts the host's final-only registry.
  Previously it required a memory tool even when the host intentionally allowed
  only final. Prompt counting masked that rejection as context exhaustion,
  preventing completion after a correct repair. A native Qwen-template regression
  covers final-only rendering with historical memory calls. Quota allocation
  passed 3/3 in the final development matrix, with context evictions in every run.
- Campaigns accept a separate task directory and a mandatory clean freeze.
  The lock covers production/evaluator source, the analysis plan, hardware,
  model/runtime artifacts, fixtures, settings, and schedule. Twelve new fixtures
  and their analysis plan were committed before any holdout model execution.

[Implementation validation](validation/README.md) includes 24 passing CTest
groups, 23 benchmark Python tests, and a final focused context/template test run.
The main checkout was rebuilt after all measured model runs finished. The
measured executable in the clean evaluation checkout was not rebuilt or changed.

## Development selection, kept separate

| Candidate | Development result | Selection outcome |
| --- | ---: | --- |
| [First repair](candidate1/README.md) | 82/87 | Atomic/quota 3/3 each; exposed final-only schema bug |
| [Shorter prompt experiment](candidate2/README.md) | 84/87 | Rejected: atomic transfers 0/3 |
| [Final candidate](final-development/README.md) | 83/87 | Original repair wording restored; final-only fix retained |

The final candidate passed the four regression tasks **12/12**, the invariant
set **60/60**, and atomic transfers **3/3**, with no loop warnings in those sets.
Dependency ordering passed 3/3 and event replay 2/3. The four retained failures
were one query-parser run, two interval-union runs, and one event-replay run.
These were incorrect repairs, not successful repairs discarded only at final
prompt rendering. No successes are substituted across candidates.

## New holdout outcome

The [holdout report](holdout/README.md) retains all **108 scheduled runs**:
12 new tasks, three repetitions, and three harnesses. The candidate was frozen
from clean revision `5222616b2aebeae91d4942fadbc64f9a1d79c371`, with no source,
binary, scoring, task, or prompt changes after viewing holdout outcomes.

| Harness | Recorded passes | All-run E2E median |
| --- | ---: | ---: |
| Forge | 30/36 | 23.39 s |
| OpenCode | 29/36 | 32.43 s |
| Aider | 21/36 | 16.20 s |

These are **descriptive observations from a rejected evaluation**, not a
validated comparative claim. Forge's one-run point-estimate lead cannot satisfy
the required positive task-cluster lower confidence bound. The frozen reporter
refused to produce that interval.

OpenCode's `holdout_go_window-opencode-r001` changed `service_test.go` to add a
case index and expected value to a failure message. Although diagnostic-only,
the edit violates the preregistered rule that any protected-file change
invalidates the measurement. The run remains non-passing and in the denominator;
it was not replaced, repaired after execution, or excluded. The
[original diff](holdout/opencode/holdout_go_window-opencode-r001/workspace.diff),
[rejected audit](holdout/audit.json), and
[reporter rejection](holdout/report-rejection.txt) are preserved.

Forge also lost the new retraction/replay task **0/3 versus OpenCode 1/3**.
Its three retraction edits left incorrect balances and exhausted the action
limit. Its three rolling-window runs exhausted the per-call generation budget
before a complete native call and made no edit. All six independent verifiers
failed. These remaining failures are separate from the fixed final-only
completion bug.

## Promotion decision

| Gate | Decision |
| --- | --- |
| Four regression tasks, 12/12 | Passed in final development matrix |
| Invariants, 60/60 | Passed in final development matrix |
| Hard-cluster advantage | Not met on fresh holdout: retractions 0/3 versus 1/3 |
| At least 80/87 development | Passed: 83/87 |
| Fresh positive bootstrap lower bound | Not met: comparison rejected before interval calculation |
| Retained latency advantage and complete valid evidence | Not established by the rejected comparison; all timing/token/resource records retained |

Next reliability work is the retraction repair loop, generation exhaustion on
rolling windows, and preserving protected fixtures across comparison harnesses.
Any tuning informed by this holdout makes it development evidence for the next
candidate, which requires another untouched, clean-frozen holdout. The original
promotion thresholds remain unchanged.

## Provenance and retention

The holdout [protocol](../../holdout/2026-09-02/ANALYSIS.md) was frozen before
inference. The [lock](holdout/protocol-lock.json) has SHA-256
`e32927b0e334a5706bb38ad457218d9e0ad9294e64524bcbeb95bf75e9e2c73f`.
The measured Forge executable SHA-256 is
`9abe126279d9d042a26afaf1983b1df1671092ffcd9ba6e8c0f47740c13d70be`.
The archive verifies all 72 frozen source hashes and each complete seeded
schedule. It retains every numeric record, diff, verifier output, and failed
fixture source, with [616 byte-verified copied files](holdout/export-integrity.json).
Full raw sessions remain at the source directory recorded in each audit.

The [invalid-evaluation retention helper](retain_invalid_holdout.py) was written
after execution solely to archive the rejected evidence. It invokes the frozen
strict auditor, requires rejection, and reproduces that rejection after copying;
it changes no evaluator rule or outcome and computes no substitute interval.
The [descriptive census](holdout/descriptive-summary.json) reports counts and
ordinary all-record medians. All 108 records have positive token, timing and
resource measurements; the one protected-file mutation is the recorded
measurement violation.

This is evidence from small synthetic Go/Python tasks on one Windows/CUDA host
and Qwen3-Coder-30B-A3B Q4_K_M. It does not establish general repository accuracy,
cross-model reliability, broad platform support, or completion of Forge's design.
