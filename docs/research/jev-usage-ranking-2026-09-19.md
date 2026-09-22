# Jev usage ranking — the best ways to use Jev to complement an LLM in an agentic harness

**Research date:** 2026-09-19. **Scope:** TypeSafe AI's System One model (`jev-latest`, pinned `jev-1.13.0` in every measurement cited) used as a complement to a main LLM inside an agentic harness — a coding agent, a browser agent, a support router, an eval pipeline. **Method:** four independent research units (vendor docs; practitioner code and community; mechanism literature; Forge's measured record) produced 115 claims; four adversarial refuters then re-fetched every source and recomputed every number: 109 survive, 6 weakened (fixes folded in below), 0 refuted, 0 unverifiable, plus 30 new findings. Evidence tiers in the ranking: **measured** (preregistered harness runs) > **independent** (third-party code, posts, evaluations) > **vendor** (TypeSafe materials) > **research** (analogous mechanisms in papers and engineering posts). Full claim → source → verbatim-quote chain: run directory `wf_jev_rank_b0588a99`.

**Bottom line.** The best-evidenced ways to use Jev in an agent loop are (1) scoring and re-ranking shortlists, (2) grading outputs offline, and (3) gating risky actions — decision points where a typed, cheap, ~0.5 s judgement complements the main LLM. The most-tested way (post-failure repair feedback) currently carries a negative measured verdict as shipped, and the strongest Forge-measured win (retrieval rerank) is unreachable in the current loop. Two properties drive every ranking below: Jev returns typed decisions, never text [3], and its answers are stochastic per sample — consistency, not determinism, is the design target [2][17].

What Jev is, in one paragraph. One request carries a shared JSON state and a batch of typed questions (Choice / Score / Noul); answers come back as choices with per-option probabilities, rubric scores, or a yes/no probability [3]. Text-only state, English-primary [5]; ~70-500 ms vendor-reported [1][8] and 125-984 ms across Forge's judge runs (`benchmark/results/2026-09-18-judge-shadow-replay/README.md`: 125-547 ms shipped path, 317-484 ms replay; `benchmark/results/2026-09-17-judge-rerank-v2/README.md`: max 984 ms); $0.042/M input tokens, output free [8]. Early access, waitlisted [1][2]; the launch drew immediate community explainers and build galleries [45][39]. It cannot generate text, execute work, or be the authority for anything [3][6]; the vendor positions it as a component to "score, judge, verify, guardrail" LLM prompts, traces and outputs [1][6], with the FAQ's own scope note: common-sense judgments — classifying content, routing requests, scoring responses, evaluating information — while extended reasoning (math, chess-like planning) is "better suited to large reasoning models" [2].

## The ranking

### Tier A — strongest evidence today

**1. Score and re-rank a shortlist — one Noul per candidate, batched into a single request.** *(loop point: retrieval / context selection; also review-candidate selection)*
Give Jev a candidate list (retrieved passages, candidate files, review targets, search hits) with one narrow question per candidate ("does this excerpt contain the answer?"), sort in code by returned probability, fail open to the deterministic order.
- Measured: Forge's corrected v2 retrieval-rerank screen met its preregistered bar on all three clauses — MRR 0.288→0.716 (+0.427), hit@1 5.0%→71.2%, zero rank-1 regressions, 79/80 engagement, median 531 ms, ~$0.0123 for the whole 120-cell screen (`benchmark/results/2026-09-17-judge-rerank-v2/README.md`). This is the largest measured quality delta in this review.
- Vendor-run: same shape on 40 legal queries — Top-1 5%→18%, Top-10 38%→62%, 1,200 calls for $0.0645 [16].
- Independent re-implementations: review evidence selection [27], browser target selection [25], the "retrieve, then judge" pattern in a practitioner guide [35]; the code-domain analogue (execution-grounded candidate selection) is among the strongest published selection results [57].
- Forge status: shipped (`src/repo/retrieval.c`), but agent-level engagement is zero — the measured population never calls `retrieve_context` (0 of 1,258 retained session event logs), so E3 recorded "the mechanism does not engage in this population" (`benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md`; `docs/research/jev-augmentation-avenues-2026-09-18.md`). The way is sound; the surface is not reached. Two wiring gaps before expanding: the 32-candidate cap vs 50-hit callers (over-cap fails LIMIT, then fails open) and the rerank/trim pinning gap.
- Caveats: per-sample stochastic — use it as an ordering, never as labels; for high-stakes selection, sample N times and aggregate — GenRM, a generative verifier using majority voting, outperformed discriminative verifiers and LLM-as-judge in best-of-N evaluations [54], and sampling-and-voting helps generally [58].
- Next: decide the reachable surface (wire rerank where the agent actually retrieves, or select a retrieval-forcing population) before any further inclusion change.

