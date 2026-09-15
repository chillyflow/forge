# Spike 003 — does removing the per-candidate allocations in the grammar mask work?

**Question.** Spike 002 attributed the constrained-decode cost to the grammar mask
alone: the chain over a full 151,936-candidate array costs 12–34 ms per token
while the same chain over 64 candidates costs 0.005–0.07 ms, and the chain with
*no* grammar costs 0.04 ms. That is **~80 ns per candidate**, and the pinned
implementation
(`build/_deps/llama-src/src/llama-grammar.cpp`, `llama_grammar_apply_impl`)
spends two heap allocations per candidate: `decode_utf8()` returns
`std::pair<std::vector<uint32_t>, llama_partial_utf8>` **by value**, and the
caller copies that vector into `candidates_decoded`.

**Why it decides anything.** If the cost is per-candidate *setup*, the fix is
exact — no distribution argument, no equivalence risk — and it belongs upstream
in llama.cpp rather than in Forge. If the allocations are not where the time
goes, the exact route is closed and only candidate reduction (spike 002's
bounded, riskier path) remains.

## Pre-registered bar

Frozen before the measurement runs. Because this machine has **no CUDA toolkit**
(`nvcc` absent), llama.cpp cannot be built from source with CUDA here; the patch
can only be exercised in a **CPU source build**, and `build-gpu`'s
`FORGE_LLAMA_PREBUILT=.tools/llama-cuda` runtime is a released DLL that no local
patch can affect. The run is therefore a **paired A/B inside one source build
tree**, which is what the claim needs: same tree, same machine, one variable.

| # | Hypothesis | Bar | Meaning |
| --- | --- | --- | --- |
| H1 | Cost | patched full-vocab grammar `apply` **≥ 5× faster** than unpatched — median over ≥ 10 iterations in each of ≥ 3 grammar states, and in the real per-step path | the allocations are the cost |
| H2 | Semantics | allowed top-20 lists identical on **every** step; top-1 agreement **100 %**; the generated sequence for seed 42 has the **same FNV-1a hash** before and after | the patch changes nothing observable |
| H3 | Deployment | the patch is carried as a `PATCH_COMMAND` file and applies cleanly to a pristine `bb4caa754` extraction | it is landable, not a local hack |

**Falsifier, stated in advance.** Slower than 2× ⇒ the allocation theory is
wrong; do not land the patch and do not propose it upstream.

**Limits recorded up front.** CPU-only build: absolute milliseconds are *not*
comparable to spike 002's GPU-build numbers (the sampler work is CPU-side in both,
so the **ratio** is the transferable quantity). One model, one prompt, one
grammar, one machine. The evidence is a paired A/B, not a population study.

## Method

1. `git apply` the patch — `cmake/patches/llama-grammar-apply-alloc.patch`,
   applied to the populated source tree the same way the new `PATCH_COMMAND`
   applies it to a fresh extraction.
2. Build the source-mode tree: `cmake -B build-src -DFORGE_WITH_LLAMA=ON
   -DFORGE_LLAMA_PREBUILT=` (empty ⇒ source build) `-DFORGE_BUILD_TESTS=OFF`,
   target `forge_sampler_attribution` — spike 002's harness, unchanged except for
   a printed token-sequence hash.
3. Run unpatched, then patched, into retained logs, same arguments both times.

Run: `forge_sampler_attribution --model <gguf> --prompt <file> --steps 24`

## Results

Run 2026-09-15. Both runs on the same machine, **same source build tree**
(`build-src`), same model, same prompt (2,753 tokens), same grammar, `--steps 24`.
The only variable between the two runs is the 45-line patch.

> **Why a CPU source build.** `build-gpu` sets `FORGE_LLAMA_PREBUILT=.tools/llama-cuda`,
> so its `llama.dll` is a **released llama.cpp binary** — no local patch can be
> exercised there. This machine has no CUDA toolkit (`nvcc` absent), so llama.cpp
> cannot be built from source with CUDA either. Absolute milliseconds here are
> therefore *not* comparable to spike 002's GPU-build numbers; the **ratio** is
> the transferable quantity, because the mask is CPU work in both builds.

**Paired A/B — patch applied by the build's own `PATCH_COMMAND`, verified in the
binary** (patch file 10:37:47 → source patched by the build 10:44:21 →
`llama-grammar.obj` recompiled → `forge_sampler_attribution.exe` relinked
10:44:25).

