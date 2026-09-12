# Spike 001 — is N-way batched decode affordable?

**Question.** Can a laptop agent produce N candidate continuations for far less
than N× the wall time, by decoding them in one batch through one set of weights?

**Why it decides anything.** Cloud harnesses are built around token scarcity:
candidates cost money, so they are generated one at a time and share a single
token budget. Forge's own `--candidates` does exactly that — it generates
sequentially and divides the remaining budget by the candidates left, so adding
candidates makes each candidate *weaker*. If decode is bandwidth-bound and
batching is near-free, that design is backwards and reliability-through-
redundancy (generate N, verify, keep a passer) becomes a real strategy. If
batching costs ~N×, the idea is dead and Forge should stop pursuing it.

## Pre-registered bar

Frozen before the measurement run (the `steps=8` smoke test was instrument
validation only — at that step count the decode phase barely runs).

| decode_x at N=8 | verdict | meaning |
| --- | --- | --- |
| ≥ 3.0× | **VALIDATED** | 8 candidates' decode costs ≤ 1/3 of 8 separate generations; the economics invert and the strategy is worth building |
| 1.5× – 3.0× | **PARTIAL** | real but modest; only worthwhile if candidates are cheap for other reasons |
| < 1.5× | **INVALIDATED** | batching buys little; abandon reliability-through-redundancy |

`prefill_x` should be ≈ N, since the batched path prefills once and forks the KV
cache. If it is not ≈ N, prefix sharing via `llama_memory_seq_cp` is not doing
what this spike assumes and the whole shape is wrong.

## Method

`sweep.c` loads the real model and, for N in {1,2,4,8}, produces N continuations
two ways:

- **sequential** — N cold runs, each clearing the KV, prefilling the 4,114-token
  prompt alone, and decoding alone. This is what `--candidates` does today.
- **batched** — one prefill into sequence 0, fork the KV N ways with
  `llama_memory_seq_cp`, then decode all N in a single batch per step.

Config mirrors `src/inference/llama_backend.c` (`n_ctx` 8192, `n_batch` 512,
`n_ubatch` 256, all layers on GPU) and Forge's campaign sampling
(`top_k` 20, `top_p` 0.8, `temp` 0.6), with a **distinct RNG seed per
sequence** so candidates diverge. Greedy decoding would keep all N identical,
which would keep MoE expert routing identical and flatter the batched result.

Prefill and decode are timed separately, with `llama_synchronize` before each
boundary. Timing only the total would have been misleading: at short step counts
the prefill dominates and reports a fake speedup.

Prompt: a real rendered prompt from a prior verified session
(`session/context/0008.txt`, 4,114 tokens), not a synthetic string.

## Results

Model: Qwen3-Coder-30B-A3B-Instruct-Q4_K_M, all layers on GPU (RTX 5090 Laptop,
24 GiB). Prompt: 4,114 real tokens. 127 generated tokens per candidate. Zero
decode failures.

| N | seq_prefill | seq_decode | bat_prefill | bat_decode | prefill_x | decode_x |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 1454 ms | 735 ms | 1476 ms | 722 ms | 0.99× | 1.02× |
| 2 | 2957 ms | 1437 ms | 1478 ms | 875 ms | 2.00× | 1.64× |
| 4 | 5920 ms | 2892 ms | 1478 ms | 1368 ms | 4.00× | 2.11× |
| 8 | 11890 ms | 5940 ms | 1487 ms | 2045 ms | **8.00×** | **2.90×** |

Aggregate decode throughput: 176 → 290 → 371 → **497 tok/s** while serving
1 → 2 → 4 → 8 sequences. Per-token decode cost falls from 5.69 ms to 2.01 ms
(35% of single-stream). Batched prefill is flat at ~1,478 ms regardless of N,
confirming `llama_memory_seq_cp` shares the prefix rather than duplicating it.

## Verdict: PARTIAL

**The decode bar is missed.** `decode_x` at N=8 is **2.90×**, against a
pre-registered threshold of 3.0× for VALIDATED. That is a miss, narrowly, and it
is not reclassified: the bar was frozen before the run and 2.90 sits in the
1.5–3.0× PARTIAL band.

**The prefill half is exactly as predicted.** `prefill_x` = 8.00× at N=8 —
perfectly linear, which is what the prefix-sharing design assumes.

### What the pre-registered bar missed

The bar isolated decode, which turned out to be the *smaller* half of the
effect. End to end, for N=8:

| path | total |
| --- | --- |
| sequential (what `--candidates` does today) | 11890 + 5940 = **17,830 ms** |
| batched (one prefill, forked, one decode) | 1487 + 2045 = **3,532 ms** |

**5.05× cheaper overall.** Restated the way that actually decides things: one
candidate today costs 2,189 ms, and **eight batched candidates cost 3,532 ms —
1.61× the price of one.**

This is recorded as an additional observation, not as a reclassification. The
pre-registered bar was the wrong *single* bar to freeze, because affordability is
a function of prefill and decode together, and freezing only the decode half
understates the result. That is a lesson about the bar, not a licence to move it.

### Caveat that will bite in production

Candidate length shifts the balance against batching. At 512 generated tokens
per candidate the composite falls to roughly **3.6×** (prefill stays fixed while
decode grows 4×). The prefill saving is length-invariant; the decode saving is
not. Longer candidates and longer prompts trade off differently, and a real
implementation should re-measure at its actual generation budget.

### What worked

- `llama_memory_seq_cp` prefix sharing: exactly linear, no duplication.
- Batched decode through one weight read: per-token cost down to 35%.
- Divergent candidates (per-sequence RNG seed) — the measurement is not
  flattered by identical sequences keeping MoE routing constant.

### What didn't

- Batched decode is **sublinear, not free**: 2.90× for 8 streams, not 8×. MoE
  expert routing scatters across divergent branches, so the shared weight read
  buys less than a dense model would give.
- The pre-registered decode bar was not met.

### Recommendation for the real build

Worth building, on the strength of the composite (1.61× the cost of one
candidate for eight), but with the honest expectation that the win is mostly
**prefill amortization**, not free decode. Concretely, in `candidate_search.c`:

1. Prefill once and fork with `llama_memory_seq_cp` instead of re-running each
   candidate cold.
2. Decode candidates in one batch (`n_seq_max = N`) rather than sequentially.
3. **Stop dividing the budget by candidates left.** That is the token-scarcity
   assumption; it makes each candidate weaker as N grows. Give each candidate
   its own budget — memory arithmetic allows it: 8 branches of 512 tokens on a
   4,114-token prompt is ~810 MB of KV against 24 GiB of VRAM and a 17.28 GiB
   model.
4. Verify all N against the host tests Forge already runs, and keep a passer.
   This is the part cloud harnesses cannot afford and the reason the strategy
   exists.

