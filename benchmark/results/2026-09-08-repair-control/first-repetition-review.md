# First-repetition repair trace audit

Scope: read-only audit of the first repetition in the frozen repair-control comparison. Findings are descriptive of these individual trajectories, not a model ranking or an estimate of repeated-run reliability. Current and minimal share the current decoder; the historical checkpoint remains a confounded whole-system reference. No source changes or additional inference were performed for this audit.

All four first-repetition current/minimal retraction pairs were inspected. Primary composite success is current 2/4, minimal 0/4. Existing external verifier logs show minimal paraphrased has a passing final workspace despite its composite failure. These counts describe this repetition only; the frozen primary outcome is unchanged.

| Task | Current | Minimal | Current generated tokens | Minimal generated tokens |
| --- | --- | --- | ---: | ---: |
| Renamed | Fail, 16 actions, 57.407 s | Fail, 16 actions, 29.813 s | 4,032 | 3,195 |
| Distractor | Pass, 10 actions, 26.718 s | Fail, 16 actions, 25.454 s | 1,254 | 2,340 |
| Paraphrased | Pass, 9 actions, 24.281 s | Composite fail: tests pass, no final, 16 actions, 29.798 s | 982 | 3,086 |
| Original | Fail, 16 actions, 55.312 s | Fail, 16 actions, 34.266 s | 3,617 | 3,929 |

Times are the benchmark result's end-to-end wall time, including cold startup and external verification. Action numbers below include every model generation. A successful process exit is distinguished from a passing test result.

## Renamed

- Minimal actions 1–4 list/read/read/reread the two files. Action 5 runs `python test_ledger.py`, a definition-only file with no unittest entry point, gets empty output and exit 0, and then claims tests pass. Action 6 submits a whole-function replacement whose only change removes `if True`; this does not alter the defective behavior. Action 8 finally invokes unittest and receives cash 7 versus expected 0.
- Minimal action 9 incorrectly attributes the failure to the duplicate-post case and supplies identical old/new function text. The tool rejects the no-op. Action 10 adds `del entries[reference]`, unrelated to handling a retraction before its post; actions 11–13 repeat an argv array beginning with literal `>` and all fail before process launch. Action 14 reruns the failing tests; action 15 reverses action 10. The last action demonstrates that the duplicate-post case returns 0, which was already true. It never isolates the actual failing retract-before-post case.
- Minimal action allocation: one listing, three reads, four attempted edits (one rejected), eight commands. The final workspace only removes `if True`. No final action remains, although the generated-token budget is mostly unused.
- Current obtains real failing unittest evidence at action 3. It spends ten of sixteen actions reading files; four actions submit whole-file hunks (7, 10, 12, 15), one runs tests, and action 16 requests final. Action 7 explains that early retractions need handling but merely removes `if True` and adds comments.
- Current action 10 adds the relevant voided-key check but places `continue` before `positions.setdefault`, so a cancelled account disappears rather than remaining at zero. The failure changes from cash 7 to `{}`. Action 12 adds only comments. Action 15 explicitly recognizes that an empty result is wrong, yet retains the early continue and adds an unnecessary pending-retractions dictionary. Ten automatic validation commands detect these failures, but no recovery episode is recorded (`loop_warnings=0`).

Evidence, relative to this report's directory:

- `comparison/cells/generalize_retractions_renamed-minimal-r001/run/generalize_retractions_renamed-minimal-r001/session/events.jsonl` (calls/results 5–16; action 9 has no committed patch).
- The same minimal session's `tool/000006.patch`, `tool/000010.patch`, and `tool/000015.patch`; run `workspace.diff` confirms only removal of `if True` survives.
- `comparison/cells/generalize_retractions_renamed-current-r001/run/generalize_retractions_renamed-optimized-r001/session/events.jsonl` (actions 3, 7, 10, 12, 15, 16).
- The same current session's `tool/000007.patch`, `tool/000010.patch`, `tool/000012.patch`, and `tool/000015.patch`; the associated tool raw outputs contain post-edit assertion failures.
- Historical checkpoint action 3 also removes `if True` and adds a comment. Subsequent edits alternate that comment; one replacement is identical. This reference fails in 16 actions and 51.188 s, but its decoding differences prevent attributing the difference to orchestration alone.

## Distractor

