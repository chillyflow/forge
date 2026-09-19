# Draft answer: does the latest Studio driver's llama.cpp support benefit Forge?

**Determination: no material benefit — and the premise needs correcting before the
question can even be answered.** The driver's llama.cpp entry is a compatibility
line about CUDA toolkit versions, not a performance feature, and the throughput
NVIDIA is actually advertising lives in upstream llama.cpp code.[1][4]

## The driver entry is about CUDA versions, not speed

The September Studio Driver (616.92, WHQL, 2026-09-09) says in its *Applications*
section that it provides "optimal support for ... Adobe Premiere, Blender, Maxon
Redshift, and llama.ccp support for CUDA 13.3 and CUDA 13.4."[1] The same notes
list **Fixed Application Bugs: N/A**, and the only fixed general bugs are browser
flicker, virtual-display creation and RDP black screens.[1] CUDA 13.x requires
only driver >= 580 and is backward compatible across the 13.x family.[2] This
machine already runs driver 616.56 and already reports **CUDA UMD Version 13.4**,
so the capability that note describes is present before any upgrade, and 616.56 to
616.92 is a same-branch hop carrying display fixes.[1][2] The 616.92 feedback
thread is dense with display and external-monitor regressions attributed to that
hop — a real cost for zero inference return.[7]

## The advertised gains are upstream llama.cpp work, and Forge's pin already has it

NVIDIA attributes its llama.cpp numbers — up to 2x on Qwen3.6-27B and 1.6x on
Qwen3.6-35B-A3B on GeForce RTX 5090 — to multi-token prediction (MTP) speculative
decoding plus programmatic dependent launch, distributed through upstream
llama.cpp, LM Studio and Ollama rather than through the driver.[3] Independent
coverage puts the general claim at up to 1.9x on RTX 5090 and confirms the
optimizations ship in the open-source projects.[4] Forge's pinned llama.cpp
(`bb4caa754`, v0.2.0, 2026-08-21) **already contains both**: its `common/arg.cpp`
defines the `DRAFT_MTP` speculative type and its `ggml-cuda/common.cuh` defines
`GGML_CUDA_USE_PDL` with `ggml_cuda_pdl_sync()`/`ggml_cuda_pdl_lc()` engaged at
Hopper-and-above, which covers this machine's compute capability 12.0.

## The headline number is unreachable for Forge's models — measured, not assumed

MTP needs a model that ships draft heads: llama.cpp merged MTP support in May 2026
for MTP-specific GGUFs, driven by `--spec-type draft-mtp`.[5][6] Parsing Forge's
working GGUF directly shows `qwen3moe`, 579 tensors and **zero** MTP/NextN or
draft-like tensors or metadata keys. Even with such a model, Forge could not use
it: its backend does ordinary token-by-token decoding and its own config rejects
the setting — `src/core/config.c:350`, "speculative decoding is not implemented",
with `docs/DESIGN_CHECKLIST.md` §31/24 recording speculation as missing.

## The residual code delta, measured against a frozen bar

With CUDA redistributables proven byte-identical across arms, so that only the
llama/ggml code version varies, I ran an interleaved A/B (A,B x3, 3 reps each) on
`Qwen3-Coder-30B-A3B-Instruct-Q4_K_M` and pre-registered a +5% materiality bar
before the first run. Forge's pin (build 10566) measured **5150.52 t/s** prefill
and **230.18 t/s** decode; upstream `b10948` measured **5285.09 t/s** and
**224.77 t/s** — **+2.61%** prefill and **-2.35%** decode, against within-arm
coefficients of variation of 0.35-1.36%. The bar was not met (local evidence:
`benchmark/results/2026-09-13-nvidia-llamacpp-support/`). The negative
result is retained as recorded; no run was repeated for a better number.

## What I recommend

Do not update the driver for llama.cpp reasons: zero expected inference return and
a non-trivial display-regression risk. Do not bump the llama.cpp pin for speed
either; +2.61% prefill and -2.35% decode does not justify a pin change. If
NVIDIA's headline is the actual goal, the lever is a model plus a Forge feature:
add a Qwen3.6-MTP-class GGUF and implement the speculative-decoding path that
`config.c` currently rejects. Note the unmeasured link honestly: the claim "the
driver hop yields exactly zero" is inferred from the release notes, the
already-satisfied CUDA compatibility requirement and the absence of any
compute-path fix, not measured, because installing a display driver is outside a
benchmark's blast radius.

## Sources

[1] https://www.nvidia.com/en-us/drivers/details/278455 — NVIDIA Studio Driver 616.92 | Windows 11 (official release notes)
[2] https://docs.nvidia.com/deploy/cuda-compatibility/minor-version-compatibility.html — CUDA Compatibility — Minor Version Compatibility (NVIDIA docs)
[3] https://blogs.nvidia.com/blog/rtx-ai-garage-computex-spark-local-agents — NVIDIA RTX AI Garage — Faster Local AI Agents on RTX PCs and DGX Spark
[4] https://tech-insider.org/nvidia-local-ai-24gb-vram-gpus-1-9x-boost-2026 — NVIDIA Local AI: 24GB VRAM GPUs Get 1.9x Boost (Tech Insider, 2026-09-04)
[5] https://mer.vin/2026/05/run-qwen-3-6-mtp-in-llama-cpp-faster-local-inference-with-built-in-speculative-decoding — Run Qwen 3.6 MTP in llama.cpp (MTP merged 16 May 2026, PR #22673)
[6] https://forums.developer.nvidia.com/t/mtp-llama-cpp-a-look-at-qwen3-6-27b/370298 — MTP+llama.cpp: a look at Qwen3.6-27B (NVIDIA Developer Forums)
[7] https://www.nvidia.com/en-us/geforce/forums/game-ready-drivers/13/590373/geforce-grd-61692-feedback-thread-released-9926 — GeForce GRD 616.92 Feedback Thread (Released 9/9/26)
