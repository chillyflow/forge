# First six G1 repetitions: 4 pass, 2 fail

This summarizes exactly the six completed G1 `s42-r001` runs in scheduled order. Primary passes require actual agent completion, complete passing verification, unchanged protected files and unchanged frozen identities. Ongoing repetitions were not inspected. The [summary JSON](first-repetition-summary.json) retains the six outcome records, action traces, validation checkpoints, context blocks, edit contents, classifications and **291 read-input hashes**, all rechecked unchanged. [Reproduce the analysis](summarize-first-repetition.py). This is a first-repetition result, not completion or promotion of the full campaign.

| Task | Primary | Actions | Input tokens | Generated tokens | Agent seconds |
|---|---|---:|---:|---:|---:|
| Renamed | **Fail** | 32 | 261,982 | 8,910 | 197.484 |
| Distractor | Pass | 32 | 261,915 | 10,666 | 256.937 |
| Go API pagination | Pass | 17 | 74,080 | 1,716 | 48.703 |
| Paraphrased | **Fail** | 32 | 261,612 | 12,443 | 253.095 |
| Go multifile transfer | Pass | 28 | 151,868 | 3,185 | 69.953 |
| Original | Pass | 25 | 176,591 | 8,478 | 180.250 |

“Agent seconds” is the retained envelope duration. The two failed runs each independently execute two terminal tests with one failure. Their acceptance envelopes have `complete=false` and `test_count=0`; that zero is not a claim that the independent failed-workspace verifier ran no tests. All six preserve protected files and frozen run identities.

## Behavior changes and completion control

| Task | Edits that change observed behavior | Rejected identical patches | Rejected finals | Compaction starts / evictions | Reserved completion |
|---|---|---|---|---|---|
| Renamed | 4 introduces duplicate-post regression; 7 restores it. Original failure never fixed. | 30 | 29, 32 | 26 / 62 | Validate 31; final 32 rejected |
| Distractor | 8 narrows failure; 21 passes; 23 reverts; 29 passes again. | None | 27 | 26 / 58 | Validate 31 passes; final 32 |
| Go API pagination | 4 repairs parsing/error responses and page offset; tests pass at 5. | None | None | None / 0 | Passing validation 16 reserves final 17 |
| Paraphrased | None. Both applied edits leave all observed outcomes unchanged. | 9, 10, 11, 19, 23, 25 | 21, 24, 28, 32 | 23 / 148 | Validate 31; final 32 rejected |
| Go multifile transfer | 5 credits destination/rejects negative amount; 7 allows full-balance debit. Tests pass at 8. | None | None | None / 0 | Passing validation 27 reserves final 28 |
| Original | 20 narrows failure; 22 first passes full unchanged suite. | 13 | None | 21 / 34 | Passing validation 24 reserves final 25 |

Compaction here means omission of complete older exchanges, not rewritten model summaries. The eviction metric counts omitted segments across planning events. The original maximum remains 32 actions. A passing checkpoint reserves the next action for final; it does not require waiting until action 32. This is visible in both Go runs and original. The three runs reaching the action limit get the reserved validation at 31 and final at 32. Both failures are correctly rejected rather than accepted on model claims.

The Go implementations pass their first post-edit test runs, but host validation then stops on formatting: pagination action 6 and transfer action 17. Subsequent commands largely repeat tests and formatting. Their later host validations pass after formatting. No additional behavior edit or compaction is needed. Transfer also makes one malformed process-start command at action 20, then recovers. Source edits through `gofmt` are separate from recorded `apply_patch` artifacts; the terminal hashes and successful host verification include them.

AST change is not equivalent to behavioral progress. Renamed action 19, distractor 17, paraphrased 12, and original 9/10/19 are comment-only. Removing `if True` changes the AST but does not change the observed outputs. Original action 5 also deletes a cancelled posting from an internal dictionary; its tested outputs remain 0/7/0. The table counts observed behavior transitions rather than every syntactic edit.