- Current reads the extra legacy file at action 1, obtains failing unittest evidence at 4, and submits its first repair at 8. That whole-file hunk makes the exact needed change: replace `if True` with `if event['id'] not in cancelled`, after `totals.setdefault`. Automatic validation passes, action 9 reruns the tests, and action 10 finishes.
- Minimal action allocation: one listing, five reads, two edits, eight commands. Action 5 again runs the definition-only test file, action 7 attempts unittest without importing it, and action 8 finally invokes the proper runner.
- Minimal action 9 removes `if True` and adds `del postings[target]`. Action 11 says it will address the early-arrival case but only adds two comments asserting that the case is handled. Actions 14 and 16 isolate the already-passing post-then-retract case. Action 15 runs the failing suite through `TextTestRunner` but exits 0 because its process does not propagate the failing test result. The trajectory exhausts the action limit without implementing the relevant condition.

Evidence, relative to this report's directory:

- `comparison/cells/generalize_retractions_distractor-current-r001/run/generalize_retractions_distractor-optimized-r001/session/events.jsonl` (actions 4, 8–10), `session/tool/000008.patch`, and `workspace.diff`.
- `comparison/cells/generalize_retractions_distractor-minimal-r001/run/generalize_retractions_distractor-minimal-r001/session/events.jsonl` (actions 5, 7–16), `session/tool/000009.patch`, `session/tool/000011.patch`, and `workspace.diff`.

## Paraphrased

- Current gets failing unittest evidence at action 3. Its first edit, action 7, makes the exact guard change after account initialization. Action 8 reruns the passing tests; action 9 reaches successful completion. Four automatic validation commands run across the trajectory.
- Minimal gets real failing unittest evidence at action 6, after another definition-only invocation at 4. Edit 7 removes `if True` and adds a comment, while claiming to repair early retractions. Edit 9 says a previously cancelled post should be ignored but instead adds a final subtraction loop without removing the existing per-retraction subtraction, producing double cancellation.
- Minimal edit 11 genuinely changes the hypothesis: store postings, collect cancellation IDs, and compute balances afterward. That fixes cancellation order but omits zero entries for accounts whose postings are all cancelled. Edit 13 correctly adds account initialization for all stored postings. Actual unittest passes at action 14; the external verifier also reports two tests passing.
- The minimal benchmark result remains failed because actions 15 and 16 are an unnecessary definition-only test invocation and source reread, leaving no final action at the hard limit. This is a completion failure, not a remaining functional failure. The model generated a passing alternative candidate within approximately thirty seconds and 3,086 tokens. Its four candidate edits and repeated validation should not be flattened into a claim that the minimal interface cannot produce a repair.
- Minimal action allocation: one listing, four reads, four edits, seven commands. A separate functional-verifier outcome is necessary when interpreting the composite success metric.

Evidence, relative to this report's directory:

- `comparison/cells/generalize_retractions_paraphrased-current-r001/run/generalize_retractions_paraphrased-optimized-r001/session/events.jsonl` (actions 3, 7–9), `session/tool/000007.patch`, and `workspace.diff`.
- `comparison/cells/generalize_retractions_paraphrased-minimal-r001/run/generalize_retractions_paraphrased-minimal-r001/session/events.jsonl` (actions 6–16), `session/tool/000007.patch`, `session/tool/000009.patch`, `session/tool/000011.patch`, `session/tool/000013.patch`, and `verification.stderr` (two tests, OK).

## Original

- Current obtains real failing unittest evidence at action 2. Action 6 describes how the early retraction enters `cancelled` before the posting, but its submitted hunk only removes `if True`. Actions 9 and 12 only add comments, and action 15 rewrites those comments. The condition guarding the balance increase is never implemented, despite increasingly specific explanations. Ten automatic validation commands continue to observe the failure; `loop_warnings` remains zero. Allocation is ten reads, four edits, one explicit test command, and a final request at 16.
- Minimal first changes code at action 4: remove `if True` and add a comment about unmatched retractions. Action 5 invokes the definition-only test file; action 6 tries the unavailable Unix path `/usr/bin/python`; real unittest runs at 7. Action 8 submits identical replacement text and is rejected. Action 9 adds `del postings[target]` without addressing the early-arrival case.
- Minimal finally isolates all three test inputs at action 12: post-then-retract returns 0, retract-then-post returns 7, and duplicate-post/retraction returns 0. This is direct executable evidence for the exact failing subcase. Yet actions 13 and 15 submit two further identical replacements. Action 15's explanation is accurate: an earlier cancellation should prevent the later posting from being added to totals. The proposed code still has no guard. Action 14 and the final action 16 merely reproduce the same failing input again.
- Minimal allocation is one listing, two reads, five attempted edits (three no-ops rejected), and eight commands. Existing external verification still fails. This case establishes that wrong failing-case localization is not the entire problem: even after the right counterexample and correct explanation are present, the model can fail to encode the repair.

