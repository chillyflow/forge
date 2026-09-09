# Tier-1 multi-model sweep — action plan (written 2026-08-31)

Self-contained plan for a fresh session. Goal: before starting phase 2 (per-state
decode routing, §32 literal), measure the thought-channel mechanism on additional
**non-thinking** instruct models. This is the cheap tier that needs **zero forge
code changes**. Natively-thinking models (Qwen3-Thinking, gpt-oss, Magistral,
Nemotron, North-Mini-Code) are explicitly **out of scope** — running them today
measures harness artifacts, not models (2048-byte routed-thought cap is run-fatal,
token-0 grammar amputates native reasoning in baseline arms, no `enable_thinking`
control via the legacy template API, special-token text divergence). Their support
is phase-2 work and phase 2's acceptance test.

## Why (decision context, verified 2026-08-31)

- Every published §32 number rests on ONE model: Qwen3-Coder-30B-A3B-Instruct
  Q4_K_M, greedy, 10 tiny Go fixtures
  (`benchmark/results/2026-08-30-elicited-sweep/`). Project goal is lifting ANY
  local model.
- A six-agent adversarially-verified audit confirmed: the routed mechanism is
  vocab-portable (no hardcoded token IDs; brace-ban table, EOG ban, cue
  tokenization, trigger regex all derived per-vocab at runtime;
  `src/inference/llama_backend.c:240-289,427-431`), and the chat template is read
  from the GGUF with llama.cpp's heuristic recognizing Mistral/Llama-3/Gemma/Phi
  (`src/inference/llama_backend.c:694-696`; pinned llama.cpp
  `cmake/Dependencies.cmake:51`).
- Baselines are saturated (optimized, no-thought, thought-optional-decode-only all
  10/10), so only a WEAKER model creates headroom to detect whether routed thought
  ever *helps* — the lift claim has never been testable.
- Rig correction: the GPU is a **24 GB** RTX 5090 Laptop (24,463 MiB per
  `benchmark/results/2026-08-30-elicited-sweep/environment.json`), not 32 GB.

## Three questions this sweep answers

1. **Portability**: does the cue/ban/trigger mechanism work unchanged on a
   different tokenizer (Tekken) and template family (Mistral, Llama-3)?
2. **Headroom / lift**: on a weak model whose baseline is below 10/10, does
   routed thought raise the pass rate? (First real test of the project claim.)
3. **Stress**: does a model with mediocre JSON discipline (Llama-3.1-8B) break
   the grammar+ban mechanism, and how do failures present?

Any mechanism break found = a phase-2 requirement, recorded, not patched ad hoc.

## Ground rules

- Branch `codex/thought-channel`, **local only — NEVER push** (no authorization,
  no upstream). Local commits on the branch are the established pattern.
- One consolidated results tree **per model**; `benchmark/consolidate_arms.py`
  structurally refuses cross-model merges (identity includes `model_sha256`) —
  that is correct behavior, do not work around it.
- Cross-model comparisons: **pass rates and turn counts only**. Token counters
  (`prompt_tokens`, `generated_tokens`, `grammar_fallback_tokens`) are
  tokenizer-relative — never compare across models, and never compare
  `grammar_fallback_tokens` across routed/non-routed arms even within a model.
- All arms of one model must run on the same forge binary; record
  `forge_binary_sha256` (run.py does this in `environment.json`).
- Greedy/seed-42 comes from forge compiled defaults (`src/inference/inference.c`),
  applied uniformly — fine. Note in the writeup: Devstral recommends temp 0.15
  (near-greedy, on-distribution); Llama-3.1 has no strong low-temp guidance.

## Rig map

- Windows repo: `C:\Users\flowc\dev\forge`, branch `codex/thought-channel`
  (tip was `3b32a88` when this plan was written). Windows builds use the
  repo-pinned cmake at `.tools/cmake/data/bin/cmake.exe`.
- WSL2 rig (Ubuntu, CUDA build, RTX 5090 Laptop 24 GB): clone at `/root/forge`,
  on the branch at `cba84f2` (behind Windows — sync first). Models at
  `/root/models/`. Pre-branch dirty state is in `git stash`; untracked originals
  in `/root/results-backup`. Do not disturb either.
- Sync Windows→WSL via **git bundle** (no network remote): on Windows
  `git bundle create <file>.bundle codex/thought-channel`, copy under `/mnt/c/...`,
  then in WSL `git fetch <bundle> codex/thought-channel && git checkout` the tip.
