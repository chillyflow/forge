# Candidate-10 no-edit-gate screen: outcome — flag closed

Executed September 11, 2026. 14 runs, all retained, on the frozen
candidate-10 runtime (`428c72cd…`) with the loop-noop-gate policy (bounded
repair plus a one-turn `apply_patch` exclusion after a rejected
identical-replacement edit, host-enforced). Loop-pilot profile; protocol
frozen before the first run; G0 on this candidate passed 5/5.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **1/12** — original 1/3; renamed, paraphrased, distractor 0/3 |
| Go (2 manifests × 1) | **1/2** |

Preregistered bar: (a) Go 2/2; (b) identical→identical transition rate below
50% (comparator 60–76%); (c) Python ≥4/12 directional.

| Criterion | Observed | Verdict |
| --- | --- | --- |
| Go no regression | 1/2 — one `go_multifile_transfer` run died at turn 14 with the native-parse limit ("Generation limit reached before one complete native call"), not a gate-induced failure (the gate engaged at most once and the failure is a template-parse limit mode observed elsewhere) | missed |
| Transition rate | 10 identical→identical / (10+9) = **52.6%**, down from 66.7% (c06 comparator), but above the 50% bar | missed |
| Python success | 1/12 vs comparator 2/12 — within noise, no lift | missed |

## Mechanism

The gate **engaged strongly**: 11 of 12 failing runs render at least one
`NOOP_EDIT_GATE` control line (1–4 occurrences per run), and the
transition rate fell by 14 points. The mechanism works; the effect size did
not clear the preregistered bar, and the single-repetition Python schedule
could not have detected a small effect in any case (the identical binary in
c09/c09b scored 5/12 then 1/12).

## Disposition

Per the preregistered rule and the Hermes handoff corrections: record the
outcome, close the flag, do not re-run for a better repetition. The flag
stays in the tree, default-off and G0-verified (6 deterministic contract
tests). No default change; no gate batch. Screening evidence only, never
written to `outcomes.json`, never pooled with gate outcomes.

The one Go failure is retained as-is; the `go_multifile_transfer` run is
neither re-run nor excluded.
