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

## Completion and evidence

The coordinated implementation and integration are complete. All five workstreams
are available through the [agent loop options](../AGENT_LOOP.md), alongside the
existing candidate validation/completion checkpoint. Structural impact remains
conservative syntactic evidence; resolved relationships and coverage mapping are
still repository-intelligence work. Interactive questions require one candidate
until clarifications can be shared across independent trajectories.

The accepted Windows GPU build passed 31 CTest checks with one opt-in model test
skipped. A separate native-template probe passed on the local Qwen3-Coder model.
Review corrections and all build/test logs are retained in the
[verification record](../../benchmark/results/2026-09-09-agent-loop-v1/validation/README.md).

The frozen seven-arm diagnostic completed all 42 scheduled runs. Minimal passed
4/6; semantic, reflection and combined passed 3/6; candidate, best-of-two and
impact passed 2/6. No failed root or discarded child passed secondary terminal
verification. All impact plans fell back, so this pilot did not measure targeted
validation savings. The mechanisms remain opt-in: implementation acceptance does
not establish an accuracy improvement. See the
[complete results and evidence](../../benchmark/results/2026-09-09-agent-loop-v1/README.md).

The original six fixtures are examined development inputs, including four variants
of one Python problem. No default promotion, preservation or fresh-holdout gate
has been accepted. Release requirements remain tracked in [ROADMAP.md](../ROADMAP.md).
