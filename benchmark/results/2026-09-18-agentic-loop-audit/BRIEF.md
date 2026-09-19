# BRIEF -- Forge agentic-loop performance audit + Jev augmentation seams
Run: C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5   Repo: C:/Users/flowc/dev/forge   Revision: d16e6736918808daa9c1f1993093a6dcd6ea0af8 (branch main; untracked evidence dirs are read-only)
Date: 2026-09-18 18:07

## Mission
Two deliverables over the same read: (1) find REMAINING performance opportunities in Forge's
local-model agentic loop, located and falsifiable; (2) find every place a hosted typed-judgment
model (TypeSafe System One, "Jev") could augment the loop. Read-only discovery: no repo edits,
no builds, no tests, no model runs, no git writes, no network. You write only into the run dir.

## What Forge is, and what "performant" means here
Forge is a C17 local coding agent that links llama.cpp directly: reusable KV prefixes,
token-budgeted context selection, grammar-constrained tool calls, host-owned validation.
Judging criteria, in priority order:
1. wall-clock per completed task and its components: startup/model load, prefill, decode,
   sampling, tool execution, validation, indexing, hosted-judge latency.
2. wasted work: re-prefill after a cache break, cumulative input tokens re-sent every turn,
   unproductive turns/actions (repeat or no-op patches, cap-deaths), duplicate host computation.
3. failure-class removals: pinned-context failures (16,384 was a constant while ~5 GiB VRAM sat
   unused), action-wall / cumulative-input deaths.
4. host overhead inside the loop: allocations, hashing, snapshotting, process spawns, IO
   flushes, per-token work.
Correctness constraints never yield to speed: host validation authority, budgets, retained
failures, preregistered bars, defaults unchanged unless measured.

## Evidence culture (binding)
- No speedup may be asserted without a measurement or a deterministic proof; label reasoning as reasoning.
- docs/RUN_EFFICIENCY.md is a launch gate for model batches; you launch nothing.
- Failures are retained and reported as failures.
- CLOSED AXES -- do not file claims to reopen these. If you have contradicting measured evidence,
  put it in NOTES only: host evidence presentation (v2/v3/v4-reverted, bounded repair, c04
  elide-noop, c05 host-defects); thought-history retention (measured harmful); sampler-variance
  control; repetition penalty 1.05/last-64; prefix stability admission floor; exact-argv
  run_command dedup; best-of-N as budget splitting; the retraction family / Qwen3-Coder /
  loop-pilot profile; CUDA Unified Memory.
- SETTLED mechanisms (spikes/commits; verify implementation state, do not re-propose): greedy
  grammar fast path + reduced-prefix fallback (spikes 002/003/006, commit 017cbb0b),
  grammar-forced-run elision falsified (007), speculative decode blocked (008), N-way batched
  decode measured (001), first-byte prefilter (004), grammar-after-topk (005).
  See spikes/*/README.md and docs/plans/agent-performance-avenues.md section 1.

## Reference sources (open what your task needs; never rely on memory)
- docs/plans/agent-performance-avenues.md -- evidence, closed axes, M/A avenues, recommended order
- docs/plans/agent-performance-implementation.md -- P0.1 (KV type/flash-attn/planner sizing),
  P0.2, P0.3 planned; P1-P4 gated. Verify CURRENT implementation status against source; file
  claims only where reality lags or contradicts the plan.
- docs/ARCHITECTURE.md, docs/MODEL.md, docs/CONFIG.md, docs/RETRIEVAL.md, docs/CHECKPOINTS.md,
  docs/AGENT_LOOP.md, docs/MEMORY.md, docs/STATE.md, docs/VALIDATION.md, docs/DIAGNOSTICS.md,
  docs/INDEX.md, docs/WATCH.md, docs/SUMMARIES.md, docs/REPRODUCIBILITY.md
- benchmark/README.md (measurement definitions: prompt_tokens, cached_tokens, prefill_tokens,
  decode_ms, sampling_ms, index_ms, validation_ms), benchmark/ANALYSIS.md
- forge.toml.example, docs/CONFIG.md -- the config surface
- spikes/*/README.md
- include/forge/judge.h + src/judge/judge.c -- the existing Jev integration
- docs/research/jev-integration-2026-09-17.md -- existing Jev proposal (read-routing MVP)
- benchmark/results/2026-09-17-judge-rerank/ (v1: NOT MATERIAL, bar not met) and 2026-09-17-judge-rerank-v2/ (post-fix: MATERIAL, bar met on all three clauses); E3-RESULTS.md records zero agent-level engagement -- arm 1 (retrieval rerank)
- benchmark/results/2026-09-18-typesafe-repair-feedback/README.md -- arm 2 (repair feedback):
  continue bar met (directional); population manifest frozen for the larger campaign
