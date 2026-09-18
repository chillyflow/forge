# Preregistration (v2) — hosted judge rerank screen after the snapshot-deadline fix

Written 2026-09-17, **before the first scheduled cell** of this screen — the
corrected-implementation experiment required by the v1 stopping rule ("a
corrected implementation would be recorded as a separate experiment"). It does
not amend, supersede or reinterpret v1, which stands as recorded (NOT MATERIAL,
one retained cell). The bar and stopping rule below are **identical to v1**;
nothing is relaxed and no denominator changes. Frozen bar; no re-running for a
better repetition; every cell retained, including failures. Development screen
on one frozen query set over this checkout — not a holdout, not an agent-loop
result, and not a promotion decision for any default.

## Question

Does TypeSafe's System One model (Jev, pinned `jev-1.13.0`) improve the ranking
of `forge retrieve` evidence for the frozen queries against this repository —
now with the snapshot-deadline defect fixed — i.e., does the optional hosted
rerank move the target code to earlier positions and keep it inside the output
budget, versus the deterministic ordering?

## Relationship to v1

- v1 measured MRR **+0.424** (0.288 → 0.712), hit@1 5.0% → 71.2%, 27/40
  queries improved, zero rank-1 regressions — and returned **NOT MATERIAL**
  solely because one judged cell (`id10-on-r2`, 1 of 120) exceeded the
  retrieval snapshot's 5-second scope deadline and failed with
  `FORGE_ERR_LIMIT`; the frozen stopping rule retains failed cells.
- Defect mechanism: the judge call runs inside the retrieval snapshot scope,
  whose relative deadline is `min(deadline_ms, now + timeout_ms)` — 5,000 ms by
  default — so hosted latency competed with the retrieval's own work.
- Fix, commit `5520c33a`: a new `options.rerank_budget_ms` extends the
  snapshot's relative timeout when a rerank callback is configured (an absolute
  deadline still caps it); callers pass `forge_judge_budget_ms()` — two attempts
  at the configured timeout plus the retry backoff, 4,500 ms at the frozen
  2,000 ms timeout. Without a rerank callback the deadline math is unchanged.
  Scoring, reorder direction and output shape are unchanged from v1; judge-off
  output remains byte-identical to the pre-judge build.
- Change-set effect on the population: the fix's files include 4 of the 40
  target files (id12, cq03, cq06 → `src/tools/tools.c`; cq01 →
  `src/repo/retrieval.c`). Every frozen target substring was verified present in
  the current files at freeze time. Baseline ranks may differ slightly from v1;
  this screen is read standalone against its own bar.

## Integration under test (identity)

- `--judge` CLI grant plus the `[judge]` configuration table. The judge scores
  every pre-trim candidate with one batched Noul request (one question per
  candidate); retrieval reorders by descending score — stable, reorder-only:
  candidates are never added or dropped. Any judge failure keeps the
  deterministic order and is reported in the output's `rerank` object.
- Frozen judge question, verbatim from `src/judge/judge.c`:
  - instruction: "Does the excerpt in `candidates[N].snippet` contain the
    definition, implementation, or explanation that best answers the query in
    `state.query`?"
  - criteria true: "The excerpt contains the specific code, definition, or
    explanation the query asks for."
  - criteria false: "The excerpt is unrelated to the query, only shares
    incidental words, or is clearly less useful than a direct answer."
- Endpoint `https://api.typesafe.ai`, model pinned `jev-1.13.0`; the response's
  returned model id is recorded per call. Raw request/response pairs are
  recorded under `.forge/judge-raw-v2/judge` (excluded from the workspace
  index) and archived with the results. The API key is read from
  `TYPESAFE_API_KEY` and never written to configuration or artifacts.
