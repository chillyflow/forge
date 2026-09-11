# Agent performance implementation plan

Status: planned, September 11, 2026. Written to be executed from a fresh session
with no prior conversation context.

This implements a deliberately narrow slice of
[agent-performance-avenues.md](agent-performance-avenues.md). Read that document
first: it carries the evidence, the closed axes and the reasoning for each
avenue. This plan does not repeat that argument, and does not relitigate it.

## Track, and what this plan is not

**Track: capability only.** This plan does not touch the comparative promotion
gates in [beat-opencode-reliability.md](beat-opencode-reliability.md), which
remain open and unmet. Capability work feeds those gates; it cannot substitute
for them. Any claim that Forge beats another agent with local models still
requires a freshly frozen holdout with a positive task-cluster interval lower
bound, exactly as that campaign specifies.

**Not a fifth loop campaign.** Three items are in scope (P0.1, P0.2, P0.3) and
two phases follow from them. The remainder of the avenues document is
explicitly out of scope until those report.

**Read [../RUN_EFFICIENCY.md](../RUN_EFFICIENCY.md) before launching any batch in
this plan.** It records how the September campaigns spent effort inefficiently —
314 model runs in three days — and gives the preflight checklist that prevents
the recurrence. Its §2 is a launch gate, not advice.

## The one structural rule

**Deterministic gates precede model gates, always.**

Forge already has an explicit simulated backend (`--script actions.json`,
`src/cli/main.c:101`) and 24 GPU-free unit and integration test files. Edit-
surface work is *tool semantics* — whether an edit applies, whether a stale
address is refused, whether a no-op is rejected — and those are decidable by
construction, at thousands of cases, for zero GPU time and zero sampling
variance.

A phase may only proceed to model runs after its deterministic layer passes. A
hypothesis that the deterministic layer cannot decide is underspecified, and
that is not a reason to spend a GPU batch.

**Why this rule exists.** The previous five campaigns each went from idea to a
six-fixture GPU batch in one step. None could resolve its own effect: screen c09
and its own preregistered confirmation c09b scored 5/12 and 1/12 on a
byte-identical binary, profile and schedule. Model time was the first
instrument; here it is the last.

## P0 — no GPU

### P0.1 Expose the memory controls Forge does not have

**Verified problem.** `src/inference/llama_backend.c:1293-1310` sets only
`n_gpu_layers`, `n_ctx`, `n_batch = 512`, `n_ubatch = 256`, `n_threads` and
`n_threads_batch`. Forge has no KV cache type, no flash-attention control, no
expert placement and no host-tier control.

The consequence is measured, not theoretical: 11 runs in the all-green campaign
reached 14,348–15,214 segment tokens against a 14,336-token input capacity while
roughly 5 GiB of VRAM sat unused. That capacity was `context = 16384` minus a
2,048-token output reserve — a constant, not a hardware limit.

**Change.**

- `llama_context_params` fields to set, from the retained header copy
  (`.scratch/llama.h:351-393`): `type_k`, `type_v` (both `enum ggml_type`, marked
  `[EXPERIMENTAL]`), `flash_attn_type` (`enum llama_flash_attn_type`), and
  `offload_kqv`.
- **Verify those field names against the actually pinned dependency before
  coding.** The retained header is a copy, and it does not contain
  `use_mmap`/`use_mlock`; do not conclude those fields are absent upstream.
- **Enforce, do not document, the dependency:** llama.cpp ignores non-fp16 KV
  types unless flash attention is enabled. The host must refuse an unsupported
  combination rather than silently ignoring it, and a test must prove the
  refusal. A silently ignored flag would produce a measured "no effect" that is
  actually a configuration error — the exact failure mode this project has
  already paid for.
- Surface: `--cache-type-k`, `--cache-type-v`, a flash-attention control, and
  matching `[inference]` keys in `forge.toml`; update `docs/CONFIG.md` and
  `forge.toml.example`.
- The `--gpu-layers auto` planner in `src/core/hardware.c` must size context
  against **measured free VRAM and the selected KV type**, not a fixed default.
  The model/KV metadata path already exists (`docs/CONFIG.md:181`).

**Acceptance.**

- Flags parse, validate, and round-trip through the TOML profile; invalid
  combinations are refused with an explicit error and a nonzero exit.
- **Defaults are unchanged**: f16 KV, existing context default, byte-identical
  behaviour when the new flags are absent.
- Deterministic tests cover: default preservation; refusal of non-fp16 KV
  without flash attention; planner arithmetic at the measured KV bytes/token;
  and a context figure the planner raises when VRAM allows.
- No model run is required for this item.

### P0.2 Deterministic edit-surface contract tests

Written **before** the edit tool, for the format P2 introduces. These decide
whether the format is sound at all:

- a valid content-hash address applies exactly
- a stale address is refused (file changed since read)
- an invented address is refused — and, once P2's grammar exists, unrepresentable
- a replacement byte-identical to the selected span is rejected as a no-op
- **a count-changing span is accepted** — the case `apply_hunk`'s guard rejects,
  and one reason the earlier refutation of the edit channel does not transfer
