# Completed G1 contrast: distractor passes, renamed fails

The two completed Python runs differ most clearly in **which failing input the model keeps investigating**. Distractor consistently investigates the second iteration and eventually makes the required behavioral edit. Renamed repeatedly misidentifies the first, passing iteration and never fixes the failing branch. Both retain complete failure evidence, repeat mistaken claims, make ineffective edits, and consume nearly the full input budget. The passing run even reverts a correct intermediate candidate after confusing an old failed validation with the current source.

This report covers only completed `outcome.json` records. The [completion snapshot](g1-completed-snapshot.json) contains the first three finished G1 runs: renamed, distractor and Go API pagination. Only the two Python runs were examined in detail; no further completed Python record existed at the final snapshot. No unfinished streaming run was read. No runtime, source, fixture or retained-input changes, builds, or GPU/model calls were made.

The [comparison JSON](g1-two-python-comparison.json) retains 64 actions, full model outputs, mandatory context blocks, edit contents, validation identities and 122 input hashes. [Reproduce it](compare-completed-g1.py) with Python; it compiles each retained source version directly and runs the unchanged two-test suite in memory, without importing from the retained workspace or writing bytecode. All read hashes remained unchanged. The detailed [renamed diagnosis](renamed-r001-report.md) also includes the cache falsification and complete failure paths.

| Observed result | Renamed r001 | Distractor r001 |
|---|---:|---:|
| Independent terminal tests | Fail, 1/2 | Pass, 2/2 |
| Actions | 32 | 32 |
| Input tokens | 261,982 | 261,915 |
| Input remaining | 162 | 229 |
| Generated tokens | 8,910 | 10,666 |
| Output remaining | 23,858 | 22,102 |
| Envelope agent seconds | 197.484 | 256.937 |
| First complete-exchange omissions | Action 26 | Action 26 |
| Segment evictions | 62 | 58 |
| Largest actual prompt | 13,333 | 13,977 |
| Host validations | Fail, fail, fail | Fail, fail, pass |
| Mandatory implementation-source observations | 14/32 prompts | 23/32 prompts |

## Behavioral transitions and their preceding evidence

The table shows outputs for the three exact protected-test loop inputs: `[post,retract]`, `[retract,post]`, and duplicate posts followed by two retractions. Expected values are all `{'cash':0}`. Numbers abbreviate the `cash` value; `{}` means the key is absent. These are direct executions of retained code, not model claims.

| Distractor version | Outputs | Full unchanged suite | Preceding observation / next behavior |
|---|---|---|---|
| Initial and action 5 | 0 / 7 / 0 | Fail | Reads source and tests. Removes `if True` and adds a comment claiming future cancellation; no observed behavior improvement. |
| Action 8 | 0 / {} / 0 | Fail | Action 7 runs real unittest and reports 7 versus 0. Model correctly identifies `[retract,post]`, adds a cancellation guard, but skips account initialization. |
| Action 17 | 0 / {} / 0 | Fail | Repeated direct probes show `{}`. Adds only a comment; AST unchanged. |
| Action 21 | 0 / 0 / 0 | **Pass** | Action 20 explicitly prints first input zero, second `{}`. Model traces skipped account initialization and adds the missing initialization in that branch. |
| Action 23 | 0 / {} / 0 | **Fail again** | Action 22 directly returns the expected zero. Model nevertheless says “validation is still failing,” then removes the correct change. Source becomes byte-identical to action 8. |
| Action 29 | 0 / 0 / 0 | **Pass** | Failed validation 25 and probes 26/28 still identify the exact second input. Model restores the branch initialization. Probe 30 succeeds, reserved validation 31 passes, final 32 completes. |

The first correct distractor implementation therefore exists **eight actions before the final repair**, and before context omissions begin. It is lost to a subsequent edit rather than rejected by any fresh host validation. The analysis's full-suite replay confirms action 21 passes; the original run only directly probed it at action 22.

Renamed begins at 0/7/0, regresses to 0/7/7 at action 4, returns to 0/7/0 at action 7, and stays there. Action 19 is comment-only; action 30 is a rejected byte-identical patch. Its own actions 14 and 26 print 0/7/0, but later reasoning repeatedly calls the first case the failure and labels the debug results correct. Correctly isolating the input is associated with the useful distractor edits. This is an observed association and the model's stated reasoning, not a controlled causal experiment.

## History versus current evidence

