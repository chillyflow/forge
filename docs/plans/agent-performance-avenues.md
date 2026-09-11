# Agent performance avenues

Status: planned, September 11, 2026. This document surveys avenues the
September campaigns did not pursue. It starts no model runs, changes no code,
and changes no default. It exists because the loop campaigns exhausted one
class of change and the evidence now points at three different ones.

Read [the all-green plan](agent-loop-all-green.md) and
[the fresh design](agent-loop-fresh-design.md) first for the acceptance ladder,
populations, profiles and evidence rules, which remain in force.

Every number marked *Forge-measured* was recomputed from retained artifacts
under `benchmark/results/`. Numbers marked *external* come from the cited work
and have not been reproduced on this hardware.

## 0. Why this document exists

The loop campaigns concluded that **the loop is not the ceiling on this task
family**, and that conclusion stands for the lever they varied. That lever was
always the same: **how the host presents evidence to the model.** Six
variations of it were measured and none lifted the retraction family.

What was never varied is *what the model must produce*, *what the sampler is
allowed to produce*, and *where the model's working state lives*. Those are
three different classes of change, and the external evidence for the first and
third is considerably stronger than anything measured for the second.

The reconciliation with Forge's own data is uncomfortable but clean. Forge
measured that failing runs emit `apply_patch` calls whose `new_text` is
byte-identical to `old_text` — *Forge-measured*: 28 such calls across eight
runs, every one a complete, correctly-escaped whole-function span of 21–26
lines and 720–993 bytes. Nothing truncated, nothing mis-escaped. The model
quotes perfectly and changes nothing.

The harness literature's headline result is the same observation from the other
side: *"Often the model isn't flaky at understanding the task. It's flaky at
expressing itself."* Forge responded to an expression failure by adding
evidence to the prompt, which does not touch expression.

## 1. Closed axes — do not re-propose these

| Closed | How |
| --- | --- |
| Host evidence presentation | v2, v3, v4-reverted, bounded repair, c04 elide-noop, c05 host-defects |
| Thought-history retention | measurably harmful; 15/15 fixture runs lost with it |
| Sampler variance control | temperature 0 still diverges run to run |
| Repetition penalty 1.05 / last-64 | never engaged its target; rate unmoved 40.2% → 40.2% |
| Prefix stability (admission floor) | no efficiency gain; axis closed |
| Exact-argv `run_command` dedup | 1 event in 12 runs; workload has near-duplicates, not exact ones |
| Best-of-N as budget splitting | two candidates split one budget and often both failed |
| Retraction family, Qwen3-Coder, loop-pilot profile | recorded exit; reopen needs a different hypothesis class |
| CUDA Unified Memory as the RAM/VRAM answer | see §3.4 — the literature is against it |

An edit-channel hypothesis was also investigated and refuted before
implementation, and is recorded in the fresh-design plan so it is not proposed
again. §4 explains why that refutation does not cover the format proposed here.

## 2. M0 — measurement, before any of this

**This is the first thing to fix, and it gates everything else.**

Every conclusion in the last four campaigns rests on **six fixtures** and n=12
clustered runs. On the identical binary, profile and schedule, screen c09
scored 5/12 and its preregistered confirmation c09b scored 1/12. The plans
themselves say *"one repetition per fixture cannot isolate causes, establish
statistical superiority, or justify defaults"* — and close/promote decisions
were then made at exactly that sample size.

By contrast, the external edit-format result in §4 used *external*: 180 tasks ×
3 runs × 16 models, on real React sources, scored by a mechanical metric that
does not depend on the model at all.

**Required:** a larger, diverse, edit-heavy fixture corpus, with primary
metrics that are model-independent and therefore resolvable at n in the
hundreds:

- edit-application success rate (did the intended change land)
- byte-identical-replacement rate (the measured degeneracy, directly)
- output tokens per completed task
- mechanical-fix recall against a supplied oracle

These are the exact quantities §3–§5 move, so without them those avenues cannot
be evaluated either.

## 3. Memory: RAM and VRAM in tandem

This section answers a specific question: is there unbroken ground in using host
RAM and device memory together, that current systems are not exploiting?

