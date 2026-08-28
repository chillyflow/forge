# Forge

**A native local coding agent built to spend fewer tokens on getting work done.**

[![Native CI](https://github.com/chillyflow/forge/actions/workflows/ci.yml/badge.svg)](https://github.com/chillyflow/forge/actions/workflows/ci.yml)

Forge's agent runtime is C17. It links directly to llama.cpp, maintains reusable
sequential KV prefixes, selects context under a token budget, and executes
grammar-constrained tools. Go source navigation uses Tree-sitter and SQLite.

**Development preview, not a production sandbox or a proven performance win.**
The [roadmap](docs/ROADMAP.md) distinguishes implemented behavior from the larger
design's research milestones. No model weights are bundled or downloaded by the
runtime. llama.cpp and GPU dependencies include C++; Forge does not claim its
entire dependency graph is C.

The tested local model is [Qwen3-Coder-30B-A3B Q4_K_M](docs/MODEL.md), with verified
download provenance and GPU settings documented separately.

## Build

CMake 3.24+, a C17/C++17 compiler, Git, and Python 3.10+ for tests. The first build
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
forge inspect DeleteRecord --depth 1 --workspace ./my-repository
forge references DeleteRecord --workspace ./my-repository
forge search "context.WithCancel" --workspace ./my-repository

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

### Tools

| Tool | Behavior |
| --- | --- |
| `read_file` | Bounded line ranges with line numbers |
| `list_directory` | Sorted indexed file map |
| `search_text` | Literal source search, bounded results |
| `find_symbol` | Go declaration/signature/body expansion |
| `get_references` | Go identifier occurrences, not type-resolved references |
| `apply_patch` | One exact, unique replacement; atomic file replacement |
| `run_command` | Argument-vector execution, separate stdout/stderr, deadlines |
| `git_diff`, `git_status` | Git inspection with external diff drivers disabled |
| `expand_output` | Retrieve a page of recorded raw output |

The registry generates a GBNF grammar. Schema validation runs again before
dispatch. Malformed or unknown actions never execute.

### Sessions

Each run writes `.forge/sessions/<random-id>/` containing `events.jsonl`,
`metrics.json`, `patch.diff` (tracked files), actual prompts in `context/`, and
raw tool results in `tool/`. Replay reads events only and **never executes
recorded tools**. It is an audit replay, not inference replay or session resume.
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

[Architecture](docs/ARCHITECTURE.md) · [Security](docs/SECURITY.md) ·
[Build details](docs/BUILD.md) · [Roadmap](docs/ROADMAP.md) ·
[Third-party notices](docs/DEPENDENCIES.md)

MIT license for Forge. Dependencies and model weights retain their own licenses.
