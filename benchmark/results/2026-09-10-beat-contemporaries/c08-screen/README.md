# Candidate-08 command-dedup screen: outcome

Executed September 11, 2026. 14 runs, all retained, on the frozen
candidate-08 runtime with the loop-dedup policy (bounded repair + identical
`run_command` verdict reuse while no host-observed mutation intervened).
Loop-pilot profile; protocol frozen before the first run. G0 on this
candidate passed 5/5 before any model run.

## Result

| Population | Result |
| --- | --- |
| Python (4 manifests × 3) | **1/12** — distractor 1/3; original, renamed, paraphrased 0/3 |
| Go (2 manifests × 1) | **2/2 — no regression** |

Comparator on the same 14-schedule, frozen candidate-06 runtime,
loop-repair: Python 2/12, Go 2/2. One run apart — within noise, no success
claim made or shown. The single pass (`distractor-r002`) served zero reused
verdicts and passed host validation plus independent verification, so no
stale-pass question arises.

## Mechanism verdict: no engagement — axis closed

`command_verdict_reused` events across all 12 Python runs: **1 event in 1
run** (`original-r002`, which failed). The preregistered bar required reuse
in a majority of Python runs. The machinery works (the one event is a
correct exact-argv hit, verified in session artifacts); the workload does
not contain what it caches.

Root cause, stated plainly: the observed waste is *near-duplicate*
commands — hand-written reproductions of the same protected case with
varied inline scripts, flags, and probes — not *exact-duplicate* argv.
Exact argv equality is the only sound cache key without execution
semantics; anything looser (normalizing away arguments) risks serving a
verdict for a different computation. Addressing near-duplicate waste needs
semantic command understanding, which is a different hypothesis, not a
tuning of this one.

Also contributing: any executed edit, command, or validation clears the
single slot by design, and failing runs interleave patches and validations
between repros, so even exact repeats rarely arrive back-to-back.

The flag stays in the tree, default-off, G0-verified (6 deterministic
contract tests); it changes nothing unless requested. No gate batch follows
from this screen.

Screening evidence only — never written to any `outcomes.json`, never pooled
with gate outcomes, not re-run. All 14 runs retained under `runs/`.
