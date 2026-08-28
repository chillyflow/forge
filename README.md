# Forge

**A native local coding agent built to spend fewer tokens on getting work done.**

[![Native CI](https://github.com/chillyflow/forge/actions/workflows/ci.yml/badge.svg)](https://github.com/chillyflow/forge/actions/workflows/ci.yml)

Forge's agent runtime is C17. It links directly to llama.cpp, maintains reusable
sequential KV prefixes, selects context under a token budget, and executes
grammar-constrained tools. Go source navigation uses Tree-sitter and SQLite.

**Development preview, not a production sandbox or a proven performance win.**
The [full design checklist](docs/DESIGN_CHECKLIST.md) and [roadmap](docs/ROADMAP.md)
distinguish implemented behavior from unfinished design requirements.
No model weights are bundled or downloaded by the
runtime. llama.cpp and GPU dependencies include C++; Forge does not claim its
entire dependency graph is C.

The tested local model is [Qwen3-Coder-30B-A3B Q4_K_M](docs/MODEL.md), with verified
download provenance and GPU settings documented separately.

## Build

CMake 3.24+, a C17/C++17 compiler, Git, and Python 3.10+ for tests. Go and gofmt
are needed for Go validation; CI exercises real checks with Go 1.27.0. The first build
fetches pinned native dependencies. Subsequent builds can run offline.

```sh
# CPU inference (Linux/macOS)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# NVIDIA: requires a locally installed CUDA toolkit
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON
cmake --build build-cuda --parallel

# Development/tests without llama.cpp (--model fails explicitly)
cmake -S . -B build-core -DFORGE_WITH_LLAMA=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

On Windows, use Visual Studio 2022 and add `--config Release` to build and
`-C Release` to CTest. Executables are in `build/Release`. A matching prebuilt CUDA
backend is supported; see [the build guide](docs/BUILD.md).

## Use

```sh
forge complete "Write a Go binary search function" --model /models/coder.gguf
forge run "Fix the failing storage tests" \
  --workspace ./my-repository --model /models/coder.gguf \
  --gpu-layers -1 --allow-write --allow-exec

forge index --workspace ./my-repository
forge index pkg/storage/store.go --workspace ./my-repository
forge index-info pkg/storage/store.go --workspace ./my-repository
forge watch --workspace ./my-repository --wall-ms 60000 --json
forge inspect DeleteRecord --depth 1 --workspace ./my-repository
forge references DeleteRecord --workspace ./my-repository
forge search "context.WithCancel" --workspace ./my-repository
forge retrieve DeleteRecord --depth 1 --workspace ./my-repository
forge validation-plan pkg/storage/store.go --workspace ./my-repository
forge validate pkg/storage/store.go --workspace ./my-repository --allow-exec
forge hardware-plan --model /models/coder.gguf --json

forge stats /path/to/repo/.forge/sessions/SESSION
forge context /path/to/repo/.forge/sessions/SESSION
forge replay /path/to/repo/.forge/sessions/SESSION --json
```

No arguments prints help. `forge --model MODEL` opens a simple task prompt.
`--json` emits versioned JSON-lines events. `forge complete` streams generated
text. `forge run` records model outputs, tool calls, context plans, and metrics.
Ctrl+C requests cancellation; GPU kernels are not preempted mid-dispatch.

**Read-only by default.** `--allow-write` permits contextual patches.
`--allow-exec` permits **unsandboxed executable code with your user privileges**,
including network access. Use a disposable checkout/container for untrusted
repositories. Read the [security model](docs/SECURITY.md) before enabling execution.

Go workspaces with edits or launched commands undergo formatting checks, compilation, affected
and reverse-dependent tests, vet, and broad verification before an agent's final
answer is accepted. Failures return to the agent for repair; denied execution
does not become a successful verification. This checks the active Go environment,
not every build tag/platform or the correctness of arbitrary task claims.
Validation also compares bounded workspace input snapshots, including unindexed
test fixtures, before accepting success. See [the validation contract](docs/VALIDATION.md).
`--no-auto-validation` is an explicit ablation.

Repository indexing retains bounded Go syntax trees and applies incremental
Tree-sitter edits after source changes. `index PATH` updates only named inputs
after an initial full index; `index-info PATH` exposes committed syntax hashes.
`watch` continuously updates the index using native filesystem notifications.
Agent runs also watch for external edits and reject responses generated from
observed stale inputs. When native coverage is unavailable, agent runs fall
back to bounded workspace snapshots; the explicit `watch` command fails.
See [indexing](docs/INDEX.md) and [watching](docs/WATCH.md) for limits.

### Configuration

Forge loads an optional workspace `forge.toml`. `--profile FILE` supplies a base
profile, `--config FILE` chooses the project configuration, and CLI values win.
`--no-config` disables automatic discovery. See [configuration](docs/CONFIG.md)
and [the example](forge.toml.example). Files never grant tool permissions.
`network=false` refuses process execution because this runtime has no network
sandbox. `--gpu-layers auto` uses measured hardware and bounded GGUF estimates;
numeric layer settings remain explicit overrides.

### Tools

| Tool | Behavior |
| --- | --- |
| `read_file` | Bounded line ranges with line numbers |
| `list_directory` | Sorted indexed file map |
| `search_text` | Literal source search, bounded results |
| `retrieve_context` | Exact symbol → package graph → literal → FTS5, with snapshot provenance and budgets |
| `find_symbol` | Go declaration/signature/body expansion |
| `get_references` | Go identifier occurrences, not type-resolved references |
| `apply_patch` | One exact, unique replacement; atomic file replacement |
| `run_command` | Argument-vector execution, separate stdout/stderr, deadlines |
| `git_diff`, `git_status` | Git inspection requiring process permission; configured filters can execute |
| `expand_output` | Retrieve a page of recorded raw output |

The registry generates a GBNF grammar. Schema validation runs again before
dispatch. Malformed or unknown actions never execute.

### Sessions

Each run writes `.forge/sessions/<random-id>/` containing `events.jsonl`,
`metrics.json`, actual prompts in `context/`, and
raw tool results in `tool/`. Structured `working_state.json` separates model notes
from observed edits and validation. `validation/` contains stage plans, reports,
and exact captured stream bytes; `context/` includes complete logical snapshots.
Replay reads events only and **never executes
recorded tools**. It is an audit replay, not inference replay or session resume.
Git is not executed during finalization: a configured clean/process filter could
mutate files after validation. Request `git_diff` explicitly for a recorded tool
result. The [native edit journal](docs/EDITS.md) records exact per-edit content,
diffs, and outcomes without Git; automatic aggregate `patch.diff` remains pending.
Sessions contain source and command output: keep them private unless reviewed.

`--script actions.json` is an explicit deterministic **test backend**. Its metrics
are marked `simulated: true`; token counts are estimates, not inference data.
It is never substituted when a real model is missing or fails to load.

## Benchmarks

Ten small Go repair fixtures are included. The Python driver is a development
tool; neither the CLI nor library needs Python at runtime.

```sh
python benchmark/run.py --forge build/forge --model /models/coder.gguf \
  --gpu-layers -1 --variants optimized no-kv --output /tmp/forge-results
```

The driver creates a fresh checkout for every task/variant, verifies tests
independently, rejects test modifications, and records model SHA-256 and hardware.
See [benchmark methodology](benchmark/README.md).

[Latest local smoke results](benchmark/results/2026-08-28-normalized/README.md):
Forge and OpenCode each solved **10/10** tiny Go tasks with identical prepared
file hashes, GGUF and GPU. Forge evaluated 31,739 prompt tokens versus 73,136;
the wall-time boundaries differ, so this is not an overall speed claim.
The [earlier 9/10 run and ablations](benchmark/results/2026-08-28/README.md)
remain available. Fixture normalization changed the starting files; do not
combine the two suites or attribute the difference solely to runtime changes.

## Embed

```cmake
set(FORGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/forge)
target_link_libraries(your_program PRIVATE Forge::forge)
```

The public API has no llama.cpp, SQLite, JSON, or Tree-sitter types. See
[`examples/embed.c`](examples/embed.c). Model ownership, one-run agent lifetimes,
policy callbacks, cancellation, and event callbacks are explicit. This is an
experimental API, not a promise of stable ABI compatibility yet.

Embedding APIs also expose [physical prefix checkpoints](docs/CHECKPOINTS.md),
[scoped arenas and file slices](docs/MEMORY.md), and
[normalized diagnostics](docs/DIAGNOSTICS.md). [Indexed summary inputs and caching](docs/SUMMARIES.md)
support caller-generated text and bounded model generation with checked dependencies;
`forge summarize` reuses valid cached text without another inference call. Model
identity is declared by the host; automatic agent summary selection remains open.
[Staged retrieval](docs/RETRIEVAL.md)
combines exact symbols, package imports, literal text and FTS in one indexed
snapshot with explicit budgets. Checkpoints are independent
in-memory copies bound to one loaded model instance. An opt-in bounded cache
captures eligible prefixes during normal prefill and restores exact matching
tokens. Process-restart session resume is not implemented.

[Architecture](docs/ARCHITECTURE.md) · [Security](docs/SECURITY.md) ·
[Build details](docs/BUILD.md) · [Roadmap](docs/ROADMAP.md) ·
[Third-party notices](docs/DEPENDENCIES.md)

MIT license for Forge. Dependencies and model weights retain their own licenses.