**2. Grade outputs offline and online — rubric judge over answers, checks and traces.** *(loop point: analysis / evals)*
- Measured (independent): Good Start Labs' production grading — 6,003 rubric checks, 10,500 grading calls, zero failed gradings, ~0.5 s per call, 91.5% verdict agreement with Claude Fable 5.1, at $160 per million graded answers vs $33,000 (Fable) and $260 (DeepSeek) [38]. Agreement is not accuracy, and the same source's advice is redundancy, not replacement: "two cheap readers that fail differently", grade the steps, send disagreements to a person [38] — independently echoed by an r/AI_Agents commenter ("run five times and take the majority vote") [43].
- Integrations: Braintrust ships Jev as a scorer keeping choice, confidence and probabilities in result metadata for experiments and online scoring [40]; LangChain's partner package preserves probabilities and confidence for downstream routing [30]; an independent production classifier pins `jev-1.13.0` for news threat scoring with criteria written as described situations because Jev does not count [31].
- Caveats: fix an operating point (GSL uses pass at p≥0.70) and recalibrate per task; LLM judges reach >80% human agreement [59] but carry documented biases (position [60], self-preference [61]). Mitigations: order-balanced aggregation [60] and disjoint-family panels [62] — a typed judge from a different family than the main LLM is structurally aligned with that mitigation.
- Forge status: applies directly to G05 (offline failure-taxonomy classification; labels never become gates or denominators) and to shadow/replay analysis.

**3. Gate actions before execution — permission and risk screen.** *(loop point: guardrails / permission screen)*
One batched request before running a command or tool: Nouls (destructive? exfiltrating? beyond scope?) plus a severity Score; code thresholds map to allow / ask / deny.
- Independent ×3, convergent shape: pi-jev (four typed questions in one request, ~300 ms per judgement, shadow mode default, every error path fails open) [23]; Hermes' jev-approvals catalog plugin (APPROVE / DENY / ESCALATE, upstream errors fail closed) [33]; flaviocopes' "smart if statement" (read-only / reversible / destructive before running) [36]. Vendor cookbook guardrails run the same pattern over LLM messages [14]; the academic analogue (Llama Guard) shows the classifier category works [70].
- Caveats: state is not treated as hostile — injected text can move answers, so this is one filter, not a security boundary [11][13]; fail-open vs fail-closed is a deliberate per-action choice (both postures are published) [23][33]; shadow-first. Data leaves the machine — gated command text and policy are sent to the service, which Hermes' plugin discloses [33].
- Forge status: not integrated (deterministic permission handling exists); an experiment only if Forge gains an action surface worth screening.

### Tier B — strong candidates; experiment next

**4. Decide what stays in context — tool-output pruning and admission.** *(loop point: tool-result interpretation / context assembly)*
Keep or drop chunks of oversized tool output before it returns to the main model; prune retrieval candidates before the budget trim; treat context rot as first-class.
- Evidence: jev-pruner — an independent Claude Code plugin using one Noul per chunk of oversized bash stdout (≤10k-token outputs pass untouched; a chunk is kept if any query clears the bar or was unscored; errors/diffs/binary bypass) [22]; vendor guidance to filter and retrieve in code first because "Jev suffers from context rot" [11][35]; browser-use's numbered element tables [25].
- Counter-evidence: no-mistakes' live A/B of a review pre-brief listed 0 of 40 candidates, added ~19,155 tokens (+26% billed input) and moved nothing — a threshold-saturation failure, not a mechanism failure [32].
- Forge status: seams D4 (context ranking), D22 (diagnostic-view ranking); D44's truncation premise is untriggered in the measured population and stays off. Measure output sizes and thresholds first; preserve prefix stability.

**5. Route and triage — model tier, effort, next path.** *(loop point: escalation / triage)*
- Evidence: LiteLLM's production complexity router ships a `jev` classifier (default `jev-latest`, 3 s timeout, circuit breaker, env-key guard) [29]; a Claude Code mod routes tier/effort/risky per turn from three questions and deliberately ships main-model routing OFF because switching the main model mid-session invalidates the prompt cache [28]; routing literature shows large cost cuts when a cheap classifier gates an expensive model (RouteLLM >2× in cases [63]; FrugalGPT up to 98% [64]).
- Forge status: single local model — no tier to route to; effort/thinking-budget routing and escalation remain possible. Best-of-N candidate selection is a closed axis for Forge.
- Caveats: cache-invalidation economics; a routing change needs its own eval.

