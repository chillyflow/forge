# Verdict: does the latest NVIDIA Studio driver's llama.cpp support benefit Forge?

Date: 2026-09-13. Status: **INVALIDATED as a driver benefit; PARTIAL as a code
benefit, and gated on prerequisites Forge does not currently meet.**
All numbers below are from runs executed on this machine under the frozen
preregistration in `PREREGISTRATION.md`. Nothing was re-run for a better
repetition; the negative result stands as recorded.

## Question as asked

"Determine if the latest NVIDIA Studio driver llama.cpp support will benefit Forge."

## What "llama.cpp support" in that driver actually is

The September NVIDIA Studio Driver (616.92, WHQL, released 2026-09-09) states in
its **Applications** section: "The September NVIDIA Studio Driver provides optimal
support for the latest new creative applications and updates including Adobe
Premiere, Blender, Maxon Redshift, and llama.ccp support for CUDA 13.3 and CUDA
13.4." (The typo "llama.ccp" is NVIDIA's.)

Read carefully, that is a **compatibility statement about CUDA toolkit versions**,
not a driver-side performance feature. The same release notes list **Fixed
Application Bugs: N/A**, and the only fixed general bugs are browser flicker,
virtual-display creation and RDP black screens — display-path items.

## Channel 1 — the driver itself: ZERO benefit

| Check | Finding | Evidence |
|---|---|---|
| Does the driver gate CUDA 13.3/13.4 for Forge? | No. CUDA 13.x requires driver **>= 580**; forward/backward compatible across the major family. | NVIDIA CUDA Compatibility Guide |
| What is installed? | Driver **616.56**, reporting **CUDA UMD Version 13.4** — the capability the note describes is already present. | `nvidia-smi` on this machine |
| Same branch? 616.56 -> 616.92 | Yes, within R615/R616. The hop carries display fixes and the known-issue list, no compute or inference change. | 616.92 release notes |
| Risk of the hop | The 616.92 feedback thread is dense with display/HDMI/external-monitor regressions attributed to it. On a laptop, a display regression is a real cost for zero inference return. | NVIDIA GRD feedback thread |

The one link that cannot be measured without installing 616.92 and rebooting is
the assertion "the driver hop yields exactly 0.0%." It is not asserted as
measured; it is inferred from the release notes, the already-satisfied CUDA
compatibility requirement, and the absence of any compute-path fix. Installing a
driver is also outside a benchmark's blast radius, so it was not done.

## Channel 2 — the llama.cpp code those notes are gated on: measured, not material

NVIDIA's real llama.cpp claims (2x on Qwen3.6-27B, 1.6x on Qwen3.6-35B-A3B, up to
1.9x general throughput on RTX 5090) come from **upstream llama.cpp commits** —
multi-token prediction (MTP) speculative decoding, programmatic dependent launch
(PDL), and tensor parallelism — not from the driver. Distribution is explicitly
via upstream, LM Studio, Ollama and the llama.cpp webUI.

**First finding: Forge's pin already contains the features NVIDIA is promoting.**
Verified in `build/_deps/llama-src` (pin `bb4caa754`, v0.2.0, 2026-08-21):

- `common/arg.cpp` — `COMMON_SPECULATIVE_TYPE_DRAFT_MTP` (MTP spec type present)
- `ggml/src/ggml-cuda/common.cuh` — `GGML_CUDA_USE_PDL`, `ggml_cuda_pdl_sync()`,
  `ggml_cuda_pdl_lc()` (PDL present, engaged at `>= GGML_CUDA_CC_HOPPER`; the
  RTX 5090 Laptop is compute capability 12.0, so PDL is in range)

**Second finding: the MTP headline is structurally unreachable for Forge's models.**
MTP requires a model that ships MTP/NexN draft heads. Parsing Forge's working GGUF
directly:

- `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`: architecture **qwen3moe**,
  579 tensors, **zero** MTP/NextN/draft-like metadata keys or tensor names.
- The machine's other two GGUFs are `Qwen3-30B-A3B-Thinking-2507` and
  `Devstral-Small-2-24B`; no Qwen3.6-MTP model is present.

**Third finding: Forge could not use MTP even with such a model.** Forge's own
backend does ordinary token-by-token decoding and rejects the setting:
`src/core/config.c:350` — "speculative decoding is not implemented";
`docs/DESIGN_CHECKLIST.md` §31/24 records draft-model and n-gram speculation as
**Missing** (no draft configuration, verification path, acceptance counters or
mode comparisons).