Distractor action 23 receives the current, correct `service.py` in `CURRENT_SOURCE_OBSERVATION`. It includes the initialization before `continue`. The context also retains complete validation 1's older failure, explicitly qualified as applying to its recorded inputs. The IDs differ: current inputs `05ef9e1adda1f502`, validation inputs `6122d673a2844cf0`. The immediately preceding probe has returned `{'cash':0}`. Despite all three facts, the model reasons from the old failure and reverts the fix. Source absence is therefore not a sufficient explanation.

The same pattern reappears at distractor action 30: the context shows initialization **before** `continue`, but the model claims it occurs after `continue`. The next direct probe succeeds and the reserved action 31 then validates the whole candidate. In renamed, action 29 similarly sees current ledger and explicit failing output yet falsely claims all cases pass. Both trajectories demonstrate that retaining fresh source and labeling historical observations does not ensure that the model reconciles contradictions.

The mandatory `PREVIOUS_APPLIED_DELTA` also includes historical `assistant_content`, not just the observed delta. This can repeat mistaken explanations prominently. Distractor's source focus stays on `service.py` for 23 prompts; its irrelevant `legacy.py` is read once and never edited. Renamed spends nine source-observation prompts on generated debug files and five on tests. This contrast supports investigating implementation-source selection, but distractor's wrong action 23 occurs with the right source present, so source selection alone cannot explain or cure the problem.

There is no misleading host pass. Distractor validations 1 and 2 fail and validation 3 passes. At both final calls the host checks the relevant candidate; early final 27 is rejected. On the other hand, its `current_inputs` hash changes after direct Python probes without an intervening source edit, and validation 2 is novel although action 23 restores byte-identical action-8 source. Auxiliary/generated-file churn is a plausible explanation for broader snapshot novelty, but the passing run does not retain intermediate cache snapshots, so that cause is **unconfirmed**. Bytewise snapshot novelty should not be interpreted as a behavioral improvement.

Both runs first try `python test_*.py`, which exits zero while running no unittest suite. Distractor then runs `python -m unittest` and sees the real failure. Arbitrary command success is distinct from host validation. Incidental stdout is especially distracting in renamed because its generated `*_test.py` probes are imported by broad validation; distractor uses inline probes and creates no debug files.

## Reserve and capacity behavior

Both runs reach all 32 actions without pinned-context failure. Complete-exchange omissions begin at action 26. Latest complete failure summaries remain available through the final repair period; the reserved validation and final actions are delivered under the original context/input/output limits. Distractor makes its last edit at action 29, probes at 30, passes the forced validation at 31 and completes at 32. Renamed uses action 30 for a rejected no-op and fails the corresponding final check.

Thus the capacity/reserve mechanism demonstrably keeps the final check available and correctly distinguishes pass from fail. This comparison does not show that the reserve caused a correct implementation: distractor had already produced one at action 21, discarded it, and recovered it later. More output tokens alone do not explain the difference; both retain over 22,000 generated-token allowance.

## Actionable, task-independent hypotheses

1. Make **current candidate unvalidated** explicit when current and latest validation inputs differ. Keep the prior diagnostic, but require reasoning to reconcile a new passing diagnostic with that older failed candidate before reverting an implementation. Test that the mechanism leads to whole-candidate validation rather than premature acceptance.
2. Keep observed path/diff/hash separate from model reasoning in mandatory edit memory. Prefer implementation source implicated by the failure, while recognizing that correct source presence alone is insufficient.
3. Preserve concrete failing iteration/parameter values where a framework can obtain them. Require a claimed failing input to agree with an observed result before reusing it as the explanation. No task-specific values or oracle patch should be embedded in the runtime.
4. Distinguish unchanged/reverted executable behavior from broader input snapshot novelty. Supported AST equivalence can identify comment-only edits; it will not catch every ineffective edit (removing `if True` changes AST but not these outcomes). Retain complete validation as the success authority.

A useful next experiment would measure whether the model validates a newly corrected candidate before reverting it, and whether the next behavioral edit addresses the observed failing input. Keep the same original budgets and require independent full-suite success; fewer repeated words or different tools are not success evidence.

Full distractor evidence: [outcome and archive identity](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/outcome.json), [complete journal](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/session/events.jsonl), [action 23 context](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/session/context/0023.json), [action 30 context](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/session/context/0030.json), [passed validation 3](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/session/validation/0003.json), [terminal source](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/terminal-workspace/service.py), [independent verifier](../runs/development-G1-loop-pilot-generalize_retractions_distractor-s42-r001/harness/generalize_retractions_distractor-loop-repair-r001/verification.stderr).