Evidence, relative to this report's directory:

- `comparison/cells/generalize_retractions_original-current-r001/run/generalize_retractions_original-optimized-r001/session/events.jsonl` (actions 2, 6, 9, 12, 15, 16), `session/tool/000006.patch`, `session/tool/000009.patch`, `session/tool/000012.patch`, `session/tool/000015.patch`, and `verification.stderr`.
- `comparison/cells/generalize_retractions_original-minimal-r001/run/generalize_retractions_original-minimal-r001/session/events.jsonl` (actions 4–16), `session/tool/000004.patch`, `session/tool/000009.patch`, `session/tool/000012.raw`, `session/tool/000013.raw`, `session/tool/000014.raw`, `session/tool/000015.raw`, `session/tool/000016.raw`, and `verification.stderr`. No patch artifacts exist for rejected no-ops 8, 13, or 15.

## Raw native output versus parsed edit and journal

A follow-up checked three specific edits directly against their preceding raw `model_output`, rather than relying only on normalized `tool_call` arguments. XML parameter content was compared byte for byte after removing only the surrounding `<parameter=...>` markers and their framing line breaks. No code or inference changed.

| Run/action | Raw event → normalized call → result event | Boundary comparison | Journal comparison |
| --- | --- | --- | --- |
| Original minimal 13 | 3016 → 3018 → 3020 | Raw `old_text` and `new_text` are identical, each 939 bytes; both exactly match the normalized arguments. | Rejected as conflict before edit preparation. Neither before/after nor patch artifacts exist for call 13. |
| Original minimal 15 | 3697 → 3699 → 3701 | Raw `old_text` and `new_text` are identical, each 939 bytes; both exactly match the normalized arguments. | Rejected as conflict before edit preparation. Neither before/after nor patch artifacts exist for call 15. |
| Original current 12 | 2650 → 2652 → 2663 | Raw `new_text` is 1,006 bytes and exactly matches the normalized hunk text; start, end and file SHA also match. | The before-file SHA equals the raw hunk anchor. The after-file equals raw `new_text` plus its preserved final newline. The journal records applied/ok. |

Both minimal no-op parameter strings have SHA-256 `7b055dd5447ee48a846ea6ab57c63096ad91f7c19983cd74cbf321595d8c7aeb`. Each equals the previously committed call-9 after-file excluding its final newline, and the retained final `failed-workspace/service.py` equals that call-9 after-file exactly. No successful later edit is being hidden by the no-op records.

The current action-12 raw replacement has SHA-256 `46334be3982f97c492afa12f8c0d261475d205f7ef26ff1e50d6ab25e724c985`. Its sole source change is the comment `# But we need to make sure that when the posting arrives, it doesn't get processed`. Python syntax trees before and after are identical when location metadata is excluded. The tool's final-newline preservation is expected hunk behavior, not a lost model change.

Evidence:

- `comparison/cells/generalize_retractions_original-minimal-r001/run/generalize_retractions_original-minimal-r001/session/events.jsonl`, exact event sequences listed above; `session/tool/000009.after`, `session/tool/000013.raw`, `session/tool/000015.raw`, and `failed-workspace/service.py`.
- `comparison/cells/generalize_retractions_original-current-r001/run/generalize_retractions_original-optimized-r001/session/events.jsonl`, sequences 2650 and 2652; `session/tool/000012.before`, `session/tool/000012.after`, `session/tool/000012.patch`, `session/tool/000012.edit.json`, and `session/tool/000012.edit-result.json`.

Conclusion for these samples: the ineffective/no-op edits are already present in the model's raw native output. The parser neither erases a proposed fix nor substitutes old text for new text, and the edit journal faithfully reflects the submitted operation. These samples support the model-proposal semantic-gap conclusion; they do not establish that every possible native-parser path is bug-free.

## Additional Go multi-file transfer comparison

The same first-repetition fixture yields current failure in 16 actions / 48.109 s / 2,337 generated tokens, versus minimal success in 12 actions / 18.813 s / 1,247 tokens.