**Short answer: the mechanism is well-trodden, the *policy* is open, and Forge
is currently leaving a measured failure on the table that is not a hardware
limit at all.**

### 3.1 Verified current state

From `src/inference/llama_backend.c:1293-1310`, Forge sets exactly these fields
before loading a model:

```
mp.n_gpu_layers = gpu_layers          /* model params: also mmap (default on), mlock (default off) */
cp.n_ctx, cp.n_batch = 512, cp.n_ubatch = 256
cp.n_threads, cp.n_threads_batch
```

Nothing else. Specifically **Forge exposes no** KV cache type, no flash-attention
control, no MoE expert placement, no `mmap`/`mlock` control, no tensor override,
and no host-buffer tiering. `forge.toml.example` carries `context = 16384` and
`gpu_layers = "auto"`. `speculative = false` is explicitly rejected as
unimplemented.

Hardware, *Forge-measured* from `benchmark/results/2026-09-11-capability/*/environment.json`
and the retained hardware plan:

| | |
| --- | --- |
| GPU | NVIDIA GeForce RTX 5090 Laptop, **23.89 GiB** total, 22.61 GiB free |
| RAM | **31.43 GiB** total, 21.89 GiB free |
| CPU | Intel Core Ultra 9 275HX, 24 logical |
| Model | `qwen3moe`, 48 layers, 17.28 GiB of tensors |
| KV | **98,304 bytes/token** (f16 K+V, one sequence) |

### 3.2 The arithmetic

| context | f16 KV | q8_0 KV | q4_0 KV | model + f16 | model + q8 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 16,384 | 1.50 GiB | 0.75 GiB | 0.38 GiB | **18.78 GiB** | 18.03 GiB |
| 32,768 | 3.00 GiB | 1.50 GiB | 0.75 GiB | 20.28 GiB | **18.78 GiB** |
| 49,152 | 4.50 GiB | 2.25 GiB | 1.12 GiB | 21.78 GiB | 19.53 GiB |
| 65,536 | 6.00 GiB | 3.00 GiB | 1.50 GiB | 23.28 GiB | 20.28 GiB |
| 131,072 | 12.00 GiB | 6.00 GiB | 3.00 GiB | 29.28 GiB | 23.28 GiB |

VRAM remaining after model + 16,384 f16 tokens, before any compute buffers:
**5.11 GiB** — enough for a further **55,842 f16 tokens of context**, or 111,685
at q8_0.

### 3.3 Finding: the context wall was self-imposed

The all-green campaign's largest recorded failure mechanism was the pinned-context
limit: *Forge-measured*, 11 runs reached 14,348–15,214 segment tokens against a
**14,336-token input capacity** (16,384 context − 2,048 output reserve).

The configuration was arithmetic: 16,384 was a constant. The hardware was never
the constraint — roughly **5 GiB of VRAM sat unused** while those runs failed.

Scope this honestly: raising context removes the *pinned-context* failure class
and reduces the compaction pressure that measured 16.5% of one run spent
re-prefilling. It does **not** by itself fix the retraction failures, which trip
the action and cumulative-input budgets first, and it does not reduce cumulative
input tokens, which are re-sent each turn.

### 3.4 What is already done, and is therefore not unbroken ground

- **Static layer/tensor placement**: `-ngl`, `-ot`, `--n-cpu-moe`. Decided at
  load time by tensor-name regex. Offloading `\.ffn_.*_exps\.weight=CPU` while
  keeping attention on GPU is standard practice for MoE.
- **KV cache quantization**: `--cache-type-k`/`--cache-type-v`; `q8_0` halves
  KV at negligible quality cost and under 5% throughput hit *external*.
  Asymmetric K8V4 beats K4V8 by ~7× at equal bits *external*, because keys
  shape the attention pattern and values only carry content.
- **Unified memory / oversubscription**: exists, and the evidence is *against*
  it. Oversubscribed UVM carries a documented performance cost; llama.cpp has
  recorded out-of-memory failures during warmup with UVM enabled; the active
  research (AutoUVM) is about *mitigating* UVM with automated prefetching. **Do
  not propose UVM as the tandem-memory answer.**
