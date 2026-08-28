# Implementation status against the design

This repository implements a development preview of the first local-agent
milestones. The complete design is a multi-release plan, including experiments
whose benefits must be measured. An implementation item is not a claim of model
accuracy, security isolation, or superiority over another harness.

## Implemented

- C17 library/CLI with isolated llama.cpp types and explicit ownership.
- Direct GGUF inference, model chat templates, tokenizer, streaming completion.
- CUDA source-build option and matching Windows prebuilt-DLL option.
- Agent state transitions, policy callbacks, cancellation and hard limits.
- Generated GBNF tools plus strict post-generation JSON schema validation.
- Read/search/contextual patch/command/Git/Go symbol/reference/output tools.
- Native subprocess capture, separate streams, deadlines and descendant cleanup.
- Context segments, pinned task/system/latest evidence, utility/token budgeting.
- Stable prompt ordering, real token-prefix KV reuse and cache metrics.
- Conservative fallback for recurrent/hybrid cache behavior.
- SQLite file/symbol/reference/import index and Go Tree-sitter parsing.
- Changed-file reparsing, deletion handling and repository generations.
- Go JSON diagnostic compaction and bounded generic output summaries.
- Bounded working-state memory and repeated-action loop detection.
- Source-context invalidation after known edits.
- Session artifacts, metrics, context inspection and read-only replay.
- Isolated Go benchmark runner, ten fixtures, independent verification/ablations.
- Cross-platform core CI, sanitizers, and direct-backend compilation.

## Partial: do not overstate these

| Design area | Present | Missing |
| --- | --- | --- |
| Context DAG | Segment parent/source dependencies | General graph optimizer |
| Repository graph | Go symbols, occurrences, imports | Type resolution, call/reverse-dependency graph |
| Incremental indexing | Hash-based changed-file reparsing | OS watcher, saved incremental AST edits |
| Working memory | Memory action, bounded outcome state | Structured facts/hypotheses, summary dependency cache |
| Validation scheduler | Suggested affected Go package; benchmark verifier | Automated staged/reverse-dependent tests |
| Context checkpoints | Persistent active sequential KV prefix | Multiple snapshots, disk KV resume |
| Observability | Tokens, reuse, durations, bytes, plans | Integrated peak RSS/VRAM collection |
| Configuration | CLI flags and reference profile | TOML loader and hardware planner |
| Library ABI | Opaque types and ownership rules | Stable ABI guarantee/install package |
| Benchmark release | Runnable fixtures and local measurements | 25–50 diverse tasks, robust external comparisons |

## Remaining optimization/research milestones

1. Equal-model/equal-hardware comparisons with established harnesses.
2. Broader Go repository graph and deterministic validation scheduling.
3. OS change watcher and dependency-aware cached summaries.
4. Multiple physical checkpoints and persisted session resume.
5. N-gram/draft-model speculation, then source-aware seeding and ablations.
6. Config loading and measured hardware memory planning.
7. Additional language AST/diagnostic adapters with correctness tests.
8. Strict OS isolation, resource quotas, race-resistant filesystem handles.
9. Stable packaged `libforge` ABI, allocator hooks and richer backpressure.

## Deliberately deferred, as in the plan

MCP, multiple agents, cloud providers, web browsing, editor extensions, a TUI,
embeddings, remote execution, plugins, automatic model downloads, and a custom
inference engine/tokenizer.

The design's v0.1 performance gate remains a real measured reduction in prompt
processing against an established local harness using the same GGUF/hardware.
Its v1.0 gate additionally requires broad platform/language support and published
task-success/timing evidence. Neither follows from the development version alone.
