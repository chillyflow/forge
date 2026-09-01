# Benchmark methodology

`tasks/*.json` contains 29 deterministic tasks: ten tiny Go smoke repairs, six
reasoning-gated Go repairs, and thirteen campaign tasks split across Go and
Python. The campaign set covers multi-file changes, APIs, refactors, compiler or
import failures, repository exploration and algorithms. These remain controlled
synthetic fixtures, not SWE-bench or representative evidence of general
repository-level performance. `generate_tasks.py` and
`generate_campaign_tasks.py` recreate them for review.

## Campaign workflow

Preflight every broken fixture and every supplied oracle before using a model:

```sh
python benchmark/preflight.py --suite all --output /tmp/preflight.json
```

OpenCode 1.18.25 is the first established comparison harness. Aider 0.86.2 is
the second; its adapter refuses any other version. Install it in an isolated
environment from the checked-in top-level pin:

```sh
python -m venv .tools/aider
.tools/aider/bin/python -m pip install -r benchmark/requirements-aider.txt
```

The parity pilot runs three tasks through Forge, OpenCode and Aider with cold
model lifecycles and the same independent verifier. The orchestrator writes a
`protocol-lock.json` containing source, task, model, executable, adjacent DLL
and Aider package identities before any model run:

```sh
python benchmark/campaign.py --mode pilot \
  --forge /tools/forge --opencode /tools/opencode --aider /tools/aider \
  --server /tools/llama-server --model /models/coder.gguf \
  --output /results/parity-pilot
```

After reviewing the pilot artifacts, use `--mode campaign` for all 29 tasks and
three randomized repetitions. Pass `--repetitions N`, `--tasks ...`, or a
different `--order-seed` to make an explicit protocol change. `report.py`
structurally checks model, fixture, context, decode and lifecycle identity, then
reports per-task and aggregate p50/p90/p95, sample standard deviation, failures,
tokens, peak process-tree RSS and peak device VRAM.

The primary campaign uses only Forge `optimized` so the external harnesses have
one matched comparison arm. After that result is frozen, run Forge mechanism
ablations separately against the same lock and task order, for example:

```sh
python benchmark/run.py --forge /tools/forge --model /models/coder.gguf \
  --suite all --repetitions 3 --order-seed 20260831 \
  --variants optimized no-kv no-semantic no-compaction grammar-first \
  --output /results/forge-ablations
```

Portability checks use new per-model output directories and the same selected
task subset; never consolidate token counts across model tokenizers.

Every schema-v2 record separates:

- `startup_seconds`: cold model/server load.
- `agent_seconds`: the harness task phase.
- `verification_seconds`: the same external manifest verifier.
- `end_to_end_seconds`: cold server/process spawn through verifier exit.

OpenCode and Aider also record server teardown in cold mode. Warm server runs are
supported by their individual adapters and are labeled `warm`; the reporter
refuses to mix warm and cold data without an explicit override. RSS covers the
harness/model process roots and descendants. VRAM sampling uses whole-device
`nvidia-smi` usage because per-process VRAM is unavailable on some WDDM systems;
the scope and pre-run baseline are stored with every measurement.

`run.py` creates a fresh temporary Git checkout for every task/variant, runs Forge,
and checks an independent verifier after the model finishes. Modifying the
original test file invalidates the result. Raw session artifacts are copied to
the output directory before removing the runner-owned temporary checkout.

The primary endpoints, interval method, matched-pair rule, subgroup reporting,
measurement-invalid conditions, and claim scope are frozen in
[`ANALYSIS.md`](ANALYSIS.md). The repeated parity gate validates the measurement
layer only and is excluded from the primary 29-task dataset.

```sh
python benchmark/run.py --forge ../build/forge --model /models/coder.gguf \
  --suite smoke --tasks add clamp --variants optimized no-kv no-semantic \
  --output /tmp/forge-bench
```

Go 1.24+ and gofmt must be on PATH; CI uses Go 1.27.0. Every fixture's `go.mod`
declares `go 1.24`, and an older toolchain makes each `go` invocation attempt a
toolchain download instead of running the fixture. The failures then look like
agent errors rather than an unusable environment. Fixtures use only the standard
library.
All runners use shared `utf8-lf-language-format-v2` preparation: write UTF-8/LF
files and format Go inputs before timing, Git baseline creation and protected-file hashing.
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

