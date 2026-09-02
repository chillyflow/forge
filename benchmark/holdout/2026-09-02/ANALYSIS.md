# September 2 fresh holdout protocol

Pre-registered before any model execution on these tasks. This population is
separate from the 29 development tasks and their earlier bootstrap result.

## Population and freeze

- Twelve new synthetic tasks, six Go and six Python, three cold repetitions.
- Atomic changes: seat reservations and optimistic document versions.
- Dependencies: build waves and transitive prerequisite closure.
- Replay: revisioned projections and out-of-order posting retractions.
- Remaining tasks: mount resolution, rolling windows, LRU updates, recursive
  configuration overlays, quoted CSV records, and rescheduled priority queues.
- Each task ID is a bootstrap cluster. The six atomic/dependency/replay tasks
  are also reported individually; their failures cannot be hidden in the total.
- These are new implementations, assertions, and failure mechanisms authored
  for this evaluation. They are not representative real repository tasks.
  Atomicity, dependencies, and replay intentionally remain related problem
  domains to the development failures; no claim of domain independence follows.
- Broken/reference preflight must pass before inference. The reference files
  are never materialized into the agent workspace.
- Run `campaign.py --task-dir benchmark/holdout/2026-09-02/tasks --mode campaign
  --require-clean` from a clean checkout containing this plan and the candidate.
  Write outputs under an ignored directory. Preserve the original lock.
- Freeze task bytes, evaluator/source hashes, executable and adjacent DLL
  identities, model, hardware, protocol, schedule, and decode limits. Run all
  legs without a source or binary change. Do not resume by replacing records.

## Fixed execution and analysis

Use Qwen3-Coder-30B-A3B-Instruct Q4_K_M, context 16,384, output reserve 2,048,
16 actions, temperature 0, seed 42, order seed 20260902, GPU layers -1, embedded
template, and the same Windows/GPU installation as the development campaign.
The per-task process timeout is 900 seconds; verifier timeout is 120 seconds.
Forge uses optimized/native; OpenCode 1.18.25 and Aider 0.86.2 use their existing
adapters. Harness execution order is Forge, OpenCode, Aider, serially on the
same GPU. Cold model startup is included; machine background load is a limitation.

The primary contrast is Forge minus OpenCode, over all 36 matched repetitions.
Aider is secondary. Invoke the reporter with labels in the fixed order
OpenCode, Forge, Aider. Use the existing 20,000-resample task-cluster percentile
bootstrap, base seed 20260901 (including the reporter's label-derived seeds).
Report pass-rate differences and their 95% intervals, all-run E2E distributions,
and matched successful-pair E2E differences. No one-sided replacement interval.

Passing requires normal harness completion, independent verifier success, and
unchanged protected files. Timeouts, incomplete completions, syntax failures,
and all ordinary verifier failures remain in the denominator. Missing or zero
token counts, missing timing/resource data, protocol drift, missing repetitions,
or modified protected files invalidate the measurement, as in the original
analysis plan. Report them; do not silently exclude or rerun them.

Promotion requires the Forge-minus-OpenCode 95% lower bound strictly above zero,
retained latency advantage and complete evidence, in addition to the original
development regression, invariant, and hard-cluster gates. A tie, a wide interval
crossing zero, or a failed hard cluster leaves acceptance open. Report the outcome
even if the candidate fails. Do not change prompts, runtime, tests, scoring, or
the population after viewing these model outcomes. Any later tuning makes this
set development evidence and requires another untouched holdout.

Forge remains a development preview regardless of this result. Durable resume,
deeper repository semantics, speculative decoding, broader language/platform
evidence, isolation, observability, and a stable packaged library API remain
separate unfinished product work.