| arm | unpatched | patched | speedup |
| --- | --- | --- | --- |
| P1 `llama_sampler_sample` (what Forge times as `sampling_ms`) | 25.08 ms | 20.51 ms | **1.22×** |
| P1 full-vocab chain apply | 23.07 ms | 19.53 ms | **1.18×** |
| P2 mid-action: grammar sampler alone | 29.00 ms | 27.44 ms | **1.06×** |
| P2 mid-action: full chain | 29.35 ms | 27.84 ms | **1.05×** |
| P2b permissive: full chain | 29.99 ms | 23.55 ms | **1.27×** |

Semantics: identical generated sequence both runs — FNV-1a hash `21c6df5710bb7794`
over 24 tokens, first ids `27 14172 13429 29 198 27 1688 89871` — and
allowed-top-20 lists identical on 24/24 steps, top-1 agreement 15/15,
0 reference-allowed tokens outside the raw top-64. **The patch changes nothing
observable**, as intended.

**Where the cost actually is** — the P3 size sweep, stock grammar, mid-action
state, uniform random candidate samples, 10 iterations each:

| candidates in the array | apply | ns/candidate |
| --- | --- | --- |
| 1,024 | 0.083 ms | 81.0 |
| 4,096 | 0.321 ms | 78.3 |
| 16,384 | 1.403 ms | 85.6 |
| 65,536 | 13.037 ms | **198.9** |
| 151,936 | 31.220 ms | **205.5** |

**Cross-checks (`run-2026-09-15-gpubuild-24step.txt`, spike 002's directory).**
The same harness re-run against the **prebuilt-DLL GPU build** reproduces the
curve — 74.6 / 85.3 / 78.0 ns per candidate up to 16,384, then **175.0** at 65,536
and **179.1** at 151,936 — so the cache-bound inflection is not an artefact of the
CPU build. All three runs (unpatched CPU, patched CPU, prebuilt GPU) produce the
**same 24-token sequence**, hash `21c6df5710bb7794`, first ids
`27 14172 13429 29 198 27 1688 89871`: the patch and the two llama.cpp builds are
all semantically identical on this fixture.

## Verdict

- **H1 FALSIFIED.** The bar was ≥ 5×; the measurement is **1.05–1.22×** on every
  arm that matters, and the pre-registered falsifier threshold (2×) is not met
  either. The two per-candidate heap allocations in `llama_grammar_apply_impl`
  are **not** where the time goes. Per the frozen rule the patch is **not
  landed**: `cmake/Dependencies.cmake` is restored, and the patch lives in this
  directory as evidence rather than as build machinery.
- **H2 passed.** Identical token hash, identical allowed-top-20 lists, identical
  top-1: the refactor is semantics-preserving. That is what makes the *negative*
  result informative rather than a broken experiment.
- **H3 passed, and is reusable.** The patch was applied by the build's own
  `PATCH_COMMAND` and verified end to end — including the non-obvious part: the
  extracted source lives **inside this repository's work tree**, so `git apply`
  resolves patch paths against the repo toplevel and *silently skips* the file
  unless the command sets `GIT_CEILING_DIRECTORIES`. That mechanism is recorded
  in `llama-grammar-apply-alloc.patch` and in this directory for the next patch.
- **The real mechanism, now measured.** ns/candidate is flat at ~80 ns while the
  *touched* candidate set fits in cache (≤ 16k) and roughly **doubles to ~200 ns
  once it does not** (65k+), staying linear thereafter. At full vocabulary the
  mask is **memory-latency bound**, not allocation bound: 151,936 scattered
  per-token piece strings plus 24-byte candidate structs. That also explains why
  a permissive two-rule grammar costs the same as the deep native grammar at the
  same array size — the walk depth is irrelevant; the traffic is not.
- **Consequence for the fix.** Neither "remove allocations" (this spike) nor
  "reduce to raw top-K" (spike 002's bounded path) addresses the measured cost
  head-on. The lever that does is **touching fewer bytes per candidate**: a dense
  per-token first-code-point array (1 byte × 151,936 ≈ 152 KB, L2-resident) that
  can reject a candidate before any pointer chase into the piece cache — with the
  token-rule positions (`LLAMA_GRETYPE_TOKEN`, which match by id, not by
  character) unioned in, and a fallback to the current path when the allowed
  first-character set is large. In the states this spike measured — 1–3 tokens
  allowed out of 151,936 — that prefilter should reject essentially everything
  from a dense scan, which is where a ≥ 10× is available. That is spike 004, and
  it needs its own pre-registered bar: this spike's 31.2 ms at full vocabulary is
  its baseline.