The OpenCode and Aider adapters use a separately installed harness and matching
`llama-server`. They run only trusted synthetic fixtures, with remote services,
auto-sharing, model downloads and analytics disabled. The loopback server has an
ephemeral authentication key. Generated harness configuration is private
benchmark state, not a file to publish. KV is cleared between tasks, while
prefix reuse within a task remains enabled. Batch sizes and thread counts match
Forge's current defaults.

```sh
python benchmark/opencode.py --opencode /tools/opencode \
  --server /tools/llama-server --model /models/coder.gguf \
  --lifecycle cold --repetitions 3 --suite all --output /tmp/opencode-results
python benchmark/aider.py --aider /tools/aider \
  --server /tools/llama-server --model /models/coder.gguf \
  --lifecycle cold --repetitions 3 --suite all --output /tmp/aider-results
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
`thought-routed-required-decode-only`. Routed decoding force-decodes a `Thought: ` cue (host scaffold, stripped
before validation and excluded from the thought census), then enforces a
minimum prefix budget (32 tokens, or a quarter of the turn budget if smaller)
by excluding action-opening tokens, and keeps end-of-generation tokens
excluded until the action begins, so every routed action gets decode-time room
for a plain-text prefix and cannot end a generation actionless. The budget
guarantees sampled tokens, not visible text — a model can still spend it on
whitespace, and required routed thought rejects exactly that case. During the suppressed and
pre-action phases the grammar greedy fast path is disabled, so routed runs
record those tokens under `grammar_fallback_tokens`; do not compare that counter
across routed and non-routed arms. Results recorded before this mechanism existed
(binary `24bf9a18…`, the 2026-08-30 routed sweep) measured routed arms that
elicited nothing; `results/2026-08-30-elicited-sweep/` re-measures every arm on
the cue mechanism, with a doubled-turn-cap control and a five-replicate
retention analysis (`analyze_retention.py`).

Phase 2 bounds the routed states. The reasoning phase ends at a think budget
(default at most 256 tokens, reduced to half the remaining turn budget near
exhaustion; `--thought-budget N` overrides it) by
swapping the never-triggered lazy grammar for an eager one so the action must
open constrained. Leading-whitespace tokens are excluded after a forced swap
until the first action token, and every grammar arm ends generation at the
token that completes the action object instead of waiting for an end token. The
`thought-routed-unbounded-decode-only` arm (`--no-thought-budget`) is the
same-binary A/B for the budget. Per-state counters `think_tokens`,
`forced_actions`, `forced_action_progress_tokens`, and `action_stops` land in
`metrics.json`; action selection, argument, patch, final, and memory tokens have
separate counters. Structured selection/arguments are deterministic even when
patch/final sampling uses temperature. `think_tokens` is tokenizer-relative
and never comparable across models, and `action_stops` approximates turn count
on every grammar arm rather than signaling anomalies. The complete same-binary
re-sweep is checked in at `results/2026-08-31-phase2-resweep/`; the budget arms
and reasoning-gated fixtures are at `results/2026-08-31-reasoning-gated/`.
`consolidate_arms.py` and `analyze_failures.py` classify custom cues as well as
the default cue.

Use `--suite reasoning-gated` for the six logic fixtures, or `--suite all` for
both suites. Budget calibration arms are
`thought-routed-budget-256-decode-only`, `-512-`, `-1024-`, and `-1536-`, plus
the unbounded control. Native-thinking models use `thought-native-decode-only`;
`thought-native-disabled-decode-only` is the matched lazy-grammar control with
template thinking disabled. Native arms must use a chat template that supports
the `enable_thinking` variable.

Multi-model runs are consolidated per model, never across models.
`consolidate_arms.py` includes `model_sha256` in the identity it verifies, so it
structurally refuses to merge arms from different models; that is intended, not
an obstacle to work around. Publish one results tree per model and compare only
pass rates and turn counts across them. Token counters are tokenizer-relative
and are not comparable between models, and `grammar_fallback_tokens` is not
comparable between routed and non-routed arms even within one model. Use
`--chat-template` only when a GGUF's embedded template is not recognized; the
value applied is recorded in each run's `environment.json` as `chat_template`
(`embedded` when no override was passed). `results/2026-08-31-tier1-models/`
measures three further non-thinking instruct models this way.

Independent runs with the same seed did not always produce identical action
traces. Treat the KV ablation as complete task runs, not a matched-token
microbenchmark. The initial two-task grammar ablation did preserve both action
traces, including the failing task; its older `sampling_ms` includes GPU waits.
