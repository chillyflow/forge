# W4 stop-loss screen: outcome

Single-variable test of trial abort after 3 consecutive identical-replacement
edit rejections (loop-repair + 2 candidates + abort) against the W0 candidate-2
comparator (loop-repair, single trial, 4/12). 12 runs, all retained.

This is a separately declared arm: not a substitute for a G1 outcome, G1's
profile is unchanged, and this is not pooled with any gate. It writes nothing
to any candidate directory and never touches `outcomes.json`. Protocol frozen
in `protocol.json` before execution (forge `45f15552…`, model `fadc3e5f…`).

## Result

| Arm | Passes | Rate | 95% CI (Clopper–Pearson) |
| --- | --- | --- | --- |
| candidate 2, single trial (W0 comparator) | 4/12 | 33.3% | [0.099, 0.651] |
| **stop-loss, 2 candidates + abort (this screen)** | **3/12** | **25.0%** | [0.055, 0.572] |

Per manifest (passes out of 3 repetitions):

| Arm | original | renamed | paraphrased | distractor |
| --- | --- | --- | --- | --- |
| candidate 2 (W0) | 2/3 | 1/3 | 0/3 | 1/3 |
| stop-loss (here) | 0/3 | 1/3 | 1/3 | 1/3 |

Passing runs: `renamed-s42-r001`, `paraphrased-s42-r003`, `distractor-s42-r002`.
`original` passed no repetition. The difference from the comparator is one run;
at this sample size that is not a meaningful difference in either direction
(CIs overlap almost entirely).

## Mechanism check: the abort never fired

`stop_loss_abort` events across all 12 runs (root + trial sessions): **0**.

Identical-replacement `apply_patch` calls still occurred (24 of 85 live-trial
`apply_patch` tool calls, rate 0.282 — live trial sessions only, excluding the
`failed-workspace` byte copies), but never three consecutively: every identical
rejection was separated by at least one other action (reads, commands, applied
edits), which resets the streak by design (`src/core/agent.c:1911`). The abort
condition, as specified, is rarer in practice than hypothesized.

Consequence: this screen effectively measured the trial split without the
abort — loop-repair best-of-2 — and its 3/12 is consistent with the v1 pilot's
null split result (best-of-2 2/6 vs minimal 4/6, different population, do not
pool). The split confound disclosed in `protocol.json` is therefore the whole
story here, not a footnote.

## Verdict

3/12 with `original` missing on every repetition meets the preregistered
falsification rule (`<7/12, or passes missing on any manifest, closes the
abort axis`). **The abort axis is closed.** The abort itself is inert rather
than refuted as a trigger — it never activated — so a differently-shaped
stop rule (cumulative count, wall/action budget reallocation without abort)
would be a different hypothesis, not a re-run of this one. Any such proposal
needs a plan amendment: the fresh-design stopping rule permits at most two
screened candidates before W4, and both are spent.

All 12 runs retained under `runs/` with per-run `attempt.json`
(returncode/passed), `harness/` results and terminal workspaces.
`instrumentation.json` was produced by `../extract_instrumentation.py` after it
was extended to discover live trial sessions (`session/trial-*/.forge/
sessions/*`, always excluding the `failed-workspace` byte copies) and merge
per-trial analyses with no cross-boundary transition pairs; outcome strings
join trials with `+`. Single-trial reproducibility was verified first: all 36
W0 rows regenerate byte-identical. The mechanism counts above use the plan's
predicate (`tool_call` with `tool == "apply_patch"` and non-empty
`old_text == new_text`).
