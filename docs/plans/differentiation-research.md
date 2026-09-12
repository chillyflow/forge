# Differentiation research: capabilities that require the process boundary not to exist

Status: active research direction, September 11, 2026. This document frames how
Forge seeks differentiation. It records the test a candidate idea must pass, the
three literatures that are actually relevant, and the candidate set with its
current verdicts. It supersedes no existing plan; the acceptance ladder and
evidence rules in [agent-loop-all-green.md](agent-loop-all-green.md) remain in
force.

## 1. The frame

Forge's structural advantage is **not** speed. It is that the runtime owns the
sampler, the KV cache, and the batch configuration in-process, rather than
speaking to an inference server over HTTP.

That difference is not quantitative. A harness on the far side of an HTTP
boundary **cannot** — by construction, not by choice — do the things in §4.
This is why surveying agent-harness literature harder would not have helped: it
studies a class of system that is structurally blind to this space. It would
have produced more prompt-engineering advice, which is the class the September
campaigns already exhausted.

**The research question is therefore not "what techniques exist?" It is:**

> Which of Forge's *measured* failures are only fixable in-process?

## 2. The test

A candidate idea must satisfy **both**:

1. **It attacks a measured failure.** Not a hypothesised one. Something in
   `benchmark/results/` or the ROADMAP's partial table.
2. **The fix is only possible in-process.** If an external harness could do it,
   it is not a differentiator — it is a feature others will also ship.

Novelty alone is explicitly not a reason. An idea that fails either half is
recorded in §5 as rejected, so it is not re-proposed.

## 3. The three literatures

They do not cite each other, and the gap between them is where this work lives.

| Literature | What it knows | What it is missing |
| --- | --- | --- |
| **Inference systems** — vLLM, SGLang, llama.cpp internals, KV management | prefix caching, KV quantisation, scheduling, determinism sources | any notion of an *agent*: branches, rollback, tasks, workspace state |
| **Agent architecture** — search, verification, memory | branching, self-repair, experience reuse | treats inference as a black box with a bill attached |
| **Program analysis** — ASTs, diffs, grammar-constrained generation | making edits structurally valid | never applied *to decoding itself* |

Forge sits at the triple point. Searches should target the intersections, not
any one of them alone.

## 4. Candidate differentiators

| # | Angle | Measured failure it attacks | Only in-process? | Verdict |
| --- | --- | --- | --- | --- |
| D1 | **Deterministic decoding** | 5/12 vs 1/12 on a byte-identical binary; `AGENTS.md` forbids expecting byte-identical prose | Yes — owns batch shape | **Lead** |
| D2 | **KV-tree candidate search** | best-of-N failed on *budget splitting*, not mechanism | Yes — prefill is free in-process | Strong |
| D3 | **Grammar-legal edit addresses** | 28 byte-identical no-op patches | Yes — owns sampling | Strong |
| D4 | **Persistent cross-run KV** | 16.5% of a run re-prefilling; every run starts cold | Yes | Good |
| D5 | **Agent-semantic KV tiering** | cached tokens oscillating 10.3k↔1.1k | Yes | Good |
| D6 | **Logprob-gated verification** | wasted actions on doomed repairs | Partly — APIs expose logprobs | Moderate |

## 5. Rejected — recorded so they are not re-proposed

- **Expert residency in host RAM (M1).** Fails half 1: there is no measured
  failure. The tested 30B-A3B fits VRAM with ~5 GiB spare, so the technique buys
  nothing today. It becomes live only if model size or context demand grows.
- **Speculative decoding as a capability play.** Fails half 1. The honest
  consumer-GPU measurement on a bandwidth-saturated single stream is ~0% median,
  and it forces `--parallel 1`. Efficiency only, if ever.
- **CUDA Unified Memory.** Fails the "attacks a measured failure" half and the
  literature is actively against it.

## 6. D1 — Deterministic decoding

**This is the lead hypothesis and it is not a capability claim; it is a
measurement claim, which makes it higher-value than the rest.**

### The mechanism, from the literature

Nondeterminism in local inference originates **below sampling**:

> "the model produces significantly different outputs when the number of GPUs,
> evaluation batch size, or hardware versions change — even if the same random
> seed and greedy decoding are used" — *Understanding and Mitigating Numerical
> Sources of Nondeterminism in LLM Inference*, arXiv 2506.09501

> "batch size changes which kernels run and in what order" — practitioner
> consensus, and the reason multi-slot llama.cpp servers are not reproducible

The cause is floating-point **reduction order** changing with matrix shape. It
is arithmetic, not thermal, not theological.

### Why Forge specifically can fix it

A multi-tenant server cannot: it batches whatever requests arrive. Forge is
single-user and single-sequence, and already sets `cp.n_batch` and
`cp.n_ubatch` (`llama_backend.c:1300-1302`). Holding the *shape* constant —
including padding a trailing partial micro-batch to a fixed size — is available
to Forge and to almost nothing else in this space.

### Why it matters more than it sounds

Two campaigns died on this. Screen c09 scored 5/12 and its own preregistered
confirmation c09b scored **1/12 on a byte-identical binary, profile and
schedule**; the documented cause was execution nondeterminism. `AGENTS.md`
carries the scar: *"do not require byte-identical free-form prose across cold
and cached CUDA generations."*