- **Expert offloading research**: ktransformers (SOSP '25), MoE-Infinity,
  HybriMoE, importance-driven expert scheduling. All static or
  request-level, all aimed at models far larger than VRAM.

### 3.5 What is genuinely unbroken

Both llama.cpp's own tracker and the literature confirm the same gap.
Issue #20757 *"Two-tier GPU+RAM expert cache for MoE offload (pluggable eviction
policy)"* and discussion #24528 *"RFC: MoE expert cache, VRAM caching of hot
CPU-resident experts"* are **open**; the summary in the tracker is explicit that
current mechanisms allow *static partitioning* but not *on-demand swapping of
individual experts during inference*, and that ktransformers does not do dynamic
expert transfer either. The proposal on the table is to use **offline calibration
(imatrix)** data to decide which experts are hot.

**M1 — Agent telemetry as the placement signal.** Every existing approach
estimates MoE hotness offline, from calibration data, once. Forge runs *the same
repository, over the same task family, repeatedly*, and can measure expert
activation **online, per project**, from its own routing telemetry. Pin the
observed hot set in VRAM; stream the cold set from the 21.89 GiB of free RAM.
Nobody does workload-adaptive, per-project, online expert residency driven by an
agent's own history.

**Current value: none on the tested models.** This is a bet on *larger* models,
not a present win. The 30B-A3B fits in VRAM with roughly 5 GiB spare, so moving
experts to host RAM buys nothing today and costs decode time (§3.5 cost warning).
It becomes real at 60B+ MoE or at very long context — plausible, since two of
the three campaign models already sit at the VRAM limit, but not a today
problem. Sequence it last, behind M2, and only if model or context requirements
actually grow.

**M2 — KV tiered by agent semantics.** llama.cpp's KV policy is FIFO / context
shift, which Forge *measured* as harmful (cached tokens oscillating 10.3k↔1.1k
across turns 23–30). But an agent's context is not a stream. It has structure:
system prompt, permissions and task are immutable for the run; the current file
is stable; tool output is volatile. The optimal policy pins immutable and stable
segments in VRAM and evicts only volatile segments — and Forge can regenerate
volatile segments from the session log rather than store them at all. No system
tiers KV by agent semantics.

**M3 — Tiered checkpoint and candidate KV.** Forge already has physical prefix
checkpoints, bound to one loaded model instance, currently capped at 256 MiB.
Best-of-N currently does not promote a stored prefix; it re-runs. A tiered
checkpoint store — current branch hot in VRAM, alternatives in RAM, promoted on
selection — makes search cost *prefill-free* rather than budget-split. This
addresses the actual reason best-of-N failed, which was allocation, not
mechanism. No external harness can do it at all: rollback outside the process
means re-prefill.

**M4 — Phase overlap.** Prefill is compute-bound; decode is bandwidth-bound.
llama.cpp sequences them. A process that owns both could issue prefill of the
next segment on a separate stream while the current action decodes. Not
reachable from outside the process.

**M5 — Pinned host memory for staging.** The standard high-performance expert-
streaming design uses page-locked buffers with double-buffered prefetch. mmap
gives page-cache-backed weights, not pinned transfer buffers. llama.cpp's mmap
path does not pin; a C host could.

**M6 — RAM as a warm tier for model swap.** The capability campaign evaluates
three models of 14–18 GiB each against 31.43 GiB of RAM. One resident in VRAM
and one held warm in RAM turns a disk read into a memcpy. Marginal on this
machine's RAM budget; recorded for completeness.

**Cost warning on full expert offload:** *computed* — an A3B model at ~4.5 bits
per active parameter reads ~1.69 GB per token when experts live in host RAM,
which caps decode at roughly 19–38 tok/s over 32–64 GB/s PCIe before any
overhead. The model already fits in VRAM. **Do not offload experts to buy
context** — buy context with KV type (§3.2), and reserve expert tiering for
running a model that does not fit, or for M1's hot-set residency.

## 4. A1 — Content-hash line addressing for edits

**Hypothesis.** Requiring the model to *reproduce* old content is what fails;
addressing by a stable per-line identifier is what works.