- WSL `/tmp` is wiped between sessions (VM idle shutdown) — keep ALL outputs
  under `/root/` (e.g. `/root/bench-tier1/`). Archive finished raw runs to
  `C:\Users\flowc\dev\forge-bench-raw\` (existing pattern).
- `wsl.exe` invocations can transiently fail under GPU load with
  `Wsl/Service/0x8007274c` — treat connection failure as "retry in ~10 s", not
  process death. Verify long runs with an explicit ALIVE/DEAD/FINISHED echo.
- WSL build: `cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON`
  then build; verify ctest green before sweeping.

## The models (all verified on HF 2026-08-31; sizes ≈ GiB)

| # | Model | Quant / size | Repo | Template | Why |
|---|-------|--------------|------|----------|-----|
| 1 | Qwen3-4B-Instruct-2507 | Q8_0, 4.3 GB | `unsloth/Qwen3-4B-Instruct-2507-GGUF` | ChatML | Headroom: weak, non-thinking, same template family as baseline → isolates capability. Q8 removes quant confound. |
| 2 | Devstral-Small-2-24B-Instruct-2512 | Q4_K_M, 14.3 GB | `unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF` (base: `mistralai/`) | Mistral/Tekken | Portability: different lab, tokenizer, template; strong agentic coder (SWE-bench Verified 68.0). Text-only use; ignore vision mmproj. Apache 2.0. |
| 3 | Llama-3.1-8B-Instruct | Q4_K_M, 4.9 GB | `bartowski/Meta-Llama-3.1-8B-Instruct-GGUF` | Llama-3 | Stressor: third template family, weak strict-JSON discipline — the honest "any local model" test. Llama 3.1 Community License (fine for benchmarking). |

Download to `/root/models/`. Verify each file's SHA-256 against the value the HF
repo publishes for that exact file (file page / HF API LFS metadata) — required
by the `docs/MODEL.md` provenance pattern. Record: filename, byte size, HF
revision (repo commit), SHA-256, license.

## Steps

### 0. Sync + build
Bundle-sync the WSL clone to the Windows branch tip. Build `build-cuda` at the
tip, run ctest, record the forge commit and binary SHA-256.

### 1. Harness prep (small, do first, commit separately)
`benchmark/run.py` has no `--chat-template` pass-through and does not record
which template was applied. Add: an optional `--chat-template` arg forwarded to
forge, and an `environment.json` field recording the flag value or `"embedded"`.
The three chosen models should not need the override (their templates are in the
pinned llama.cpp's detection set), but the provenance field is needed for honest
reporting, and the escape hatch matters if detection fails (see smoke step).
Test the change on one fixture with the baseline model before proceeding.

### 2. Per-model smoke test (BEFORE any full sweep)
One fixture (e.g. `add`), routed arm, per model:

```
python benchmark/run.py --forge build-cuda/forge --model /root/models/<GGUF> \
  --tasks add --variants thought-routed-decode-only --output /root/bench-tier1/smoke-<slug>
