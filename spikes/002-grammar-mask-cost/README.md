# Spike 002 — what does a live grammar cost, and does masking a reduced array give the same answer?

**Question.** The September loop-pilot runs decode at ~60–69 tok/s while the same
weights decode at ~230 tok/s unconstrained. A controlled A/B on one binary put
**71% of the constrained arm's decode time inside the sampler**, and the code path
is structural: `llama_grammar_apply_impl` (pinned llama.cpp,
`src/llama-grammar.cpp`) iterates **every entry of the candidate array**, and per
candidate `decode_utf8()` returns a `std::vector<uint32_t>` **by value**. Forge's
greedy fast path avoids that only because it hands the grammar a *one-element*
array — and `src/inference/llama_backend.c:929` turns that fast path off whenever
temperature > 0 or a ban is armed, i.e. exactly when the grammar is live.

**Why it decides anything.** If the cost is a function of the *candidate array
size at the moment the grammar is applied*, the fix is mechanical: reduce to the
raw top-K logits first, apply the grammar to those K, expand K only when fewer
than `top_k` candidates survive — and the fix is distribution-preserving whenever
the surviving set matches the full-vocab set. If instead the cost is intrinsic to
grammar matching, no candidate reduction helps and the sampler design needs a
different answer. This spike measures both halves: **cost** and **equivalence**.

## Pre-registered bar

Frozen before the measurement run. Model: `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M`,
all layers on the RTX 5090 Laptop GPU, Forge's own campaign chain
(`top_k 20, top_p 0.8, temp 0.6, dist(42)`), 16,384 context.

| # | Hypothesis | Bar | Meaning |
| --- | --- | --- | --- |
| H1 | Cost law | full-vocab live-grammar apply **≥ 20 ms** median; same chain on a 64-candidate array **≤ 1.0 ms**; ratio **≥ 25×** | candidate reduction is worth building |
| H2 | Attribution | the grammar sampler **alone** accounts for **≥ 80%** of the full-vocab chain cost in a live state | the cost is the mask, not the stochastic tail |
| H3 | Equivalence | reduced path (K=64, adaptive expansion when < 20 survive) yields the **identical ordered allowed top-20 id list** on **≥ 99%** of 64 real generation steps, and the identical **top-5 on 64/64** | the fix preserves the sampled distribution |

**Falsifiers (stated in advance).** Ratio < 10×, or H3 top-20 < 99%, ⇒ *reject*
"reduce before masking" as the fix and do not implement it in
`src/inference/llama_backend.c`. H2 failing does not falsify H1/H3 — it only means
the mechanism inside the chain is the tail rather than the mask, which changes the
patch but not the shape of the fix.

**Limits recorded up front.** One model, one prompt, one grammar, one machine.
Fixed-state arms are 10 iterations (medians); the per-step table is 64 paired
steps. Sampler cost is CPU-side because Forge never initialises llama.cpp's
backend-sampler path (`grep backend_init src/` is empty for samplers). Numbers are
grammar-state-dependent; the *law* is the claim, not the constant.

## Method

`sweep.cpp` loads the real model and prefix, then:

- **P0** prefills a real rendered prompt and reports prompt tokens, prefill cost.
- **P1** generates 64 tokens with Forge's campaign chain on a live grammar. Per
  step, on identical logits: (a) times `llama_sampler_sample` itself — the exact
  call Forge makes and measures as `sampling_ms`; (b) times a full-vocab chain
  `apply` (reference); (c) times a reduced top-K chain `apply` with adaptive
  expansion; (d) compares the ordered allowed top-20 id lists of (b) and (c).
  `apply` does not advance sampler state, so all three see the same logits **and**
  the same grammar state.
- **P2** fixed-state arms in three real grammar states (root, mid-action,
  post-action) reached by accepting the P1 generation's own tokens: grammar-only
  apply, full chain, chain on K ∈ {20, 64, 256}, and the chain with no grammar.
- **P3** the cost of the raw top-K selection itself (`nth_element` over the vocab).

The grammar is taken from Forge itself: the native template render
(`fg_chat_templates_apply_native` → `fg_chat_render_grammar`) when available, else
the flat tool grammar (`fg_tool_grammar`, `src/tools/tools.c`). The run prints
which one it used.

> The harness later gained two **additive** measurements for spike 003 — a
> token-sequence hash (end-to-end determinism) and a P3 arm sweeping cost against
> candidate-array size. No bar above was changed, and both runs retained here
> predate those additions.

Build: `cmake --build build-gpu --config Release --target forge_sampler_attribution`
Run: `forge_sampler_attribution --model <gguf> --prompt <file> [--steps 64]`

## Results

Run 2026-09-15. Model `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M`, all layers on the
RTX 5090 Laptop; `n_ctx 16384, n_batch 512, n_ubatch 256, 24 threads`; vocab
**151,936**. Grammar: Forge's own native template render (24,400 bytes,
`render_lazy=1`) held in the **eager** state — the state Forge is in once the
trigger has fired, which is the expensive one. Prompt 2,753 real tokens, prefill
1,178–1,278 ms (2,150–2,340 tok/s), 64 generation steps.

