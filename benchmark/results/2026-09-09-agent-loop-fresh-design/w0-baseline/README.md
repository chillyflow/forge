# W0 — calibrated baseline for the Python retraction family

Executed September 9–10, 2026. 36 runs, all 36 retained. No code change: all
three arms ran on the **unchanged candidate-2 runtime**
(`candidate-02/runtime/forge.exe`, SHA-256 `fa3b099c…`), which is byte-identical
to the current `build-gpu/Release/forge.exe`.

This is baseline/screening evidence. It is **not** scheduled gate evidence, it
was never written to any candidate's `outcomes.json`, and it must not be pooled
with gate outcomes.

Schedule, profile and manifest hashes were frozen in `protocol.json` before the
first run. Instrumentation was prespecified in the same file and computed by
`../extract_instrumentation.py`, which reproduces every published candidate-2
table exactly (see "Extractor validation" below).

## Result

| Arm | `--variants` | Passes | Rate | 95% CI (Clopper–Pearson) | Manifests with ≥1 pass |
| --- | --- | --- | --- | --- | --- |
| plain control | `minimal` | 0/12 | 0.0% | [0.000, 0.265] | 0/4 |
| checkpoint only | `candidate-checkpoint` | 1/12 | 8.3% | [0.002, 0.385] | 1/4 |
| candidate 2 | `loop-repair` | 4/12 | 33.3% | [0.099, 0.651] | 3/4 |

Per manifest, passes out of three repetitions:

| Arm | original | renamed | paraphrased | distractor |
| --- | --- | --- | --- | --- |
| plain control | 0/3 | 0/3 | 0/3 | 0/3 |
| checkpoint only | 0/3 | 0/3 | 1/3 | 0/3 |
| candidate 2 | 2/3 | 1/3 | 0/3 | 1/3 |

**The explicit statement W0 was run to produce.** On pass rate, neither the
checkpoint policy nor bounded repair is separated from the plain control at this
sample size: checkpoint-only versus plain control gives Fisher exact two-sided
p = 1.0, and candidate 2 versus plain control gives p = 0.093. Neither is
**worse** than the plain control, and the point estimates are monotone in added
machinery (0 → 1 → 4). So the honest verdict on pass rate is **equal, not
separated**, with the ordering favouring candidate 2.

Candidate 2's 4/12 here is consistent with the 2/8 it recorded on the same
family in its own G1 batch; both are draws from the same population.

## The decision W0 gates

The plan's rule: *"If the plain control matches or beats candidate 2, the loop
machinery is not the problem and W1 should not be built."*

The plain control scored 0/12 against candidate 2's 4/12 and won no manifest at
all. **It does not match or beat candidate 2, so that condition is not met and
W1 is built.** This is a directional decision on point estimates, exactly as the
plan words it — it is not a claim of statistical separation, which the data do
not support.

## The largest measured effect is not the pass rate

Cross-tabulating how each run terminated against what the independent verifier
found:

| Arm | declared done, verified | **declared done, refuted** | exhausted budget, failed |
| --- | --- | --- | --- |
| plain control | 0 | **7** | 5 |
| checkpoint only | 1 | **0** | 11 |
| candidate 2 | 4 | **0** | 8 |

Every one of the plain control's seven `ok` terminations was a false completion
claim: the agent asserted the task was finished and the independent verifier
refuted it. Across the 24 gated runs there were **zero**. Fisher exact
two-sided p = 9.5e-05.

This is a far cleaner effect than the pass-rate difference, and it was not what
W0 was designed to measure. It says the host gate's demonstrated function on
this family is **suppressing false completion**, converting confident wrong
answers into honest budget exhaustion. It does not, on this evidence, say the
gate makes the model repair better.

## Instrumentation

| Arm | identical replacements | rejected finals | `run_command` (exact repeats) | hit 32-action wall | mean forced-opener rate |
| --- | --- | --- | --- | --- | --- |
| plain control | 18/67 = 26.9% | 0 | 125 (31 = 25%) | 4/12 | 0.120 |
| checkpoint only | 19/75 = 25.3% | 10 | 138 (20 = 14%) | 4/12 | 0.185 |
| candidate 2 | 33/82 = 40.2% | 14 | 161 (17 = 11%) | 8/12 | 0.398 |

The repeat column counts a `run_command` whose whitespace-normalised `argv`
exactly matches one issued earlier in the same run. It falls monotonically as
machinery is added — the inverse of the identical-replacement ordering — so
redundant command re-issuing and no-op editing are not the same phenomenon.

