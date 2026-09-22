# Jev rerank surface — implementation plan

Date: 2026-09-20. Repo HEAD `bfadc7bf`.

**Problem.** The best-measured jev delta (retrieval rerank: MRR 0.288→0.716, hit@1 5%→71%, zero rank-1 regressions) is inert because the agent loop never calls `retrieve_context` — the minimal/repair schema only exposes `read_file`, `apply_patch`, `run_command`, `list_directory`, `final`. E3 recorded zero engagement and did not proceed.

**Goal.** Wire rerank to a surface the benchmark loop actually hits, then re-measure. Kill if engagement stays zero after wiring.

**Win condition.** Avoided local-model work (turns, tokens, prefill) > jev latency + serialization + wrong-route recovery, *and* measurable MRR/hit@1 improvement at that surface.

---

## Decision: which surface

Three candidate surfaces, ranked by measured reach and engineering cost:

| Surface | Current reach | Wiring cost | Notes |
|---|---|---|---|
| **S1: `search_text` (literal `instr(content, query)` scan, path-ordered, max 50 hits)** | read_file is the #1 tool in retained logs (1086/1179); `search_text` is a distinct call but same read-shaped surface | **Low** — add rerank callback to `search_text` dispatch path | N2: path order is literal-incidental, not BM25; rerank here replaces path order deliberately. N3: 50 hits > 32-candidate cap → over-cap fails LIMIT, fails open silently. **Must clamp or raise cap.** |
| **S2: route minimal-loop reads through `retrieve_context`** | Zero today; would require adding `retrieve_context` to minimal schema | **High** — schema change + prompt redesign | High reach but invasive; conflicts with minimal-loop design intent |
| **S3: retrieval-forcing population** | Task-dependent; can select tasks whose prompts naturally trigger retrieval | **None** — population selection only | Doesn't fix the wiring; defers the decision |

**Recommendation: S1 first** (lowest cost, fastest signal, read-shaped surface already dominant). S3 as fallback if S1's literal-scan semantics degrade rerank's value. S2 deferred — invasive, conflicts with minimal-loop contract.

---

## Phase 1 — Prerequisite: N3 fix (clamp/raise candidate cap)

**Why now.** Without this, wiring rerank to `search_text` silently fails open on most calls (50 hits > 32 cap), recording rerank failures as engagement while keeping path order — a silent no-op that contaminates the screen.

**Changes:**
1. Raise `FORGE_JUDGE_DEFAULT_MAX_CANDIDATES` from 32 to 64 in `include/forge/judge.h` (still well under `FORGE_JUDGE_MAX_CANDIDATES` = 256).
2. In `src/tools/tools.c` `search_text` dispatch, clamp `fg_repo_search` results to `c->config.judge->max_candidates` *before* passing to rerank (callers pass N, search returns up to 50, rerank gets min(returned, cap)).
3. Add a test in `tests/unit/test_judge.c`: call rerank with 40 candidates, verify all scored; call with 200 (above hard cap), verify LIMIT returned and fail-open preserves input order.
4. Record the `rerank_scored` count vs `candidates` count in `forge_retrieval_stats` so the screen can distinguish "rerank scored all" from "rerank failed open silently."

**Verification:** Build + full GPU suite (`build-gpu/Release/forge_unit.exe`). New rerank-cap tests pass.

---

## Phase 2 — Wire rerank into `search_text`

**Changes:**
1. In `src/tools/tools.c` (~line 1838), the `search_text` dispatch currently calls `fg_repo_search(c->repo, query, 50, e)` directly. Replace with:
   - Call `fg_repo_search` to get up to 50 candidates (paths + snippets).
   - If `c->config.judge` is set, call `forge_judge_rerank` over the candidates with the same options pattern as `retrieve_context` (budget = `forge_judge_budget_ms`, fail-open).
   - If rerank succeeds, reorder the candidate array by score (descending) before serialization.
   - If rerank fails or judge is NULL, keep existing path order.
2. Extract the rerank options wiring from `retrieve_context` (lines 1841-1856) into a shared helper `forge_retrieval_rerank_options(c, options)` so both call sites stay in sync.
3. The serialized output format stays the same (JSON array of path/snippet/score objects) — rerank only changes ordering, not schema.

**Verification:**
- Unit test: mock transport returning fixed scores for 3 candidates; verify output order matches score order, not path order.
- Unit test: mock transport returning error; verify output order equals input order (fail-open).
- Integration: run `forge retrieve` with and without `--judge`, confirm same paths returned, different order when judge is armed.

