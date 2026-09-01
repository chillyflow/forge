# Primary campaign analysis plan

Status: pre-registered after the repeated parity gate and before the 29-task
primary matrix. The five-task gate is measurement validation only and is not
part of the primary analysis dataset.

## Locked population and scope

- 29 checked-in tasks, three repetitions, and the three frozen harnesses.
- The protocol lock fixes task bytes, protected-file hashes, model and runtime
  bundles, context/decode settings, order seed, hardware, and cold lifecycle.
- Claims are limited to this suite, model, hardware, configuration, and cold
  lifecycle. No general coding-agent, model-family, or hardware claim follows.

## Endpoints

1. Primary: pass rate for each harness over every scheduled task/repetition.
   Failures and timeouts remain in the denominator.
2. End-to-end time for all runs, successful or not. Report the full
   distribution, including median, p90, mean, standard deviation, and a 95%
   confidence interval for the median.
3. Pairwise end-to-end differences on matched task/repetition pairs where both
   harnesses pass. Direction is `right - left`; positive values mean the right
   harness is slower. Also report paired pass-rate differences over all pairs.
4. Descriptive per-language and per-category pass/timing summaries. These are
   scoped slices, not separately powered confirmatory claims.

## Confidence intervals

- Use a deterministic 20,000-sample task-cluster percentile bootstrap with seed
  `20260901`.
- Resample task IDs with replacement and retain all repetitions belonging to
  each sampled task. This preserves the repeated-measures structure.
- Use the bootstrap mean for pass-rate and pass-rate-difference intervals, and
  the bootstrap median for end-to-end and matched-pair timing intervals.
- Report point estimates and interval bounds even when an interval is wide; do
  not hide unstable or failed strata.

## Measurement validity and exclusions

A campaign is measurement-invalid, rather than an ordinary task failure, if a
record has zero/missing prompt or generated tokens; incomplete cold-start,
agent, verification, end-to-end, RSS, or VRAM fields; a changed protected file;
mismatched fixture/protected hashes; a non-reproducible seeded order; or a
mismatched locked configuration. Ordinary verifier failures remain preserved
and included. No post-hoc run deletion or alternate success definition is
permitted.

The reporter must reject incompatible configurations and must retain every
source result directory. The full 261-run matrix begins only after the final
protocol lock is generated from a clean commit containing this plan.