## Original's newly verified pass

The completed [original terminal](../runs/development-G1-loop-pilot-generalize_retractions_original-s42-r001/harness/generalize_retractions_original-loop-repair-r001/terminal-workspace/service.py) was copied, byte for byte, into a new temporary directory containing source files only. The exact original verifier command, `python -m unittest discover -v`, ran with bytecode writes disabled. It reports **two tests passed**, exit 0; all three copied source hashes remain unchanged. The disposable directory was removed after checking its resolved path. [Verification result](original-fresh-verify/result.json), [full stderr](original-fresh-verify/stderr.txt), [source-stage analysis](original-r001-analysis.json). No GPU or runtime invocation was used.

Original initially misidentifies the duplicate-post iteration as the failure. It makes comment edits and repeats malformed Python probes. Its debug file at action 17 shows the duplicate case zero, first case zero, and second case seven. At action 18 the model begins with a mistaken “debug test is passing” claim and object-identity theory, but then identifies the actual second iteration. Action 19 describes the right data flow yet changes only comments. Correct diagnosis is therefore not immediately sufficient even in this passing run.

At action 20 it finally adds an executable cancellation guard. Fresh-source results change from 0/7/0 to 0/{}/0: the unwanted amount is gone, but the zero account entry is absent. Host validation 21 supplies that new precise failure. Action 22 adds account initialization in the cancelled branch, producing 0/0/0 and passing the unchanged suite. Its next manual probe has a syntax error, and action 24 still refers to an older failed validation in its reasoning. Crucially, it calls host validation rather than undoing the repair. Validation 24 passes and final 25 follows immediately.

This contrasts with distractor, which first passes at 21 but reverts its correct code at 23 while reasoning from an old failure. It contrasts with renamed/paraphrased, which never make the missing behavioral repair and repeatedly probe a passing iteration. Original also creates a debug file and temporarily points the host source block at it, yet recovers. Debug-file displacement and historical assertions can contribute to confusion, but neither is alone sufficient to explain pass versus fail.

## Prior negative evidence limits the recommendation

The [September 8 recovery README](../../../2026-09-08-repair-recovery/README.md) records an already rejected traceback-locals experiment, v4 (`c5c3f13`). It exposed the actual failing input without modifying the test. The model described the **right failing order and still generated ineffective code**. The full matrix regressed from 7/10 to 5/10 and lost the prior contrast/paraphrased passes; the change was reverted despite deterministic, local-suite and GPU-probe checks passing.

Consequently, supplying the failing operand is **not a new or qualified fix**. This negative result constrains the earlier diagnostic-capture suggestions in the individual reports: they are hypotheses, and the same unqualified traceback-locals change should not be proposed again as if untested. Original's action 19 independently reinforces the gap between saying the correct diagnosis and changing executable behavior.

The unresolved mechanism is how to turn current evidence into a substantive, validated repair while preserving correct candidates and existing successes. Any next experiment must measure actual behavioral edits, full validation and preservation across the frozen population. Better wording, the right operand, fewer repeated calls or more remaining output capacity are insufficient success criteria. These six runs establish functioning capacity/completion controls and two remaining first-repetition failures; they do not establish an all-green repair mechanism.

Full outcomes: [renamed](../runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001/outcome.json), [distractor](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/outcome.json), [pagination](../runs/development-G1-loop-pilot-go_api_pagination-s42-r001/outcome.json), [paraphrased](../runs/development-G1-loop-pilot-generalize_retractions_paraphrased-s42-r001/outcome.json), [transfer](../runs/development-G1-loop-pilot-go_multifile_transfer-s42-r001/outcome.json), [original](../runs/development-G1-loop-pilot-generalize_retractions_original-s42-r001/outcome.json). Earlier bounded analyses remain in [renamed](renamed-r001-report.md), [the two-run comparison](g1-two-python-comparison.md), and [paraphrased](paraphrased-r001-report.md).
