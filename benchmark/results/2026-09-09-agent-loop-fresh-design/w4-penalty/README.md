# W4 penalty screen: outcome

Single-variable test of vendor-suggested repetition penalty 1.05 / last-64
against the W0 candidate-2 comparator (loop-repair, temp 0.6, no penalty).

## Result

| Arm | Pass | Manifests (orig/renamed/para/distr) |
| --- | --- | --- |
| candidate-2 (W0 comparator) | 4/12 | 2/3, 1/3, 0/3, 1/3 |
| penalty (this screen) | **1/12** | 0/3, 0/3, 1/3, 0/3 |

Only `w4-penalty-generalize_retractions_paraphrased-s42-r001` passed.

## Mechanism check

Identical-replacement rate **35/87 = 40.2%** vs W0 candidate-2 33/82 = 40.2%
(Fisher exact two-sided p = 1.00). The no-op-after-no-op transition rate is
**17/29 = 58.6%** vs the comparator's 18/30 = 60.0% (p = 1.00).

**The penalty did not move the degeneracy it was hypothesised to fix — at all.**

*Correction:* an earlier revision of this file gave the denominator as 84. The
correct figure is 87, recomputed with `../extract_instrumentation.py` over all
12 retained `events.jsonl` (per-run: 5, 9, 5, 6, 6, 4, 9, 6, 7, 11, 9, 10). The
numerator, 35, was always right. The rate is therefore *identical* to the
comparator rather than slightly above it, which strengthens rather than weakens
the verdict.

**Why it did not engage.** The repeated unit is a whole-function `apply_patch`
span of 720–993 bytes, and the repeats are separated by entire turns. A
`repetition_last_n` window of 64 tokens cannot see a repetition at that scale,
so the sampler had no opportunity to act on the target behaviour. What this arm
measured is "repetition penalty 1.05 over 64 tokens", not penalty-based
suppression of no-op edits in general. The preregistered rule closes this arm as
specified; a sequence-level sampler over a window covering a full edit span
would be a **different hypothesis**, not a re-run of this one — and note that the
H-ECHO result already makes any purely-sampling explanation less likely a priori.

## Verdict

1/12 <= 4/12. The penalty axis is **closed** per the preregistered
falsification rule in `protocol.json`. Do not escalate to presence penalty
or quant changes on the strength of this result; the next step is the Tier 2
runner-up (adaptive early-stop / stop-loss), as a separate single-variable arm.

All 12 runs retained under `runs/` with `protocol.json` frozen before execution.