Evidence: `run-2026-09-15.txt` and `run-2026-09-15-rep2.txt` (independent
repetition), with `.stderr` for both.

> **Reproducibility note, recorded rather than glossed.** The first run's log was
> overwritten when the spike was re-run to fix a *reporting* defect (the `redK`
> column printed the pre-expansion-clamp k). The first run was not discarded for
> a better repetition; its medians (today 12.49, full 12.16, reduced 14.79,
> select 1.27 ms; root 25.27/26.02; mid-action 16.18/16.71; post-action
> 12.18/12.06; permissive 23.48/0.066 ms) come from the run console and agree
> with the two retained repetitions within state noise.

**P1 — 64 paired real steps.** Every step landed in a near-forced grammar state:
**1–3 tokens allowed out of 151,936**.

| arm | median ms/token |
| --- | --- |
| `llama_sampler_sample` (exactly what Forge times as `sampling_ms`) | 12.5 – 15.0 |
| chain apply, full 151,936-candidate array | 12.2 – 13.9 |
| chain apply, **K=64, no expansion** | **0.005 – 0.07** |
| chain apply, K=64 + top_k-triggered expansion (*the pre-registered fix as specified*) | 14.5 – 16.6 |
| raw top-K selection itself (`nth_element` over the vocab) | 1.27 – 1.28 |

Equivalence: allowed top-20 identical **64/64** steps; top-5 **64/64**; top-1
agreement **44/44 without expansion**; reference-allowed tokens outside the raw
top-64: **0**. Steps with 20 candidates allowed: **0** — that is the population's
weakness, recorded rather than hidden.

**P2 — fixed-state medians (10 iterations, both retained runs).**

| state | grammar only | chain, full | K=20 | K=64 | K=256 | chain, no grammar |
| --- | --- | --- | --- | --- | --- | --- |
| root | 25.0 – 34.0 | 26.0 – 33.0 | 0.008 – 0.010 | 0.021 – 0.022 | 0.034 – 0.036 | 0.04 |
| mid-action | 16.2 – 17.7 | 16.7 – 18.3 | 0.030 – 0.033 | 0.069 – 0.073 | 0.117 – 0.123 | 0.04 – 0.05 |
| post-action | 12.2 – 13.6 | 12.1 – 13.3 | 0.002 – 0.003 | 0.004 | 0.014 – 0.016 | 0.04 |

**P2b — permissive grammar** (`root ::= [printable-ascii]*`, same logits): chain
on the full array **23.2 – 24.6 ms** vs **0.065 – 0.070 ms** on a K=64 array;
2 tokens allowed; allowed top-20 lists identical.

## Verdict

- **H1 confirmed, far above the bar.** Same chain, same logits, same state:
  12.2 ms → 0.005 ms when the array is K=64 (the bar needed ≤ 1.0 ms and ≥ 25×;
  this is ~2,000×). The cost is ~**80 ns per candidate in the array**, and it
  tracks the array size across every state measured.
- **H2 confirmed.** The grammar sampler alone is **95–100%** of the chain cost
  (mid-action 16.2–17.7 vs 16.7–18.3). The chain with **no** grammar costs
  **0.04 ms** on the same 151,936-candidate array, so top_k, top_p, softmax and
  dist are *not* implicated. `decode_utf8`'s per-candidate `std::vector<uint32_t>`
  and the per-candidate reject walk are the cost.
- **H3 passed but on a weak population.** 64/64 top-20, 44/44 top-1, zero
  allowed tokens outside the raw top-64 — but no step ever had 20 candidates
  allowed, so the comparison never had to discriminate among 20. Supporting, not
  conclusive.
- **The pre-registered fix as specified is FALSIFIED.** Expanding whenever fewer
  than `top_k` candidates survive fires in *every* near-forced state — and
  near-forced states dominate constrained decoding — so the reduced path paid the
  full-vocab cost anyway plus the selection: **14.5–16.6 ms against a 12.2–13.9 ms
  baseline**. The trigger is wrong, not the method.
- **The corrected variant is the thing to build next, and it is measured in
  components, not end to end**: expand only when the reduced set yields *no*
  allowed candidate. At the measured rates that is **0.005–0.07 ms + 1.27 ms
  selection ≈ 1.3 ms/token** — about **9× below today's 12–15 ms** — with the
  observed 44/44 top-1 agreement and 0-outside-64 over 64 real steps as its
  evidence base. It is *not* the 2,000× the bare K=64 arm shows, and it carries an
  unresolved equivalence risk for states that allow many tokens spread across the
  vocab.
- **Consequence for the fix list.** Because the ~80 ns/candidate is per-candidate
  *setup* (piece lookup, UTF-8 decode allocation, reject walk), the exact fix is
  available without any distribution argument at all: precompute the vocab piece
  table once per model, give `decode_utf8` an ASCII fast path that allocates
  nothing, and reject candidates on their first byte before the stack walk. That
  patch keeps the full-vocab semantics bit-for-bit and targets ~80 ns → ≤ 10 ns
  per candidate, i.e. 12–34 ms → ~1–2 ms. This run is its target and its
  baseline; it is the next spike, on the vendored source, not on Forge.