```

Checks (inspect the session dir under the output: `events.jsonl`, `context/`,
model output):
- No turn-1 `FORGE_ERR_MODEL` "Cannot tokenize prompt or apply chat template" —
  that means llama.cpp's heuristic rejected the embedded template; fix by passing
  a llama.cpp template NAME via the new `--chat-template` (supported names are in
  the pinned llama.cpp source, `build*/_deps/llama-src/src/llama-chat.cpp`), and
  record the override.
- The templated prompt in `context/` looks structurally right for the family
  (e.g. `[INST]` for Devstral, `<|start_header_id|>` for Llama-3).
- The routed turn shows the forced `Thought: ` cue followed by prose, then a JSON
  action — not prompt echo, not an immediate `{`, not a limit death at turn 1.
- Baseline smoke too (`--variants optimized`, same fixture) to confirm the model
  can complete the protocol at all.
If a model fails smoke for mechanism reasons, do NOT sweep it — record the
failure mode as a phase-2 requirement and continue with the others.

### 3. Full sweep — 4 arms × 10 fixtures × 3 models
Arms (exact variant names): `optimized`, `no-thought`,
`thought-routed-decode-only`, `thought-routed-required-decode-only`.
(Retention and turn-cap questions are settled — 15/15 discordant pairs
p = 6.1e-05, and the turncap control — do not re-run those per model.)

One `run.py` invocation **per arm** for resumability (prior driver.sh pattern:
loop arms, write progress + DONE marker, run detached under `/root`):

```
python benchmark/run.py --forge build-cuda/forge --model /root/models/<GGUF> \
  --variants <ARM> --output /root/bench-tier1/<slug>/<ARM-dir-as-runpy-lays-out>
```

(Check run.py's output layout before scripting; the elicited sweep used per-arm
invocations into one root per arm set.)

Expected cost basis: the 9-arm elicited sweep was 28.2 min wall on the baseline
MoE (~198 tok/s decode, ~54% of wall = repeated model load, fresh process per
record). This 4-arm subset on the baseline model summed to ≈ 11.4 min. Estimate:
Qwen3-4B ≈ 10 min, Llama-8B ≈ 10–15 min, Devstral 24B dense (~3–4× slower
decode) ≈ 30–45 min. Whole sweep ≈ 1–1.5 h of rig time plus ~24 GB downloads.

### 4. Consolidate + analyze (per model)
- `benchmark/consolidate_arms.py` once per model → per-model
  `results.json` / `thought-census.json`. Census caveat: `classify()` assumes the
  bare-JSON action protocol; with these non-thinking models that is fine, but
  eyeball a few raw outputs per model anyway.
- Tables per model: pass count per arm, turns, failure reasons (limit vs parse vs
  wandering). Within-model comparisons only for token metrics.
- Answer the three questions explicitly:
  1. Portability: did all Devstral/Llama arms run mechanism-clean?
  2. Lift: Qwen3-4B baseline vs routed arms — did routed thought add passes?
     (Also check: does `no-thought` differ from `optimized` off-ceiling?)
  3. Stress: Llama-3.1 failure taxonomy — mechanism failures vs capability.
- Floor warning: if a weak model's routed arms are 0–2/10, the contrast is
  unreadable on these fixtures — report it as a floor, not as "routed hurts".

### 5. Publish + commit (local only)
- `benchmark/results/<date>-tier1-models/<slug>/` per model + a top-level README
  with the cross-model pass/turn table and the caveats below. Verify every README
  claim programmatically against the JSONs (established discipline).
- Add rows to `docs/MODEL.md` (filename, size, revision, SHA-256, context 16,384
  / 2,048 reserved, offload, "Greedy, seed 42", license) for each new model.
- Note in `benchmark/README.md` that multi-model runs are per-model trees.
- Archive raw run roots to `C:\Users\flowc\dev\forge-bench-raw\`.
- Commit in logical chunks on `codex/thought-channel`. Do not push.

## Caveats that MUST appear in the results README

- 24 GB RTX 5090 Laptop rig; greedy/seed-42 for all models (Devstral's
  recommended temp is 0.15 — near-greedy; others have higher recommendations,
  so greedy is somewhat off-distribution for them).
- Forge packs the entire context into ONE user-role message
  (`src/inference/llama_backend.c:68`) — a prompt-fidelity confound that hits
  models with strong system-role priors (Llama-3) harder. Constant across arms
  within a model; a cross-model confound.
- `parse_special=true` on prompt tokenization means each model's special-token
  strings in repo text tokenize as control tokens — fixtures are not guaranteed
  byte-equivalent stimuli across models (unlikely to matter for these fixtures;
  state it).
- Fixture set is 10 tiny Go repairs; 10-percentage-point granularity per arm.

## Known landmines (from the 2026-08-31 audit — check, don't rediscover)

- Silent cue-skip: if `Thought: ` tokenizes to >16 tokens or fails, the cue is
  skipped silently while the brace/EOG bans stay armed — the documented
  10/10-death configuration (`src/inference/llama_backend.c:427-431`,
  `src/internal.h:195-199`). A pending background-task chip exists to harden
  this; if it has not landed, verify cue force-decode is visible in each model's
  smoke-run output before sweeping.
- Unrecognized template → turn-1 hard death; GGUF with NO template metadata →
  silent ChatML imposition (`llama_backend.c:694-696`). Smoke catches both.
- `--gpu-layers auto` degrades non-whitelisted architectures to CPU; run.py's
  default `-1` bypasses this — keep `-1`.
- Independent same-seed runs are not guaranteed identical traces (documented in
  `benchmark/README.md`); treat runs as complete task runs.

## Baseline numbers for comparison (elicited sweep, 2026-08-30, Qwen3-Coder-30B-A3B)

optimized 10/10 · no-thought 10/10 · thought-optional-decode-only 10/10 ·
thought-required 4/10 · thought-required-decode-only 7/10 · thought-routed 8/10 ·
thought-routed-decode-only 8/10 · thought-routed-required 8/10 ·
thought-routed-required-decode-only 7/10. Elicitation 203/203 routed actions.
Retention: 15/15 discordant pairs favor stripping, p = 6.1e-05. Routed failures
are within-turn token-budget deaths, not turn starvation (turncap control).
