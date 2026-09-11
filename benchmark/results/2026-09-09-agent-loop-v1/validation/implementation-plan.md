# Remaining agent loop implementation

This task coordinates the remaining agent-loop items from the gap assessment.
The design's release gates and deliberately deferred integrations stay separate.
The existing candidate checkpoint and all measured evidence are preserved.

| Work | Owner | Dependencies | Acceptance |
| --- | --- | --- | --- |
| Best-of-N | Coordinator | Safe content rollback, candidate validation | Independent bounded trajectories, validation in the real workspace, deterministic selection, preservation of initial dirty contents, shared limits and full evidence |
| Semantic loops | Semantic-state task | Complete workspace scanner | Conservative canonical input evidence plus host diagnostics detects comment/rephrasing cycles; ambiguity cannot establish equivalence |
| Symbol impact | Impact task | Existing Go index and reverse imports | Changed symbols, caller/test evidence, targeted preliminary checks with explicit unresolved cases and authoritative broad fallback |
| Failure reflection | Coordinator | Failed validation or semantic loop | One bounded diagnostic action per episode, no tool side effects, no successful-path tax, original total limits preserved |
| Interactive partner | Conversation task | Native history and question dispatch | Same loaded model across tasks, real retained exchanges, ask-user callback and CLI, EOF/refusal/cancellation, bounded history |
| Integration and measurement | Coordinator | All above | Full GPU CTest, scripted contracts, real-model diagnostics and individually selectable ablations; no unsupported promotion claim |

Parallel tasks own separate modules. Shared public headers, schemas, agent
dispatch, CMake and final integration belong to the coordinator. Builds and GPU
runs are serialized. New CMake sources trigger the existing Serena compilation
database maintenance script. Implementation does not count as accuracy evidence;
measurements retain failures and distinguish development results from fresh gates.