**6. Choose tools and skills; bind closed-set arguments.** *(loop point: tool use)*
- Evidence: vendor skill-suggestion cookbook cut wrong skill loads 16.8%→7.3% and needless loads 9.8%→4.0% across 488 requests — with the honest caveat that some choices the agent had right come back wrong once a suggestion is attached [15]; Composio compiles a tool set into Jev questions so Jev picks the tool and binds closed-set arguments, returning call / partial / abstain — "Jev answers closed questions. It never writes text." [24]; Hermes' jev-typesafe plugin exposes check/route/score tools to the main LLM [34].
- Forge status: the minimal loop has a small tool set; the larger menu (skills, symbol tools) is the natural home.
- Caveats: menu quality drives value; watch the suggestion-attachment failure mode [15].

**7. Triage escalation and stop-loss.** *(loop point: escalation / stop-loss)*
- Evidence: vendor confidence-routing pattern (per-action thresholds; 0.6 floor routes to a human) [7][10]; the consistency cookbook maps 0.30-0.70 to an explicit "uncertain" band for human review [17]; calibration literature: verbalized confidences are typically better calibrated than conditional probabilities but systematically overconfident, and no elicitation technique consistently wins [66][67]; conformal methods give set-level guarantees at the price of a calibration set [68]; self-knowledge (P(IK)) partially generalizes across tasks but is poorly calibrated on new ones — recalibrate per task [65].
- Forge status: G04 seam — shadow-only; any control use needs an explicit decision.
- Caveats: thresholds are brittle; calibrate on the harness's own population.

### Tier C — conditional, measure-first, or niche

**8. Repair-loop advisory feedback (post-failure guidance).** *(loop point: candidate/repair loop)*
The most-tested way — with a negative measured verdict as shipped. Forge appends one batched judgment (failure-family, evidence-gap, repair-readiness, next-action) after a failed candidate validation. Its first screen met the continue bar (3P vs 1P) (`benchmark/results/2026-09-18-typesafe-repair-feedback/README.md`); the preregistered 64-cell campaign then **refuted** the hypothesis for the frozen population — treatment 25/32 vs control 26/32 — no net gain (one engaged cluster +1, the most-engaged cluster −2 with the preregistered guardrail tripped) — and the shadow replay explains why: the consumption thresholds are saturated (`uncertain` fires 60/60; all 75/75 judgments render the same "inspect" guidance) and answers are stochastic at N=4 (0/15 episodes reproducible; 5/15 flip a choice) (`benchmark/results/2026-09-18-judge-shadow-replay/README.md`; `benchmark/results/2026-09-19-typesafe-repair-feedback-campaign/README.md`).
- The literature agrees with the shape of that result: Self-Refine showed ~20% average gains with a same-model critic [47], but that setup is exactly the one later work found fragile — intrinsic self-correction without external, actionable feedback fails and can degrade reasoning [48]; iterative refinement against a same-context evaluator invites reward hacking [51]; PRMs are brittle out-of-domain [71]; self-critique can diminish planning performance [73]. Grounded, actionable feedback is what works [49][50]; the evaluator-optimizer pattern is a named production shape with the same gate ("clear evaluation criteria", measurable value) [46].
- The better-shaped variant: pi-jev's output judge — a failure-class Choice (confidence-gated) indexing a fixed advice table; repair guidance as a lookup, not generated prose [23].
- Forge status: parked as shipped behind `--judge`, out of default profiles. Next: retune the consumption thresholds on a larger shadow set (next_action_confidence never reaches the 0.55 bound — max 0.42 across the 60 replayed samples; evidence_gap flips across the 0.60 gate) before any further campaign.
- Caveats: 6 clusters × 4 reps excludes only large effects; a constant-hint treatment cannot attribute outcomes to typed guidance.

**9. Order validation — which test or command runs first.** *(loop point: validation ordering)*
Forge's D17+G11 seam: ordering-only; the broad stage stays authoritative and every planned command still runs; measure the validation share of wall time first. No external analogue with measured effect was found; the closest research is step-level (process) verification — effective but label-hungry [52], with automatic supervision reducing the label cost [72], and mostly studied in math reasoning [53]. Experiment, after the measurement. The staged review pipeline [27] is the closest shipped shape.

