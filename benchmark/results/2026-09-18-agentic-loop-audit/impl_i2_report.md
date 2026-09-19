# impl_i2 report — P0.1 KV/flash/offload surface + KV-aware planner + three hot-path fixes

Repo: C:/Users/flowc/dev/forge, branch main, HEAD d16e6736 (nothing committed or pushed).
Build: `export PYTHONPATH='C:/Users/flowc/dev/forge/.tools'; .tools/bin/cmake.exe --build build-gpu --config Release --parallel` → exit 0, 0 warnings/errors.
Tests: `export PATH='/c/Users/flowc/dev/forge/.tools/go/bin:$PATH'; .tools/bin/ctest.exe --test-dir build-gpu -C Release --output-on-failure` → **36/36 pass** (checkpoint_model reported Skipped: it needs a real model argument; run manually below). Logs: `%LOCALAPPDATA%/Temp/ctest_i2_full2.log`, `i2_build2.log`.

Files touched (only what each item needed; I1's uncommitted edits in agent.c, process.c, text.c,
chat_template.cpp, internal.h, judge.c, watch.c, tools.c were neither reverted nor reformatted):
include/forge/forge.h, include/forge/config.h, src/core/config.c, src/core/hardware.c,
src/inference/inference.c, src/inference/llama_backend.c, src/cli/main.c,
tests/unit/test_config.c, tests/integration/test_config_cli.py, forge.toml.example, docs/CONFIG.md.

## P0.1 — KV type / flash attention / KQV offload surface + KV-aware planner

Field names verified against the pinned header before coding:
`build/_deps/llama-src/include/llama.h:366 flash_attn_type`, `:381 type_k`, `:382 type_v`,
`:392 offload_kqv`; pinned defaults `type_k/type_v = GGML_TYPE_F16`, `flash_attn_type = AUTO`,
`offload_kqv = true` (llama-context.cpp:3528,3539-3544).

1. **Load path** (`src/inference/llama_backend.c`, `fg_llama_init`):
   - `cp.type_k`, `cp.type_v`, `cp.flash_attn_type`, `cp.offload_kqv` are set from
     `forge_model_config.cache_type_k/cache_type_v/flash_attn/offload_kqv`.
   - The string surface is validated at config/CLI parse time into `forge_kv_type`
     (`f16|q8_0|q4_0|q5_0`); the load maps it to the ggml type via `kv_cache_ggml_type`.
   - **Dependency enforced, not documented**: non-f16 K or V with `flash_attn != ENABLED` is
     refused before the model file is touched — at config validation (CLI: explicit error,
     nonzero exit) *and* again at load for library callers. Rationale verified in the pinned
     source: llama.cpp silently upgrades/hard-fails quantized V only in its own ways
     (llama-context.cpp:3596-3605), so Forge refuses instead of relying on version-specific
     behaviour; a silently ignored flag would be measured as "no effect".
   - `kv_type_matches_ggml()` verifies the planner's block-size ratio table against the pinned
     `ggml_type_size`/`ggml_blck_size` at load, so the planner can never mis-size silently.
   - **Defaults unchanged**: setting f16/f16/AUTO/true equals `llama_context_default_params()`
     byte for byte.
2. **Surface**: `[inference] cache_type_k`, `cache_type_v` (`"f16"` default, `"q8_0"`, `"q4_0"`,
   `"q5_0"`), `flash_attn` (`"auto"` default, `"on"`/`"off"` or boolean), `offload_kqv` (default
   true); CLI `--cache-type-k`, `--cache-type-v`, `--flash-attn auto|on|off`, `--offload-kqv`,
   `--no-offload-kqv`; usage text, `forge.toml.example`, `docs/CONFIG.md` updated (the stale
   "no config keys for … KV quantization" sentence removed, planner text rewritten).
3. **KV-aware planner** (`src/core/hardware.c`):
   - `forge_model_requirements.kv_type` threads the selected type into the planner; the measured
     f16 payload is scaled by the pinned ggml block layout (`bytes = f16_bytes × numerator /
     (2 × denominator)`): q8_0 = 34/32, q4_0 = 18/32, q5_0 = 22/32. When K and V differ the
     planner sizes against the larger-cost type (conservative, never optimistic).
   - The halving walk is replaced by a binary search over 128-token steps for the **largest
     fitting context between the minimum valid context and the requested context**; the result
     never exceeds the requested context, and the minimum is the floor when nothing fits.
   - `plan->kv_format` reports the type actually used; assumptions strings name it.
