# Configuration and hardware planning

Forge reads a small, strict TOML schema using the maintained
[tomlc17 parser](https://github.com/cktan/tomlc17) pinned to
`64a063b8636a4b48d142f978270f5e53e605e240`, with an archive SHA-256 in
`cmake/Dependencies.cmake`. TOML syntax is handled by that parser; Forge validates
the schema and UTF-8 independently. Unknown keys, wrong types, duplicate keys,
invalid numbers and unsupported modes fail with a diagnostic. Syntax and schema
errors include the source file and, where available, line and column.

## Files and precedence

The CLI applies these layers in order:

1. Native defaults from `forge_config_init`.
2. One explicitly selected `--profile FILE`, if provided.
3. `--config FILE`, or `forge.toml` in the selected `--workspace`.
4. Explicit CLI flags, regardless of where they occur in the argument list.

`--no-config` disables workspace discovery; an explicitly selected profile still
applies. It cannot be combined with `--config`. A missing automatically discovered
workspace file is fine. A missing explicitly selected file is an error. No user
home config, environment config, recursive directory search or implicit model
profile is loaded. The existing JSON profile is historical benchmark metadata;
only the new TOML profiles are loadable configurations.

Each file can start with `extends = "profiles/base.toml"`. The parent is applied
before the child's keys, and absent keys keep their previous values. A chain is
limited to eight files including the selected file. Normalized path cycles fail;
aliases such as symlinks are also bounded by the depth limit. Every file must have
valid keys and scalar values. Cross-field checks, such as output reserve versus
context, run on the final inherited configuration.

Each load is transactional: a parse, inheritance or validation error leaves the
previous configuration unchanged. CLI overrides cannot repair an invalid file.

Model paths and `extends` paths are relative to the file defining them, including
inherited files. CLI paths are relative to the invocation directory. Paths are
made absolute with lexical `.`/`..` normalization. Model files need not exist while
parsing a configuration; loading or inspecting the model verifies them. There is
no environment-variable, tilde or shell expansion. On Windows, use a full drive
path, UNC path or relative path; ambiguous drive-relative/root-relative paths are
rejected. Existing platform filesystem encoding limitations still apply.

Configuration files must be regular files, valid UTF-8, and at most 256 KiB each.
No path or string accepted by the schema may contain embedded NUL bytes.

## Supported fields

All fields are optional. Integer fields reject floats, strings and booleans.

| Table | Key | Value and behavior |
| --- | --- | --- |
| `model` | `path` | Nonempty local GGUF path; never downloads a model. |
| `model` | `context` | Integer 128–1,048,576; sets model and agent context capacity. |
| `model` | `chat_template` | Nonempty template/name passed to the inference backend, at most 64 KiB. |
| `model` | `enable_thinking` | Boolean Jinja template control. Omission preserves the template's legacy/default behavior; an explicit value requires template support. |
| `inference` | `gpu_layers` | `"auto"`, `-1` for all layers, or 0–65,535. Default 0. |
| `inference` | `threads` | Integer 0–1,024; 0 leaves thread selection to the backend/planner. |
| `inference` | `temperature` | Finite integer or float 0–2. |
| `inference` | `repetition_penalty` | Finite integer or float in (0, 2]; 1.0 disables the penalty. Default 1.0. |
| `inference` | `repetition_last_n` | Integer 0–1,024; last N tokens penalized, 0 disables. Default 0. |
| `inference` | `seed` | Integer 0–4,294,967,295. |
| `inference` | `reuse_prefix` | Boolean; enables existing prompt-prefix reuse. |
| `inference` | `grammar_fast_path` | Boolean; enables existing greedy grammar fast path. |
| `inference` | `speculative` | Only `false`; `true` is rejected because speculative decoding is not implemented. |
| `inference` | `cache_type_k` | KV cache K element type: `"f16"` (default), `"q8_0"`, `"q4_0"` or `"q5_0"`. |
| `inference` | `cache_type_v` | KV cache V element type; same values as `cache_type_k`. |
| `inference` | `flash_attn` | `"auto"` (default), `"on"`, `"off"`, or a boolean (`true` = on). |
| `inference` | `offload_kqv` | Boolean, default `true`; offloads KQV operations (including the KV cache) to the GPU. |
| `inference.checkpoints` | `enabled` | Boolean, default `false`; enables the bounded physical prefix cache on supported backends. |
| `inference.checkpoints` | `max_bytes` | Integer 4,096–1,073,741,824; aggregate manager allocation cap, default 268,435,456. Not RSS/VRAM. |
| `inference.checkpoints` | `max_entries` | Integer 1–64; default 8 retained prefixes. |
| `inference.checkpoints` | `min_prefix_tokens` | Integer 1–1,048,576; default 128 actual matched tokens. |
| `inference.checkpoints` | `max_captures_per_prompt` | Integer 1–4; default 2 capture attempts during normal prefill. |
| `agent` | `output_reserve` | Positive tokens per turn, strictly smaller than context. |
| `agent` | `max_turns` | Integer 1–1,000. |
| `agent` | `max_tokens` | Total generated-token limit, 1–2,147,483,647. |
| `agent` | `max_input` | Cumulative prompt-token limit, 1–2,147,483,647. |
| `agent` | `max_tool_bytes` | Per-output-stream capture cap, 1–16,777,216 bytes. |
| `agent` | `max_file_bytes` | Read/patch file cap, 1–16,777,216 bytes. |
| `agent` | `wall_timeout_ms` | Run deadline, 1–604,800,000 milliseconds. |
| `agent` | `semantic_output` | Boolean; enables supported tool-output compression. |
| `agent` | `compact_context` | Boolean; enables existing context compaction. |
| `tools.shell` | `timeout` | Integer 1–86,400 **seconds**; CLI `--timeout-ms` is in milliseconds. |
| `tools.shell` | `network` | Boolean restriction described below; never grants execution permission. |
| `index` | `languages` | Exactly `["go"]`; other/empty/duplicate language lists fail. |
|| `judge` | `endpoint` | Nonempty `http(s)` base URL; default `https://api.typesafe.ai`. |
|| `judge` | `model` | Nonempty model alias or pinned id (e.g. `jev-1.13.0`); default `jev-1.13.0`. The response's versioned id is recorded per call. |
|| `judge` | `api_key_env` | Nonempty environment variable name holding the API key; default `TYPESAFE_API_KEY`. The key is read at call time and never stored in configuration or artifacts. |
|| `judge` | `record_dir` | Optional directory for raw request/response artifacts; resolved relative to the file defining it. |
|| `judge` | `timeout_ms` | Integer 100–30,000 milliseconds per attempt; default 2,000. |
|| `judge` | `max_candidates` | Integer 1–256 candidates per rerank request; default 32. |
|| `judge` | `confidence_threshold` | Float 0–1; minimum top rerank score to accept a reordering. 0 disables the confidence gate and preserves the original order on every success. Default 0. |
|| `judge` | `feedback_next_action_threshold` | Float 0–1; next-action confidence below which the agent treats the guidance as uncertain and inspects source. Default 0.55. |
|| `judge` | `feedback_failure_confidence_threshold` | Float 0–1; failure-family confidence below which the agent treats the guidance as uncertain. Default 0.45. |
|| `judge` | `feedback_repair_readiness_threshold` | Float 0–1; repair-readiness confidence below which the agent treats the guidance as uncertain. Default 0.45. |
|| `judge` | `feedback_evidence_gap_threshold` | Float 0–1; evidence-gap at or above which the agent inspects source before editing. Default 0.60. |

KV cache element types are the ggml type names `f16`, `q8_0`, `q4_0` and `q5_0`.
The defaults (`f16`/`f16`, `flash_attn = "auto"`, `offload_kqv = true`) are the
pinned llama.cpp defaults, so an unconfigured load is unchanged. llama.cpp only
honors quantized KV cache types together with flash attention; selecting a
non-f16 type without `flash_attn = "on"` is refused with an explicit error and a
nonzero exit instead of being silently ignored. The CLI equivalents are
`--cache-type-k`, `--cache-type-v`, `--flash-attn auto|on|off`, `--offload-kqv`
and `--no-offload-kqv`. Validation runs per file before CLI overrides are
merged, so a profile that selects a non-f16 type must also set
`inference.flash_attn = "on"` itself; a `--flash-attn on` on the command line
cannot repair the file's own state.

See `forge.toml.example` and `profiles/*.toml`. The `judge` table configures an
optional hosted judgment service (TypeSafe "System One"/Jev) and never grants
its use: the CLI `--judge` flag does. When granted, `forge retrieve` and the
agent's retrieval tool may rerank candidates before budget trimming; any
service failure keeps the deterministic order and is reported in the output's
`rerank` object. The API key is the one documented environment-variable
exception — read from the named variable at call time because no vault is
built in. There are deliberately no config
keys for tool permission grants, script fixtures, draft models, the
reasoning-channel wire policy, network sandbox backends or
unsupported index languages. The model-level `enable_thinking` key controls
template rendering; it does not enable routed reasoning by itself.

The reasoning channel is CLI-only. `--no-thought`, `--thought-required`,
`--thought-routed`, `--thought-native`, `--thought-history`, `--thought-decode-only`,
`--thought-budget`, `--no-thought-budget` and `--thought-cue` are the whole
surface. Successful-action thoughts are dropped from later prompts unless
`--thought-history` is given; the native protocol retains a failed action's
thought beside its diagnostic for recovery. The budget and cue controls require
`--thought-routed`; a budget of 0
and a cue containing `{` are rejected, and when both budget flags are given the
last one wins. The routed default is at most 256 reasoning tokens, reduced to
half the remaining per-turn budget when smaller. `--thought-native` selects a
cue-free lazy grammar and enables template thinking; a later
`--disable-thinking` creates the matched safe baseline. The standalone
`--enable-thinking` / `--disable-thinking` switches override
`model.enable_thinking` by normal CLI precedence.

`--prompt-protocol native` is the default and renders structured roles and
function schemas through the model's chat template. `--prompt-protocol flattened`
selects the legacy single-user-message protocol. Native prompts include host
action-budget state and reserve the last action for `final`, which runs required
host validation. The protocol setting is separate from `--thought-native`.

Checkpoint CLI overrides are `--checkpoint-cache`, `--no-checkpoint-cache`,
`--checkpoint-cache-bytes`, `--checkpoint-cache-entries`,
`--checkpoint-cache-min-tokens` and `--checkpoint-cache-captures`. Numeric limits
alone do not enable the cache. `--no-kv-reuse` bypasses both live-prefix and
automatic checkpoint reuse/capture even when the manager is configured.
Explicitly enabling it on a scripted or unsupported model fails; it never
substitutes simulated state for a physical cache. There is no disk persistence
between CLI runs. See [checkpoint ownership, semantics and metrics](CHECKPOINTS.md).

## Shell security

Configuration cannot grant `--allow-write` or `--allow-exec`. Those remain explicit
CLI permissions. The process runner is unsandboxed and may execute repository
code with the user's network and filesystem access.

An explicit `tools.shell.network = false` therefore **forbids subprocess tool
execution**: combining it with `--allow-exec` fails before execution. Forge cannot
truthfully promise network isolation for the current runner. `network = true`
only removes this requested restriction; it still requires `--allow-exec` to run
commands. Omitting `network` retains the existing explicit unsandboxed-execution
contract. A timeout does not provide a security boundary.

## Hardware recommendations

`forge hardware-plan --model /path/to/model.gguf --json` reports host measurements,
optional model metadata, and recommendations without running inference. Without
`--model`, it reports hardware and unknown model fit. It never downloads models.

Host RAM is measured through `GlobalMemoryStatusEx` on Windows, `sysconf` and Linux
`MemAvailable`, or macOS `sysctl`/VM statistics. The macOS available value uses free
plus inactive pages and is an estimate of reclaimable memory. CPU reporting
includes logical CPUs available to the process where affinity can be queried,
architecture, and available SSE2/AVX/AVX2/NEON features. AVX checks include operating
system support for YMM state. Thread recommendations leave one logical CPU free
when possible and cap inference threads at eight; this is not a benchmark result.

With llama.cpp, Forge enumerates local usable ggml GPU devices and their available
memory. The hardware command may initialize backend discovery/driver queries but
does not create an inference context or load tensor data. RPC devices are ignored.
A build without llama.cpp reports GPU detection as unavailable, which does not
mean there is no physical GPU. Only GPU 0 is budgeted; multiple device memories
are never pooled. Integrated/unified GPU memory is capped by available RAM and
is never added to it.

The pinned Metal backend exposes a process working-set budget rather than globally
free dedicated VRAM, even though its generic device type is `GPU`. Reports mark
this with `memory_is_budget`; that budget is also capped by available RAM. Native
Apple silicon additionally reports shared memory. This avoids treating a Metal
working-set allowance as extra physical memory.

Model inspection uses the pinned llama.cpp vocabulary-only loader with an empty
GPU-device list and zero GPU layers. It reads GGUF metadata/tensor sizes, not
weights. KV estimates currently recognize scalar conventional attention metadata
for `llama`, `qwen2`, `qwen2moe`, `qwen3` and `qwen3moe`. Array-valued dimensions,
recurrent/hybrid architectures and MLA remain unknown. For one f16 sequence:

```text
KV payload per token = layer_count × kv_head_count × (key_length + value_length) × 2 bytes
```

Absent head dimensions use embedding length divided by attention-head count.
Absent KV-head count uses the attention-head count. These defaults follow the
supported conventional layouts; incompatible metadata disables the estimate.
Sliding-window layers use a full-context upper estimate. The reported payload is
not the total allocation: padding, batches, compute buffers, allocator behavior,
backend workspaces and peak load memory are not included.

The planner scales that measured f16 payload to the selected
`cache_type_k`/`cache_type_v` by the pinned ggml block layout (`q8_0` packs 32
elements into 34 bytes, `q4_0` into 18, `q5_0` into 22); when K and V differ it
sizes against the larger-cost type, so the estimate is never optimistic. The
reported `kv_format` is the type that was used.

The planner adds a 12.5% model/KV margin and reserves at least 1 GiB or 10% of
available memory. Full GPU placement also requires a coarse host staging floor
of 1 GiB plus model bytes/16. Partial offload is suggested only when the entire
model/context also fits host RAM, and each layer is budgeted at twice the average
layer size. Layers can differ significantly; this remains a heuristic.

If needed, the recommended context is the largest fitting 128-token step between
the minimum valid context (the larger of 128 tokens and the output reserve plus
one) and the requested context, searched against measured free memory and the
selected KV type. The recommendation never exceeds the requested context. Unknown model/KV information
disables automatic offload and caps the context recommendation at 4,096 when the
output reserve permits. No draft model is enabled; the reported KV format is the
selected cache type (`f16` by default). `estimated` fit is not
proof of successful loading. OS memory measurements may exclude process/container
limits and may change immediately after sampling.

`gpu_layers = "auto"` requests application of these recommendations. Numeric GPU
settings remain explicit. The CLI emits assumptions and any context reduction or
unknown/insufficient fit warning rather than silently claiming a fit guarantee.
An insufficient automatic plan stops before loading weights. An explicit numeric
GPU setting can bypass that heuristic when the user has measured a valid fit.

## Embedding API

Include `forge/config.h`, call `forge_config_init`, then overlay files with
`forge_config_load` or a document with `forge_config_parse`. The latter requires a
source path to define relative paths, even when that source file does not exist.
Apply explicit caller overrides last, call `forge_config_validate`, and call
`forge_config_check_exec` before allowing subprocess tools.

The configuration owns its copied file strings. Public `model` string pointers can
be temporarily replaced by borrowed CLI/caller strings; private storage is freed
by `forge_config_destroy`. Config objects must not be shallow-copied. A subsequent
successful load invalidates previously borrowed pointers into config storage.
Keep the config (and any borrowed overrides) alive while the model/agent uses its
strings. Resolve `FORGE_GPU_LAYERS_AUTO` through the planner before model loading.

`forge_hardware_plan` is pure and accepts injected `forge_hardware` and
`forge_model_requirements` values. This makes its unknown-memory, overflow, unified
memory, context and offload behavior testable without a GPU or model download.
