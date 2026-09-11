# Retraction-family limit: recorded exit (W4.4)

Recorded September 10, 2026, under the beat-contemporaries campaign. This is
the fresh-design plan's W4 item 4 honest exit for one population: the four
Python `generalize_retractions_*` manifests at the loop-pilot profile
(temperature 0.6, seed 42, 32 actions, 16384 context, Qwen3-Coder-30B-A3B
Q4_K_M on the RTX 5090 Laptop GPU).

## Statement

This model on these manifests does not support the G1 gate at the required
18/18 denominator. No further gate batches will be spent on this population.

## Evidence

| Attempt | Change class | Result (own population and profile; do not pool) |
| --- | --- | --- |
| W0 candidate 2 | bounded repair history | 4/12 |
| candidate-04 | H-ECHO no-op elision | 2/12 — hypothesis refuted |
| candidate-05 | host-defect repairs | 1/12 — claims nothing, no lift |
| W4 temperature 0 | sampler variance removal | 0/12; trajectories still diverge |
| W4 repetition penalty 1.05/64 | sampling suppression | 1/12; rate unmoved at 40.2% |
| W4 stop-loss + split | trial abort on 3 consecutive no-ops | 3/12; abort never fired (0 events) |

Every Python failure in the bounded-repair era ends at the 32-action wall with
99.4%+ cumulative input consumed. The failing half of `apply_patch` is
`new_text` generation (0/73 anchor rejections): the model resends whole
functions byte-identical rather than changing the unguarded posting branch.
Run-to-run variance is neither seed- nor temperature-controllable (GPU
execution nondeterminism, per `AGENTS.md` and the pinned llama.cpp notes).

## Scope of this exit

* It covers exactly the four retraction manifests at the loop-pilot profile.
  It is not a claim about other families, other profiles, other models, or
  the loop machinery in general (the same machinery holds Go 4/4 and the
  tranche-2 lead elsewhere).
* It does not close efficiency work: token/latency screens use this family as
  a workload, not as a gate.
* Reopening requires a plan amendment plus a new hypothesis from a different
  class (model capability, not host evidence presentation) — six variations
  of the evidence-presentation class have been measured and none lifted the
  rate.

## What continues instead

Phase 1 efficiency screens (prefix stability, `run_command` dedup, grammar
early-stop + n-gram speculative, TALE-lite budgets), Phase 2
execution-grounded search, Phase 3 model-capability escalation, Phase 4 fresh
holdout proof — per the beat-contemporaries plan.