**External evidence.** Tagging each line with a 2–3 character content hash and
editing by reference: beats `patch` in **14 of 16 models**, **+15 points**
average, **−20% output tokens** typical and **−61%** best case; Grok Code Fast 1
6.7% → 68.3%; GPT-5.1 Codex Mini 60.0% → 77.5%. Aider's own benchmark: format
alone swung GPT-4 Turbo **26% → 59%**. Patch failure rates were 50.7% (Grok 4)
and 46.2% (GLM-4.7) against models that "just don't speak the language."
**The weakest models gain the most** — which is precisely Forge's target class.
Counter-evidence exists: at least one party attributes the gain to tool-schema
overhead rather than addressing (see §7).

**Forge status: not tested.** The plan's refutation covered *exposing
`apply_hunk`*, and rested on `src/tools/tools.c:1487`. That guard is narrower
than the plan states: it is native-protocol only, fires on **shrinking** hunks
with ≤3 aligned differences, and has a separate grow-path with an overlap
allowance. It also rested on *0 anchor rejections in 73 calls* — but anchor
*success* is the wrong metric. Hashline's benefit is that the model never
reproduces old content at all, and Forge's failures show it reproduces content
perfectly while changing nothing.

**Why only Forge.** Forge already generates its GBNF grammar from the tool
registry and already carries SHA-256 and symbol hashes. It can make the
**address itself grammar-legal** — build the edit grammar from the current
file's live line-hash set, so a stale or invented address is *unrepresentable*
rather than rejected after the fact. No external harness owns sampling.

**Falsifiable test.** On the M0 corpus: byte-identical-replacement rate falls,
edit-application success rises, output tokens per completed task fall. Requires
the M0 metrics to detect, hence §2 first.

## 5. A2 — In-generation degeneracy intervention

**Hypothesis.** The degeneracy is internal to generation, so it should be
interrupted during generation rather than discouraged in the prompt.

**Evidence.** Forge's own strongest finding: the H-ECHO hypothesis was refuted
*while confirming the mechanism engaged perfectly* — 43 elision events against
43 independently counted identical replacements, with the transition rate
unmoved at 59.0% vs 60.0%. The campaign's conclusion: the degeneracy is
*internal to the model, not auto-catalysed through the transcript.*

If that is true, every prompt-side intervention is looking in the wrong place.

**Forge status: not attempted.** Forge counts identical replacements *after* the
run. Detecting that the in-flight generation is reproducing the anchor span, then
aborting and re-sampling, or biasing away from the copied span, is only possible
inside the sampler. Forge already ships the machinery class: lazy→eager grammar
swap, forced-action progress tokens, forced cue decoding, early termination when
the action object closes.

**Pre-empt the objection.** This is not the closed repetition-penalty arm. That
arm failed for a stated mechanical reason — *"a 64-token penalty window cannot
see a repetition made of 720–993 byte edit spans separated by whole turns."* A
span-aware intervention is a different mechanism, not a larger window. State
this explicitly or it will be dismissed as a re-run.

## 6. A3 — Rollback and rewind with KV restoration

**External evidence.** AgentRewind (*external*): joint context + environment
checkpoints with a compact *rewind memory*, **+25.6 points** task success in its
best configuration across seven models, four execution strategies and three
harnesses. Companion result: retained KV after an aborted context breaks
rollback consistency, requiring a transaction-local cache restore — and full
restart is not an acceptable substitute.

**Forge status: primitives exist, policy does not.** Forge has in-process
physical KV checkpoints, a candidate store with journaled apply/revert, an edit
journal and bounded input snapshots — and it has already measured the symptom
the companion paper describes (the 10.3k↔1.1k oscillation). What it lacks is
checkpoint *selection* and the carry-forward memory; its best-of-N restores the
workspace and discards the failed trajectory entirely.

**Why only Forge.** Rollback outside the process means re-prefill. Forge can
restore a prefix, which is the whole point.

## 7. A4/A5 — smaller levers

