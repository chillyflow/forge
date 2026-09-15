# Spike 006 — A/B gate for reduced-candidate greedy sampling

**Question.** `sample_reduced_greedy` (src/inference/llama_backend.c) masks a raw
top-K prefix instead of the whole vocabulary when greedy sampling falls off the
one-candidate fast path, which is the case that pays a full-vocabulary grammar mask
(~12-34 ms per token in the pinned runtime). Does it change any output, and does it
buy anything in a real run?

## Pre-registered bar

Frozen before the first cell. Same model, same task, same workspace path, same seed;
the only variable is the binary.

| # | Hypothesis | Bar |
| --- | --- | --- |
| H1 | Exactness at temperature 0 | generated token count and the final assistant message **identical** before/after |
| H2 | Exactness at temperature 0.6 | identical as well (the reduced path is disabled above temperature 0, so any difference is a bug) |
| H3 | Cost | `sampling_ms` at temperature 0 at least **20 % lower**, with decode wall-clock not worse |

**Falsifier:** any output difference at either temperature ⇒ the change is wrong and
is reverted, regardless of the cost numbers.

**Limits recorded up front.** One task, one fixture, one repetition per cell. The
task is small (6-turn cap), so H3 is a direction test, not a precise effect size;
the sampler cost it targets is attributable only because spikes 002-005 measured it
directly. Temperature 0.6 is included to prove the change is *inert* there, not to
measure it.

## Method

`cell.sh <label> <temperature> <forge.exe>` resets the fixture workspace to a fixed
byte-identical state, runs the repair task under the arm configuration (native
protocol, `--minimal-agent`, seeder 42, 6-turn cap), and copies the session
`metrics.json` and run log here under the label.

Cells: `before-t0`, `after-t0`, `before-t06`, `after-t06`.

## Results

Three binary iterations, all on the same fixtures and paths: `before`, `after`
(first implementation), `after2` (refined after the gate caught a regression).

**Fixed-path cells** (`cell.sh`, identical prompt in every cell because the
workspace path is fixed; temperature 0 unless stated):

| cell | generated | decode ms | sampling ms | sampler share | fallback | fast |
| --- | --- | --- | --- | --- | --- | --- |
| before-t0 | 598 | 2,845 | 30 | 1.1 % | 244 | 360 |
| after-t0 | 598 | 2,813 | **64** | 2.3 % | 244 | 360 |
| after2-t0 | 598 | 2,844 | 48 | 1.7 % | 244 | 360 |
| before-t06 | 576 | 8,266 | 5,689 | 68.8 % | 582 | 0 |
| after2-t06 | 576 | 8,109 | 5,534 | 68.2 % | 582 | 0 |
| before-t0-flat | 386 | 1,891 | 63 | 3.3 % | 1 | 385 |
| after2-t0-flat | 386 | 1,844 | 32 | 1.7 % | 1 | 385 |
| before-t0-bigpatch | 1,931 | 9,390 | 248 | 2.6 % | 158 | 1,779 |
| after2-t0-bigpatch | 1,931 | 9,890 | 268 | 2.7 % | 158 | 1,779 |

**Harness cells** (`run.py`, the real holdout fixtures, native, temperature 0):
`holdout_go_lru` and `holdout_py_closure` PASS in all iterations. Their generated
token counts differ between invocations (944 vs 1,350; 1,234 vs 1,208) because the
harness uses a fresh random temporary workspace whose path is part of the prompt -
recorded so it is not misread as a regression. Their sampler share is 2.2 % / 1.7 %
before and 2.6 % / 0.4 % after: cold fallbacks in every case.

### What the gate established

- **Exactness holds**: generated sequences and the fast/fallback splits are
  identical in **all four** fixed-path cells, across all three iterations.
- **The first implementation regressed the cold case** (t0: 30 -> 64 ms sampling).
  Cause: it ran a vocabulary-wide selection before knowing whether the constraint
  was real, and every cold fallback paid it. The refinement probes the raw maximum
  through the whole chain first, which both answers that question and removes the
  fallbacks that existed only because a ban blocked the one-candidate path.
- **H3 was not met by these cells**: no 20 % sampling reduction, because every
  fallback the gate could reach is *cold* (<= 0.3 ms per token: the grammar is
  still awaiting its trigger, so the mask returns immediately).

### The hot population is on record, not in this gate

The `2026-09-03-native-force` holdout family (temperature 0, native, same model
and fixtures) ran with **61-68 % of decode inside the sampler** and 443-2,034
fallback tokens per run at about **27 ms each** - for example `holdout_go_lru`:
sampling 12,224 ms of decode 20,062 ms; `holdout_py_closure`: 15,846 of 23,436.
That is the full-vocabulary mask on a live grammar, and it is exactly what the
ladder replaces: spike 005 measured the same operation at **0.157 ms with an
identical token on 24/24 steps (43.7x)**. The gate could not reproduce that state
today (its fallbacks never coincide with a live grammar), so the win there is
**inferred** from the recorded distribution plus the mechanism measurement, not
demonstrated end to end.

## Verdict

- **H1 and H2 hold**: no output change anywhere, across four configurations and
  three binary iterations. The suite agrees: 34/34 pass.
- **H3 fails on the gate's population, and the failure is informative**: the
  change is cost-neutral where the fallback is cold and removes a documented
  27 ms/token hot spot where it is not. It is landed as exact risk reduction with
  a stated, measured effect - not as a speedup the gate demonstrated.
- **Kept as evidence**: the regressed iteration (`after-t0`, 64 ms) is retained
  next to the refined one, because the gate is what caught it.
