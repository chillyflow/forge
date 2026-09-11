# Archived history pressure replay

This is a **scripted mechanism replay, not real-model coding success**. Eleven unchanged failed prefixes were each replayed under the append-only control and bounded repair, for 22 scheduled executions. Native tool arguments and assistant prose are preserved; only validation and final are appended, within the original 32-action limit.

The script backend counts `ceil(serialized UTF-8 bytes / 4)` and does not use Qwen's tokenizer or embedded chat template. Different token counts, fixed non-adaptive actions and finite-script exhaustion after rejected finals limit comparison with the original GPU runs. No budgets were increased, fixture repairs supplied, old paths rewritten, or failed outcomes removed.

| Archived failure | Append-only exact prefix | Bounded exact prefix | Bounded reaches next action | Bounded terminal tests |
| --- | ---: | ---: | --- | --- |
| [distractor-candidate](cells/generalize_retractions_distractor-candidate-r001/bounded/result.json) | 23/23 | 23/23 | True | False |
| [distractor-impact](cells/generalize_retractions_distractor-impact-r001/bounded/result.json) | 24/24 | 24/24 | True | False |
| [distractor-reflection](cells/generalize_retractions_distractor-reflection-r001/bounded/result.json) | 6/28 | 6/28 | False | False |
| [original-candidate](cells/generalize_retractions_original-candidate-r001/bounded/result.json) | 28/28 | 28/28 | True | False |
| [original-semantic](cells/generalize_retractions_original-semantic-r001/bounded/result.json) | 26/26 | 26/26 | True | False |
| [paraphrased-candidate](cells/generalize_retractions_paraphrased-candidate-r001/bounded/result.json) | 27/27 | 27/27 | True | False |
| [paraphrased-impact](cells/generalize_retractions_paraphrased-impact-r001/bounded/result.json) | 26/26 | 26/26 | True | False |
| [paraphrased-semantic](cells/generalize_retractions_paraphrased-semantic-r001/bounded/result.json) | 26/26 | 26/26 | True | False |
| [renamed-candidate](cells/generalize_retractions_renamed-candidate-r001/bounded/result.json) | 28/28 | 28/28 | True | False |
| [renamed-impact](cells/generalize_retractions_renamed-impact-r001/bounded/result.json) | 29/29 | 29/29 | True | False |
| [renamed-semantic](cells/generalize_retractions_renamed-semantic-r001/bounded/result.json) | 29/29 | 29/29 | True | False |

[protocol.json](protocol.json) freezes the schedule, original limits, source hashes, scripts and runtime identities before execution. [summary.json](summary.json) retains every outcome. Each cell contains the command, source actions, script, stdout/stderr, metrics/events/context snapshots in the copied terminal workspace, and unchanged-manifest independent verification output.

Scripts and outputs lived outside fresh temporary workspaces, so their files did not alter candidate input snapshots. Fresh fixture hashes matched the original campaign. The two invalid no-shell `>` argv calls in distractor/reflection remained unchanged. Source/runtime hashes and all protected-file hashes were checked after the complete schedule.

Reproduce using a new output directory: `python benchmark/replay_loop_pressure.py --forge build-gpu/Release/forge.exe --output <new-directory>`. The output is immutable: the runner refuses to overwrite an existing replay directory.

## Observed dispatch and replay limits

Bounded repair reaches a native validation dispatch beyond every original prefix (**11/11**); **10/11** also retain every accepted archived ACTION exactly. The append-only control reaches that dispatch in **8/11**. Bounded context omission activates in **7/11** runs. All **22/22** independent terminal verifications fail. No new repair was supplied.

The strict prefix column above intentionally reports false for distractor/reflection: its original completed reflection is supplied verbatim but rejected under the script backend's 256-token JSON-count allowance. Bounded recovery consumes that attempt and continues to the next actions; the original GPU run accepted it under Qwen tokenization. This is not an exact tokenization replay.

For renamed/impact and renamed/semantic, the 29-action prefix is followed by validation at action 30 and final at 31. The existing host reserve correctly rejects final at 31 because that action is reserved for validation. This finite replay tail does not test final-at 32 capacity. These outcomes remain in the denominator and are classified separately from context limits. Other failed appended finals may end in finite-script exhaustion.

[analysis.json](analysis.json) records both the raw dispatch and strict-prefix counts, all tail conflicts, and a strict existence/hash check of every preregistered source input. [runner.py](runner.py) is an exact retained copy of the executed runner identified in the protocol.
