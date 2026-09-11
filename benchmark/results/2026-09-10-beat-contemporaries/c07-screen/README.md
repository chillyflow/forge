# Candidate-07 stable-prefix screen: outcome

Executed September 10–11, 2026. 14 runs, all retained, on the frozen
candidate-07 runtime with the loop-stable-prefix policy (bounded repair +
monotonic admission floor: a dropped exchange is never re-admitted when the
per-turn budget loosens). Loop-pilot profile; protocol frozen before the
first run. G0 on this candidate passed 5/5 before any model run.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **1/12** — original 1/3; renamed, paraphrased, distractor 0/3 |
| Go (2 manifests × 1) | **2/2 — no regression** |

Comparator on the same 14-schedule, frozen candidate-06 runtime,
loop-repair: Python 2/12, Go 2/2. The Python difference is one run —
within noise, no success claim made or shown.

## Efficiency verdict: no gain measured — axis closed

| Measure (mean per run, 14 runs) | candidate-06 loop-repair | candidate-07 stable-prefix |
| --- | --- | --- |
| Prefill tokens Σ(prompt−cached) | 97,093 | 102,839 (+6%) |
| Prompt tokens | 223,133 | 228,986 (+2.6%) |
| Cached tokens | 126,041 | 126,146 (≈flat) |
| Turns | 29.0 | 29.3 |
| Cached-token oscillation amplitude (max−min) | 10,253 (max 12,675) | 10,224 (max 12,669) |

The preregistered bar required mean per-run prefill DOWN with reduced
oscillation. Prefill is up 6% (point estimate; unresolvable at n=14
clustered runs, but certainly not down) and oscillation amplitude is
identical to the token. **The axis is closed.**

## Why it likely failed

The 16.5% re-prefill figure that motivated this work came from one
pathological run (`renamed-s42-r002`, alternating 10.3k↔1.1k cached). Across
14 runs here, cached tokens average ~126k in both arms — prefix reuse
already works well on average, and mean prefill is dominated by steady
prompt growth and trajectory length (turns), not by re-admission. Where the
floor could bind (loosened budgets re-admitting old exchanges), the effect
is too rare to move the mean; where budgets only shrink (the common late-run
case), the floor cannot bind by construction. Additionally, this screen
carried no floor-engagement telemetry (the `bounded_context` event does not
log the floor), so "engaged but useless" cannot be separated from "rarely
engaged" — any revival must add that counter first.

The flag stays in the tree, default-off, G0-verified; it changes nothing
unless requested. No gate batch follows from this screen.

Screening evidence only — never written to any `outcomes.json`, never pooled
with gate outcomes, not re-run. All 14 runs retained under `runs/`;
`instrumentation.json` via the two-trial-aware extractor.
