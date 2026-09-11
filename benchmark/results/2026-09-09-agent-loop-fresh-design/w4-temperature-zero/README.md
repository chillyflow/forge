# W4 arm 1 — the Python retraction family at temperature 0, 32 actions

Executed September 10, 2026. 12 runs, all retained. A **separately declared
arm**: it is not a substitute for a G1 outcome, G1's profile is unchanged, and
this is not pooled with any gate.

Run on the unchanged candidate-2 runtime, so no code change is under test. Every
setting is identical to W0's candidate-2 arm except the temperature, which makes
the comparison single-variable.

## Why this arm

The only prior temperature-0 evidence for this family
(`2026-09-08-repair-control`) used a **16-action** budget, so sampler variance
and budget were entangled. The plan asks for temperature 0 under the *same*
32-action budget to separate sampler variance from capability.

## Result

| Arm | Passes | Rate | 95% CI |
| --- | --- | --- | --- |
| candidate 2, temperature 0.6 (W0) | 4/12 | 33.3% | [0.099, 0.651] |
| **candidate 2, temperature 0** | **0/12** | **0.0%** | [0.000, 0.265] |

No manifest passed at temperature 0: original 0/3, renamed 0/3, paraphrased 0/3,
distractor 0/3. Fisher exact two-sided p = 0.093, so the decrease is not
established as significant at this sample size; what *is* established is that
removing sampler variance did not lift the rate.

**All 12 runs reached the 32-action wall.** Identical replacements were 27/76 =
35.5%, against 40.2% at temperature 0.6 — essentially unchanged.

## The finding that matters: temperature 0 is not deterministic here

With temperature 0 and seed 42 held fixed, the three repetitions of each
manifest still produced **different trajectories**:

| Manifest | turns | `apply_patch` outcome strings across the three repetitions |
| --- | --- | --- |
| original | 32, 32, 32 | `ONOOOOOOO`, `OOOOONNNNO`, `OOONON` |
| renamed | 32, 32, 32 | `OONNN`, `OOOO`, `OONNNNOOO` |
| paraphrased | 32, 32, 32 | `OONOO`, `OON`, `OONNNNNN` |
| distractor | 32, 32, 32 | `ONOOOON`, `OOON`, `OOOONN` |

Every manifest diverged. Run-to-run variance on this family is therefore neither
seed-controllable — the design review already established that `--seed 42` is
byte-identical across repetitions — **nor temperature-controllable**. What
remains is execution nondeterminism in GPU inference, which `AGENTS.md` already
warns about: "Do not require byte-identical free-form prose across cold and
cached CUDA generations: the pinned llama.cpp `tools/server/README.md` documents
batch-size-dependent logits under `cache_prompt`."

## Conclusion

The arm answers the question it was declared to answer, and the answer is
negative in both directions:

1. Temperature 0 does not lift the pass rate on this family; the point estimate
   moves from 4/12 to 0/12.
2. Temperature 0 does not remove run-to-run variance, so sampler variance was
   never the thing to control.

This **confirms the ceiling rather than lifting it**, exactly as the plan
predicted for this arm and as the design review's "reasoning gap, not a plumbing
gap" conclusion implies. Combined with the H-ECHO refutation, it removes the
last profile-side explanation that was executable on the current model.

## Caveats

- Declared-arm evidence, not gate evidence. Never pooled with a gate outcome.
- 12 runs cluster by manifest; the effective sample is nearer 4 than 12.
- A 0/12 result has a wide exact interval, [0.000, 0.265]. It does not establish
  that the true rate is zero; it establishes that this arm found no pass.
- Execution order is this batch's fixed order, not G1's shuffled order.
- Trajectory divergence is measured over `apply_patch` outcome strings and turn
  counts, which is sufficient to show non-determinism but is not a full
  token-level comparison.
