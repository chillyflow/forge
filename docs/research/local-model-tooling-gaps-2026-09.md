# Tooling Gaps: Making Local Models Feel Like Frontier Cloud Models

*Research brief — 2026-09-12. Scope: what tooling does not yet exist (or is immature)
for closing the "experience gap" between a locally-served open-weight model and a
frontier model accessed through a cloud API. Framed for the Forge project.*

## The framing: separate the model from the experience

The single most useful distinction found in this research is that the cloud-vs-local
gap is not one gap. Cloud providers bundle two very different things:

1. **The model** — capability, which local open weights genuinely trail on complex
   multi-step reasoning and long-horizon agentic reliability.[12][21]
2. **The serving/harness layer around the model** — prompt caching, context
   compaction, tool-call reliability, retries/recovery, observability, streaming,
   accounting. This layer is largely *reproducible locally* and is where most of the
   improvable "feel" lives.

The measurable version of this: on SWE-bench, identical model weights scored through
different harnesses swing 10–20 percentage points, and the harness is usually the
explanation for diverging vendor numbers rather than the model.[18] That means a large
share of the local experience gap is a **tooling gap, not a weights gap** — which is
exactly where a project like Forge can win.

A tractable definition of the target experience, then: *first token arrives fast,
prefixes are cached automatically, tool calls almost never malform, the agent recovers
from its own errors across long sessions, and the user can see where time and tokens
went.* Each of the seven gaps below is a missing piece of that.

## Gap 1 — Structured output / tool calling that is reliable *by construction*

**Cloud experience:** frontier APIs enforce JSON schemas server-side; a malformed tool
call is effectively impossible, so agent code can assume parseability.

**Local reality:** local models routinely emit syntactically invalid or
structurally non-compliant output; a semantically correct answer that violates the
schema is operationally indistinguishable from a wrong answer.[2] The mitigation stack
exists — GBNF grammars in llama.cpp, Outlines, LLGuidance, XGrammar, Ollama's `format`
parameter — and with constraints active even small models produce perfect
syntax.[13] But the tooling is immature in exactly the ways that matter for agents:

- A 2026 arXiv study found that on **MoE models such as Qwen3-30B-A3B, decoder-side
  constraints give virtually no improvement** — Outlines left exact-match unchanged and
  LLGuidance *collapsed* AST match below the unconstrained baseline.[2] The most popular
  local coding models are MoE, so the flagship constrained-decoding tools largely do
  not work on them.
- CFG grammars for real Python (indentation, nested structures) **crashed the decoder**
  in both Outlines and LLGuidance in that same study — current grammar tooling is not
  mature enough for production-scale code generation.[2]
- Constrained generation samples a *different distribution* than the raw model, and can
  be pathologically bad (forcing an ellipsis into a long structured object yields an
  invalid result that nonetheless "requires" closing tokens).[14]

**Tooling opportunity:** semantic-aware constrained decoding that survives MoE routing;
per-model calibration of tool schemas (keep tool count low, 3–5, per the practical
guidance[13]); and a decode-time or post-hoc repair layer that turns "structurally
almost right" into "valid" without a full re-sample. This is directly adjacent to
Forge's grammar-constrained-tools premise.

## Gap 2 — Context management for long-horizon work (compaction, memory, clearing)

**Cloud experience:** agents like Claude Code, Codex and Gemini CLI ship a layered
context stack — compaction to compress history, tool-result clearing to drop
re-fetchable output, and a persistent memory tool for cross-session recall.[19]

**Local reality:** a theory paper analysing the dominant agent loop notes that
Codex's context-compaction mechanism "is identical whether the LLM is a frontier LLM
accessed via an API or a small open-weight LLM running locally"[3] — i.e. local agents
inherit a design built for a 1M-token cloud window and a billable token, not for a 32K
window and a fixed VRAM budget. Community reports put the pain plainly: users say
context compaction (not hallucination or raw context limits) is the biggest daily
problem with long-running coding agents.[3]

