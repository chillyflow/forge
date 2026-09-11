# Implementation status against the design

This repository is an incomplete implementation of the [full design](DESIGN_CHECKLIST.md).
Implemented behavior is separate from model accuracy, platform runtime evidence,
security isolation and performance comparisons. Required phases remain in scope
until implemented and verified.

The benchmark-driven sequence for closing the current OpenCode accuracy gap is
tracked separately in the [OpenCode reliability campaign](plans/beat-opencode-reliability.md).

## Implemented

- C17 library/CLI with isolated llama.cpp types and explicit ownership.
- Direct GGUF inference, model chat templates, tokenizer, streaming completion.
- CUDA source-build option and matching Windows prebuilt-DLL option.
- Agent state transitions, policy callbacks, cancellation and hard limits.
- Generated GBNF tools plus strict post-generation JSON schema validation.
- Exact greedy grammar fast path with a full-mask fallback and ablation flag.
- Read/search/contextual patch/command/Git/Go symbol/reference/output tools.
- Native subprocess capture, exact binary streams, deadlines and descendant cleanup.
- Context DAG dependencies, immutable/cacheable flags, shared closure budgeting,
  transitive invalidation and validated logical export/import.
- Stable prompt ordering, real token-prefix KV reuse and cache metrics.
- Independent, bounded in-memory physical prefix checkpoint handles.
- Conservative fallback for recurrent/hybrid cache behavior.
- SQLite file/symbol/reference/import index and Go Tree-sitter parsing.
- Transactional path-delta indexing, retained Go trees with incremental edits,
  source/AST/declaration/symbol hashes and bounded cache counters.
- Native filesystem watching with explicit loss/reopen recovery, snapshot
  fallback and rejection of responses generated from observed stale inputs.
- Normalized Go/compiler/Cargo/Rust/pytest diagnostic adapters, exact raw
  streams and bounded views with explicit missing/ambiguous information.
- Scoped arenas, allocator hooks, binary slices and owned/mapped file views;
  generation JSON and file-reading runtime integration.
- Typed model memory separated from host evidence, token-aware compaction,
  canonical action/diagnostic loop detection and no-op patch conflicts.
- Go package import/reverse-import planning plus automatic Go/Python six-stage validation.
- Bounded input snapshots and fail-closed validation evidence recording.
- Transactional TOML profiles/configuration and metadata-only hardware planning.
- Source-context invalidation after known edits.
- Session artifacts, metrics, context inspection and read-only replay.
- Isolated Go/Python benchmark runners, 29 development and 12 new holdout
  synthetic fixtures, independent verification, repeated Forge/OpenCode/Aider
  measurements, clean freeze enforcement, evidence audits and mechanism ablations.
- Native system/user/assistant/tool prompt protocol, action-budget guidance,
  guarded line hunks and explicit flattened-protocol compatibility.
- Opt-in minimal-loop candidate validation with net workspace snapshots,
  persistent repair episodes and reserved validation/final actions; measured as
  a development experiment, with default promotion still unaccepted.
- Opt-in sequential best-of-N with shared total budgets, independent content
  copies, journaled real-workspace selection and guarded restoration.
- Conservative canonical failed-state detection and one bounded diagnostic
  action per failed-validation episode, with validation/final actions reserved.
- Preliminary Go declaration-impact and test-name targeting with broad final
  verification; Python lexical evidence retains conservative fallback.
- Persistent in-process native conversations and user questions through CLI and
  library callbacks. Questions currently require one candidate.
- Cross-platform core CI, sanitizers, and direct-backend compilation.

## Partial: do not overstate these

| Design area | Present | Missing |
| --- | --- | --- |
| Context DAG | General dependencies, shared closure, flags, snapshots | Richer relevance/profiler and semantic candidates |
| Repository graph | Go declarations, occurrences, package import/reverse graph and syntactic impact/test candidates | Resolved calls/types and sound symbol/test coverage mapping |
| Incremental indexing | Native watch/delta updates, retained Go trees, transactional edits, syntax hashes and declaration impact | Additional AST languages, resolved semantic impact, large-repository performance evidence |
| Working memory | Typed claims, host outcomes, validation and compaction | Semantic summary dependency cache and resume |
| Validation scheduler | Six-stage Go/Python verification and opt-in preliminary Go impact/test-name targeting | Sound coverage mapping and languages beyond Go/Python |
| Candidate completion | Opt-in validation/completion checkpoints, conservative semantic loops, bounded failure reflection and real-workspace best-of-N selection | Measured promotion, preservation and fresh-holdout gates |
| Interactive sessions | Bounded actual conversation history, user questions, one loaded model across tasks | Disk resume and shared clarifications across multiple candidates |
| Context checkpoints | Active sequential reuse plus independent same-instance host snapshots | Automatic semantic checkpoint policy, aggregate eviction, disk KV resume |
| Diagnostics | Named bounded adapters with normalized evidence and raw streams | Additional formats and language validation schedulers |
| Memory | Arena/slice/file-view APIs, action JSON and read-file callers | Broader lifetime migration and measured application memory savings |
| Observability | Tokens, reuse, durations, bytes, plans, arena/index/watch counters | Full event/profile reporting and integrated peak RSS/VRAM collection |
| Configuration | TOML profiles/CLI precedence and hardware estimates | Additional models, KV/draft planning and measured fit coverage |
| Library ABI | Opaque types and ownership rules | Stable ABI guarantee/install package |
| Benchmark release | Development matrix plus a clean-frozen 12-task holdout, three repetitions across Forge/OpenCode/Aider, retained timing/token/failure evidence and explicit rejection of invalid measurements | Valid fresh comparative holdout, remaining reliability gates and larger repository tasks |

