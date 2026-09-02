# OpenCode reliability campaign

This is the post-campaign implementation and promotion plan for closing Forge's
measured correctness gap to OpenCode without surrendering Forge's latency
advantage. It is evidence-driven: the observed failure concentration points to
orchestration reliability, not inference speed.

## Campaign baseline

- Forge passed 61/87 runs; OpenCode passed 75/87.
- Forge passed 0/23 runs that emitted a loop warning and 61/64 runs without one.
- Seven failures ended with syntactically broken Go files after Forge had
  already diagnosed a missing brace.
- Forge scored 0/12 on four systematic gap tasks while OpenCode scored 12/12.
- Forge's all-run median remained faster: 16.50 seconds versus 26.84 seconds.

The four first-line regression clusters are:

1. `reasoning_route_specificity`: the correct comparator was derived three
   times, but whole-function edits dropped the closing brace.
2. `py_compiler_syntax`: the import-blocking colon was repaired, but Python
   tests never ran and the agent repeatedly read an empty file.
3. `go_multifile_transfer`: the destination credit was repaired and the
   remaining exact-balance `<=` defect was diagnosed, but the agent returned to
   the wrong file.
4. `reasoning_quota_allocation`: the agent generated large byte-identical
   patches instead of the narrow tie-break edit.

## Tranches

### September 2 checkpoint

The [tranche-2 development campaign](../../benchmark/results/2026-09-02-tranche2-native/README.md)
records Forge 83/87, OpenCode 71/87, and Aider 69/87. The task-cluster bootstrap
interval for Forge minus OpenCode is +13.79 percentage points
[+3.45, +27.59]; matched successful-pair E2E time favors Forge by a median
5.46 seconds [4.04, 6.62]. All scheduled failures remain included.

This does not close promotion gate 5: the campaign froze an uncommitted tree,
reused development tasks, and includes diagnostic-specific repair guidance.
OpenCode's resumed execution also has provenance limits documented in the report.
Keep the original promotion requirements below unchanged.

Same-binary gate 11 passed the four regression tasks at 12/12, and invariant
gate 3 passed 60/60, both without loop warnings. The full campaign reproduced
only 11/12 on the regression tasks: quota allocation repaired the code but
exhausted the prompt budget before completion. Atomic transfers failed 0/3
against OpenCode's 1/3, so gate 3 remains open. Dependency ordering and event
replay passed 3/3 each. That checkpoint required the atomic-transfer repair loop,
completion under prompt pressure, and a new evaluation frozen from a clean revision.

### September 2 clean repair and holdout

The [repair and holdout report](../../benchmark/results/2026-09-02-tranche2-repair/README.md)
records the final candidate at clean revision `5222616`. Rendered-context
budgeting now removes optional bundles before failing, and native template
validation accepts the host's final-only registry. Repair instructions require
baseline tests and tracing the first incorrect expression before editing.

The final development matrix passed 83/87: all four regression tasks 12/12,
all invariants 60/60, atomic transfers 3/3, quota allocation 3/3, dependency
ordering 3/3, and event replay 2/3. Regression and invariant sets had no loop
warnings. Four failures remain in that matrix; earlier candidates' successes
are not substituted for them.

A separately authored, preregistered 12-task holdout completed all 108 scheduled
runs from that clean revision: Forge 30/36, OpenCode 29/36, and Aider 21/36.
Forge passed both atomic and both dependency tasks 3/3, but lost retractions
0/3 to OpenCode's 1/3 and failed rolling windows 0/3. The latter exhausted
generation before making an edit; the retraction repairs remained incorrect.

The comparison is rejected: OpenCode changed a protected test's diagnostic
message in `holdout_go_window-opencode-r001`. The frozen rule invalidates any
protected-file mutation. The reporter refused to calculate confidence intervals;
all records, the mutation, and the rejection are retained. No repetition was
replaced or excluded. The small descriptive point-estimate lead is not acceptance.

Tranche 2 remains open. Next work is the retraction repair loop, rolling-window
generation exhaustion, and protected-fixture integrity across harnesses, followed
by another untouched, clean-frozen holdout after tuning. Keep all promotion
thresholds below unchanged. Forge remains a development preview regardless of
these local reliability fixes.

### Tranche 1 — edit, validate, recover

Deliver together because each feature closes a different part of the same
failure cycle:

- Transactional edit-v2: SHA-256-anchored line hunks, immediate no-op
  rejection, staged Go syntax checks, and atomic publication only after the
  candidate passes its preconditions.
- Validation-driven control: compiler-level Python syntax checks, targeted and
  broad unittest/pytest scheduling, first-stall validation, and fail-closed
  handling when validation is applicable but unavailable.
- Recovery-mode loops: reject the first repeated action without executing it,
  preserve the current diff and diagnostic, and require a context-independent
  materially different next action under the remaining budget.

Acceptance for this tranche is local unit/integration coverage plus zero target
mutation for rejected edit candidates, actionable validation diagnostics, and
no fatal repeated-action path.

### Tranche 2 — native prompt/tool protocol A/B

Compare the current flattened single-user-message transcript against native
system/user/assistant/tool roles and template-native tool schemas. Keep Forge's
authorization, path policy, JSON argument validation, event evidence, and
command limits identical between arms. Freeze model, hardware, task order,
budgets, and timing boundaries before running the A/B.

### Tranche 3 — failure-conditioned reflection

Permit one short diagnostic-reasoning turn only after a failed validation or a
recovery episode. Do not globally enable routed thinking: the broad ablations
did not improve task success. Attribute any benefit separately for test failure
and loop-conditioned cases.

### Tranche 4 — structural retrieval

After the repair loop is reliable, add selective index invalidation,
changed-symbol impact, and related-test retrieval. The campaign failures usually
already contained the relevant files, so retrieval is deliberately sequenced
after edit/validation/recovery correctness.

## Promotion gates

1. Recover the four systematic gap tasks at 12/12 with zero terminal loops and
   zero syntax-broken workspaces.
2. Preserve 60/60 on the 20 tasks Forge already passes in every repetition.
3. Beat OpenCode on the hard holdout clusters—atomic transfers, dependency
   ordering, and event replay—rather than merely tying the aggregate.
4. Reach at least 80/87 before another full campaign. A 76/87 point-estimate
   lead is too fragile to promote.
5. Make the final comparative claim only on a newly frozen holdout where the
   task-cluster bootstrap lower bound for Forge minus OpenCode is above zero.
6. Preserve the current latency advantage and complete timing/token/outcome
   evidence for every run.

## Measurement protocol

- Freeze task inputs, cluster labels, evaluator, model artifacts, prompt arms,
  tool policy, hardware, and revision before execution.
- Record terminal workspace syntax, loop/recovery episodes, validation commands,
  exact edit outcomes, wall time, tokens, and missing measurements per run.
- Report paired task-cluster bootstrap intervals and cluster-level failures, not
  only an all-run point estimate.
- Run the four regression clusters and invariant 60/60 set before spending a
  full-campaign budget.
- Treat a missing measurement, unavailable validation, or syntax-broken final
  workspace as non-passing; never impute success.

## Explicit non-priorities

Do not prioritize disabling KV reuse, semantic compression, the grammar fast
path, or compaction: recent ablations showed no correctness gain and generally
increased latency or token use. Checkpoints and speculation remain later speed
work; neither is expected to close this accuracy gap.