**Correction.** An earlier revision of this file reported 113, 126 and 149
repeats. Those were wrong: the extractor read a `command` argument key, but
`run_command`'s argument is `argv`, so every call normalised to the empty string
and all but the first counted as a repeat. The call counts (125, 138, 161) were
never affected, and no other metric read that key. Fixed in
`../extract_instrumentation.py` and recomputed above.

**A caution the plan's framing invites.** Within candidate 2's own G1 runs the
identical-replacement count separates passes from failures. **Across arms it
inverts**: the highest identical-replacement rate belongs to the best-performing
arm. Candidate 2 also survives longer (8/12 reach the action wall against 4/12),
so it has more turns in which to emit one. Identical-replacement rate is
therefore not a quality measure across arms, and a W1 screen must not treat a
drop in it as evidence of improvement on its own.

The plain control issues no rejected finals because it has no host final gate at
all — that is what makes its seven false completions possible.

## H-ECHO replicates out of sample

The plan derives H-ECHO from candidate 2's twelve G1 runs. These 36 runs are
independent of that derivation, so they are a genuine replication.

| Population | P(identical \| previous applied) | P(identical \| previous identical) | odds ratio | Fisher p |
| --- | --- | --- | --- | --- |
| plain control | 9/37 = 24.3% | 9/16 = 56.2% | 4.0 | 3.2e-02 |
| checkpoint only | 10/45 = 22.2% | 9/15 = 60.0% | 5.2 | 1.1e-02 |
| candidate 2 | 15/40 = 37.5% | 18/30 = 60.0% | 2.5 | 9.0e-02 |
| **all 36 W0 runs** | **34/122 = 27.9%** | **36/61 = 59.0%** | **3.7** | **8.9e-05** |
| plan's figure (12 runs) | 9/36 = 25.0% | 19/25 = 76.0% | 9.5 | 1.9e-04 |

The asymmetry replicates, and it replicates in the **plain control**, which has
no checkpoint or bounded-repair machinery. That independently confirms the
plan's claim that the degeneracy predates that machinery rather than being
created by it.

These counts follow the convention documented in
`../extract_instrumentation.py`, which computes them: a pair whose predecessor
is a rejection for some other reason is skipped rather than spliced across. The
distinction is immaterial for candidate 2's arm, which contains no such
rejection, and does move the plain-control and pooled cells, so the rule is
recorded in code rather than left implicit.

The effect is **materially smaller out of sample** — odds ratio 3.7 against the
plan's 9.5, and a 59% rather than 76% repeat rate. That is the expected
regression when a statistic is re-measured off the sample that generated the
hypothesis. Any W1 power calculation should use 3.7, not 9.5.

This remains correlational, exactly as the plan states. A model that has stopped
converging will both emit no-ops and keep emitting them; the transition
statistic cannot separate auto-catalysis from being stuck. Only the intervention
can.

## Deviations and caveats

- Execution order is this batch's arm-interleaved schedule, not G1's shuffled
  order: `run.py` ignores `--order-seed` under `--no-randomize`. Recorded, not
  claimed identical, as the plan requires.
- Arms were interleaved within each repetition so wall-clock drift cannot be
  confounded with arm.
- The pooled 12-run intervals are optimistic. Outcomes cluster by manifest, so
  the effective sample size is nearer 4 than 12, and the parent plan's ban on
  treating overlapping suites as independent observations applies.
- 36 of 36 runs completed; none were discarded, re-run, or replaced.

## Extractor validation

`../extract_instrumentation.py` was validated against candidate 2's retained
evidence before being used here. It reproduces, exactly:

- all eight published identical-replacement counts and denominators
  (0/6, 1/8, 1/7, 2/6, 3/10, 5/8, 6/8, 10/14);
- the rejected-final multiset (passes 0 and 1; failures 1, 2, 3, 3, 4, 4);
- every per-run `apply_patch` outcome string, including `OOONOOOO`,
  `OONNOOOONO` and `OOOONNNNNNNNNN`;
- the H-ECHO statistic to the last digit: 9/36, 19/25, odds ratio 9.5,
  Fisher p = 0.000186.

One correction was made during that validation: an earlier draft counted
non-`ok` `validate_candidate` results as rejected finals. That over-counts,
because those are genuine failed validations (`broad_tests` exit 1), not refused
completions. The correct predicate is the `final_rejected` event, which the host
emits for exactly this purpose.
