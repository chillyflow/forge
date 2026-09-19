# Jev in Forge: a bounded decision service

Research date: September 17, 2026. Repository inspected at `9df0fe2b73f0ef17d9a422daadea26b9fd1d5049`. Assumption: “Jev” means TypeSafe AI's System One model. This is a proposed experiment, not an implemented integration or a measured Forge speedup. No authenticated Jev calls or model benchmarks were run.

**Recommendation.** Add an optional host-owned decision provider and test it at ambiguous failed-validation episodes. Its first job should be choosing one useful, already specified source read, or deferring to Forge's existing model. Keep the local coding model responsible for generating patches and explanations. Expand into retrieval ranking or reasoning-budget routing only after the first decision demonstrates value.

The opportunity is reducing expensive, unproductive model turns. Merely attaching another opinion to every turn adds latency and can disturb prompt caching.

**What Jev actually supplies.**

Jev accepts shared text/structured state and returns predefined choices, rubric scores, or a yes/no probability (`noul`). It does not generate arbitrary code, shell commands, search queries, or prose. Multiple independent questions can share a request; each sees the same state, not another question's answer. TypeSafe recommends narrow judgments, with their combination implemented in code. [Introduction](https://docs.typesafe.ai/introduction), [state](https://docs.typesafe.ai/concepts/state), [parallel questions](https://docs.typesafe.ai/patterns/fan-out).