4. **Deterministic tests** (all GPU-free, no model):
   - `tests/unit/test_config.c`: new `kv_control_tests()` — default preservation (f16/f16,
     AUTO, offload on, f16 plan); name/ratio/flash-name helpers; TOML round-trip; refusal of
     non-f16 KV without `flash_attn = "on"` through parse *and* through direct
     `forge_config_validate`; unknown names, wrong TOML types and out-of-range enums refused.
   - New `planner_kv_tests()` — campaign qwen3moe arithmetic (17.28 GiB tensors, 98,304 f16
     B/token, 22.61 GiB free VRAM, 21.89 GiB free RAM): f16 recommendation **8,704** tokens
     (largest fitting 128-step; the old halving walk stopped at 8,192 — asserted
     `context_tokens > 8192`), q8_0 derived from the ggml block layout
     (`98304/2 × 34/32 = 52,224 B/token`) fits the requested 16,384 with
     `estimated_kv_bytes = 855,638,016`, q4_0 fits, invalid type refused.
   - `planner_tests()` updated where the search intentionally raises the result: the 6 GiB RAM
     case now recommends 7,168 (was 4,096 by halving); every other existing planner assertion
     is unchanged and still passes.
   - `tests/integration/test_config_cli.py`: new `test_kv_cache_type_requires_flash_attention`
     proves the CLI refusal (explicit `flash_attn` error, **nonzero exit**) plus accepted
     round-trips and the mixed-type conservative rule; three malformed-option cases added.
5. **Real-model evidence** (not required by P0.1, run to verify the surface end to end):
   - `forge complete` defaults: output `alpha`, `flash_attn = auto`, KV buffer **1792.00 MiB**.
   - `--cache-type-k q8_0 --cache-type-v q8_0 --flash-attn on --gpu-layers -1`: output `alpha`,
     `flash_attn = enabled`, KV buffer **952.00 MiB**, `attn_rot_k/v = 1` (quantized-KV path).
     952/1792 = 0.53125 = 34/64 exactly — the planner's scaling is confirmed by the actual
     llama.cpp allocation.

## Hot-path fixes (llama_backend.c)

- **01#11 decode_batch built/freed a llama_batch per call** → `llama_state.batch` is one
  reusable scratch batch allocated once at load, sized to `llama_n_batch(ctx)` (512), refilled
  per call; freed in `llama_destroy`. All call sites (prefill chunks, cue decode, native
  opener, decode loop) go through it unchanged; logits flags, positions, seq ids are rewritten
  every call exactly as before. Evidence: `forge_checkpoint_model` (real Qwen3-0.6B, flattened,
  prefill + per-token decode + save/restore accounting) 4/4 matched cases; native agent run
  2 turns, exit 0.
- **02#6 tokenize_text_allocated double-walked the tokenizer** → single pass: allocate
  `min(len + 4, 1048576)` and tokenize once; the 1M guard is a post-check on the returned count
  (negative return = required size; > 1M → LIMIT, else MODEL). Evidence: all real-model runs
  tokenize correctly; native output byte-identical before/after the change.
  *Deviation from the claim:* the claim proposed `len + 1`; the pinned tokenizer can add BOS
  *and* EOS/SEP and SPM prepends a space, so `len + 4` is the true bound (checked in
  llama-vocab.cpp:3395-3535). Still one pass, still bounded, no behaviour change observed.
- **02#14 preserved-token strings re-tokenized every generation** → the resolved ids are cached
  in `llama_state` keyed on an FNV-1a fingerprint of the preserved marker strings themselves
  (they are template constants; the key invalidates if the list ever changes). Evidence: two
  native generations in one process (agent run) with a warm cache produced the expected
  behaviour and the same bytes as a cold cache (native run repeated in a fresh process is
  byte-identical: `i2_native_run1.out` == `i2_native3.out`).

**Nothing was reverted**: no fix caused a test failure or a real-model regression.

## Test summary

| check | result |
| --- | --- |
| build-gpu (Release) | pass, 0 warnings |
| ctest full suite | 36/36 pass (checkpoint_model skipped: needs a model arg) |
| forge_config_unit (defaults, refusal, planner arithmetic, raised context) | pass |
| config_integration (CLI refusal with nonzero exit, round-trip) | pass |
| forge_checkpoint_model + Qwen3-0.6B (real, flattened) | pass, 4 matched cases |
| forge_checkpoint_model --automatic + Qwen3-0.6B (restore/reuse, multi-generation) | pass, 2 records |
| CLI: explicit defaults, --no-offload-kqv, --flash-attn on alone | exit 0 each |
| native generation ×2 in one process (preserved-token cache warm) | pass, exit 0 |
| q8_0 KV + flash attention on GPU | pass, 952 MiB vs 1792 MiB f16 |