**Tooling opportunity:** a **local-native context manager** that exploits direct access
to the KV cache and the tokenizer rather than treating the model as a black-box API —
token-budgeted selection, structural eviction (drop/replace whole files, not fuzzy
summaries), and KV-level reuse of the surviving prefix. Cloud agents *can't* do this
because they only see an API; a locally-linked agent can. This is the clearest
structural advantage a local agent has.

## Gap 3 — Automatic, robust prefix caching (and knowing when it misses)

**Cloud experience:** prompt/prefix caching is automatic and, importantly, *visible* —
you get cache-hit token counts in the usage object for free.

**Local reality:** llama.cpp does per-slot prefix caching[7], but it is fragile in
practice: on hybrid/SWA models the server silently falls back to **"forcing full prompt
re-processing due to lack of cache data"**, so a request returns 200 OK with correct
output while prefill quietly never gets cheaper.[8] The symptom is invisible unless you
grep the server log. Prompt caching also interacts badly with MoE and with multi-slot
setups, requiring manual `--np`/`--cram`-style tuning.[7]

**Tooling opportunity:** a cache layer that (a) makes prefix reuse automatic and correct
across model architectures, (b) *reports* hit/miss and recomputed-token counts the way a
cloud usage object does, and (c) surfaces the regression loudly instead of in a log line.
Forge's reusable-KV-prefix design is aimed here; the missing industry piece is the
observability and the architecture-robustness.

## Gap 4 — Latency and throughput parity (and picking the right accelerator)

**Cloud experience:** you get 80–150+ tok/s and a fast first token without thinking
about it.[12]

**Local reality:** speed is now *adequate* for agent work — practitioners place the
usable bar at 20–30 tok/s, roughly GPT-5.5-with-high-reasoning speed, and current
35B-A3B-class models clear it (~30–40 tok/s on recent hardware at 50K context).[1] The
gap is that speedups are not turnkey: speculative decoding delivers 1.8–2.3× for a
70B+1B draft pairing[15] but **hurts** on small already-fast quantized models, and
classic llama.cpp speculative decoding *gets slower* on Apple Silicon while the same
idea through MLX wins 2.43×.[16] Whether a speedup helps depends on model size, runtime
and workload, and the choice is left to the user.

**Tooling opportunity:** automatic, per-workload selection of the acceleration strategy
(draft model vs n-gram vs `--spec-*` variants vs MTP), benchmarked in situ, plus
KV-cache offloading so long contexts don't collapse throughput — the on-device
counterpart being disk-aware KV offloading, which targets the *decode* stage that
cloud-oriented offloaders ignore.[9][20]

## Gap 5 — Agentic-loop reliability and bounded recovery

**Cloud experience:** frontier models "recover" — they notice a failed step and correct;
the loop stays on rails across long chains.

**Local reality:** the loop-level reliability gap shows up **around step 6–7 in a
chain**, where frontier models recover and local ones don't.[21] An 8-level agentic
failure-mode gauntlet run across 26 local models is community evidence of how commonly
tool-selection, multi-step chaining and error-recovery fail.[11] Academic work shows the
fix is *targeted, bounded* recovery — classify the failure, choose a recovery action
(retry / argument repair / tool substitution / retrieval refresh / replan / escalate /
graceful degradation) under a recovery budget — and that this beats both naive retry
(85.3%) and full replanning (88.2%) with a single attempt (94.0%).[10]

**Tooling opportunity:** productized self-healing orchestration for local agents —
failure-class detection wired to specific recovery actions and a budget, plus
**verified side-effect receipts** (did the commit actually land? did the command return
0?) so an agent can't "confidently report an action it never completed." Observers of
production agent systems call the confidence/verification divergence a better early
warning than latency or error rate.

## Gap 6 — Evaluation and observability that fit local models

**Cloud experience:** provider dashboards, token accounting, and standardized
capability evals.

