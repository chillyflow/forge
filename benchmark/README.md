# Benchmark methodology

`tasks/*.json` contains ten tiny Go repair tasks. They are a smoke suite, not
SWE-bench or representative evidence of general repository-level performance.
`generate_tasks.py` recreates them deterministically for review.

`run.py` creates a fresh temporary Git checkout for every task/variant, runs Forge,
and checks an independent verifier after the model finishes. Modifying the
original test file invalidates the result. Raw session artifacts are copied to
the output directory before removing the runner-owned temporary checkout.

```sh
python benchmark/run.py --forge ../build/forge --model /models/coder.gguf \
  --tasks add clamp --variants optimized no-kv no-semantic \
  --output /tmp/forge-bench
```

Go 1.24+ and gofmt must be on PATH; CI uses Go 1.27.0. Every fixture's `go.mod`
declares `go 1.24`, and an older toolchain makes each `go` invocation attempt a
toolchain download instead of running the fixture. The failures then look like
agent errors rather than an unusable environment. Fixtures use only the standard
library.
Both runners use shared `utf8-lf-gofmt-v1` preparation: write UTF-8/LF files and
format Go inputs before timing, Git baseline creation and immutable-test hashing.
Each result records the preparation version and per-file/aggregate SHA-256.
This avoids turning pre-existing fixture formatting into an agent failure.
Older published runs used the original unformatted fixtures and must not be
treated as measurements of these normalized inputs.
The runner executes code; use trusted manifests and an isolated machine.
Do not point native `forge bench` at a valuable checkout without reviewing the
prompt and verifier.

## Reporting

Report GGUF filename/SHA-256, llama.cpp revision, GPU/driver, context size, output
budget, GPU layers, sampling settings, harness version, verification command,
task count, failure count and warm/cold conditions. Preserve all failures.

- `prompt_tokens`: logical prompt tokens across calls.
- `cached_tokens`: sequential prefix tokens not decoded again.
- `prefill_tokens`: new prompt tokens submitted for evaluation.
- `generated_tokens`: sampled tokens excluding end-of-generation.
- `raw_tool_bytes`: captured output, not bytes discarded after process limits.
- `visible_tool_bytes`: result bytes retained for model context.
- `wall_seconds`: driver time including model loading and verification.
- `duration_ms`: agent time; model loading is reported separately.

Never compare simulated token estimates with actual tokenizer counts. A no-KV
Forge ablation is not a comparison with another product. External comparisons
must use the same GGUF, hardware and tasks, preserve each harness's normal tool
protocol, and report configuration differences.

Disk KV caching, speculation, and OS isolation are not current variants. No
speedup is assumed before measurement. Review raw artifacts before publishing.

## Established-harness comparison

The optional adapter uses a separately installed OpenCode and matching
`llama-server`. It runs only the trusted synthetic fixtures, with remote tools,
subagents, auto-sharing, plugins and model downloads disabled. The loopback server
has an ephemeral authentication key; it is unrelated to any Hugging Face token.
The generated OpenCode configuration is private benchmark state, not a file to
publish. KV is cleared between tasks, while prefix reuse within a task remains
enabled. Batch sizes and thread counts match Forge's current defaults.

```sh
python benchmark/opencode.py --opencode /tools/opencode \
  --server /tools/llama-server --model /models/coder.gguf \
  --output /tmp/opencode-results
python benchmark/summarize.py --forge /tmp/forge-results \
  --opencode /tmp/opencode-results --forge-revision ACTUAL_GIT_COMMIT \
  --output /tmp/public-numeric-results
```

The summarizer reads server counters, including metadata/title inference, rather
than counting only user-visible agent steps. It copies numeric records and a
small environment allowlist, not raw logs, prompts or generated credentials.
Task fixtures are deliberately small, and one run is not statistically robust.

`--variants optimized grammar-first` compares the greedy grammar fast path with
full-vocabulary masking. The fast path still validates every selected token.
Its counters include end tokens; `generated_tokens` excludes them. `sampling_ms`
excludes waiting for GPU work in revisions after `cb3254b`, while `decode_ms`
includes sampling and event callbacks. Do not add those two durations together.

Thought-channel arms are available as `no-thought`, `thought-optional-decode-only`,
`thought-required`, and `thought-required-decode-only`. Prefix each thought arm
with `thought-routed` in the variant name to use a plain-text reasoning prefix
and lazy action-grammar trigger; for example, `thought-routed-decode-only` or
`thought-routed-required-decode-only`. Routed decoding enforces a minimum
prefix budget (32 tokens, or a quarter of the turn budget if smaller) by
excluding action-opening tokens, and keeps end-of-generation tokens excluded
until the action begins, so every routed action gets decode-time room for a
plain-text prefix and cannot end a generation actionless. The budget guarantees
sampled tokens, not visible text — a model can still spend it on whitespace, and
required routed thought rejects exactly that case. During the suppressed and
pre-action phases the grammar greedy fast path is disabled, so routed runs
record those tokens under `grammar_fallback_tokens`; do not compare that counter
across routed and non-routed arms. Results recorded before this budget existed
(binary `24bf9a18…`, the 2026-08-30 routed sweep) measured routed arms that
elicited nothing.

Independent runs with the same seed did not always produce identical action
traces. Treat the KV ablation as complete task runs, not a matched-token
microbenchmark. The initial two-task grammar ablation did preserve both action
traces, including the failing task; its older `sampling_ms` includes GPU waits.
