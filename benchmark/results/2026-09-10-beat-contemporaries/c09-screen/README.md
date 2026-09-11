# Candidate-09 budget-guidance screen: outcome — first positive signal

Executed September 11, 2026. 14 runs, all retained, on the frozen
candidate-09 runtime with the loop-budget policy (bounded repair +
per-turn remaining input/generated token counts in the candidate control
state + concise tool-shaped work discipline in the instructions).
Loop-pilot profile; protocol frozen before the first run. G0 on this
candidate passed 5/5 before any model run.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **5/12** — paraphrased 2/3, renamed 2/3, distractor 1/3, original 0/3 |
| Go (2 manifests × 1) | **2/2 — no regression** |

Comparator on the same 14-schedule, frozen candidate-06 runtime,
loop-repair: Python 2/12 (original only), Go 2/2.

## Adjudication, stated carefully

Token bar: mean generated tokens 8,824 vs 9,306 (**−5.2%; bar was −10% —
missed**); mean input tokens 212,630 vs 223,133 (−4.7%; bound was +3% —
met). The token hypothesis as preregistered did not clear.

Success, which was explicitly NOT the claim: 5/12 vs 2/12 with 3-manifest
coverage against the comparator's 1. Fisher exact two-sided p = 0.371 —
not significant at n=12 clustered runs, so this is a directional signal,
not an established lift. But it is the first favorable success signal in
eleven loop screens, and its shape is unusual: paraphrased passed 2/3 after
passing 0/3 in W0 candidate-2, 0/3 in the c06 comparator, and 0/6 across all
v1-pilot arms. Failures in both arms still consume exactly 32.0 turns; the
token edge is compositional (more early-finishing passes: 7 vs 4), not
per-turn concision — the exemplars did not measurably shorten turns.

Every pass is legitimate: harness `passed: true` (requires independent
verification), protected files unchanged, and no pass served a reused
verdict (dedup flag was off; validation always re-executes regardless).

## Decision

Neither close nor promote. The token bar missed, so no efficiency claim.
The success signal is too strong to bury and too weak to promote, so it
gets exactly one preregistered confirmation screen on the same frozen
binary and schedule (new run ids, `c09b-`): confirm at **≥3/12 Python with
≥2 manifests covered** and the flag escalates to broader-suite measurement
(Phase 2/4 populations where success actually varies); anything less closes
the success reading and the flag with it. No gate batch follows from this
screen under any outcome — G1's 18/18 denominator is untouched.

Screening evidence only — never written to any `outcomes.json`, never pooled
with gate outcomes for a p-value. The confirmation is a second preregistered
look at new data, not a re-run of this one.