## Required remaining work

1. Improve the implemented opt-in loop interventions before promotion: the
   [seven-arm diagnostic](../benchmark/results/2026-09-09-agent-loop-v1/README.md)
   completed all 42 runs, and no intervention beat minimal. Implementation and
   model accuracy remain separate; see [agent loop options and limits](AGENT_LOOP.md).
   Follow the [agent-loop repair plan](plans/agent-loop-all-green.md) for context,
   recovery and candidate-allocation fixes, with explicit all-pass development
   and preservation gates on one frozen implementation.
   Close the retraction and
   rolling-window failures, pass preservation gates, then evaluate an untouched
   clean-frozen holdout; extend comparisons to larger repository tasks.
2. Resolved repository relationships, structural diff impact and progressive retrieval.
3. Dependency-aware cached summaries and larger-repository watcher measurements.
4. Automatic semantic checkpoint selection/eviction and persisted session resume.
5. N-gram/draft-model speculation, then source-aware seeding and ablations.
6. Complete profiles, measured hardware planning and context/memory observability.
7. Additional language AST/diagnostic adapters with correctness tests.
8. Strict OS isolation, resource quotas, race-resistant filesystem handles.
9. Broader scoped-memory adoption, asynchronous processes, full event replay,
   stable packaged `libforge` ABI and richer backpressure.
10. Extend native tool-protocol and decoding-mode comparisons to additional model
    classes and complete the remaining required ablations.

## Deliberately deferred, as in the plan

MCP, multiple agents, cloud providers, web browsing, editor extensions, a TUI,
voice, runtime GitHub integration, remote execution, plugins, automatic model
downloads, a custom inference engine/tokenizer and advanced Windows sandboxing.
Embeddings are conditional on measuring a need after graph/FTS retrieval.

The design's v0.1 performance gate remains a real measured reduction in prompt
processing against an established local harness using the same GGUF/hardware.
Its v1.0 gate additionally requires broad platform/language support and published
task-success/timing evidence. Neither follows from the development version alone.

The [remaining-loop diagnostic](../benchmark/results/2026-09-09-agent-loop-v1/README.md)
used one frozen runtime across seven arms and six examined Go/Python fixtures,
one repetition each. Minimal passed 4/6; semantic, reflection and combined passed
3/6; candidate checkpoints, best-of-two and impact passed 2/6. Median cold time
was 66.117 s for minimal and 137.453 s for combined. All protected inputs and
shared budgets passed audit. No failed root or discarded child had a passing
terminal workspace. Semantic warnings and bounded reflection activated, but every
impact plan fell back to broad checks, so test-targeting latency gains remain
unmeasured. The full Windows GPU suite passed 31 tests with one opt-in skip.
These mechanisms remain opt-in; persistent conversations have separate contract
tests, and questions currently require one candidate. This diagnostic provides
no superiority, preservation, fresh-holdout or release-gate claim. Its temperature
and budgets differ from the earlier checkpoint experiment below.

The [candidate validation and completion diagnostic](../benchmark/results/2026-09-08-candidate-checkpoint-v3/README.md)
completed 9/18 runs versus 6/18 for the same-binary minimal control across six
examined Go/Python fixtures. The entire gain came from one distractor fixture.
Median cold time was 27.759 s versus 28.351 s, while aggregate time was higher
at 533.140 s versus 475.279 s. Neither arm lost a test-passing terminal workspace
to missing completion, so that specific benefit was not demonstrated. The
earlier 7/18 tie and its prefix-cache defect remain recorded separately; the
final implementation also failed its add smoke after damaging an earlier
passing edit. The feature remains opt-in, with no superiority, preservation or
fresh-holdout claim. These loop experiments do not close the design release gates.

The earlier [tranche-2 campaign](../benchmark/results/2026-09-02-tranche2-native/README.md)
records Forge 83/87 versus OpenCode 71/87 and Aider 69/87, with a positive
task-cluster interval and lower median latency than OpenCode. The dirty freeze,
development-task reuse, diagnostic-specific guidance, and resumed comparison
leg prevent a fresh-holdout promotion claim.

The [subsequent repair and holdout](../benchmark/results/2026-09-02-tranche2-repair/README.md)
fixed the identified atomic-transfer and quota-completion failures: both passed
3/3 in the final 83/87 development matrix, with regression gates 12/12 and
invariants 60/60. The new clean-frozen holdout recorded Forge 30/36, OpenCode
29/36 and Aider 21/36. Forge failed retractions and rolling windows 0/3 each;
OpenCode passed one retraction repetition. OpenCode also edited a protected
test's diagnostic message, so the frozen reporter rejected the comparison and
produced no confidence interval. All 108 runs and that violation are retained.
Tranche 2 remains unaccepted; a development lead cannot satisfy the fresh gate.

The [normalized comparison](../benchmark/results/2026-08-28-normalized/README.md)
records 10/10 repairs for both Forge and OpenCode, with less evaluated prompt
work for Forge on that one small run. The older 9/10 run and failed formatting
attempt remain published under their original inputs/revisions. Small samples,
different timing boundaries and unfinished requirements keep this a development
preview, not a completed implementation of the entire multi-release design.