- Minimal reads both implementation files and the test at actions 2–4. Its action-5 raw explanation identifies both defects: the debit guard wrongly rejects an exact-balance transfer (`<=` instead of `<`), and the transfer credits the source rather than destination. Actions 5 and 6 apply those two precise replacements in `ledger/ledger.go` and `ledger/transfer.go`. Tests pass at actions 7 and 8; it rereads files at 9–11 and finishes at 12. External Go JSON verification also reports pass. No automatic validation intervenes between the two edits.
- Current sees both files at 1–2 and gets the exact-balance failure at 3. It rereads source at 4–5, but edits only the destination-credit defect at 6. Automatic tests correctly still report `exact: insufficient funds 10 2`. Action 8 adds a redundant second debit in the transfer function. It spends 9–13 on reads and a failing test rerun, then action 14 removes the extra debit and returns to its still-failing action-6 contents. It never proposes or applies the missing `<=` to `<` change in `ledger.go`. It rereads the test at 15 and is restricted to final at 16.
- No current candidate is observed passing: all four saved `validation_result` records have `passed=false`, explicit test calls 3/10 fail, and terminal external verification fails. Successful formatting/compile stages do not establish a passing candidate. The return-to-failed-workspace warning at 14 records a loop, but the rollback edit is applied; its feedback explicitly permits an intermediate step in a repair across files. There is no recorded rejection of an intended ledger-guard edit.
- Current allocation is ten reads, three edits, two explicit test commands, and final. Its extra orchestration is visible: 15 validation commands, 9 context evictions, and 43,910 prefill tokens, versus no automatic validation/eviction and 2,948 prefill tokens for minimal. Reported validation time is 3.937 s; prefill time is 17.061 s versus 1.388 s. These measurements explain overhead but do not isolate which prompt/control mechanism caused the different proposals. The trace supports testing uninterrupted multi-file candidates; it does not prove that automatic validation alone caused the incorrect current repair.

Boundary check: raw XML path/old/new parameters match normalized arguments for all three current edits (6, 8, 14) and both minimal edits (5, 6). Every corresponding after-file equals applying that exact replacement to the journaled before-file. Current's incomplete/wrong repair originates in the model proposal, not a parser/write-tool substitution.

Evidence, relative to this report's directory:

- `comparison/cells/go_multifile_transfer-current-r001/run/go_multifile_transfer-optimized-r001/session/events.jsonl`: raw-output/call sequences 621/623 (action 6), 976/978 (8), 1995/1997 (14); `session/tool/000006.patch`, `session/tool/000008.patch`, `session/tool/000014.patch`, their `.before`/`.after` files, `session/tool/000014.raw`, `session/context/0016.txt` (final-only registry), `result.json`, and `verification.stdout`.
- `comparison/cells/go_multifile_transfer-minimal-r001/run/go_multifile_transfer-minimal-r001/session/events.jsonl`: raw-output/call sequences 480/482 (action 5), 633/635 (6); `session/tool/000005.patch`, `session/tool/000006.patch`, their `.before`/`.after` files, `session/tool/000007.raw`, `session/tool/000008.raw`, `workspace.diff`, and `verification.stdout`.

## Implications within this repetition

The recorded committed diffs match the supplied edit arguments. These inspected failures are not evidence of the edit tool silently misapplying a correct patch: the model encodes a different or ineffective operation, sometimes only comments, while its explanation claims a repair. Exact-match no-op rejection works but cannot detect this semantic gap by itself.

Renamed and distractor minimal failures repeatedly attribute the loop's assertion to an already-passing subcase. Original eventually isolates the correct failing input and states the right behavioral change, but sends identical replacements twice afterward. More diagnostic detail alone is therefore insufficient evidence of a repair. A bounded candidate experiment should preserve the executable failing input and the actual previous diff, require another concrete candidate, and evaluate that candidate against the failing case and regression tests. New reads, comment edits, or corrected explanations must not be counted as successful recovery.

Action allocation is a separate failure mode: minimal paraphrased produces a passing alternative implementation and passing tests at action 14, then spends its last two actions without completing. Reserve actions for completion and distinguish test-validated workspaces from the primary composite outcome. Current produces the precise minimal fix in two trajectories; minimal produces a valid alternative in one. The evidence supports controlled tests of candidate generation/verification and completion discipline, not a conclusion that either more orchestration or a different model must solve the problem.

Audit complete: scope stops at the four retraction pairs and the Go multi-file transfer pair from repetition one (ten matched current/minimal runs, plus the renamed historical reference). The model was not rerun. The paraphrased completion-only distinction is supported by the original saved external `verification.stderr`; no numeric verifier return code was invented, and no primary outcome was changed.