**Fourth finding: the residual code delta is below the materiality bar.** With
CUDA redistributables proven byte-identical between arms (so the only variable is
the ggml/llama code version), the latest upstream release does not beat Forge's
pin on Forge's own model:

| Test | Arm A — Forge pin `bb4caa754` (10566) | Arm B — upstream `b10948` | Delta | Within-arm CV |
|---|---|---|---|---|
| pp512 (prefill) | 5150.52 t/s | 5285.09 t/s | **+2.61%** | 0.82% / 1.07% |
| tg128 (decode) | 230.18 t/s | 224.77 t/s | **-2.35%** | 0.35% / 1.36% |

Bar (frozen before run 1): material benefit required **>= +5.0%** on either test
with within-arm CV < 5%. **NOT MET.** Prefill moves marginally forward, decode
moves marginally backward; neither is a reason to touch the pin.

## What worked

- The two channels were separable, and separating them before running anything is
  what made a cheap, decisive experiment possible.
- Byte-identical CUDA redistributables across arms eliminated the obvious confound.
- Interleaved A,B x3 with tight within-arm CV (0.35–1.36%) gave a trustworthy
  negative within ~90 seconds of GPU time.
- Preregistering the bar made "not material" an outcome rather than an argument.

## What didn't

- The preregistered hypothesis that the driver channel is zero is *inferred*, not
  measured — no driver was installed.
- **Mechanism engagement was not counted** (RUN_EFFICIENCY rule 1.10). llama-bench
  reports no per-kernel or PDL-engagement telemetry, so "the new code path fired
  and was useless" cannot be separated from "the new code path did not fire for
  this configuration." The +2.61%/-2.35% split is consistent with a
  flash-attention quant-selection change (upstream replaced `GGML_FA_ALL_QUANTS`
  with `GGML_FA_QUANTS` on 2026-09-09) altering defaults between the two builds —
  a hypothesis, not a finding; it was not tested.
- Three reps, one fixture, one model. This is sufficient to reject a >= 5%
  materiality claim; it is **not** sufficient to assert a decode regression.

## Surprises

- The direction was not the expected one. NVIDIA's own list of CUDA decode-side
  work since the pin (branchless Q4_K/Q5_K `mmvq` unpack, MoE expert fusion and
  reduced expert-reduction overhead) targets exactly Forge's configuration — MoE
  with Q4_K_M — and still did not produce a decode win here.
- The driver's llama.cpp mention is *cosmetic* — a support-matrix line about CUDA
  toolkit versions, sitting beside a feature the installed driver already has.

## Recommendation for Forge

1. **Do not update the driver for llama.cpp reasons.** Zero expected inference
   return; non-zero display-regression risk. Update it for display reasons or not
   at all.
2. **Do not bump the llama.cpp pin for speed.** The measured delta is +2.61%
   prefill / -2.35% decode against a frozen +5% bar. A pin bump is justified only
   when a specific bug or feature is needed.
3. **If NVIDIA's headline is the actual goal, the lever is a model plus a Forge
   feature, not a driver:** add a Qwen3.6-MTP-class GGUF *and* implement the
   speculative-decoding path (`src/core/config.c` currently rejects it). Only then
   is the advertised 1.7–2x class of gain on the table — and it should be
   preregistered and measured like this one was, on Forge's own fixture, because
   the vendor numbers are per-model and per-hardware (RTX 5090 **desktop**).
4. **Keep this run as the frozen baseline.** Forge's current, machine-measured
   reference on `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M`, 49/49 layers on GPU:
   **pp512 ~5150 t/s, tg128 ~230 t/s.**

## Evidence retained

- `PREREGISTRATION.md` — bar, arms, schedule, stopping rule, falsifiable expectation
- `run_ab.sh`, `schedule.log` — the frozen schedule and cell timestamps
- `arm{A,B}-rep{1,2,3}.md` / `.stderr` — all six cells, raw
- `SUMMARY-stats.json` — parsing, medians, CVs, bar evaluation
- `control-hashes.txt`, `treatment-hashes.txt` — binary and CUDA DLL hashes
- Treatment binaries (not committed): `%LOCALAPPDATA%\Temp\forge-llama-b10948\B\`
