# Candidate 02 context contract review

Read-only review during the frozen G1 campaign. No source, build, configuration,
or model execution was changed. The recommendations below are hypotheses for a
separately identified candidate, not accepted model-quality improvements.

## Smallest next experiment: make the mandatory host record observational

Two concrete rendering hazards are already present in the current implementation.

1. `minimal_action` prepends model prose as `assistant_content` to normalized
   action JSON. The successful-edit branch stores
   `fg_compress_output(action, 2048, ...)` as `checkpoint.last_delta`.
   `candidate_control` then labels this complete action excerpt
   `PREVIOUS_APPLIED_DELTA (historical tool arguments)`. Thus model hypotheses
   can be copied into mandatory host evidence; a long hypothesis can also crowd
   actual patch arguments out of that excerpt. In the renamed r1 fixture,
   prompts 0016 and 0028 contain `assistant_content` inside this host record.
   Prompt 0007's excerpt starts with a truncated tail, losing its action identity.
2. `candidate_validate` retains the complete tool feedback string in
   `latest_feedback`, including instructions such as `PASS. Call final now` or
   `NOT PASSED. The repair episode remains active`. `candidate_control` replays
   that string even when `current_inputs` differs from `latest_validation_inputs`.
   The historical label and both hashes are present, but the old imperative is
   still included in the most recent mandatory user message. Host final gating
   remains correct; the prompt can nevertheless direct a model using an old
   candidate's result.

Recommended change has no public context API:

- Capture the previous edit from parsed tool name and arguments, excluding both
  `assistant_content` and `thought`. Retain its journal identity and path before
  any bounded old/new-text excerpt. Label truncation as an excerpt. Keep full raw
  action/model output in the existing session artifacts.
- Keep the last complete validation's outcome, input identity, validation ID,
  failed-command identity, and diagnostic body as observations. Render current
  operational guidance from current checkpoint state. If hashes differ, state
  that the prior result applies to different inputs and current input has not
  been verified by that result. Do not replay the prior completion/repair
  imperative. Keep the prior diagnostic available, explicitly historical.
- Continue keeping incomplete attempts separate from the last complete result;
  reads, reverts, changed assertions, and fewer failures do not establish a pass.
- Optionally include a bounded list from the existing `modified[]` array to
  preserve multi-file awareness. Label it "paths written during this run";
  these paths can include reverts and are not proof of a remaining net diff.
  Report an omitted-path count if bounded. Keep the existing fresh source excerpt
  at `last_path`; changing source focus would be a separate experiment.

This changes the mandatory record without a history reset, new model action,
extra reflection attempt, budget increase, workspace rollback, or final-gate
change. It cleanly tests whether conflicting host guidance contributed to the
observed reversals. It does not establish that retained assistant reasoning is
the cause of the model failures.

## Deferred recovery-reset proposal

`forge_context_plan_bounded` only limits history by prompt size. It selects pinned
closures, then admits complete optional bundles newest first. Old bundles become
eligible again whenever prompt space grows, including when final schema/state
becomes smaller. `minimal_append` seals raw records, so destructive rewriting of
assistant prose is neither necessary nor appropriate.

For a later, separate reset experiment, the smallest new selection contract is:

```c
char *forge_context_plan_bounded_since(
    forge_context *, size_t input_budget, size_t first_optional_index,
    size_t *tokens, size_t *evicted, forge_error *);
```

- The existing bounded function delegates with index zero, preserving behavior.
- A boundary in `0..forge_context_size(ctx)` is a selection boundary, not a
  staleness claim. Required pinned dependency closures remain eligible below it.
- New optional admissions and their previously unselected dependencies must all
  be at or above the boundary; a newer optional root must not pull an old action
  back in through its dependency. Keep existing actual-render counting, output
  reserve, chronological suffix, and explicit mandatory-input error contracts.
- Record a monotonic boundary only after a complete failed validation exchange
  has been appended and its complete evidence retained in host state. Do not reset
  after each read, comment edit, intermediate multi-file write, incomplete
  validation, or malformed reflection. Choose and preregister the precise reset
  trigger before testing; an arbitrary reset count is not evidence of recovery.