**Local reality:** the eval landscape is noisy — harness variance of 10–20 points on
identical weights, vendor-controlled evals, and a "20–25 point Verified-to-Pro gap"
that never appears in release notes.[18] Open-weight models do appear on SWE-bench[5]
and Terminal-Bench,[6] but there is no standardized, reproduction-oriented **local
agent** evaluation, and local *inference* observability (prefill vs decode timing, KV
cache occupancy, cache-hit rate, effective context) is essentially absent — you get log
lines, not metrics. The general agent-observability field has matured for cloud
systems,[17] but it assumes a metered API backend.

**Tooling opportunity:** an open local-agent eval harness (speed *and* capability,
run on your own hardware, with failures retained) plus a local inference observability
layer. The practitioner workflow that already exists — a combined speed/memory bench and
a curated personal task set that also exercises tool calling — is the shape to
productize, not a leaderboard.[1]

## Gap 7 — Model routing and the local↔cloud boundary

**Cloud experience:** one endpoint, the vendor picks the model.

**Local reality:** the practical enterprise pattern is "local-first with cloud
fallback" — route 80–90% of routine queries, agentic loops and private RAG to local
machines and reserve the cloud for the hardest reasoning, with a sensitivity gate that
strips what must not leave.[22] Routing by capability (does this request exceed the
local model's context or reasoning budget?), by sensitivity, and by cost is recognized as
the right architecture, but it is described as an architecture to build, not a tool you
install.

**Tooling opportunity:** a drop-in router that knows the **capability envelope of the
specific local model** (its verified context ceiling, its tool-calling reliability, its
multi-step degradation point) and escalates on a defined signal — with the
confidentiality decision made before, not after, the escalation.

## Two adjacent gaps worth naming

- **Harness↔model co-design.** Frontier vendors tune the model and the harness together;
  open models increasingly ship optimised for one harness (e.g. Qwen models in Qwen-Code),
  and RL-on-any-harness work shows base-model coding performance varying by harness.[23]
  Tooling opportunity: a harness that adapts its prompting/parsing to the model rather
  than assuming the model was trained for it — the inverse of the current situation.
- **Consumer-hardware serving.** The strongest serving stacks (vLLM, SGLang) are
  Linux-first and effectively assume multi-GPU, while multi-agent local workloads spawn
  sub-agents that each carry their own context.[4] Tooling opportunity: continuous
  batching and multi-slot scheduling tuned for a *single* consumer GPU rather than a
  datacenter node.

## The shape of the market gap

| Gap area | Cloud gives it free | Local tooling maturity | Best lever for a local agent |
|---|---|---|---|
| Structured output / tool calls | Enforced schema | Tools exist but **fail on MoE**, crash on real CFGs[2] | Decode-time repair + MoE-aware constraints |
| Context management | Compaction + clearing + memory[19] | Borrowed from cloud design[3] | KV-native, structure-aware selection |
| Prefix caching | Automatic + visible | Works but silently regresses[8] | Correct-by-architecture + cache metrics |
| Latency | 80–150 tok/s[12] | Adequate (20–30 tok/s)[1], tuning manual | Auto-select acceleration + KV offload[9] |
| Loop reliability | Models self-correct | Degrades ~step 6–7[21] | Bounded recovery + verified receipts[10] |
| Eval / observability | Dashboards, accounting | Fragmented, harness-noisy[18] | Open local eval + inference metrics |
| Routing | Vendor picks | Architected, not tooled[22] | Envelope-aware escalation router |

## What this means for Forge

Three of the seven gaps are structural advantages of *linking llama.cpp directly* rather
than driving a model through a provider API:

1. **KV-native context management (Gap 2).** A black-box agent cannot see the cache; a
   linked agent can select context by token budget and reuse surviving prefixes. This is
   the strongest differentiator and matches Forge's reusable-KV-prefix premise.
2. **Architecture-robust prefix caching with metrics (Gap 3).** Bake in the hit/miss
   accounting the ecosystem makes invisible today.[8]
3. **Bounded recovery + verified side effects (Gap 5).** Because Forge owns the tool
   loop, it can attach recovery budgets and post-condition checks that a generic harness
   cannot.[10]

The two gaps that are *research* problems, not just engineering, and where a small team
should be cautious: reliable constrained decoding on MoE (Gap 1 — the literature shows
the current tools actively failing[2]) and long-horizon reliability at the model level
(Gap 5's cap is often the weights). The defensible play is to make the *harness* layer
so good that the local model's effective capability approaches its ceiling — and to
measure that with the eval/observability layer from Gap 6 so the claim is evidence-backed
rather than asserted.

## Sources

[1] https://magazine.sebastianraschka.com/p/using-local-coding-agents — Using Local Coding Agents (Raschka, Jun 2026)
[2] https://arxiv.org/html/2606.09395v1 — Empirical Study for Structured Output Control in LLMs for SE (arXiv 2606.09395)
[3] https://arxiv.org/html/2608.01326v1 — Context Compaction Theory (arXiv 2608.01326)
[4] https://www.storagereview.com/best/local-llm-tools — Best Local LLM Tools 2026 (StorageReview)
[5] https://www.swebench.com — SWE-bench Leaderboards
[6] https://www.tbench.ai — Terminal-Bench
[7] https://github.com/ggml-org/llama.cpp/discussions/20574 — Host-Memory Prompt Caching in llama-server (llama.cpp)
[8] https://particula.tech/blog/prompt-reprocessing-swa-hybrid-models-kv-cache — Full Prompt Re-Processing: llama.cpp Cache Fixes (Particula)
[9] https://arxiv.org/html/2511.11907v2 — KVSwap: Disk-aware KV Cache Offloading (arXiv 2511.11907)
[10] https://arxiv.org/html/2606.01416v1 — Self-Healing Agentic Orchestrators (arXiv 2606.01416)
[11] https://www.reddit.com/r/LocalLLM/comments/1u2a1s8 — 26 local LLMs through an 8-level agentic failure gauntlet (r/LocalLLM)
[12] https://www.promptquorum.com/local-llms/local-llm-limitations — Local LLM Trade-Offs 2026 (PromptQuorum)
[13] https://llmconfigurator.com/en/guides/llm-json-structured-output — Reliable JSON From Local LLMs: Structured Output Guide
[14] https://news.ycombinator.com/item?id=46635309 — LLM Structured Outputs Handbook (Hacker News)
[15] https://github.com/ggml-org/llama.cpp/blob/master/docs/speculative.md — llama.cpp Speculative Decoding docs
[16] https://modelfit.io/blog/speculative-decoding-mac-llm — Does Speculative Decoding Speed Up Local LLMs on Mac?
[17] https://dev.to/trulyfurqan/7-open-source-codebase-context-tools-for-engineering-teams-3293 — 7 Open-Source Codebase Context Tools
[18] https://www.digitalapplied.com/blog/swe-bench-terminal-bench-benchmark-guide-2026 — SWE-Bench vs Terminal-Bench: Harness Governance (DigitalApplied)
[19] https://platform.claude.com/cookbook/tool-use-context-engineering-context-engineering-tools — Context Engineering for AI Agents (Anthropic Cookbook)
[20] https://handbook.modular.com/inference-optimization/kv-cache-offloading — KV cache offloading (Modular Inference Handbook)
[21] https://www.reddit.com/r/LocalLLM/comments/1t93qps — Local LLMs are 12-24 months from taking over (r/LocalLLM)
[22] https://zachrattner.com/projects/ai-mac-cluster/local-ai-use-cases — Local vs cloud AI architecture: hybrid deployment (Zach Rattner)
[23] https://arxiv.org/abs/2605.24220 — Polar: Agentic RL on Any Harness at Scale