**Speculative decoding — measured caution.** n-gram/prompt-lookup is draftless
in llama.cpp today. Vendor figures are 1.3–2× with 80%+ acceptance on repetitive
edit content. The dissenting measurement is the one that matches this machine:
on a consumer GPU already bandwidth-saturated during single-token decode, the
**median speedup was ~0%** across six diverse prompts, and the headline 4.75×
was the lookup table memorising a repeated prompt. llama.cpp also requires
`--parallel 1`, which conflicts with Forge's prefix design. Forge's workload is
unusually repetitive, so acceptance *may* be high — but that is a hypothesis.
**Test at flag level only, on diverse prompts, measuring decode separately, and
never on a repeated prompt.** Grammar × speculation is not free either:
constraining across multi-token draft states needs exponential state precompute.

**A5 — per-state schema overhead.** A credible dissent on §4 attributes its gain
to tool-description and schema overhead rather than addressing. Forge already
budgets rendered schemas and keeps tiny reflection/final registries; measuring
and shrinking per-state schema cost is cheap and also attacks the documented
local-model failure mode where too many exposed tools cause wrong selection.

## 8. Larger bets, sequenced later

**A6 — symbol/AST-addressed editing.** Forge has tree-sitter, symbol hashes and
declaration ranges. Editing by node hash composes with §4. Note Diff-XYZ
(*external*) found *no single format dominates across models*, which argues for
testing formats per model rather than standardising one.

**A7 — self-improving agent (skill/experience libraries).** A large active area.
Forge has exactly zero cross-run learning: every run starts cold, and the
artifacts it retains — `working_state.json`, digests, the edit journal — are
never fed back. Highest ceiling, highest overfitting risk. The repo's own rule
applies: improvement must be shown on *later, unseen* work against a fixed
control, and copying one lesson into many runs is not independent confirmation.

**A8 — multi-model on one GPU.** The SLM thesis (*external*) is that agent loops
run few narrow, repeating tasks, so a small specialist often matches a generalist
giant on what actually executes. Forge links llama.cpp directly and could route
per state. **Hard constraint here:** 23.89 GiB VRAM against 14–18 GiB per model
means one resident model at a time, so this requires model switching, which lands
back on M6.

## 9. Recommended order

| # | Avenue | Why first | Cost |
| --- | --- | --- | --- |
| 1 | **M0** corpus + model-independent metrics | Gates everything else; without it no result is falsifiable | Low |
| 2 | **M0b** context/KV configuration + measurement | Removes a measured failure class for near-zero implementation; establishes the memory baseline everything else is judged against | Very low |
| 3 | **A1** content-hash addressing, grammar-enforced addresses | Best external evidence; hits the measured failure; −20–61% output tokens relieves the action wall | Medium |
| 4 | **A3 + M3** rewind with tiered checkpoint KV | Reuses existing primitives; structural C advantage | Medium |
| 5 | **A2** in-generation degeneracy intervention | The genuinely new hypothesis class the exit requires | Med-high |
| 6 | **M2** agent-semantic KV tiering | Directly targets the measured 16.5% re-prefill loss; open problem upstream | Med |
| 7 | **A5, A4** schema overhead, speculation | Cheap efficiency work | Low |
| 8 | **M1** online expert residency — **only if model size or context grows** | Not a current win: the tested 30B-A3B fits with ~5 GiB spare, and host-side experts cost decode time | High |
| 9 | **A6–A8** | Bigger bets | High |

Row 2 is the one-line change with the largest measured effect in this document:
11 runs failed at a 14,336-token capacity while roughly 5 GiB of VRAM sat
unused, because `context = 16384` was a constant rather than a computed fit.

The concrete, phase-gated version of rows 1–5 is in
[agent-performance-implementation.md](agent-performance-implementation.md).
Rows 6–9 are out of scope until that plan's phases report.

## 10. Evidence rules

Unchanged from the existing plans, restated because they bind here:

- One frozen source/runtime/configuration per gate; a change makes a new candidate.
- Failures are retained and reported as failures; a passing terminal workspace
  never overwrites a failed primary result.
- Outcomes from overlapping suites are not independent observations and are never
  pooled to manufacture significance.
- An interrupted batch is recorded as interrupted, not as a result, and is not
  silently re-run for a better repetition.
- External numbers are labelled external and are never presented as Forge-measured.
- A mechanism confirmed to activate *without* moving the outcome is a refutation
  of the hypothesis, not evidence of neutrality.