---

## Phase 3 — Engagement screen (agent-level)

**Goal.** Measure whether rerank engages at the `search_text` surface and whether it regresses.

**Design:**
1. Population: 20 tasks from the existing benchmark pool (mix of engaged + control from the frozen repair-feedback manifest — tasks that historically triggered multiple `search_text` or `read_file` calls).
2. Arms: control (no judge) vs treatment (judge armed, rerank on search_text). Same binary/model/judge.toml as rerank v2 screen.
3. Per-task metric: does at least one `search_text` call return rerank-ordered results (engagement), and does the task's final verdict change (regression/improvement).
4. Preregistered bar (freeze before first cell):
   - Engagement ≥ 60% of treatment cells hit rerank at least once.
   - Zero verdict regressions (treatment pass count ≥ control pass count on matched cells).
   - Rerank engagement latency p90 < 1500 ms.
5. Schedule: 40 cells (20 tasks × 2 arms), interleaved, seed 20260920.
6. Run under `docs/RUN_EFFICIENCY.md` discipline (machine idle, no Unreal Editor).

**Scripts:** Adapt `benchmark/results/2026-09-17-judge-rerank/e3_run.py` (which was never executed — E3 was retained). Rename to `rerank_surface_screen.py`.

---

## Phase 4 — Decision gate

| Outcome | Action |
|---|---|
| Engagement ≥ 60%, zero regressions, rerank fires | **WIN.** Keep the wiring. Extend to read_file if search_text engagement is high. |
| Engagement ≥ 60% but verdict regresses on ≥1 task | Triage: is the regression from reordering dropping a critical snippet? If yes, add mandatory/latest pinning before trim (N5 fix). |
| Engagement < 60% | **Surface not reached.** Fall back to S3 (retrieval-forcing population) — select tasks whose prompts naturally require search, re-screen. |
| Rerank never fires (judge errors or all fail-open) | Debug transport. If systematic, kill S1, go to S3. |

---

## Files touched

| File | Change |
|---|---|
| `include/forge/judge.h:29` | Raise `FORGE_JUDGE_DEFAULT_MAX_CANDIDATES` 32→64 |
| `src/tools/tools.c:1838-1839` | Replace direct `fg_repo_search` with rerank-aware wrapper |
| `src/tools/tools.c:1840-1857` | Extract `forge_retrieval_rerank_options` helper, share between `search_text` and `retrieve_context` |
| `tests/unit/test_judge.c` | Add over-cap and clamp tests |
| `benchmark/rerank_surface_screen.py` | New agent-level engagement screen |
| `docs/plans/jev-rerank-surface-plan.md` | This file |

## Estimated effort

- Phase 1 (N3 fix): 1 day (small, mechanical, test-covered).
- Phase 2 (search_text wiring): 1 day (mirrors existing `retrieve_context` pattern).
- Phase 3 (screen run): 0.5 day setup + GPU batch (machine-idle overnight).
- Phase 4 (decision): analysis only.

Total: ~3 days elapsed, one GPU batch.

## Out of scope (closed axes)

- **Best-of-N candidate selection** (closed axis — sequential shared-budget).
- **Routing minimal-loop reads through `retrieve_context`** (deferred — schema change too invasive for the benchmark loop; revisit if S1 fails).
- **Re-admission floor / prefix stability tradeoffs** (refuted).
- **Repair-feedback threshold retune** (separate campaign; parked by its own refutation).

## Risks

1. **Literal-scan semantics degrade rerank.** `search_text` returns path-ordered literal matches; rerank may reorder by semantic relevance but the candidate pool is already path-biased. If the pool lacks the true answer, rerank can't invent it. Mitigation: measure hit@1 delta, not just MRR.
2. **Over-cap fail-open contamination.** Even with the cap raised to 64, `search_text` returns max 50 hits so we're safe. But if a future caller raises the hit limit above the cap, the silent fail-open returns. Mitigation: record `rerank_scored` vs `candidates` in stats; assert in screen.
3. **Stochasticity at N=1.** Jev rerank is stochastic per sample; a single sample per call may flip the top hit between runs. Mitigation: in the screen, measure hit@1 over 20 tasks, not per-call stability. If instability is high, sample N=2-3 per call and aggregate (cost: $0.0004/call, still negligible).
