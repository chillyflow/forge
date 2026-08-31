# Full design completion checklist

This is a requirement audit of the [authoritative design conversation](https://chatgpt.com/share/6a917dda-943c-83e9-974c-7cfcfd6dc7bf), especially its final numbered sections 1–60. It is not a replacement scope or a declaration that the current preview completes that design.

**Audited baseline:** `6327b6c44f0cde3cc2e62618ea3ac347212240d7`, inspected on 2026-08-28. The saved source used for the audit was `.scratch/design-source.txt`. Published inference measurements describe executable revision `73c200e`, with some earlier ablations from other explicitly recorded revisions. Do not attribute those measurements to unmeasured later edits.

**Audit method:** read the source, implementation, public headers, existing test definitions, CI configuration, documentation, and checked-in benchmark records. This audit did not run builds, tests, models, sandbox probes, or benchmarks, and did not take public external actions. A test definition is coverage evidence, not proof that a particular CI run passed.

The numbered tables below preserve that baseline audit. The verified additions
in the following addendum supersede corresponding baseline findings;
other missing and partial requirements remain open.

## Verified integration addendum — 2026-08-28

This addendum and implementation are introduced together. Verification used
MSVC 19.44, Windows 11, Go 1.27.0 and the pinned native dependencies: all 11 CTest
groups passed in Debug core (16.56 s) and Release with the direct CUDA backend
linked (18.23 s). These suites include real Go subprocess checks and scripted
agent tests; linking CUDA does not prove model inference.

The [CI run at d60e4c2](https://github.com/chillyflow/forge/actions/runs/33179848895)
subsequently passed all five jobs: Ubuntu, Windows and macOS core tests, Linux
ASan/UBSan, and the pinned direct llama backend build/tests. This is not Linux
NVIDIA or macOS Metal model-inference evidence.

[Recorded Windows model runs](../benchmark/results/2026-08-28-normalized/README.md)
used the executable built from `742848a` with the verified Qwen3-Coder GGUF:
Forge and OpenCode each passed 10/10 tasks with unchanged tests and identical
prepared file hashes. Forge evaluated 31,739 prompt tokens against 73,136.
Timing boundaries differ; the ten synthetic tasks are not the required broad
suite, multiple model classes, or two established harnesses. The initial
unnormalized formatting failure is retained separately. Normalization changes
the input files, so these results do not supersede the historical suite.

| Source areas | Implemented addition | Evidence / still open |
| --- | --- | --- |
| §13, §21, §24 | Multiple context dependencies, exact shared closure, immutable/cacheable flags, transitive staleness, stable memory placement and logical export/import. | [Context API/tests](CONTEXT.md); physical checkpoints and richer relevance remain open. |
| §25–26 | Typed facts/hypotheses/decisions/files/remaining, immutable goal, separate host outcomes/validation, overflow accounting and token-aware compaction. | [State contract](STATE.md), state tests and 4k/8k agent compaction. Summary caching/resume remains open. |
| §27–30 | Canonical action/diagnostic loop detection; no-op patch rejection; staged Go checks gate final answers. | [Planner/executor](VALIDATION.md), native verification and real Go repair tests. Symbol impact/type resolution remains open. |
| §17, §30 | Module/package/import graph with reverse closure, nested modules, deletion handling and conservative fallbacks. | [Graph tests](../tests/unit/test_validation.c); syntactic graph is explicitly not sound type/call resolution. |
| §19–20, §34, §43 | Exact binary streams, UTF-8-safe rendering/paging, no-spawn metadata, root/cwd separation, callback deadlines and distinct edited-file counts. | Core/output/process tests. Broader adapters, asynchronous handles and full metrics remain open. |
| §27, §30, §43 | Input snapshots include unindexed fixtures; changed/incomplete inputs or missing evidence cannot pass. | [Snapshot contract](INPUT_SNAPSHOTS.md), native and real Go mutation tests. This is not OS isolation. |
| §41–42, configuration | Transactional TOML profiles/overlays, CLI precedence, hardware detection, GGUF geometry probe and conservative context/layer estimates. | [Configuration](CONFIG.md), unit and 10 CLI tests. Additional model classes, KV/draft choices and fit measurements remain open. |

That earlier integration did not include physical checkpoint management,
speculative decoding, native watching, summary caching or scoped arena runtime
integration. The next addendum covers subsequent work; the full multi-language
suite and stricter default process sandbox remain unfinished.
Historical benchmark numbers below remain attached to their original revisions.

## Verified runtime addendum — 2026-08-28

Local verification used Windows 11, MSVC 19.44 and Go 1.27.0. All 20 executed
CTest groups passed in both Debug core (28.32 s) and Release linked to the pinned
CUDA backend (30.36 s). The 21st registered test requires a supplied model and
skips in ordinary CTest. New tests use real SQLite/Tree-sitter, native Windows
filesystem notifications, explicit watcher doubles for forced edge cases, and
scripted agent actions; those distinctions are documented in each module.

The optional checkpoint executable was also run separately with the verified
Qwen3-Coder-30B-A3B Q4_K_M on the RTX 5090 Laptop GPU, all 49 layers offloaded,
1,024-token context and greedy decoding. All four short/source A/B cases passed
independent/repeated restores, cold-output byte parity, and foreign-instance
rejection. Prompts were 15 and 287 tokens; restored generation reevaluated one
final token versus full cold prefill. These small cases generated one output
token each. They establish limited checkpoint correctness, not long-generation
coverage, general task accuracy, an overall speedup or Linux/Metal inference.

The [CI run at 5b6d9d0](https://github.com/chillyflow/forge/actions/runs/33184454391)
passed Ubuntu, Windows, sanitizers and direct-backend jobs. macOS passed its
native/unit groups but four scripted CLI fixtures exhausted their terminal
responses after delayed FSEvents notifications; follow-up verification must
cover the bounded fixture retry fix. [Checkpoint records and provenance](../benchmark/results/2026-08-28-checkpoints/README.md)
remain attached to the tested 5b6d9d0 executable.

A subsequent security correction requires PROCESS approval for Git diff/status
because configured clean/process filters can execute code. Finalization no
longer launches Git after validation; automatic `patch.diff` collection is
disabled pending native edit evidence. Implicit index enumeration disables
fsmonitor and lazy fetch. [Security limits](SECURITY.md) remain open; these
changes are not an OS sandbox.

| Design area | Implemented addition | Remaining requirements / limits |
| --- | --- | --- |
| Physical checkpoints, phases 5/16 | [Independent bounded host-state handles](CHECKPOINTS.md), exact prefix/instance/generation checks, cancellation and failure cleanup. | No automatic semantic capture/selection/eviction, disk format, model-reload restore or session resume. Aggregate memory is caller-owned. |
| Incremental repository updates | [Retained Go source/trees](INDEX.md), transactional TSInputEdit parsing, syntax/declaration/symbol hashes, explicit path deltas, cache budgets and interruption. | File-level syntactic indexing, not resolved types/calls, semantic diff impact or exact parser RSS accounting. |
| External changes, phase 30 | [Native Windows/Linux/macOS watching](WATCH.md), explicit loss/reopen handling, post-scan dirty tracking, all-input snapshot fallback, stale-action/final rejection. | Local real-notification evidence is Windows only until this tranche passes platform CI. No atomic input proof or large-repository performance claim. |
| Immediate mutation invalidation | Unindexed edits advance generation; path separators normalize; known mutations and external signals conservatively invalidate bound source views. | Runtime invalidation is broader than the DAG's selective API until portable alias identity is available. Commands with unknown effects also invalidate conservatively. |
| Diagnostics | [Bounded normalized Go/compiler/Cargo/Rust/pytest adapters](DIAGNOSTICS.md), explicit uncertainty, omission metadata, deduplication and exact raw-stream retention. | Supported fixture formats, not every compiler/test format or multi-language validation scheduler. |
| Scoped memory, phases 28/29 | [Arenas, allocator hooks, slices and file views](MEMORY.md), generation JSON arena and owned read-file integration. | Other allocations retain existing lifetimes; mapped data is externally mutable; no whole-process memory-savings claim. |
| Events and accounting | File-change/stale-generation events, index/cache/watch/arena counters and checked state-event writes. | Full profiler, event families, asynchronous APIs and integrated peak RSS/VRAM remain open. |

The complete design remains unfinished. Required summary caches, resolved
repository relationships/retrieval, automatic checkpoint management/resume,
speculation, broad benchmarks, packaging/ABI work and isolation remain in scope.

## Resumed verification — 2026-08-28

The [watcher recovery CI run](https://github.com/chillyflow/forge/actions/runs/33189725295)
passed all five jobs, including 30 consecutive native macOS watcher suites,
all three core platforms, Linux sanitizers, and the direct llama backend build.
The monitor now honors initial continuity loss with at most three creations
before indexing, followed by explicit failure or bounded snapshot fallback.
This remains notification/correctness evidence, not GPU inference evidence.

The resumed implementation adds [native per-edit evidence](EDITS.md): exclusive
before/after content, full-file unified diffs, prepared/applied/aborted outcomes,
bounded reservations, and fail-closed outcome errors. Core/agent-change fixtures
cover normal and failure paths; CLI fixtures independently check emitted patches
with Git. This does not complete aggregate diff collection, structural impact
analysis, arbitrary command-change evidence, or durable crash recovery.

The combined watcher/journal changes passed all 21 executed local test groups
in Debug (35.52 s) and CUDA-linked Release (37.99 s). The model-only checkpoint
test was skipped because no model argument was supplied. These runs are not new
inference or performance benchmarks.

## Summary cache foundation — 2026-08-28

The reviewed summary foundation adds SHA-256 source/declaration metadata,
one shared immutable Go import graph for validation and summaries, and bounded
repository/module/package/file/symbol preparation plus transactional text caching.
[The summary contract](SUMMARIES.md) records exact evidence, identities and limits.
Local Windows checks passed all 22 executed test groups in Debug (34.30 s) and
CUDA-linked Release (36.09 s); the optional model test skipped and no new model
inference was run. These results cover this isolated summary review, before
integration with the resumed watcher/edit-journal tranche. The combined suites
passed all 22 executed groups in Debug (36.60 s) and CUDA-linked Release (39.45 s).
The [combined `c15c11b` CI run](https://github.com/chillyflow/forge/actions/runs/33191334751)
passed all five jobs, including 100 consecutive macOS watcher suites. The
[preceding watcher/journal stress run](https://github.com/chillyflow/forge/actions/runs/33191161222)
also passed all five jobs with 100 macOS repeats. An earlier isolated metadata
exclusion assertion was not reproduced in these runs; its exact cause remains
unconfirmed and failure diagnostics remain enabled. Automatic summary generation,
agent/context selection, staged retrieval and measured benefit are still open.

## Automatic checkpoint manager — 2026-08-28

Source `028082c` adds an opt-in bounded model-owned host cache, exact templated
token-prefix selection, captures during ordinary prefill, deterministic LRU,
scope/generation invalidation, and one safe fallback after failed restore.
The agent nominates its immutable SYSTEM/TOOLS boundary with a fresh context
identity; configuration/CLI flags and generation/session metrics expose the
feature. [The contract](CHECKPOINTS.md) separates aggregate manager allocations
from model/KV memory and explicit handles. Existing APIs/defaults still bypass
automatic capture unless configured and requested; the no-reuse ablation holds.

The combined local suites passed all 23 executed test groups in Debug (37.09 s)
and CUDA-linked Release (38.48 s). The ordinary model test skipped without args.
The [source CI run](https://github.com/chillyflow/forge/actions/runs/33192990295)
passed four complete jobs and all 100 macOS watcher repeats. Its macOS test suite
passed the cache/summary/native groups but failed the configuration permission
fixture after observing one of two expected scripted tool responses. The log
did not retain the relevant event payload, so a delayed stale-action rejection
is a hypothesis, not an established cause. That exact-action fixture now uses
the existing bounded snapshot monitor test executable and asserts both named
policy denials plus unchanged files. Native runtime behavior is unchanged.
The [follow-up `c3586a2` CI run](https://github.com/chillyflow/forge/actions/runs/33194306782)
passed all five jobs, including 100 native macOS watcher suites and 30 macOS
configuration suites. Locally, 30 configuration repeats passed (27.94 s), then
all 23 executed groups passed in Debug (36.39 s) and CUDA-linked Release
(38.85 s). The follow-up changes only test/CI wiring; the model records below
remain attached to the original `028082c` binaries that produced them.

Separate [recorded model checks](../benchmark/results/2026-08-28-automatic-checkpoints/README.md)
on that exact executable passed automatic A/B/A/B cold-output parity, final-token
recomputation, and generation invalidation on Qwen3-Coder with the Windows RTX
5090 Laptop GPU. Both prompts had 150 tokens and generated one token. A read-only
agent smoke also passed with one capture and live-prefix reuse, requiring no
restore. These are limited correctness checks, not an overall speedup, broad
task benchmark, long-generation or additional-platform inference result.

Full semantic-boundary selection, model-reload/disk restore, durable session
resume, automatic summary generation, staged retrieval, resolved language graphs,
speculation, remaining metrics/ABI/isolation work and the broad acceptance suite
remain open. The full design is not complete.

## Staged indexed retrieval — 2026-08-28

Source `608dc7c`, integrated with the published checkpoint test/evidence follow-up
at `4cfd258`, adds exact Go symbol, package-import neighborhood, literal and ranked
FTS5 retrieval in one indexed snapshot. The embedding API, model-free CLI and
READ-capability agent tool share source digest checks, deterministic ordering,
work/result/output limits and complete-JSON token budgeting. The graph remains
syntactic Go imports; it is not resolved call/type impact. [The retrieval
contract](RETRIEVAL.md) describes partial results, unsupported semantics and
source-only behavior.

All 23 executed local groups passed in Debug (37.77 s) and CUDA-linked Release
(43.85 s); the optional model group skipped without arguments. The [source CI
run](https://github.com/chillyflow/forge/actions/runs/33195295216) passed all five
jobs, including 100 native macOS watcher suites, 30 macOS configuration suites,
Linux sanitizers and the pinned backend build. The preceding published `d87d781`
[CI run](https://github.com/chillyflow/forge/actions/runs/33195185940) also passed
all five jobs.

A separate [real-model smoke](../benchmark/results/2026-08-28-retrieval/README.md)
on the recorded `4cfd258` executable returned the correct integer from one
retrieved Go declaration without write/process tool calls or validation commands.
The initial fixture inherited parent Git ignore rules, returned an empty index
and produced the wrong answer; that failed setup is retained. The standalone
repository rerun passed after a model-free index preflight. This is narrow
correctness evidence, not retrieval quality, task success rates or speedup.

Automatic summary generation/context selection, resolved language relationships,
semantic checkpoint hierarchy/resume, speculation, profiling/ABI/isolation work
and the full multi-model/platform acceptance suite remain required. The full
design is not complete. Older audit tables below describe their stated baseline;
these dated implementation addenda supersede only the named portions.

## Generated summaries — 2026-08-28

Source `2184388` adds bounded `forge_repo_summary_generate` and `forge summarize`.
Each call prepares a fresh snapshot; a validated hit invokes no generation, and
a miss permits one model call outside a SQLite transaction. Publication checks
dependencies again and returns a competing valid writer's text when applicable.
Model operation re-entry, UTF-8, bytes, tokens, shared deadline, cancellation and
failure accounting are tested. [The summary contract](SUMMARIES.md) documents
caller-asserted model identity and the remaining agent integration gap.

All 23 executed local groups passed in Debug (40.09 s) and CUDA-linked Release
(39.91 s), with the optional model group skipped. The [source CI run](https://github.com/chillyflow/forge/actions/runs/33197412564)
passed all five jobs including 100 native macOS watcher and 30 config suites.
An observed 15 ms fixture-preparation timeout was subsequently corrected with a
test-only 500 ms writer-lock window. The operation must still reject the blocked
publication; no runtime timeout was relaxed.

Separate [real Qwen evidence](../benchmark/results/2026-08-28-summaries/README.md)
on the hashed `2184388` executable records a correct `37` summary, identical
zero-generation reload/unrelated-edit hits, then a new summary/key describing
`53` after the fixture changes. CLI hits still load the model. An earlier v1
summary's incorrect coverage claim is retained, and v2 identifies supplied
source coverage explicitly. These are narrow checks, not broad semantic quality
or performance evidence. Automatic agent/planner summary selection and the
remaining full-design acceptance requirements are not complete.

## Status and scope rules

| Status | Meaning |
| --- | --- |
| Complete | The bounded requirement is implemented, with identified existing test or recorded-run evidence. It does not imply every related release gate is met. |
| Partial | Some implementation or evidence exists, but specified behavior or required verification is unfinished. |
| Missing | No corresponding implementation or required evidence was found in this baseline. |
| Deferred | The source explicitly postpones or makes the item conditional; absence is not an implementation failure at this stage. |

Keep implementation, runtime correctness, platform support, and performance evidence separate. Compilation does not prove inference, a script backend does not prove model behavior, a feature flag does not prove an ablation result, and an example does not establish a stable API.

The source's module layout and several API signatures, model/profile names, configuration values, and numerical speedups are illustrative. Equivalent organization and names can satisfy their intent; record deviations instead of manufacturing empty files to match a tree. The named behavior, hard invariants, deliverables, platform gates, and benchmark requirements still need to be implemented. Earlier discussion of cloud providers, remote execution, MCP, or multiple agents is superseded for the initial product by the final scope and explicit postponements.

## Sections 1–6: product and foundations

| Source | Requirement | Status | Evidence and remaining work |
| --- | --- | --- | --- |
| §1 | A local model inspects a Git repository, edits code, runs tests, diagnoses failures, and iterates to a working patch. The objective is lower task time and token use. | Partial | [Agent core][agent], [tools][tools], and [real smoke results][results] demonstrate small repairs. Forge solved 9/10 tiny Go tasks; automatic final verification, larger tasks, and the full design remain unfinished. |
| §2 | Measure wall time, input tokens, inference work, and solved tasks relative to hardware; do not substitute startup or raw token throughput for task performance. | Partial | [Benchmark records][numbers] contain task outcomes, tokens, and timing. There is no comparable median task-time result, integrated RAM/VRAM measurement, or tasks-per-hardware evaluation. |
| §3 | Keep CLI, agent, context, tools, and inference separable; provide `libforge` without requiring a terminal. Hide backend types. | Complete for the basic library boundary | [Public headers][api], [CMake][cmake], and [embedding example][embed] expose a native library with opaque model/agent/repository types and callbacks. Stable packaging/ABI and independent inference-context ownership remain v1.0 work. |
| §4 | Organize public API, core, inference, context, repository, tools/adapters, CLI, tests/fixtures, benchmark harness/results, and dependencies. | Partial | The implementation is consolidated into fewer files. The named-module inventory below maps equivalents and substantive omissions; missing checkpoints, speculation, arenas, watchers, and adapters are not merely filename differences. |
| §5 | C17; Linux x86-64 first, Windows x86-64 and macOS arm64; initial optimization on Linux plus NVIDIA CUDA. | Partial | [CMake][cmake] selects C17, [CI][ci] configures three core platforms and a Linux CPU-backend build. [Recorded GPU runs][environment] are Windows-only. No inspected artifact proves a Linux NVIDIA coding run or macOS arm64/Metal inference. |
| §6 | Keep the agent runtime C while allowing disciplined native dependencies; do not claim the entire dependency graph is pure C. | Complete | [Dependency pins][deps] use llama.cpp, SQLite, Tree-sitter/Go, and yyjson; [README][readme] states the C/C++ backend qualification. Optional libgit2/libuv are not required. No custom inference engine/tokenizer is introduced. |

## Phases 0–19

| Source / phase | Requirement | Status | Evidence and remaining work |
| --- | --- | --- | --- |
| §7 / 0 | Establish baselines before optimization: approximately 7B, 14B, and 30B coding-model classes; two established llama.cpp-based harnesses plus Forge; record the full metric inventory. | Partial | [Runner][benchrun], [OpenCode adapter][baseline], and [results][results] cover one 30B-class model, one established harness, and ten Go fixtures. Two other model classes, another harness, complete metrics, and comparable timing remain. Historical ordering cannot be repaired by relabeling later measurements as a preimplementation baseline. |
| §8 / 1 | Direct GGUF inference through a C-facing backend; opaque model/context/generation abstractions; `forge complete` streams tokens and records load, prefill/decode rates, peak RAM and VRAM. | Partial | [Backend][llama] directly loads GGUF, tokenizes/templates prompts, decodes, and streams. [Completion API][inference] and Windows measurements provide real inference evidence. The public model owns one inference context; separate public inference-context/generation objects and peak-memory measurements are absent. Rates can be derived from counters but are not the complete requested report. |
| §9 / 2 | An event/state machine with the nine named agent states, token/message/tool/file/diagnostic/budget/eviction/done events, streaming, cancellation, and a reusable callback API. | Partial | All nine state names and `forge_agent_run`/event callbacks exist in [API][api] and [agent][agent]. Most work remains in one synchronous run function. File-change, diagnostic, budget-warning, and context-eviction events and a resumable step/event-driven dispatcher are missing. See the event inventory below. |
| §10 / 3 | First-class typed tools with schema and execute callbacks; initial read/list/search/symbol/reference/patch/command/Git tools; patches as the editing primitive. | Partial | [Registry][tools] supplies every initial tool name, field validation, generated descriptions, and capability checks. It is a fixed internal table with central string dispatch, not the specified extensible callback-backed tool definition API. `list_directory` is an 80-entry indexed map, and references are syntactic occurrences. Exact contextual patches do satisfy the initial explicit-edit primitive. |
| §11 / 4 | Generate a legal-call GBNF grammar from tool schemas, use the llama grammar sampler, reject malformed actions, and compare JSON with a more compact tool protocol. Avoid initial LLGuidance/Rust dependency. | Partial | [GBNF generation and validation][tools], [sampler][llama], [unit tests][unit], and real runs cover the JSON path. The grammar fast path still constrains accepted tokens. The runtime makes every action—including final/memory—JSON; a compact protocol and tokens-per-call comparison are absent. Grammar-mask optimization is not that protocol comparison. |
| §12 / 5 | Persistent inference sessions with reusable checkpoint identity, token span, parent, repository generation, context hash, state size, validity; distinguish logical context from physical KV state. | Partial | [Logical planner][context] is separate from the [active backend token/KV sequence][llama]. That sequence persists across calls, but there is no `forge_checkpoint_t` equivalent, checkpoint metadata graph, multiple saved states, or restoration interface. |
| §13 / 6 | A semantic context DAG of typed segments with identity, content hash, version, token count, priority, immutable/cacheable properties, and dependency sets. | Partial | [Context API][contextapi] and [planner][context] have stable IDs, hashes, versions, generation, tokens, priority, one parent, and one bound source. There are no general dependency sets, immutable/cacheable flags, distinct symbol/diagnostic segment types, or graph planning/invalidation. A parent chain is not the full DAG. |
| §14 / 7 | Before every inference, reserve output space, rank information per token, preserve essential task/diff/failure/source evidence, pack context, and favor reusable prefixes. | Partial | [Planner][context] selects pinned segments and parent bundles using priority/recency per token, checks the fully rendered prompt with the tokenizer, and [tests][unit] cover overflow/parent retention. It lacks graph-distance relevance, automatic current-diff/failure/active-function candidates, and explicit expected-prefix utility. |
| §15 / 8 | Persistent SQLite `.forge/index.db` for files, symbols, references, imports, calls, diagnostics, commits, chunks, summaries, and indexed navigation/FTS. | Partial | [Repository schema][repo] provides files, symbols, name-based `refs`, imports, FTS5 chunks, and generation metadata. Calls, diagnostics, commits, summaries, file mtime, symbol hashes, and resolved reference identity/kind are missing. FTS5 storage exists, but search uses `instr` rather than ranked full-text queries. |
| §16 / 9 | Incremental AST indexing, Go first, then C/C++, Rust, Python, TypeScript; extract Go packages/imports/types/interfaces/methods/functions/calls/constants/globals; retain file/AST/symbol hashes. | Partial | [Go parser][repo] extracts several declarations, identifier occurrences, and imports; file hashes avoid unchanged-file reparsing, and [integration tests][integration] cover edits/deletions. It discards each AST, reparses changed files from scratch, lacks AST/symbol hashes and package/call modeling, and treats other languages as text. |
| §17 / 10 | File/package/symbol graph with imports, defines, references, calls, implements, and contains edges; return compact callers/callees/test relationships. | Partial | [Repository][repo] has file-to-symbol foreign keys, identifier occurrences, and raw imports. No package graph, resolved call/reference/implementation edges, reverse dependencies, or graph query API exists. |
| §18 / 11 | Progressive inspection: signature/location, body, related imports/types, then full file; expose `inspect_symbol(name, depth)` behavior. | Partial | `forge_repo_inspect` and `find_symbol` in [repository/tools][repo] support depths 0–3. Depth 2 adds 256 surrounding bytes, not related types/imports; depth 3 is capped at 32 KiB. Automatic progressive retrieval and a test for all four semantic levels are missing. |
| §19 / 12 | Preserve raw command output separately, return semantic results, allow `expand_output`; adapters for Go test/vet/golangci-lint, GCC, Clang, cargo test/check, pytest, and generic diagnostics. | Partial | [Diagnostics][diagnostics] recognizes Go JSON events and generic diagnostic/tail text; [tools][tools] records output and provides paging, with [unit coverage][unit] for Go failures and generic output. Named adapters beyond `go test` are absent. Go summaries do not yet provide a normalized failure/location/expected/actual/stack model. Raw-output byte preservation has limits noted below. |
| §20 / 13 | Every result passes classification, structured parsing, deduplication, ranking, token budgeting, and model rendering; preserve duplicate counts, root/user stack frames, and semantic diff hunks. | Partial | [Agent][agent] invokes semantic compression only for `run_command`; other results receive generic limits. [Diagnostics][diagnostics] deduplicates some relevant lines and retains a bounded tail, but does not count duplicate multiplicity, rank structured failures, select stack roots, or summarize diffs by symbols/hunks. |
| §21 / 14 | Increment repository generation on modifications and invalidate only context/summaries whose dependency hashes changed. | Partial | [Repository][repo] increments generation after changed/deleted file scans; [agent/context][agent] invalidate known source context after patches and commands. There is no watcher, full dependency-hash propagation, or summary cache; symbol/search/reference context is broadly invalidated. External changes during inference are not observed immediately. |
| §22 / 15 | Conservative KV correctness: keep only an unchanged sequential token prefix, invalidate the tail, and put stable context first; never splice independently computed file KV blocks. | Complete for conservative active-prefix reuse | [Backend][llama] compares token IDs, removes/recomputes the suffix, reevaluates the last token on exact hits, and clears state on failure; recurrent/hybrid models disable partial reuse. [Published no-KV comparison][results] records avoided prefill. Broader model/correctness tests and multi-checkpoint behavior are separate unfinished work. |
| §23 / 16 | Save/restore physical checkpoints after system/tools, repository context, task, and major planning steps; measure hits, misses, restored/recomputed tokens, restore latency, and avoided prefill. | Missing | [Backend][llama] retains one active sequence only. Neither checkpoint capture/restore APIs nor the required checkpoint metrics exist. An active prefix or a saved prompt file is not a saved physical checkpoint. |
| §24 / 17 | Canonical tool/file/symbol order, whitespace, repository summary/path formatting, and volatile metadata placement to preserve prefixes. | Partial | [Registry][tools] is fixed-order, [repository queries][repo] sort paths/symbols, and [planner][context] places stable roles first. There is no general canonicalization layer or equivalence regression suite for paths, whitespace, summaries, and volatile metadata. |
| §25 / 18 | Compact history into state retaining goal, decisions, touched files, hypotheses, failed approaches, test status, and remaining work; discard duplicate/superseded/noisy material. | Partial | [Agent][agent] accepts a bounded free-form memory string, pins the task/latest evidence, and may replace memory with recent tool-outcome lines when history is evicted. It does not ensure preservation of the named state fields, deduplicate source versions generally, or demonstrate long-session retention. |
| §26 / 19 | Typed working state for goal, facts, hypotheses, relevant files, changes, failures, and remaining tasks, periodically updated through constrained output. | Partial | The baseline [memory action][tools] constrains only a JSON string, not the named fields. [Agent memory][agent] is prose plus a bounded outcome buffer. Structured-state work reported during this audit is not yet credited. |

## Phases 20–39

| Source / phase | Requirement | Status | Evidence and remaining work |
| --- | --- | --- | --- |
| §27 / 20 | Retrieve context, execute tools, return semantic results, replan, and verify changes/tests before finishing. Bound turns, generated/input tokens, command runtime, output, and total wall time. | Partial | [Agent][agent] enforces the named budget types and [tests][integration] cover several failure paths. A model `final` action immediately returns success without deterministic verification. Native `bench` runs a separate explicit verifier, which does not make ordinary `run` verify its work. |
| §28 / 21 | Detect repeated action plus repository-content state plus latest diagnostic; inject `LOOP_DETECTED` and require a different strategy. | Partial | [Agent][agent] hashes response text with repository generation and warns/stops repeats; the [average fixture failure][results] exercises it in a real run. It lacks diagnostic hashes and canonical repository-content identity, so this is not the full specified state signature. In-flight changes need fresh verification. |
| §29 / 22 | Understand changed symbols, broken references, changed imports, and covering tests from patches; automatically derive minimal validation before broadening. | Missing | [Patch code][tools] performs exact text replacement, and `fg_repo_targets` merely derives a `go test` suggestion from the file's directory. It does not analyze structural diffs or symbol impact. |
| §30 / 23 | Deterministic cheap-check → targeted-test → package/reverse-dependent-test → full-suite scheduling; Go formatting and vet stages, independent of model command choices. | Missing | [Tool dispatch][tools] only suggests a package command; [CLI benchmark verification][cli] is one manifest-provided command. No automatic staged, affected-package, or reverse-dependent scheduler exists in the audited baseline. |
| §31 / 24 | Support no speculation, draft-model speculation, and n-gram speculation; benchmark by output mode. | Missing | [Backend][llama] uses ordinary token-by-token decoding. Grammar fast-path selection is not speculative decoding. There are no draft-model/ngram configuration or verification paths, acceptance counters, or mode comparisons. |
| §32 / 25 | Route decoding according to thinking, tool selection, tool arguments, patch, and final-response state; retain tool constraints. | Partial | [Tools][tools] give every action envelope a bounded `thought` string with host-enforced switches: `--no-thought` removes it from grammar and schema, `--thought-required` rejects an absent thought, `--thought-history` re-injects it into later prompts (off by default), and `--thought-decode-only` states that default. `--thought-routed` uses `llama_sampler_init_grammar_lazy_patterns`: output is unconstrained until a `tool`/`memory`/`final` object triggers the action GBNF, then the host bounds, validates and normalizes the prefix. The eight-arm single-binary [routed sweep][routed] measures the result: **neither elicitation mechanism actually elicits** — the optional inline field was used in 0 of 63 actions and routed produced a prefix in 0 of 67, because the lazy trigger fires as soon as the model emits `{"tool":`. `--thought-routed --thought-required` is a guaranteed-failure trap (0/10, every task failing `parse` on its first action) since required routed thought can only reject, never elicit. Only forced inline thought elicits (114/114), and there retention costs 3 of 10 tasks and 2.3x prompt tokens, stable when the turn cap is doubled — which is why retention now defaults off. Elicitation is now implemented in [the backend][llama]: a force-decoded `Thought: ` cue steers greedy decoding into prose (a bare `{` ban was measured tipping it into prompt echo instead — 10/10 benchmark limit deaths), action-opening tokens stay excluded for `FG_THOUGHT_MIN_PREFIX_TOKENS` (32) sampled tokens after the cue (bounded by a quarter of the turn budget), and end-of-generation tokens stay excluded until the action begins, so a routed generation cannot end actionless; the cue is stripped before validation, `--thought-required`, and the census. The [elicited sweep][elicited] re-measures all nine arms on this mechanism: every routed action now carries model reasoning (203/203), routed+required went 0/10 → 8/10, and routed is the best-performing reasoning form (8/10 near baseline prompt cost) — but baselines remain 10/10, and a doubled-turn-cap control shows the remaining routed failures are within-turn token-budget deaths, not turn starvation. Retention of elicited thought is decisively harmful: across five replicates, 15/15 discordant pairs favor stripping (exact sign test p = 6.1e-05, `retention-analysis.json`). The [tier-1 sweep][tier1] added the portability verdict (281/281 routed actions carried a prefix across three template families) and two mechanism defects: routed generation was never stopped at action completion, and Llama-3.1-8B burned the full 2048-token budget at turn 1 on 10 of 20 routed fixtures without opening an action. Phase 2 (this revision) makes the decode states explicit and bounded in [the backend][llama] with pure predicates in `src/inference/routing.c` (`fg_action_begin`, `fg_action_complete`, `fg_think_bounds`, unit-tested in `tests/unit/test_core.c`): the reasoning phase ends at a think budget (`--thought-budget`, default half the turn budget, a chosen unmeasured fraction; `--no-thought-budget` is the unbounded ablation, `thought-routed-unbounded-decode-only` the arm) by swapping the never-triggered lazy grammar for an eager one (tool constraints retained, all three envelope alternatives open, `forced_actions` counted); every grammar arm now ends generation at the token that completes the action object, scanned from the recorded action offset (`action_stops`); the routed prefix over 2048 bytes is truncated at a UTF-8 boundary instead of failing the run; a cue that cannot be force-decoded fails loudly instead of silently arming the bans; and `--thought-cue` replaces the scaffold (empty drops the `{`-ban window with it) for natively-thinking models — CLI/host validation in `src/cli/main.c`, `src/core/config.c`, `src/core/agent.c`, integration cases in `tests/integration/test_cli.py`. Every published sweep predates this mechanism and every grammar arm — baselines included, early stop changes them too — needs re-measuring on a phase-2 binary. Thinking versus action is now an explicit bounded decode-state machine with per-state sampler configuration, but tool selection, tool arguments, patch, and final-response decoding still share one undifferentiated action grammar with no per-state sampler policy, and no measurement yet motivates routing them separately — §32 remains Partial. |
| §33 / 26 | Seed source-aware drafts from active files, nearby code, repository symbols, and previous patches; verify proposed n-gram continuations with the target model. | Missing | No source-token lookup cache, seeded draft generation, target verification, or source-aware speculation tests/benchmarks were found under `src/`. |
| §34 / 27 | Native subprocesses with nonblocking pipes, separate stderr, timeout/cancellation, process groups, cwd, filtered environment; Windows CreateProcess/Job Objects; uniform spawn/poll/cancel interface. | Partial | [Process runtime][process] implements platform spawning, bounded capture, deadlines, cancellation, cwd, and filtering; [integration tests][integration] cover timeout/output/environment/inheritance. It exposes one blocking internal `fg_process`, not asynchronous handles or `forge_process_spawn/poll/cancel`; inference cannot overlap commands through this API. POSIX descendant escape and resource quotas remain limitations. |
| §35 / 28 | Arena ownership for model/session/generation/tool lifetimes, with create/alloc/reset/destroy operations. | Missing | [Utility buffers][util], [context][context], [agent][agent], and [backend][llama] use individual allocations. No arena type, lifetime API, reset behavior, or allocator instrumentation exists. |
| §36 / 29 | Where practical, map source files and use pointer/length slices across source, tool output, prompts, patch hunks, and token buffers. | Missing | [File reads/buffers][util], [repository slices][repo], [context][context], and [tools][tools] generally allocate/copy C strings. A length-taking buffer append or backend weight mapping does not implement the requested source/string-view architecture. |
| §37 / 30 | Observe filesystem changes, rehash/reparse only affected files, update symbols/graph/summaries, and bump generation without repeated whole-repository scans. | Missing | [Index refresh][repo] enumerates and hashes candidates on each scan. No inotify/FSEvents/ReadDirectoryChangesW or other watcher integration, event coalescing, or overflow/recovery handling was found. |
| §38 / 31 | Cache semantic summaries for repository, module/package, file, and important symbol, keyed by dependency content hashes; reuse without new model calls when unchanged. | Missing | `forge_repo_summary` in [repository][repo] returns a sorted file list. It is not a generated semantic summary, and the database has no summaries/dependency cache. |
| §39 / 32 | Retrieve exact symbols, then graph context, lexical matches, full text, and only optionally embeddings. | Partial | [Repository/tools][repo] expose symbol and literal search separately. There is no staged retrieval planner, graph expansion, ranked FTS query, or fallback policy. FTS5-backed storage alone does not satisfy the retrieval hierarchy. |
| §40 / 33 | Add local embeddings only after measuring insufficiency of graph plus FTS; prefer documentation/comments/config/issues if justified. | Deferred / conditional | No embeddings subsystem is present, consistent with the source. The graph/FTS adequacy experiment is still required before choosing to add one; embeddings must not become an invented unconditional completion gate. |
| §41 / 34 | Model profiles define template, tool format, thinking, temperature, stops, context, speculation, and output reserve, without model-specific agent branches. | Partial | [Qwen reference profile][profile] records some settings and [backend][llama] uses metadata/templates. The profile is not loaded; alternate model profiles, tool-format/stop/speculation fields, validation, and profile-driven runtime behavior are missing. The illustrative Qwen/Devstral filenames are not themselves a compatibility requirement. |
| §42 / 35 | Detect GPU/VRAM/RAM/CPU, estimate weights/KV/context/draft/headroom, then choose layers, context, KV format, and speculation automatically. | Missing | Baseline [CLI][cli] takes manual context/layer settings; [Python benchmark metadata][benchrun] reads total GPU memory externally. That is not a runtime hardware planner or measured fit estimation. Concurrent planner work remains unverified here. |
| §43 / 36 | Each session has events, metrics, patch, raw tools, context artifacts, and inference/tool/context events with timing/token transitions. | Partial | [Session][session], [agent][agent], and [tools][tools] create the named artifact structure. Missing details include per-tool durations in events, context add/evict transitions, complete metric inventory, distinct files changed, and a complete patch record for untracked/staged changes. See artifact caveats below. |
| §44 / 37 | `forge replay SESSION` reproduces model input/output, tool input/recorded output, and context transitions without executing tools; permit orchestrator comparison against recorded environments. | Partial | [Replay][session] validates sequence/version and emits saved events; [integration tests][integration] compare events and reject corrupt input. It does not consume saved model-input prompt files or reconstruct context plans/transitions, and no recorded-tool replay backend exists for orchestrator comparisons. It is an audit-log player, not session resume. |
| §45 / 38 | `forge bench task.yaml`-style native task benchmark command with pass/fail, duration/turns/tokens, avoided prefill/KV reuse, raw/visible bytes/compression, test count, and modified files. | Partial | [CLI][cli] supports JSON manifests with prompt and verifier, records a verdict and session metrics; [Python runner][benchrun] materializes isolated fixtures. Literal YAML input, comprehensive human-readable report, test count, reliable distinct-file count, and complete measurements are absent. Record an intentional format-equivalence decision if retaining JSON. |
| §46 / 39 | 25–50 reproducible tasks across bug fixes, API changes, refactors, test repairs, dependency updates, cross-file changes, compiler failures, and exploration; Go/C/Rust/Python/TypeScript repositories. | Partial | [Fixtures][tasks] contain ten tiny Go repair tasks. Other languages, categories, realistic cross-file/repository workloads, and 25–50-task coverage are missing. SWE-bench-style expansion is explicitly later and is not a prerequisite for this smaller suite. |

## Sections 47–60: experiments, releases, and completion

| Source | Requirement | Status | Evidence and remaining work |
| --- | --- | --- | --- |
| §47 | Run full Forge against disabled KV, semantic output, repository graph, context compression, speculation, and constrained tools; report time/tokens/success attributable to each change. | Partial | [Runner][benchrun] exposes `no-kv`, `no-semantic`, `no-compaction`, `grammar-first`, `no-thought`, `thought-optional-decode-only`, `thought-required`, `thought-required-decode-only`, and five `thought-routed*` variants (including the phase-2 `thought-routed-unbounded-decode-only` think-budget A/B), with `--max-turns` now a recorded runner parameter; [results][results] publish KV and limited grammar-mask comparisons; the [thought-channel ablation][thought], the eight-arm single-binary [routed sweep][routed], and the nine-arm [elicited sweep][elicited] publish complete sweeps over all ten fixtures, the latter two with doubled-turn-cap controls and the last with five-replicate retention analysis. Graph/speculation/unconstrained-tool variants are absent. `grammar-first` still constrains output and is not the constrained-tools ablation. |
| §48 | Read/write/process/network/destructive capability policy; default repository reads and compile/tests allowed, edits with diff, outside-root/network/sudo/destructive operations denied. Add named OS mechanisms later. | Partial, with a design deviation | [API][api] has only read/write/process capability categories; [tools][tools] and [security documentation][security] require opt-in writes/processes. Once process permission is granted it is unsandboxed and can access network/outside paths or launch privileged/destructive programs. The stricter read-only default is documented but is not the design's requested safe compile/test-and-edit policy. |
| §49 | Small human-readable configuration for model path/context, automatic GPU/speculation, agent limits, shell network/timeout, and indexed languages. | Missing | Baseline [CLI][cli] accepts flags and [profile][profile] is reference JSON only. No config loader, precedence/error handling, or effective-configuration report exists. TOML is the source's concrete example; a different format needs a deliberate documented decision, not a nonexistent loader. |
| §50 | Interactive bare `forge`, noninteractive `run`, `index`, `inspect`, `bench`, `replay`, and `stats`. | Partial | [CLI][cli] implements command names with qualifications in the command inventory. Bare `forge` prints help; `forge --model ...` reads one task and exits. There is no persistent interactive session/model configuration/resume workflow. |
| §51 | Respect the explicit initial postponements. | Complete as a scope boundary | The full list is reproduced in paraphrased form below. Features merely unfinished elsewhere in the design are not silently added to this list. Creating the project's public GitHub repository is distinct from implementing a GitHub tool/provider in Forge. |
| §52 | Deliver the fifteen ordered milestones from direct inference through comparative release. | Partial | See the milestone map below; a weekend slice or one optimization does not replace this sequence's unfinished deliverables. |
| §53 | Meet every v0.1 platform/function/performance criterion. | Partial — not a release signoff | See the explicit gate table below. Linux/NVIDIA runtime evidence, complete replay/reference behavior, and other required evidence remain open. |
| §54 | Demonstrate equal-model/equal-hardware median task-time and prompt-processing gains, all target platforms/languages, auto hardware setup, durable sessions, and stable library API. | Partial — not a release signoff | [Current measurements][results] are a narrow Windows smoke comparison with different timing boundaries and a repair-accuracy gap. Most v1.0 gates below remain unfinished. |
| §55 | Model context as semantic dependencies, then stable linearization and sequential checkpoint reuse; never arbitrary independent KV concatenation. | Complete invariant; partial architecture | [Backend][llama] respects sequential prefix dependence. General semantic DAG planning and physical checkpoint management are still open phases 6/16. |
| §56 | Investigate stable context linearization balancing useful information and expected reusable prefix against token cost, considering relevance, dependencies, and mutation likelihood. | Partial | [Planner][context] has fixed role order and priority/recency/token scoring. It does not estimate mutation likelihood or optimize expected cache reuse; there is no algorithm evaluation or ablation. The example objective is a research direction, not a proven benefit. |
| §57 | `forge context` reports capacity, role totals, free space, cached prefix, and next-request prefill. | Partial | [CLI][cli] dumps `context/latest.json` with individual segment IDs/kinds/costs/selection/hashes. It lacks the named aggregate display, capacity/free/reserve accounting, and physical prefix/next-prefill estimate. |
| §58 | Justify features by fewer inference calls, fewer input tokens, faster necessary inference, or correctness. | Partial evidence | Prefix reuse, constrained actions, and semantic output follow this principle. [Ablation evidence][results] only covers a subset; the unfinished scheduler/retrieval/speculation paths have no measured contribution yet. |
| §59 | Publish success rate, median task time, prompt/prefill tokens, turns, and peak VRAM for an established harness and Forge under identical model/hardware conditions. | Partial | [Smoke report][results] preserves failures and reports lower tokens: Forge 9/10 versus OpenCode 10/10. It explicitly says wall times have different boundaries. No comparable median time, peak-memory parity, or general performance/capability claim is established. Example percentages and timings in the source are not results or fixed acceptance targets. |
| §60 | Initial prototype: native executable/direct GGUF, four tools, constrained grammar, event log, prompt-processing metric, ten small Go repairs; then KV → Go diagnostics → symbols → planner → speculation. | Partial | The components and ten fixtures exist, and [real results][results] show 9/10 repairs plus KV savings. The ten-task slice is not a substitute for the full design; one task remains failed in the recorded run and downstream milestones are unfinished. |

## Named commands, APIs, and event contracts

| Name from the design | Baseline implementation | Remaining contract |
| --- | --- | --- |
| `forge complete --model MODEL PROMPT` | Direct streamed completion in [CLI/backend][cli]. | Full performance/memory report and broader runtime validation. |
| `forge` | Help without model arguments; one task prompt with `--model` or explicit test `--script`. | Configured interactive agent and continued session behavior. |
| `forge run TASK` | One model-driven run with limits, tools, event artifacts, and opt-in writes/processes. | Deterministic final validation, durable continuation, full security policy. |
| `forge index` | SQLite scan and Go indexing. | Complete graph, multi-language AST support, watcher updates. |
| `forge inspect SYMBOL` / `inspect_symbol(name, depth)` | CLI plus `find_symbol` tool and `forge_repo_inspect`; 0–3 expansion. | Semantically selected nearby imports/types; non-Go symbols. |
| `forge bench task.yaml` / `forge bench` | Requires a JSON manifest and model; Python driver prepares task files. | Decide/document YAML equivalence; full report/suite/default UX. |
| `forge replay SESSION` / `forge replay` | Requires session path; emits recorded events without tools. | Inputs/context-transition playback and replay-backed orchestration; resume is separate. |
| `forge stats` | Requires session path; prints metrics JSON. | Full named counters and readable aggregate report. |
| `forge context` | Requires session path; prints latest segment JSON. | Capacity/free/role/cache/next-prefill profiler. |
| `forge_model_load`, model/context/generation abstractions, `forge_generate` | Opaque model; public `forge_complete`; private backend callbacks. | Public independent inference-context/generation ownership. Existing `forge_context_create` is the prompt planner, not the source's inference-context constructor. |
| `forge_agent_run`, `forge_event_fn` | Public run and event callback; cancellation/policy callbacks. | Step/resume/backpressure contracts and stable versioned library packaging. |
| `forge_tool_definition_t`, schema, execute callback | Private `fg_tool_def` with field strings and fixed dispatch. | Typed extensible public registration/execution contract. |
| `forge_checkpoint_t` | None. | Capture/restore, token spans, parent, generation/hash/bytes/validity. |
| `forge_working_state_t` and goal/fact/hypothesis/file/change/failure/task sets | Free-form memory plus tool-outcome lines. | Typed state with constrained updates and observed evidence, currently in flight. |
| `forge_process_spawn`, `forge_process_poll`, `forge_process_cancel` | Blocking internal `fg_process` with cancellation polling. | Public or reusable asynchronous process handle lifecycle. |
| `forge_arena_create`, `forge_arena_alloc`, `forge_arena_reset`, `forge_arena_destroy` | None. | Scoped arenas, ownership/limits, meaningful tests. |
| `forge_slice_t` | No shared slice API; most APIs use C strings. | Pointer/length views and safe mapped-buffer lifetimes. |

The [public agent enum][api] contains all nine requested states: INIT, PREFILL, GENERATING, TOOL_REQUEST, TOOL_RUNNING, TOOL_RESULT, RECONTEXTUALIZE, DONE, and ERROR. Recording a numeric state transition does not by itself implement asynchronous event dispatch.

| Requested event family | Baseline evidence | Status |
| --- | --- | --- |
| `FORGE_EVENT_TOKEN`, MESSAGE, TOOL_CALL, TOOL_RESULT, DONE | Equivalent versioned string events in [agent/session][agent]; [integration tests][integration] exercise them. | Complete for basic event delivery; no typed public event enum. |
| `FORGE_EVENT_FILE_CHANGED` | Reindex/invalidation happens internally without a dedicated event. | Missing |
| `FORGE_EVENT_DIAGNOSTIC` | Failures are text in tool results. | Missing |
| `FORGE_EVENT_BUDGET_WARNING` | Budget exhaustion errors exist; no warning event contract. | Missing |
| `FORGE_EVENT_CONTEXT_EVICTION` | Latest selection file and aggregate segment count only. | Missing |

The initial tools are all named in the [registry][tools]: `read_file`, `list_directory`, `search_text`, `find_symbol`, `get_references`, `apply_patch`, `run_command`, `git_diff`, and `git_status`. The later `expand_output` tool also exists. Their presence must not obscure the reference-resolution, actual directory-listing, progressive-inspection, semantic-diff, and security limitations in the phase matrix. Default editing uses patches rather than an unrestricted whole-file write tool.

## Named module and artifact inventory

| Design inventory | Existing equivalent or missing behavior |
| --- | --- |
| `CMakeLists.txt`, `README.md`, `LICENSE` | Present. Core C17 builds as `Forge::forge` and the CLI. |
| `include/forge/{forge,agent,context,inference,model,repo,tool,session,event}.h` | Public declarations are consolidated in `forge.h` and `context.h`. Other header filenames are not required for their own sake. Missing generation/checkpoint/tool-registration/process contracts are substantive. |
| `src/core/{agent,session,event,arena,string,error}.c` | Agent/session are present; event handling is in session and strings/errors in `util.c`. Arena behavior is absent. |
| `src/inference/{inference,llama_backend,kv_cache,sampler,grammar,speculative,tokenizer}.c` | Inference/backend are present; active KV, sampler and tokenizer live in the backend; registry grammar is in tools. Multi-checkpoint KV and speculative decoding are absent. |
| `src/context/{context,budget,segment,retrieval,compression,prompt}.c` | Segment/budget/prompt logic is consolidated in `context.c`; agent memory/diagnostics provide limited compression. Full retrieval hierarchy and semantic working-state compression are unfinished. |
| `src/repo/{repo,file_index,symbol,graph,watcher,git,search}.c` | Most existing behavior is consolidated in `repo.c`; process tools call Git. Full graph and watcher implementations are absent. |
| `src/tools/{registry,read_file,search,patch,shell,git_diff,diagnostics}.c` | Consolidated in `tools.c` and `diagnostics.c`; callback extensibility, structural patch analysis and full semantic outputs remain. |
| `src/adapters/{gcc,clang,go_test,cargo,generic}.c` | Go JSON and generic summaries share `diagnostics.c`. No dedicated GCC/Clang/cargo/vet/lint/pytest semantic adapters. |
| `src/cli/{main,command_run,command_chat,command_bench,render}.c` | Consolidated `main.c`; initial command handling exists, persistent chat and requested profiler/benchmark rendering do not. |
| `tests/{unit,integration,fixtures}` | [Unit][unit] and [integration][integration] tests exist; fixtures are built inline or in benchmark manifests. Most future features have no coverage, and scripted tests do not exercise actual decoder state. |
| `benchmark/{harness,tasks,results}` | Drivers are in the benchmark root; task/result directories exist. Required breadth, baselines, full metrics and experiments are unfinished. |
| `vendor/` | Dependencies are fetched with [pinned CMake declarations][deps], not copied into Git. Equivalent dependency management is acceptable. |
| `.forge/index.db` | SQLite index exists; incomplete schema is detailed in phase 8. |
| `profiles/qwen-coder.json`, `profiles/devstral.json`, other profiles | One [Qwen3 reference JSON][profile], not a loader or complete profile system. Model names in the source are examples. |
| `.forge/sessions/<id>/events.jsonl` | Versioned/flushed event log; several required event classes and model-input/context-transition records are absent. |
| `.forge/sessions/<id>/metrics.json` | Present, with incomplete counter inventory below. |
| `.forge/sessions/<id>/patch.diff` | Best-effort final `git diff`; does not include untracked files or all staged changes and can be capture-limited. A run's complete changes are not guaranteed to be represented. |
| `.forge/sessions/<id>/tool/` | Composite raw text plus stdout/stderr files for processes and paged expansion. Capture caps are explicit. `fg_session_artifact` writes `strlen`, so captured embedded-NUL data after the prefix is not preserved; byte-oriented artifact writing is needed for complete raw capture. |
| `.forge/sessions/<id>/context/` | Per-turn prompt text exists, but plan metadata is only `latest.json` and is overwritten. Earlier selections/dependencies/cost transitions are not replayable from this file. |

## Required metric coverage

| Metric or provenance item | Status in audited baseline |
| --- | --- |
| Model identity and quantization | GGUF filename/hash and documented quantization/revision exist for the measured model; no three-class matrix. |
| Context size, GPU layers, sampling/model/backend settings | Available across runner metadata and report; keep one explicit per-run effective configuration as the system expands. |
| GPU/VRAM/RAM/CPU | Total NVIDIA VRAM/name/driver recorded externally; runtime hardware inventory and RAM/CPU/peak memory are incomplete. |
| Task identity and independent success/failure | Present for ten fixtures, with test-file preservation checks in the Python runner. Ordinary `run` success is not task verification. |
| Total duration / median task time | Duration counters exist; Forge and OpenCode timing boundaries differ. Comparable medians are not established. |
| Prompt/generated tokens | Real backend counters exist; script estimates are correctly marked `simulated`. |
| Prefill/decode time and rates | Counters exist; decode includes sampling/event overhead. Derived rates need explicit boundaries and must not be confused with server timing. |
| Agent turns and tool calls | Present. |
| Raw/model-visible tool-result bytes | Present for text representations; caps, embedded NUL, and discarded process bytes limit interpretation. |
| Tool-result tokens | Missing as a dedicated actual-token metric. Segment costs are not a substitute for raw/visible tool-token totals. |
| KV reused and recomputed tokens | Present as `cached_tokens`/`prefill_tokens`, with logical prompt accounting; active-prefix behavior only. |
| Tokens discarded | Missing. `context_evictions` records a segment count from planning, not discarded-token totals. |
| Files opened | Missing. |
| Files modified | Counts successful patch operations, not distinct files or changes made by commands; partial. |
| Test executions | Missing as a dedicated counter; verifier verdicts and Go pass/fail event counts are different quantities. |
| Checkpoint hits/misses, tokens restored, restore latency | Missing. |
| Per-tool duration and context added/evicted-token events | Process duration is computed internally but not exposed in the required event report; context transition metrics are missing. |
| Peak RAM and peak VRAM | Missing integrated collection; model fit/total device memory does not establish peak use. |
| Task success / hardware, comparable median improvement | Missing representative and comparable evaluation. |

## Milestone sequence from §52

| Milestone | Baseline status | Closure evidence needed |
| --- | --- | --- |
| 1. Native inference | Partial | Real token stream exists; finish measurements and target-platform runtime evidence. |
| 2. Minimal agent | Complete for the trivial-repair slice | Existing Windows real-model repairs; do not generalize to every task. |
| 3. Constrained tools | Complete for the JSON/GBNF path | Broaden decoder/registry regression coverage; compact protocol is additional phase-4 work. |
| 4. Benchmark harness | Partial | Broader tasks, provenance, metrics, two external baselines, aligned timing. |
| 5. Context segments | Complete for basic segmentation | Full DAG remains phase-6 work. |
| 6. Persistent prefix reuse | Complete for the measured active-prefix path | Reuse/avoided-prefill records exist; multi-checkpoints and broader model tests remain separate. |
| 7. Go semantic tool results | Complete for initial Go JSON summaries | Normalized diagnostic model and other adapters remain. |
| 8. Go repository index | Partial | Package relationships/resolved references/calls and incremental hashes. |
| 9. Context planner | Partial | Graph-aware retrieval and stable-linearization evaluation. |
| 10. Working-state compression | Partial | Typed retention with long-session correctness evidence. |
| 11. Incremental invalidation | Partial | Dependency propagation, external changes, watcher and summary consistency. |
| 12. Targeted test scheduler | Missing | Automatic staged tests including reverse dependents and failure gating. |
| 13. Upstream speculative techniques | Missing | Draft/ngram integration, correctness checks and per-mode measurements. |
| 14. Source-aware speculation | Missing | Seeded repository draft cache plus measured incremental benefit. |
| 15. Comparative benchmark release | Partial | Required suite/model/baseline breadth, reproducibility, aligned metrics and honest failures. |

## Exact v0.1 gate from §53

**The conjunction of these gates has not been demonstrated.** Version text `0.1.0-dev`, core CI, a Windows GPU run, or fewer tokens on ten fixtures does not replace the whole gate.

| Required gate | Audit finding |
| --- | --- |
| Linux | Partial: configured core CI/backend compilation; no checked-in Linux direct-GGUF coding-run evidence. |
| NVIDIA | Recorded Windows NVIDIA inference exists. Linux plus NVIDIA, the specified initial optimization target, remains unverified. |
| GGUF and direct llama.cpp backend | Implemented with recorded real Windows-model use. |
| Agent loop and streaming | Implemented; script event tests and real repair records cover a limited slice. |
| `read_file`, search, `apply_patch`, `run_command` | Implemented; stronger policy/async contracts remain broader design requirements. |
| Constrained tool calls | Implemented JSON/GBNF path with postvalidation; no claim that invalid actions can never arise under arbitrary failures. |
| Go repository indexing and symbol lookup | Implemented for syntactic Go declarations; full package/graph semantics incomplete. |
| Reference lookup | Partial: name-based identifier occurrences, not symbol-resolved references. |
| Semantic `go test` output | Implemented initial JSON failure-summary path; full phase-12 adapter contract remains partial. |
| Context budget manager | Implemented bounded segment selection; graph/relevance requirements remain partial. |
| Persistent KV prefix reuse | Implemented and measured on the recorded model; not arbitrary KV composition or multi-checkpoint restore. |
| Session metrics | Present but incomplete against the full metric inventory. |
| Replay | Partial: event playback without model-input/context-transition replay. |
| Benchmark suite | Ten Go smoke fixtures exist; full phase-39 suite remains incomplete. |
| Demonstrably fewer prompt tokens than at least one established local harness | Narrow numeric evidence exists: 49,444 logical / 12,865 evaluated Forge prompt tokens versus 451,073 / 74,616 OpenCode tokens on the ten recorded fixtures. Forge solved 9/10 versus 10/10. This meets a limited observed token-reduction claim, not comparable accuracy, general performance, or all v0.1 gates. |

## Exact v1.0 gate from §54

| Required gate | Audit finding |
| --- | --- |
| Reduced median task-completion time and prompt processing against standard llama.cpp harnesses, identical models/hardware | Missing comparable median-time evidence; current report explicitly forbids comparing its wall-time totals directly. Token reduction is only a small-suite observation with a success-rate difference. |
| Linux, macOS, Windows support | Partial: platform code/core CI configuration; real recorded inference only on Windows. Linux CUDA and macOS arm64/Metal runtime workflows need direct verification. |
| Go, C/C++, Rust, Python, JS/TS | Partial: Go syntax indexing; other languages have text search, not complete repository intelligence/adapters/validation. |
| Automatic hardware configuration | Missing in baseline; in-flight work is not a measured or verified completion. |
| Robust session persistence | Partial: logs/prompts/artifacts persist, but there is no resumable logical state, physical checkpoint persistence, compatibility validation, or recovery contract. |
| Stable `libforge` API | Missing stability guarantee: [README][readme] calls it experimental. Headers and a static library exist, but `FORGE_ABI_VERSION` alone does not provide struct-version negotiation, exported dependency packaging, stable ownership/threading/backpressure contracts, or ABI compatibility tests. |

## Explicit postponements and conditional work

Only the source's explicit postponements belong here. Until the performance thesis is established, defer MCP, multiple agents, cloud providers, web browsing, a VS Code/editor extension, a TUI framework, embeddings, remote execution, runtime GitHub integration, voice, plugins, a custom inference engine, a custom tokenizer, sophisticated Windows sandboxing, and automatic model downloads.

Embeddings are additionally conditional on a measured graph/FTS retrieval gap (§40). SWE-bench-style evaluation is later (§46). Future inference backends and embedding into editors/daemons are architectural possibilities, not initial provider/extension deliverables. More adventurous KV composition is explicitly later and must not violate sequential-state correctness.

The source's later namespace/seccomp/Landlock/macOS/container mechanisms are distinct from its immediate default-denial policy. Sophisticated Windows isolation may be deferred; that does not make an unsandboxed `--allow-exec` implementation satisfy default network/outside-root/destructive denial. Speculation, source-aware drafts, arenas, slices, watchers, summaries, configuration, the hardware planner, and broader benchmarks are **unfinished full-design requirements**, not §51 postponements.

## Documentation inconsistencies and cautions

1. [ROADMAP][roadmap] is a useful preview summary, but its shortened v0.1 paragraph emphasizes prompt processing and omits the full explicit Linux/NVIDIA and functional conjunction. Preserve the exact gates above rather than promoting the preview on the strength of one token comparison.
2. [SECURITY][security] honestly describes read-only defaults and unsandboxed opt-in commands. That behavior differs from §48, which allows safe compile/test and diff-producing edits while denying network, outside-root, sudo and destructive effects. This is an unresolved design deviation; a disclaimer is not implementation evidence.
3. [ROADMAP][roadmap] lists active-prefix reuse under partial checkpoints. Active prefix state is useful, but it must not be counted as the semantic capture/restore checkpoint manager required by phases 5/16.
4. Existing summaries of repository graphs, working memory, validation and replay must retain their qualifications: imports/occurrences are not a resolved graph, memory strings are not typed working state, a command suggestion is not scheduling, and event playback is not complete replay or resume.
5. [ROADMAP][roadmap]'s remaining-work list is not exhaustive. Compact tool-protocol comparison, all event families, asynchronous handle APIs, arena/slice paths, normalized adapters, full retrieval hierarchy, the context profiler, three model classes, two existing harnesses and full ablations are also in the design.
6. The current explicit-defer summary omits some source entries, including voice, runtime GitHub integration and advanced Windows sandboxing. Use the complete source list above; do not add substantive unfinished phases to it.
7. [Benchmark documentation][methodology] properly distinguishes logical from evaluated tokens, scripted from real counts, timing boundaries and the accuracy gap. Preserve those distinctions as new results are added; compile-only CI and fabricated/example numbers cannot close performance gates.

## Recommended implementation order

1. **Keep an honest acceptance baseline.** Turn the recorded repair failure and incomplete verification paths into regression cases, finish metric/provenance definitions, and make success depend on explicit observed validation where required. Preserve the old published records rather than overwriting their history.
2. **Resolve policy behavior before wider distribution.** Implement capability/default-denial enforcement with an explicit supported isolation boundary, or refuse operations when the requested boundary cannot be enforced. Keep platform-specific advanced sandbox work distinguishable from baseline policy.
3. **Finish typed state and loop/validation correctness.** Verify the in-flight working-state changes, diagnostic-aware state signatures, state retention after compaction, and an explicit final-verification contract. Do not let model-authored prose manufacture observed test success.
4. **Complete Go repository semantics and targeted scheduling.** Add package/call/reference/reverse-dependency identities and hashes, progressive related context, structural diff impact, and automatic formatting/targeted/package/full validation. Extend existing tests with multi-file, imports, shadowing, changed diagnostics, and command-made edits.
5. **Connect incremental context and persistence.** Implement dependency DAG invalidation, watcher updates, summary caching, full prompt-plan/event replay, and physical semantic checkpoints/resume with compatibility and recovery checks. Preserve conservative sequential KV correctness.
6. **Finish configuration, profiles, hardware planning, and observability.** Verify auto choices against measured weights/KV/draft/headroom; expose full metrics and context profiling. Introduce scoped arenas and slices where they simplify ownership and reduce copying, with explicit lifetimes.
7. **Broaden semantic tools and retrieval, then measure speculation.** Complete named adapters/compression and staged symbol/graph/lexical/FTS retrieval. Integrate upstream draft/ngram methods before agent-mode routing and source-seeded drafts; compare accepted target output and all required ablations.
8. **Close release evidence deliberately.** Expand to 25–50 varied tasks and all named languages, three model classes and two established harnesses; align timing boundaries and measure success, tokens, compute and memory. Run real Linux NVIDIA and macOS/Windows workflows, verify robust persistence and packaged ABI compatibility, and only then evaluate v0.1/v1.0 and full-design completion.

## Updating this checklist

For each status change, record the implementing revision, exact source/test locations, relevant test results, and—for platform/model/performance gates—the actual runtime environment, model/backend hashes, commands, outputs and limitations. A newly added function, a submitted CI job, a flag, or an agent's progress message is not by itself a completed gate. Reconcile in-flight work with this baseline before treating the document as a current signoff.

[readme]: ../README.md
[roadmap]: ROADMAP.md
[security]: SECURITY.md
[api]: ../include/forge/forge.h
[contextapi]: ../include/forge/context.h
[cmake]: ../CMakeLists.txt
[deps]: ../cmake/Dependencies.cmake
[ci]: ../.github/workflows/ci.yml
[embed]: ../examples/embed.c
[agent]: ../src/core/agent.c
[context]: ../src/context/context.c
[session]: ../src/core/session.c
[process]: ../src/core/process.c
[util]: ../src/core/util.c
[repo]: ../src/repo/repo.c
[tools]: ../src/tools/tools.c
[diagnostics]: ../src/tools/diagnostics.c
[llama]: ../src/inference/llama_backend.c
[inference]: ../src/inference/inference.c
[cli]: ../src/cli/main.c
[unit]: ../tests/unit/test_core.c
[integration]: ../tests/integration/test_cli.py
[profile]: ../profiles/qwen3-coder.json
[benchrun]: ../benchmark/run.py
[baseline]: ../benchmark/opencode.py
[methodology]: ../benchmark/README.md
[tasks]: ../benchmark/tasks/
[results]: ../benchmark/results/2026-08-28/README.md
[numbers]: ../benchmark/results/2026-08-28/summary.json
[environment]: ../benchmark/results/2026-08-28/environment.json
[thought]: ../benchmark/results/2026-08-29-thought-ablation/README.md
[routed]: ../benchmark/results/2026-08-30-routed-sweep/README.md
[elicited]: ../benchmark/results/2026-08-30-elicited-sweep/README.md
[tier1]: ../benchmark/results/2026-08-31-tier1-models/README.md