**10. Browser/UI action selection.** *(loop point: action selection)*
browser-use's jev-ultrafast asks one request per step for the operation plus every candidate target (two decisions, one round trip; author-reported 7.1 s Zurich→London — the $0.0039 cost figure is author-reported, not in the repo) [25][39]; typesafe-computer-use reports $0.0002 per decision vs $0.032 for a frontier model on a bare screenshot, with the honest caveat that "every piece of reasoning the frontier model does for free has to be rebuilt here as deterministic state" [26]; an HN practitioner reported 21-23 correct decisions for ~$0.001 [41]. Niche unless the harness drives UIs; state engineering is the cost of admission.

**11. Content guardrails — LLM input/output screening.** *(loop point: guardrails)*
Vendor cookbook: one request per message, hazard Nouls + severity Score, pass/review/block; all sample jailbreaks in the published table were detected (vendor-run, jev-1.12) [14]. Llama Guard shows the category is real [70]. Caveat from the vendor's own RAG cookbook: "a filter, and only one" — sub-threshold content still reaches the model; nothing here is a security boundary [13]. Lower priority for a coding harness whose inputs are not end-user content; even calibrated models carry irreducible error on rare facts [69].

**12. Cross-run memory and lessons admission.** *(loop point: cross-run memory)*
Reflexion's episodic self-reflection (external signals + retry budget) reached 91% pass@1 on HumanEval vs 80% for its GPT-4 baseline [50]; Forge's G07/G10 holds need an overfitting guard; no harness has yet measured a Jev-shaped admission decision. Not ready.

## Cross-cutting principles

- **One batched request per decision point.** Questions evaluate in parallel and in isolation — one answer never informs another; dependent judgments need a second request in code [3]. Speculative fan-out (ask conditionally-relevant questions now, ignore in code) is the vendor's documented default for agents, and its agent skill exists because coding agents under-batch [9][18][12]. Extra questions cost tokens, barely latency [9][18].
- **State engineering is the real work.** Filter and retrieve in code before calling; unrelated state costs accuracy [11][35]. The vendor's own CEO: coding's hard part "is actually state engineering (e.g. getting your dependencies in context) — we haven't even tried it yet" [41].
- **Stochasticity is expected; consistency is the design target** [2]. Borderline answers cross thresholds even when noise is low [17]. Never use a single sample as a label or stratum; aggregate cheap samples (majority vote / second reader) [38][43][58].
- **Thresholds saturate silently.** When a gate never fires (or always fires), the mechanism degrades to a constant — Forge's repair feedback (60/60 uncertain) and no-mistakes (0/40 listed) are both instances [32] (`benchmark/results/2026-09-18-judge-shadow-replay/README.md`). Calibrate bands on the harness's own population before relying on typed guidance.
- **No success authority, ever.** Jev cannot declare tests passed, approve a candidate, or bypass host validation; thresholds stay host-side; fail-open by design (or deliberately fail-closed at gates) (`include/forge/judge.h`; `docs/research/jev-augmentation-avenues-2026-09-18.md`; [23][33]).
- **Pin the version and record provenance.** `jev-latest` moves; pin `jev-1.13.0` when tuning thresholds and record the returned model id and server request id [8][35] (Forge: request-id capture shipped in `11e8c822`).
- **Budgets, caps, limits.** ~32k tokens shared by state + questions (docs elsewhere say 64k/request — vendor-internal inconsistency) [4][8]; Choice cardinality ≤255 with a two-stage workaround beyond [1]; 250k tok/s and 1,200 RPM, adjusting dynamically, 429s expected [8][35].
- **Adversarial state.** Untrusted text in state can steer answers; do not put Jev between hostile input and a safety-critical decision without independent checks [11][13].
- **Data leaves the machine.** State (and, in gating use, command text) is sent to the service; Hermes' plugin discloses this, and the community notes default archiving for product analysis with an opt-out [33][44]; the MCA covers data use but is not a zero-retention claim [20].
- **Legal:** MCA 2.3(f) bars publishing benchmarks or performance information about the service without a carve-out — relevant to any public benchmark practice [20].
- **The interface is separable from the model.** TypeSafe's System One adapter re-implements the same client over ordinary LLM endpoints, enabling an A/B of the decision interface against LLMs before committing to the vendor [21].

