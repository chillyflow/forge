# candidate-05 `host-defects-05` — W2 prerequisites and demonstrated defects

Frozen and screened September 10, 2026. G0 accepted 5/5. Screened under W3's
preregistered rule.

**This candidate claims nothing for the pass rate**, and its screen should not be
read as if it did. What it claims is that a correctness hole is closed and four
host defects are repaired, each with a deterministic regression.

The changes and the two items deliberately not built are described in
`../W2-CHANGES.md`.

## Screen

14 runs on `loop-repair`, so the comparator is W0's candidate-2 arm on the same
population, profile and repetitions.

| Population | Result |
| --- | --- |
| Python | **1/12** (original 1/3; renamed, paraphrased, distractor 0/3) |
| Go | 2/2 — no regression |
| Manifests with ≥1 pass | 1/4 |

W0's matched baseline is 4/12. Fisher exact two-sided p = 0.32, so 1/12 versus
4/12 is **not** an established decrease; with 12 runs clustered across 4
manifests the effective sample is nearer 4 than 12 and this comparison cannot
resolve a difference of this size. The preregistered branch is *"≤7/12 with no
material lift over W0's baseline: not a lever. Record it, keep any defect fixes,
escalate to W4."* That is the correct branch and it is recorded as written.

Read plainly: this screen provides no evidence that the defect repairs help, and
none that they hurt. That is the outcome the plan predicted for items that claim
nothing.

## The correctness fix was itself wrong once, and the suite caught it

The first implementation used a **persistent** bytecode-cache prefix with writes
enabled, chosen to avoid a per-command compile. Two pre-existing bounded-repair
tests failed immediately. The direct comparison against the previous binary on
an identical script:

| Binary | second checkpoint, after the **correct** repair | validation failures |
| --- | --- | --- |
| pre-W2 | validated, **passed** | 1 |
| first W2 attempt | validated, **failed** | 2 |

The prefix had cached `value.py` from the previous candidate. Timestamp
invalidation compares only mtime and size, the fixture's successive edits are
the same byte length, and the edits fall inside the mtime resolution — so a
**correct repair was validated against the previous candidate's bytecode**. That
is the same class of bug the item exists to close, in a worse position: it fails
good fixes rather than passing bad ones.

The shipped version sets `-B` **and** the prefix. The prefix is never written,
so every import compiles from current source, and the workspace `__pycache__`
is never consulted. A regression now asserts that the write-enabled
configuration really is unsafe, so the two-setting requirement cannot later be
reduced to one on the grounds that it looks redundant.

## Measured cost of the fix

The concern with never writing bytecode is that every command recompiles. On the
real validation workload:

| | W0 candidate-2 (pre-fix) | candidate-05 (isolated cache) |
| --- | --- | --- |
| validation ms per command | 79 (median 78) | 123 (median 125) |
| validation ms per run | 393 | 723 |

**+44 ms per command, +0.33 s per run** against a 600-second task limit and a
120-second per-command cap — 0.13% of mean run time. The cost is real, bounded,
and now stated in the plan's own `limitations` string rather than assumed away.

A microbenchmark of a bare `import unittest` under a fresh prefix suggested
roughly +400 ms per command. On this workload the true figure is an order of
magnitude smaller, so **the cost objection to the prefix approach was
overstated** — including where this campaign first raised it. Correctness was
never actually in tension with cost here.

The mean run wall time did rise, 202 s → 248 s, but that is not the fix: it
follows from 11 of 12 runs reaching the 32-action wall rather than 8 of 12,
which is a consequence of fewer early completions, not of validation cost.

## Instrumentation

| Measure | W0 candidate-2 | candidate-05 |
| --- | --- | --- |
| mean turns | 29.2 | 31.6 |
| hit 32-action wall | 8/12 | 11/12 |
| terminated `ok` | 4 | 1 |
| mean cumulative prompt tokens | 221,049 | 254,866 |
| mean identical replacements | 2.8 | 3.1 |
| mean `run_command` | 13.4 | 13.7 |

Every `ok` termination was a verified pass, as in every gated arm measured in
this campaign; there were no false completion claims.

## Caveats

- Screening evidence, never gate evidence, never pooled with a gate.
- The screen cannot resolve a 4/12 versus 1/12 difference; do not read the point
  estimate as harm, and do not read it as benefit.
- Two W2 items were deliberately not built. See `../W2-CHANGES.md` for which and
  why.
- The compaction prefix-reuse oscillation was not investigated in this pass; the
  measured re-prefill cost from candidate 2 stands as previously recorded.