- multi-line spans, CRLF preservation, UTF-8 boundaries
- a partially stale multi-address edit applies nothing (all-or-nothing)

**Acceptance.** Every case passes with no model loaded, through the existing
harness, and joins the standing suite so later phases cannot regress it.

### P0.3 M0 corpus and reporter

**The blocker this removes.** Every recent conclusion rests on six fixtures and
n=12 clustered runs. The plans themselves state that one repetition per fixture
cannot isolate causes; close and promote decisions were then taken at that size.

**Build:**

- a larger, diverse, **edit-heavy** corpus — not four wordings of one Python
  retraction task. Weight it toward mechanical edits with known inverses and
  supplied oracles, which is what the external edit-format result used.
- a frozen manifest with per-fixture hashes, in the existing
  `benchmark/results/.../tasks` shape.
- an acceptance reporter emitting the metrics below, which refuses to substitute
  a missing measurement.

**Primary metrics — model-independent by design, therefore resolvable at n in
the hundreds:**

1. edit-application success rate
2. byte-identical-replacement rate — the measured degeneracy, directly
3. output tokens per completed task
4. mechanical-fix recall against the supplied oracle

Secondary, model-dependent, reported separately and **never** used to overturn a
primary result: agent exit, protected-file integrity, terminal workspace tests,
wall time.

**Acceptance.** Corpus and reporter frozen and hashed before any model run;
reporter's own tests pass; a broken fixture is rejected at preflight.

## P1 — memory baseline (GPU, cheap)

**Question.** With P0.1 landed, does the pinned-context failure class disappear,
and what does the extra context cost?

**Method.** Replay the exact fixtures that produced the 11 pinned-context
failures, at a frozen control and at a raised context, on one binary pair.
Report prefill, re-prefill (`Σ(prompt − cached)`) and cached-token oscillation
alongside outcomes — the last two because a previous screen measured 16.5% of
one run spent re-prefilling under compaction pressure.

**Bars, preregistered before the first run:** the pinned-context failures do not
recur; re-prefill does not increase; no fixture that passed at the control
regresses.

**Honest scope — state this in the report.** P1 removes a failure *class*. It
does not fix the retraction action-wall failures, which trip the action and
cumulative-input budgets first, and it does not reduce cumulative input tokens,
which are re-sent every turn. Report it as a class removal, never as a repair-rate
improvement.

## P2 — A1 content-hash addressing

**Hypothesis.** Requiring the model to reproduce old content is the failure;
addressing by a stable per-line identifier is the fix.

**Order:** deterministic layer (P0.2) → grammar-generated addresses → model
screen on the M0 corpus.

**The Forge-native part.** Build the edit grammar from the current file's live
line-hash set, so a stale or invented address is **unrepresentable rather than
rejected**. Forge already generates GBNF from the tool registry; this makes the
grammar data-dependent. No external harness can do it.

**Bar:** against a frozen control on the M0 corpus, byte-identical-replacement
rate falls and edit-application success rises, with output tokens per completed
task not increasing. Failure is a recorded refutation, not a reason to move the
bar.

## P3 — gated on P2

Only if P2 reports. In order: **A3/M3** rewind with tiered checkpoint KV, then
**A2** in-generation degeneracy intervention. Each gets its own preregistered
bar and its own frozen candidate. A2's protocol must state up front why it is
not the closed repetition-penalty arm, or it will be dismissed as a re-run.

## P4 — gated, explicitly optional

**M2** agent-semantic KV tiering, then **M1** online expert residency only if
model size or context requirements actually grow. M1 is not a current win: the
tested 30B-A3B fits with roughly 5 GiB of VRAM spare, so offloading experts buys
nothing today and costs decode time. It becomes real at 60B+ MoE or very long
context.

## Stop rule

A phase that fails its bar exits like the retraction family: recorded, retained,
and closed — not absorbed into another round of changes. Three unproductive
phases on one avenue trigger a design review, not a fourth attempt. Budgets and
denominators are never relaxed to turn a failing gate green.

## Evidence rules

As in [agent-performance-avenues.md](agent-performance-avenues.md) §10, plus:

- Every phase reports its control and its candidate from the same frozen binary
  pair; a mid-series change makes a new candidate and re-runs the affected checks.
- External numbers are labelled external and never presented as Forge-measured.
- An interrupted batch is recorded as interrupted and is not silently re-run for
  a better repetition.
- A mechanism confirmed to activate *without* moving the outcome is a refutation
  of the hypothesis, not evidence of neutrality.

## Out of scope

Comparative claims against OpenCode or Aider. The `beat-opencode-reliability`
promotion gates. CUDA Unified Memory — the evidence is against it (§3.4 of the
avenues document). Speculative decoding beyond a flag-level measurement. A6, A7
and A8.