## Corrections and contested numbers (from the refutation pass)

- Campaign README's "per-call latency mean 521 ms" is the unweighted mean of the 24 per-cell mean latencies (521.08); the true per-call mean is 490.9 ms (15,217/31). A wording fix is suggested to that README (not applied here).
- Shadow-replay p90: 430.8 ms = linear interpolation; strict nearest-rank = 430.5; 433.3 = 55th order statistic. The measured-record claim's "nearest-rank 433.3" label was wrong; the README value stands.
- A2 screen median 422 ms uses the upper-middle convention of [125,140,391,422,500,516]; mean-of-middle-two = 406.5.
- Payload ranges 7.9-25.6 kB / 3.5-7.8 kB are pretty-serialized decimal-kB; compact serialization gives 7.6-24.9 KiB / 3.3-7.5 KiB. Both are real; records store parsed request objects.
- Vendor-internal inconsistencies: batching "12.2× cheaper / 10.0× faster" [16] vs "11.5× / 9.6×" [4]; budget 32k vs 64k [4][8]; pricing FAQ ("profitable at current prices" [2]) vs the launch-post's "can't prove it isn't subsidized" nuance [1].
- browser-use $0.0039 is not in the repo (author-reported; aggregated on madewithjev.com) [25][39]; madewithjev's "186 builds / 75 guides" is as of 2026-09-19 (day 4), not "three days" [39].
- RAG-screening claims carry the vendor's own caveat: "a filter, and only one ... Nothing here is a security boundary" [13].
- Overoptimization boundary: synthetic gold-reward setup, optimizer-dependent curve [55]. Test-time-compute boundary: MATH benchmark [56].
- E3: the v2 README's "eligible to run" is superseded — E3 ran its preflight, recorded zero engagement, and did not proceed per its clause 1 (`benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md`).
- G01 instrumentation gap closed at HEAD (commit `61694349`): `metrics.json` carries judge counters and `judge_feedback` events carry turn/candidate_attempts/validation_id linkage; thresholds remain hardcoded (`src/core/agent.c`).
- Community counterpoints: r/AI_Agents disputes the 200×/400× framing as measuring "the cheap part of the loop" [43]; r/SideProject questions reliability without per-use-case benchmarks [44]; one early-access user left the preview because Jev answered the vendor's own quickstart example wrong [42].

## What would change this ranking

- **An independent accuracy benchmark.** Agreement is not accuracy [38]; the vendor's eval references are model-generated and self-flagged as biased toward OpenAI/Anthropic [1][19], and this review found no third-party reproduction of the vendor benchmark.
- **Agent-level evidence.** Forge's largest measured delta (rerank) is retrieval-level; most ways rest on third-party code plus vendor runs, not preregistered harness A/Bs.
- **Reachability.** The ranking assumes the harness hits the decision point; Forge currently does not hit its best-measured one (E3).
- **Threshold calibration outcomes.** A2's retune decides whether typed guidance becomes actionable or stays a constant hint.
- **Version drift.** `jev-latest` moves; behavior can change under tuned thresholds [8].

## Recommended next experiments (Forge), in order

1. A1 surface decision + wiring fixes (over-cap path, rerank/trim pinning), then an agent-level engagement measurement where retrieval actually happens.
2. A2 threshold retune on a larger shadow set; re-campaign only if the bands discriminate.
3. C1 read-routing shadow adapter (feasibility report before control), batching D22/D40 questions into the same requests.
4. G04 escalation-triage shadow; D4 context-ranking shadow; D17 validation-share measurement.
5. Keep G01 instrumentation on everything; record request ids; pin `jev-1.13.0`.

## Provenance

- Claim chain: run directory `wf_jev_rank_b0588a99` — `attempt_w1..w4.md` (115 claims), `refute_r1..r4.tsv` (109 survive / 6 weakened / 0 refuted / 0 unverifiable; 30 new findings), `sources_map.json`.
- Forge artifacts cited are at HEAD `bfadc7bf` (read-only); campaign numbers include the post-verification corrections of 2026-09-19.
- Web sources: numbered below, rendered from the citation ledger.

## Sources

