# Preregistration — does the NVIDIA "llama.cpp support" benefit Forge?

Written 2026-09-13, **before the first benchmark run**. Frozen bar; no re-running
for a better repetition; every run retained, including failures.

## Question

NVIDIA's September Studio driver (616.92, released 2026-09-09) lists in its
*Applications* section: "…and llama.ccp support for CUDA 13.3 and CUDA 13.4."
Separately NVIDIA's IFA/COMPUTEX material claims up to 1.9x–2x llama.cpp
throughput on RTX 5090 from upstream optimizations. Does either reach Forge?

## Decomposition (the two channels are separable)

- **Channel 1 — the driver.** Not measurable without installing driver 616.92 and
  rebooting. The machine is on 616.56 (CUDA UMD 13.4). This channel is decided by
  release-note evidence, not by this benchmark.
- **Channel 2 — the llama.cpp code the driver notes are gated on.** Directly
  measurable, and it is the channel that carries all the claimed throughput.

## Arms

| Arm | Build | Provenance |
|---|---|---|
| A (control) | llama.cpp v0.2.0, source pin `bb4caa7540188872173c44d161602d9271386413` (2026-08-21) | Forge's `.tools/llama-cuda` prebuilt — the exact runtime Forge links against |
| B (treatment) | upstream `b10948` (2026-09-13), CUDA 13.3 x64 prebuilt | official ggml-org release |

Only the code version differs. Same instrument (`llama-bench`), same model file,
same GPU, same CUDA major version, official x64 prebuilts for compute capability
12.0 in both arms.

## Frozen inputs

- Model: `C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`
  SHA-256 `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad`
  (Forge's actual working model, MoE 30.5B total / 3.3B active, Q4_K_M.)
- Instrument: `llama-bench -m <model> -p 512 -n 128 -ngl 99 -r 3`
- Schedule: interleaved A,B,A,B,A,B → 3 invocations × 3 internal reps =
  9 samples per arm. Interleaving is to cancel thermal/clock drift.
- Machine otherwise idle (checked: no GPU compute processes before launch).

## Bar (frozen before the first run)

**Material benefit for Forge requires** median **tg128 (decode) ≥ +5.0%** or
median **pp512 (prefill) ≥ +5.0%** for arm B over arm A, with within-arm
coefficient of variation < 5% on both arms.

Below that: **not material** — recorded as a negative result, not reframed.

## Stopping rule

No re-runs for a better repetition. A failed or interrupted invocation is
reported as a lost cell, never rescheduled to improve the average.

## Pre-registered expectation (stated so it can be falsified)

The driver channel is expected to be **zero** (same R615/R616 branch; the 616.92
release notes fix only display/browser/RDP bugs and list "Fixed Application
Bugs: N/A"; the installed 616.56 already reports CUDA UMD 13.4). The llama.cpp
channel is expected to be **nonzero but model-dependent**: Forge's models are
Qwen3-era MoE with no MTP heads, so the MTP-driven headline gains are not
reachable; any real gain should come from decode/MoE kernel work.
