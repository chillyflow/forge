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
- Isolated Go/Python benchmark runners, 29 synthetic fixtures, independent
  verification, repeated Forge/OpenCode/Aider measurements and mechanism ablations.
- Native system/user/assistant/tool prompt protocol, action-budget guidance,
  guarded line hunks and explicit flattened-protocol compatibility.
- Cross-platform core CI, sanitizers, and direct-backend compilation.

## Partial: do not overstate these

| Design area | Present | Missing |
| --- | --- | --- |
| Context DAG | General dependencies, shared closure, flags, snapshots | Richer relevance/profiler and semantic candidates |
| Repository graph | Go declarations, occurrences and package import/reverse graph | Resolved calls/types, symbol impact and test mapping |
| Incremental indexing | Native watch/delta updates, retained Go trees, transactional edits and syntax hashes | Additional AST languages, semantic change impact, large-repository performance evidence |
| Working memory | Typed claims, host outcomes, validation and compaction | Semantic summary dependency cache and resume |
| Validation scheduler | Six-stage Go verification plus Python compiler syntax and unittest/pytest discovery | Symbol impact and languages beyond Go/Python |
| Context checkpoints | Active sequential reuse plus independent same-instance host snapshots | Automatic semantic checkpoint policy, aggregate eviction, disk KV resume |
| Diagnostics | Named bounded adapters with normalized evidence and raw streams | Additional formats and language validation schedulers |
| Memory | Arena/slice/file-view APIs, action JSON and read-file callers | Broader lifetime migration and measured application memory savings |
| Observability | Tokens, reuse, durations, bytes, plans, arena/index/watch counters | Full event/profile reporting and integrated peak RSS/VRAM collection |
| Configuration | TOML profiles/CLI precedence and hardware estimates | Additional models, KV/draft planning and measured fit coverage |
| Library ABI | Opaque types and ownership rules | Stable ABI guarantee/install package |
| Benchmark release | 29 synthetic Go/Python tasks, three repetitions across Forge/OpenCode/Aider, task-cluster intervals and timing/token/failure evidence | Clean frozen holdout after development, remaining reliability gates and larger repository tasks |

## Required remaining work

1. Close the atomic-transfer and completion-budget failures, then run a new
   holdout from a clean frozen revision; extend comparisons to larger repository tasks.
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

The [tranche-2 campaign](../benchmark/results/2026-09-02-tranche2-native/README.md)
records Forge 83/87 versus OpenCode 71/87 and Aider 69/87, with a positive
task-cluster interval and lower median latency than OpenCode. The dirty freeze,
development-task reuse, diagnostic-specific guidance, and resumed comparison
leg prevent a fresh-holdout promotion claim. Forge's remaining failures are
three atomic-transfer runs and one quota-allocation completion failure.

The [normalized comparison](../benchmark/results/2026-08-28-normalized/README.md)
records 10/10 repairs for both Forge and OpenCode, with less evaluated prompt
work for Forge on that one small run. The older 9/10 run and failed formatting
attempt remain published under their original inputs/revisions. Small samples,
different timing boundaries and unfinished requirements keep this a development
preview, not a completed implementation of the entire multi-release design.