- Keep user task/history messages and `ask_user` replies pinned. Below-boundary
  latest validation calls can be omitted because their complete host observation
  is retained; otherwise the current "pin newest result" rule bypasses the reset.
- Keep the boundary through repair, validation, and final, preventing old bundles
  from returning when a smaller schema frees space. Emit boundary, trigger input
  and validation identities, and omitted counts separately from budget eviction.

A boundary alone does not erase all old model prose: an older pinned `ask_user`
reply requires its assistant call, including any reasoning attached to that call.
This is a documented limitation of the smallest selection-only experiment.
Suppressing only rendered hypothesis fields could address that residual while
preserving every tool trace, but requires more plumbing: the projection must be
reconstructible for `forge_context_cache_anchor` and exported planned-token
validation. Current snapshots have no such projection metadata. Modifying only
the generation string would make the selected context, anchor verification, and
snapshot token counts disagree. `reflect_failure` also stores its hypothesis in
tool arguments, so simply stripping `assistant_content` would not fully reset
that feature's history.

Do not use `forge_context_invalidate` as a reset shortcut. Eviction is a selection
decision; it does not mean the historical observation was invalid, and must not
mark pinned user evidence or complete past diagnostics stale.

## Deterministic scenarios

For the observational-record experiment:

1. Emit a wrong hypothesis in the assistant preamble of a real edit. The raw action
   and model artifact retain it; the mandatory previous-delta record contains
   only the applied tool/arguments and no model hypothesis.
2. Validate failing candidate A, edit to candidate B, and inspect the next host
   record: A's diagnostic and IDs remain, identities differ, B is unverified, and
   A's operational imperative is absent. Revalidating B installs its own result.
3. Validate a passing candidate, mutate inputs before completion, and ensure
   passing evidence becomes historical, no old completion instruction is emitted,
   and a final cannot succeed without verifying the current inputs.
4. Follow a complete failing check with an incomplete/mutating check: the last
   complete diagnostic remains separately identified. A change in operands or
   test coverage is not represented as progress or success.
5. Edit two source files with an intermediate syntax error, read a third file,
   then complete the related repair. Written-path awareness retains both files;
   no partial write triggers a reset or loses the latest real delta.

For any later boundary experiment, additionally test no resurrection with a
larger final budget; native call/result pairing across the boundary; a newer
optional root with an older dependency; pinned task and clarification retention;
export/import retaining all raw records; explicit mandatory-input failure; and
no extra reset/reflection opportunity from repeated reads or comment edits.

## Evidence inspected

The renamed r1 run reached 32 actions, used 261,982 cumulative input tokens, and
recorded 62 context evictions. After complete failed validations at actions 6,
15, and 27, its next prompts still contained 6, 15, and 23 assistant messages with
826, 7,399, and 14,675 bytes of assistant prose respectively. The final prompt had
27 assistant messages and 18,557 prose bytes. The distractor r1 run retained
21,051 assistant-prose bytes in its final prompt yet did obtain a passing final
checkpoint; retention alone is therefore not a sufficient failure explanation.

- [Renamed r1 result](../runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001/harness/generalize_retractions_renamed-loop-repair-r001/result.json)
- [Renamed host record after second failed check](../runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001/harness/generalize_retractions_renamed-loop-repair-r001/session/context/0016.txt)
- [Renamed host record after third failed check](../runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001/harness/generalize_retractions_renamed-loop-repair-r001/session/context/0028.txt)
- [Renamed final prompt](../runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001/harness/generalize_retractions_renamed-loop-repair-r001/session/context/0032.txt)
- [Distractor final prompt](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/session/context/0032.txt)

Implementation reviewed: `src/core/agent.c` (`minimal_action`,
`candidate_validate`, `candidate_control`, `candidate_user_reply`, bounded
selection pinning and successful-edit recording), `src/context/context.c`
(`forge_context_plan_bounded`, native pair rendering), and current context,
snapshot, and conversation public/internal contracts.