- docs/research/local-model-tooling-gaps-2026-09.md -- external gaps survey

## Jev facts you must anchor to (from the research doc; do not re-derive)
Jev returns typed answers only: Choice (one of N + probabilities + confidence), Score (ordered
levels), Noul (yes/no probability). One request can batch many questions over one shared JSON
state; question ids are host-side. It never generates text, never executes work, never gates.
POST https://api.typesafe.ai/v1/systemone, bearer key from env (TYPESAFE_API_KEY),
jev-latest -> jev-1.13.0; vendor-reported 70-500 ms; $0.042/M input tokens. Constraints:
advisory only with deterministic fallback; offline configurations must keep working; TypeSafe
MCA 2.3(f) restricts publishing benchmark numbers.

## Deliverable: <RUN>/attempt_<domain>.md
Exactly these sections:

### CLAIMS
At most 18, best-first. ASCII only. One line each:
N|TYPE|FILE:LINE|OBSERVATION|RECOMMENDATION|IMPACT|CHANGE_RISK|CONF|FALSIFIER
- TYPE = GAP | WRONG | DEAD | DRIFT | RISK | COST
  GAP: missing control/mechanism the need requires; WRONG: implementation contradicts its
  need/plan/docs; DEAD: nothing reads it (cite the proof); DRIFT: docs/config/code disagree;
  RISK: works today, breaks at a stated scale; COST: hot-path cost with no offsetting need.
- FILE:LINE repo-relative, forward slashes, exact current line (a wrong line is itself a finding).
- OBSERVATION: located fact; no adjectives without a number or a code fact.
- RECOMMENDATION: concrete change, named at file/function level.
- IMPACT: effect on the criteria above; write 'unmeasured' when it is reasoning.
- CHANGE_RISK: which evidence class the change invalidates (measured-perf-baseline | gpu-suite |
  unit-tests | frozen-candidate | engagement-screen | none-observable) plus
  (safe-now | needs-verify | needs-decision).
- CONF = high|medium|low. FALSIFIER: the check that would prove the claim wrong.
Self-refute before filing: if the code you read falsifies your claim, put it in NOTES instead.

### VERIFIED-OK
At least 8 lines: OK|FILE:LINE|what you verified is correct/optimal and why.
"Fine here" is a valid, required result.

### JEV
At most 8 lines (secondary lens):
JEV|FILE:LINE|SEAM|Q_TYPE|WHAT_CODE_CANNOT_DECIDE|EXPECTED_BENEFIT|RISK|STATUS
- Q_TYPE = choice|score|noul|batch; STATUS = existing-arm-extension|new-proposal.
- A seam is a bounded typed judgment over shared state where the local model currently spends
  expensive work on a guess and deterministic code cannot decide it exactly.
  Falsifier: if code can decide it exactly, it is NOT a seam.

### NOTES
Self-refuted items, settled decisions you checked, files skipped, missing baselines, uncertainty.

## Baselines
In NOTES, list retained measurement artifacts that bear on this domain (paths under
benchmark/results/, spikes/, docs) or state 'no measured baseline for this domain'.

## Rules
Read-only. Use read_file/search_files/terminal (read-only commands only). Cite only lines you
actually read. Do not exceed the claim cap; rank by (expected effect x confidence) / change risk.