If determinism is achievable, then:
- screens become resolvable at a fraction of the run count, because run-to-run
  variance stops dominating the signal;
- "did this change help?" becomes answerable with far fewer GPU hours;
- and Forge ships something nobody else does: **reproducible local agent runs**
  as a property, not an aspiration.

If it is *not* achievable, that is also a result worth recording, because it
bounds what any future campaign can conclude from a single repetition.

### Experiments, in order

| # | Question | Method | Pass condition |
| --- | --- | --- | --- |
| E1 | Is a single generation reproducible across cold processes? | N identical `forge complete` invocations, temperature 0, fixed seed, same prompt; compare bytes | all N byte-identical |
| E2 | If not, does holding the micro-batch shape constant fix it? | pad the trailing partial ubatch to a fixed size; repeat E1 | all N byte-identical |
| E3 | Is a cached prefix equivalent to a cold one? | same prompt cold vs after a warm prefix; compare bytes | byte-identical |
| E4 | Does it hold under the agent loop, not just `complete`? | replay one fixture N times, compare terminal workspaces | byte-identical workspaces |

E1 is running. E2 is the actual fix if E1 fails. Note E3 is the one `AGENTS.md`
currently declares unachievable — overturning that line with evidence would
itself be a meaningful result.

### Honest risks

- GPU clock and thermal state could still perturb results. Testable: run E1
  under load versus idle.
- Padding costs throughput. Measure the cost; a determinism mode may be opt-in
  rather than default, which is consistent with this project's convention that
  nothing becomes default until it passes a gate.
- Some kernels may be nondeterministic regardless of shape (atomics in
  reduction). If so, record which, and whether they can be avoided.

## 7. D2 — KV-tree candidate search

vLLM and SGLang do tree-structured KV sharing for *serving many agents*
(prefix caching; SGLang's radix tree; KVFlow, NeurIPS 2025). None models an
individual agent's own **branching**: candidates, rollback, rewind, each with
workspace state attached.

Forge has the primitives — physical prefix checkpoints, a candidate store with
journaled apply/revert, an edit journal — and currently spends them on a
best-of-N that re-runs rather than sharing a prefix. A C agent can branch for
approximately zero prefill; a harness pays full prefill per branch. That cost
asymmetry is precisely why Forge's best-of-N *failed on budget splitting*: the
allocation, not the mechanism, was the defect.

**Depends on** D1 for interpretability: a search whose branches are not
reproducible cannot be evaluated.

## 8. D3 — Grammar-legal edit addresses

External evidence is strong: content-hash line addressing beat `patch` in 14 of
16 models, **+15 points** average, **−20–61%** output tokens, with the weakest
models gaining most (see [agent-performance-avenues.md](agent-performance-avenues.md) §4).
It lands exactly on Forge's measured byte-identical-replacement failure.

The differentiator is not the format — that is public. It is that Forge
generates its GBNF from the tool registry, so the address tokens can be built
from the current file's live line-hash set and a stale or invented address
becomes **unrepresentable rather than rejected**. No external harness owns
sampling and therefore none can do this.

## 9. D4 — Persistent cross-run KV

The mechanism exists and is measured: llama.cpp `/slot/save` and `state_seq`;
forks such as CachyLLama; independent tooling reporting save ≈211 ms and
restore ≈87 ms for a 219 MB / 4K-token slot. The one paper treating KV as
persistent agent memory (arXiv 2603.04428) states plainly that llama.cpp
"provides no automatic multi-agent cache management."

Forge's ROADMAP lists disk KV resume as missing. Nobody has an agent that stays
**warm on a repository between sessions** — a natural fit for a local agent that
works the same checkout repeatedly, and a direct attack on the measured 16.5%
re-prefill loss and the "every run starts cold" cost.

## 10. D6 — Logprob-gated verification

Self-REF (arXiv 2410.13284) and the emerging practice of using token logprobs to
"localize brittle action-bearing spans, then decide whether to continue, retry,
or verify" map cleanly onto an in-process agent that already does
grammar-constrained decoding and can read logits for free. Rated *moderate*
rather than strong only because hosted APIs also expose logprobs, so the
capability gap is narrower — the advantage is cost and immediacy, not
possibility.

## 11. Open research questions

1. Which CUDA kernels in the pinned llama.cpp are nondeterministic regardless of
   batch shape, and are they on the decode path?
2. Can determinism be made a *mode* (opt-in) with a measured throughput cost,
   consistent with the project's default-off convention?
3. Does fixing batch shape also stabilise the cold-versus-cached equivalence
   that `AGENTS.md` currently declares unachievable?
4. For D2, what is the actual prefill saving of a shared-prefix KV tree versus
   the current re-run best-of-N, measured rather than assumed?
5. Does grammar-legal addressing measurably reduce the byte-identical
   replacement rate on the M0 corpus (once it exists)?

## 12. Evidence rules

Unchanged, and they bind here:

- A candidate differentiator is not adopted until it passes the §2 test with a
  measured result.
- Reproducibility claims are verified by byte comparison, not by assertion.
- Negative results are recorded with the same care as positive ones, and a
  mechanism confirmed to activate without moving the outcome is a refutation.
- External numbers are labelled external and never presented as Forge-measured.
- Nothing becomes a default until it passes a gate.