The direct interface is `POST https://api.typesafe.ai/v1/systemone`, authenticated with a bearer key, with `model`, `state`, and `questions`. Answers are keyed by question ID; question IDs themselves are not seen by the underlying model. Put the actual meaning into instructions and criteria. Python and JavaScript SDKs exist; a native client can call HTTP directly. [API](https://docs.typesafe.ai/api), [SDKs](https://docs.typesafe.ai/sdk).

The current documented version is `jev-1.13.0`; `jev-latest` is a moving alias. Pin the version for experiments and record the returned model ID. Direct pricing is $0.042 per million input tokens, with free output tokens. At that rate, 5,000 billed input tokens cost $0.00021; twenty such requests cost $0.0042. This is illustrative arithmetic, not observed usage. [Models and pricing](https://docs.typesafe.ai/models).

TypeSafe reports 70–500 ms end-to-end calls and notes its published evaluations generally ran from the US West Coast. Treat those as vendor measurements, not Forge latency guarantees. Its “zero hallucinations” claim concerns schema matching; a schema-valid choice can still be wrong. The published workflow evaluation uses reference answers from other models across four business workflows, rather than independently verified coding repairs. [Launch explanation](https://typesafe.ai/blog/introducing-system-one-models-and-jev), [evaluation methodology](https://evals.typesafe.ai/).

`confidence` on Choice/Score is a statistic derived from the answer distribution, not a separately proven probability of correctness. Noul has no separate confidence field. Thresholds must be evaluated for Forge's actual decisions; neither a high score nor concentrated probabilities can establish that tests passed. [Confidence](https://docs.typesafe.ai/confidence).

The documented deployment is hosted. I found no documented downloadable Jev weights or local inference distribution in the official materials inspected. Therefore this proposal introduces an optional network dependency and sends selected task/source evidence to a service; a fully offline Forge configuration should retain its current path.

**Why this fits Forge's observed problems.**

The historical September 9 investigation found six Python failures consuming all 32 actions and 99.39–99.94% of cumulative input allowance. The problem included 28 byte-identical replacement patches across eight runs, with no anchor-matching failures in 73 patches. This suggests a failure to select a productive next action, not merely an inability to serialize an edit. The same investigation measured 44.4 seconds of re-prefill in a stalled run. These are historical cases, not current-baseline estimates. See [failure analysis](../plans/agent-loop-fresh-design.md).

Detection alone was insufficient: a later no-op gate engaged on 11/12 failing runs and reduced identical-to-identical transitions, yet produced only 1/12 Python completions. The earlier 42-run loop diagnostic also found no intervention above minimal (4/6 versus 3/6 for combined). A Jev experiment must show better completed work, not just fewer repeat warnings. See [no-op screen](../../benchmark/results/2026-09-10-beat-contemporaries/c10-screen/README.md), [loop diagnostic](../AGENT_LOOP.md).

Two qualifications matter. First, the newer reproducibility investigation traced measured run divergence to bytecode timestamps and temporary workspace paths in prompts; do not repeat the older attribution to unavoidable GPU randomness. Second, grammar sampling is a separate measured latency problem. Jev might avoid some generations but will not accelerate the sampler inside generations that still occur. See [reproducibility correction](../REPRODUCIBILITY.md), [sampling investigation](../../spikes/002-grammar-mask-cost/README.md), [reduced-greedy gate](../../spikes/006-reduced-greedy-gate/README.md).

**Where to integrate.**

Forge has two relevant loop paths. The repair profile enters `minimal_run`, not the body of the richer `forge_agent_run` loop. An integration placed only in the latter would miss the intended experiment.

| Location in the inspected revision | Proposed role |
| --- | --- |
| `src/core/agent.c:1009`, `candidate_validate` | Capture a decision snapshot after complete, stable failed validation evidence is established. Both the validation tool and a rejected final use this function. |
| `src/core/agent.c:1124` through the end of `candidate_validate` | Existing checkpoint event and retained evidence provide the episode facts. Incomplete validation should fall back, not masquerade as an ordinary test failure. |
| `src/core/agent.c:1541`, reserved-turn gates in `minimal_run` | Consume a pending decision only on an ordinary repair turn. Preserve validation, final, and reflection reservations. |
| `src/core/agent.c:1603` / `1649`, control and context rendering | Include any accepted advisory metadata late in the prompt. Do not rewrite the stable system/tool prefix. |
| `src/core/agent.c:1732`, minimal model generation | An executed host-selected read can bypass one model action-selection call; ordinary patch generation still follows. |
| `include/forge/forge.h`, `forge_agent_config` | Add a dedicated optional typed callback and user data. Proposed API, not something already available. |

The minimal loop exposes `read_file`, `list_directory`, `apply_patch`, and `run_command`. Its initial Jev menu must use supported operations such as bounded `read_file` calls. The richer loop's `retrieve_context` and symbol tools are a separate extension opportunity. Runtime retrieval currently uses exact names, package neighborhoods, literal matches and FTS/BM25; it has no semantic reranker. [Retrieval contract](../RETRIEVAL.md).

Forge's current inference backend is local GGUF or a script fixture; its tool registry is native. No reusable HTTP/MCP runtime client was found in the inspected source. The repository's Serena MCP configuration is development tooling, not a Forge runtime transport. Existing event, policy, cancellation, question, and streaming callbacks are not a decision-provider interface. In particular, don't overload the authorization callback or human question callback.

**A concrete first decision.**

The host constructs a small packet: current user task, normalized complete diagnostic excerpts, validation/input identity, recent action outcomes, already-read spans, remaining budgets, and a bounded list of candidate reads. Candidate paths and line ranges must come from observed workspace evidence, not be invented by Jev. Include fresh source snippets when needed to judge relevance. `candidate_source` already adds up to 81 lines / 4096 bytes of source evidence, so a proposed read must offer information beyond that existing context. Check actual emitted bytes/intervals and source hashes, accounting for truncation. Call Jev only when at least two novel eligible candidates remain; a single candidate needs no semantic selection service.

Use one choice question for initial control. Two optional diagnostic questions can share the request during shadow evaluation:

| Question | Type | Purpose |
| --- | --- | --- |
| Which broad failure family best matches this evidence? | Choice | Separate an apparent code defect, environment/dependency issue, stale/missing evidence, and unknown. Keep deterministic exit/status facts outside this judgment. |
| Does the available context lack source evidence needed to interpret this failure? | Noul | A semantic estimate of an evidence gap, with explicit incomplete-input handling. |
| Which supplied read would most help interpret the failure? | Choice | Return an existing candidate ID or `defer`; instructions describe each candidate and its content. |

Initially the read choice is the only control output. Log failure-family and evidence-gap answers as model estimates, without using them as extra gates: they can disagree and their Forge-specific calibration is unknown. A choice cannot depend on seeing the other answers inside the same call. Any later combination belongs in host code and needs its own evaluation. Avoid asking the broad question “solve this bug” or requiring Jev to reason through an arbitrary repair plan.

The first active mode performs at most one eligible read per failure episode, after calibrated acceptance checks. The read runs through the existing tool path, permission checks, path rules, output bounds, action accounting, and logging. Its origin is explicitly host/Jev routing, not a fabricated model-generated action. It must become valid conversation/tool evidence for the next local-model turn.

If there is no novel eligible read, Jev abstains, the response is late, or the evidence changed, Forge continues its ordinary loop. Repeated identical failures must not keep resetting the allowance. Reset rules should require a resolved episode or materially new host-observed evidence, not a new timestamp or model assertion.

This MVP may improve information gathering, but it does not demonstrate semantic replanning or stronger code synthesis. If failures already have sufficient context, read routing is the wrong intervention. That is a deliberate feasibility question for replay analysis before implementing active control.

**Provider and latency design.**

Use a dedicated provider contract, conceptually `decide(snapshot, candidates, deadline) -> candidate_id | defer`, with separate typed advisory fields and request/episode IDs. A callback keeps the core transport-independent. Prototype the service adapter with a persistent Python SDK process for replay and controlled live experiments; only add an optional native HTTP adapter if results justify it. Starting a process per decision would contaminate a subsecond latency goal.

Start with a tunable 500 ms total live-decision deadline as an experiment parameter, not a claimed suitable production default. Respect the remaining run deadline and cancellation. Disable synchronous retry/backoff on the hot path; fall back on rate limits and overload and use a circuit breaker. Offline collection can use bounded retries. The SDK exposes retry controls, but the host still needs a real end-to-end bound that includes transport and response handling. [SDK usage](https://docs.typesafe.ai/sdk/python/usage), [retry policy](https://docs.typesafe.ai/sdk/python/api/retries).

Keep source text as data; accept only known response types and candidate IDs, finite in-range numbers, and valid distributions. Recheck policy and source/input identity before consuming a response. Cache only by exact decision inputs, including the candidate menu, question/policy versions, episode budget, effective context version, source identity and pinned provider version. Source hashes alone are insufficient when the task, available actions, user clarification, or a completed reflection changes the evidence. Record probabilities for audit without rendering volatile scores, request IDs, or absolute host paths into every model prompt.

For later optimization, a request based on a completed snapshot may overlap unrelated work, but live mutation requires stale-result rejection. Don't classify a failing check while it is still running or consume a suggestion against a different workspace generation.

Expected savings must satisfy:

`avoided model/tool work > decision latency + serialization + extra reads + cache disruption + wrong-route recovery`

For example, ten 200 ms calls cost two seconds before other overhead. Avoiding one five-second unproductive generation would save roughly three seconds, while avoiding none would slow the task down. These are hypothetical numbers. Measure the actual constrained agent loop, including its prefill and sampler, rather than substituting raw llama-bench tokens/second.

**Experiments that would justify adoption.**

1. Extract decision snapshots from retained sessions, using only information available at each decision. Split by task family/repository, keeping paraphrases together. Include successful ordinary turns and cases where further reading is unhelpful. Measure whether a useful candidate is present at all before assessing the selector.
2. Run Jev in replay/shadow mode. Label acceptable action sets and clear mistakes; there need not be a unique correct read. Measure error rate versus accepted coverage, probability calibration, abstention, candidate coverage, latency percentiles and request failure rate. Shadow agreement cannot establish that acting on the route improves a task.
3. Validate the adapter and dispatch with scripted decisions, including timeout, stale response, denied read, duplicate decision, exhausted budget and malformed output cases. These mechanisms do not need expensive model runs.
4. Freeze three live arms: current Forge; the same event/menu with deterministic read selection; and the same event/menu with Jev selection. Keep budgets, reserved turns, observation format and fallback behavior matched. This separates semantic judgment from the effect of adding a read or changing the control policy.
5. Randomize/interleave runs on a fixed binary/profile and diverse held-out tasks. Preserve every failure and timeout. Use the existing independent success definition and task-cluster analysis, with sample size chosen for a stated effect and precision. Report both overall success and task time, plus paired time on commonly solved tasks so fast failures cannot masquerade as speed.
6. Promote only after demonstrating useful decision coverage, non-regressing independent completion quality, and materially better complete-task latency under preregistered bounds. “Better decisions faster” ultimately requires a joint quality/latency result, not merely a faster router or fewer repeated calls. A small pilot can establish feasibility without supporting a broad claim.

Record provider wall time and input usage separately from local-model tokens; also record fallbacks, accepted routes, executed novel reads, downstream turns, prefill/decode/sampling, tool/verification time, cache reuse and complete task latency. A request being made is not evidence that it changed behavior. Follow [run efficiency](../RUN_EFFICIENCY.md), [timing definitions](../../benchmark/README.md), and [task-cluster analysis](../../benchmark/ANALYSIS.md).

**Scope boundaries and follow-on work.**

Exact duplicate detection, budgets, permissions, stale inputs, exit codes, test completeness and final acceptance remain host computations. Jev may prioritize which test to run first in a future experiment, but cannot remove required broad validation. Preserve the distinction between model hypotheses and host evidence in [working state](../STATE.md).

After the first result, the most plausible next experiment is ranking optional retrieval excerpts before budget trimming in the richer loop. It could reduce prompt size and irrelevant reads, but it can also remove useful evidence or disturb ordering. Preserve mandatory/latest evidence and deterministic fallback. Per-episode thinking budgets or choosing among completed passing candidates are later experiments; Forge's sequential shared-budget candidate runs mean a new judge cannot erase the cost of generating those candidates.

For a local comparison, TypeSafe publishes a System One adapter that exposes similar question types over conventional LLM APIs, including custom compatible endpoints. This can test whether the decision interface helps independently of Jev, but does not supply Jev weights or its speed/calibration. [Adapter repository](https://github.com/typesafe-ai/system-one-adapter-python).

One concrete publication constraint matters for Forge's public benchmark practice: TypeSafe's current master agreement section 2.3(f) restricts publishing benchmarks/performance information. Check the applicable agreement or obtain a carve-out before publishing service measurements. Section 4.1 also says customer data is not used for model-weight training without prior consent; that does not make a hosted call local or establish zero retention. [Current agreement](https://typesafe.ai/legal/mca).

The recommended first deliverable is a replay/shadow decision adapter plus an evidence-feasibility report. Proceed to a small active read-routing experiment only if a useful, currently missed action is present often enough to repay the added call.