- Binary `build-gpu/Release/forge.exe` sha256
  `4cc9df308e6eaa4bcae516cd66f4408c4c9c9fb2cdee3248ea9abea8108de84c`;
  `judge.toml` sha256
  `3065cf948f2014b6b0720b5e368578272b0e5600789a63e14bd017a48eb62d4e`;
  `run_screen.py` sha256
  `d308ee4af12cf0fe5f6b95e5282519d78c168dd6b2d923ca6592f8344500a853`;
  `summarize_screen.py` sha256
  `037d88cd1e40cf4857cb5f067647bc59846371e52095d66523bf061402675a99`;
  `probe_determinism.py` sha256
  `20a509b616e5807f312fba88f17934a7c5c575f8fc0294e3d9e0503136701172`.
  Revision `9df0fe2b73f0ef17d9a422daadea26b9fd1d5049` plus commits `4c9a7798`
  (judge integration), `f8c9c013` (v1 campaign record), `5520c33a` (the fix).

## Population (frozen, unchanged from v1)

- 40 queries over this workspace, `query_set.json` sha256
  `87f9d7a3ca93d93a66a946c04f28c0e86b2a6afcf19dbc0c4f0e5643082545a6` — the
  identical frozen set; the generator is archived with the v1 results.
- Coverage rule (frozen): a result row covers the target when its `path` equals
  the target path and its `snippet` contains the first 60 normalized characters
  of the target line. Rank is the 1-based position of the first covering row;
  uncovered is recorded as no rank (0 contribution to MRR).
- Every query runs against the same workspace with default retrieval options.
- **Workspace discipline (lesson from v1):** v1's in-tree query-bearing
  artifacts (`benchmark/results/2026-09-17-judge-rerank/`: README.md, SUMMARY.md,
  results.json, cells/, raw/, query_set.json, build_query_set.py) are held out
  of the workspace under `.forge/judge-raw-v2/v1-hold/` for the duration of this
  screen and restored only after the post-run determinism probe has completed.
  All v2 run artifacts live under `.forge/judge-raw-v2/` (index-excluded) until
  that probe completes, so no query-bearing artifact ever enters the indexed
  tree while cells are running.

## Arms and schedule

| Arm | Command | Repetitions |
| --- | --- | --- |
| `off` | `forge retrieve QUERY --workspace . --config judge.toml --json` | 1 |
| `on` | the same command plus `--judge` | 2 |

Schedule: per query, `off` then `on` twice, in query-id order — 120 cells
total. The machine is otherwise idle. The off arm is expected deterministic; a
post-run determinism probe (eight queries re-run, workspace still in its run
state) is reported as evidence, and any rank difference is recorded as an
off-arm operational defect.

## Bar (frozen before the first cell — identical to v1, unchanged)

Material benefit requires ALL of:

1. mean MRR (1/rank, zero when uncovered; the judge arm's value is the mean of
   its two repetition MRRs) ≥ judge-off MRR + **0.10 absolute**;
2. target survival (fraction of queries whose target is covered by a returned
   row) not lower under judge-on than off;
3. hit@1 not lower under judge-on than off.

Below that: **not material** — recorded as a negative result, not reframed.

## Stopping rule (identical to v1)

No re-runs for a better repetition. Failed, interrupted, or judge-failed cells
are retained and count. Judge failures (fail-open) are reported as engagement,
never re-run. An operational defect stops the batch without discarding its
cells; a corrected implementation would be recorded as a separate experiment.

## Pre-registered expectation (stated so it can be falsified)

Amended in exactly one respect: with the fix, the retrieval phase budget is
9,500 ms (5,000 + 4,500) and no cell is expected to fail on the snapshot
deadline — v1's single failure class. The ranking prediction is unchanged from
v1: gains are expected on queries whose judge-off rank is greater than 3
(concept queries, and identifiers with many call sites); queries already at
rank 1 are expected to stay at rank 1. The plausible failure mode is the judge
promoting a file that merely mentions the query over the defining code; the
plausible null is that path/BM25 order was already good enough. Either outcome
is recorded as found.

## Reporting

Per-query ranks for both arms and both judge repetitions; MRR, hit@1, hit@3 and
survival per arm; judge engagement and telemetry (model id, latency, tokens,
per-call records); repetition flips; run wall time; and an informational v1↔v2
comparison (the bar is v2's own). Cost note: ≈120 judged calls at $0.042/Mtok
input (output free). Publication note: TypeSafe's master agreement §2.3(f)
restricts publishing benchmark/performance information; this record is internal
unless a carve-out applies.