[1] https://typesafe.ai/blog/introducing-system-one-models-and-jev
[2] https://typesafe.ai
[3] https://docs.typesafe.ai/introduction
[4] https://docs.typesafe.ai/primitives.md
[5] https://docs.typesafe.ai/concepts/state
[6] https://docs.typesafe.ai/concepts/how-to-build-with-system-one
[7] https://docs.typesafe.ai/confidence
[8] https://docs.typesafe.ai/models
[9] https://docs.typesafe.ai/patterns/fan-out
[10] https://docs.typesafe.ai/patterns/confidence-routing
[11] https://docs.typesafe.ai/model-jaggedness/jev-1.13
[12] https://docs.typesafe.ai/agent-skill
[13] https://docs.typesafe.ai/cookbooks/classifying_rag_passages
[14] https://docs.typesafe.ai/cookbooks/llm_guardrails
[15] https://docs.typesafe.ai/cookbooks/skill_suggestion
[16] https://docs.typesafe.ai/cookbooks/rerank_typesafe
[17] https://docs.typesafe.ai/cookbooks/consistency_noul_cookbook
[18] https://docs.typesafe.ai/cookbooks/parallel_questions.md
[19] https://evals.typesafe.ai
[20] https://typesafe.ai/legal/mca
[21] https://github.com/typesafe-ai/system-one-adapter-python
[22] https://github.com/tamaratran/jev-pruner
[23] https://github.com/y0usaf/pi-jev
[24] https://github.com/ComposioHQ/composio/blob/master/python/providers/typesafe/README.md
[25] https://github.com/browser-use/jev-ultrafast
[26] https://github.com/awlevin/typesafe-computer-use
[27] https://github.com/devagrawal09/jev-review
[28] https://github.com/davila7/claude-code-templates/blob/main/cli-tool/components/mods/productivity/jev-model-router/README.md
[29] https://github.com/BerriAI/litellm/blob/main/litellm/router_strategy/complexity_router/config.py
[30] https://github.com/langchain-ai/langchain/blob/master/libs/partners/typesafe/langchain_typesafe/classifier.py
[31] https://github.com/koala73/worldmonitor/blob/main/shared/jev-classify.js
[32] https://github.com/kunchenguid/no-mistakes/blob/main/benchmarks/issue-1125/results.md
[33] https://github.com/NousResearch/hermes-agent/blob/main/plugin-catalog/jev-approvals.yaml
[34] https://github.com/NousResearch/hermes-agent/blob/main/plugin-catalog/jev-typesafe.yaml
[35] https://dev.to/valyuai/how-to-use-jev-a-practical-guide-to-typesafes-system-one-model-g5e
[36] https://flaviocopes.com/jev
[38] https://goodstartlabs.com/research/verification-is-the-bottleneck
[39] https://madewithjev.com
[40] https://www.braintrust.dev/blog/evaluate-agent-responses-with-jev
[41] https://hn.algolia.com/api/v1/items/49717558
[42] https://www.reddit.com/r/PiCodingAgent/comments/1whsav6
[43] https://www.reddit.com/r/AI_Agents/comments/1wk2p48
[44] https://www.reddit.com/r/SideProject/comments/1wiw6tk
[45] https://www.datacamp.com/blog/system-one-models-jev
[46] https://www.anthropic.com/engineering/building-effective-agents
[47] https://arxiv.org/abs/2303.17651
[48] https://arxiv.org/abs/2310.01798
[49] https://arxiv.org/abs/2305.11738
[50] https://arxiv.org/abs/2303.11366
[51] https://arxiv.org/abs/2407.04549
[52] https://arxiv.org/abs/2305.20050
[53] https://arxiv.org/abs/2410.08146
[54] https://arxiv.org/abs/2408.15240
[55] https://arxiv.org/abs/2210.10760
[56] https://arxiv.org/abs/2408.03314
[57] https://arxiv.org/abs/2207.10397
[58] https://arxiv.org/abs/2203.11171
[59] https://arxiv.org/abs/2306.05685
[60] https://arxiv.org/abs/2305.17926
[61] https://arxiv.org/abs/2404.13076
[62] https://arxiv.org/abs/2404.18796
[63] https://arxiv.org/abs/2406.18665
[64] https://arxiv.org/abs/2305.05176
[65] https://arxiv.org/abs/2207.05221
[66] https://arxiv.org/abs/2305.14975
[67] https://arxiv.org/abs/2306.13063
[68] https://arxiv.org/abs/2306.10193
[69] https://arxiv.org/abs/2311.14648
[70] https://arxiv.org/abs/2312.06674
[71] https://arxiv.org/abs/2412.06559
[72] https://arxiv.org/abs/2312.08935
[73] https://arxiv.org/abs/2310.08118
