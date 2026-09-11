# Candidate-06 lock-in screen: outcome

Executed September 10, 2026. 14 runs, all retained, on the frozen
candidate-06 runtime (`candidate-06-lock-in/runtime/forge.exe`,
`45f15552…`) with the loop-repair policy. Loop-pilot profile throughout;
protocol frozen in `protocol.json` before the first run.

## Purpose

Characterization for banking the W2 correctness fixes (bytecode isolation,
scratch-script exclusion, silent-command observation, reporting-key split),
plus every other change in the frozen tree (elide/stop-loss flags, all
default-off on this arm; review defect fixes). This is NOT a promotion
screen: the preregistered bank condition is no Go regression, with Python
recorded without claim.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **2/12** — original 2/3; renamed, paraphrased, distractor 0/3 |
| Go (2 manifests × 1) | **2/2 — no regression** |
| Manifests with ≥1 pass | 1/4 |

Comparators on the same population and profile (do not pool): W0 candidate-2
4/12, candidate-05 1/12. 2/12 sits between them and is within noise of both
(Fisher exact two-sided p ≈ 0.64 vs 4/12). It claims no lift and shows no
harm — exactly the outcome this screen was designed to produce for fixes
that claim nothing.

## Verdict

Bank condition met. The W2 correctness fixes are banked in candidate-06
(G0 5/5 plus this screen). No gate batch follows from this screen.

Screening evidence only — never written to any `outcomes.json`, never pooled
with gate outcomes, not re-run. All 14 runs retained under `runs/` with
per-run `attempt.json`, `harness/` results and terminal workspaces.
