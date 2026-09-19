# Jev shadow replay — retained repair-feedback episodes (2026-09-18 population)

**Verdict: mixed signal — GO for the larger arm-level A/B with preregistered
caveats; NO-GO for treating a single Jev sample as a stable per-call label.**

Replaying all 15 retained feedback requests verbatim, 4× each, against the live
`jev-1.13.0` endpoint (60/60 HTTP 200, 0 errors, p50 371 ms / p90 431 ms /
max 484 ms) shows the judge's answers are **not deterministic** on this
population: 0 of 15 episodes reproduced identically across 4 samples, 5 of 15
flipped a choice (`failure_family` and/or `next_action`), and the shipped
conservative consumption rule is **saturated** — it classifies every sample as
low-confidence and renders the same "inspect" guidance for all 75/75
shipped + replayed samples. The instability therefore does not propagate into
the treatment text as shipped, but it does make per-call judge outputs
unusable as labels or analysis strata.

## Method

- Script: `benchmark/judge_replay.py` (`replay` and `analyze` subcommands),
  sha256 `5c982df5e5ff1b883d4da8fca2502827e585da21c9b040d71255c1afcdefbc4e`.
- Inputs (read-only): the 15 retained raw feedback records —
  `2026-09-18-typesafe-repair-feedback/raw/*.json` (6) and
  `.../engagement-screen/raw/*.json` (9). Each record holds the exact request
  the shipped client sent (`model`, `questions`, `state`) plus its response.
- Per record, the request object was re-sent **verbatim** (same fields, same
  values, same key order; re-serialized compactly) N=4 times to
  `POST https://api.typesafe.ai/v1/systemone`, model `jev-1.13.0`, bearer key
  read from `TYPESAFE_API_KEY` at call time and never printed, logged, or
  written to disk.
- Hard timeout 30 s per call; at most one retry on transport error (no retry
  was triggered). Recorded per sample: HTTP status, latency_ms, full response
  JSON, usage, server request id and date — one file per attempt under
  `raw/replay-<record>-<n>.json` (60 files, 147,041 bytes total).
- Analysis: `python benchmark/judge_replay.py analyze` → `analysis.json`
  (regenerable from the retained replay files; it embeds a `generated_utc`
  stamp, so its own hash changes on every re-run).
- Record → run mapping: each record's shipped answer tuple
  (`failure_family`, `failure_confidence`, `evidence_gap`, `repair_readiness`,
  `repair_readiness_confidence`, `next_action`, `next_action_confidence`) was
  matched against the `judge_feedback` events in every run's
  `session/events.jsonl`. All 15 tuples are unique and match exactly one event
  (15 records ↔ 15 events, bijection, no unmapped entries). Timestamps
  corroborate: implied run start (`record utc − event elapsed_ms`) is
  consistent within ≤0.5 s per run, and every record precedes its run's
  `result.json` mtime.

### Measured path

| metric | value |
|---|---|
| calls / errors | 60 / 0 (all HTTP 200) |
| model returned | `jev-1.13.0` (60/60 — pinned version confirmed) |
| latency p50 / p90 / max | 370.9 ms / 430.8 ms / 484.0 ms (min 317.0, mean 378.9) |
| shipped-path latency (15 calls) | 125–547 ms, median 406 ms |
| tokens (replay) | 108,436 input / 9,170 output ≈ $0.0046 at $0.042/M input |
| status counts | `{"200": 60}` |

## 1. Self-consistency per episode

Per-episode table: outcome = the run's `result.json` verdict (`PASS` or
cap-death at the 32-turn limit, `32t`); `ff` / `na` columns are the *sets* of
choices observed across the 4 replay samples; ranges are min–max across
samples; `shipped` is the single answer the live run received
(`ff/na/repair_readiness/evidence_gap`).

| record | task | outcome | class | ff choices | na choices | rr range | nac range | eg range | fc range | shipped |
|---|---|---|---|---|---|---|---|---|---|---|
| judge-20260918T155317Z-0000 | reasoning_event_replay | cap (32t) | mixed | code_defect | run_validation | 2.12–2.29 | 0.14–0.24 | 0.53–0.54 | 0.61–0.66 | code_defect/run_validation/2.3/0.53 |
| judge-20260918T155347Z-0001 | reasoning_event_replay | cap (32t) | mixed | code_defect | defer | 2.06–2.16 | 0.18–0.19 | 0.58–0.60 | 0.64–0.68 | code_defect/defer/2.13/0.58 |
| judge-20260918T160703Z-0000 | reasoning_event_replay | cap (32t) | mixed | code_defect | inspect_source | 1.12–1.15 | 0.36–0.42 | 0.82–0.84 | 0.39–0.46 | code_defect/inspect_source/1.11/0.84 |
| judge-20260918T160821Z-0001 | reasoning_event_replay | cap (32t) | stochastic | environment_or_dependency | defer,run_validation | 2.09–2.26 | 0.10–0.17 | 0.29–0.32 | 0.42–0.45 | environment_or_dependency/defer/2.28/0.30 |
| judge-20260918T160832Z-0002 | reasoning_event_replay | cap (32t) | stochastic | code_defect | defer,inspect_source | 2.38–2.53 | 0.09–0.13 | 0.44–0.48 | 0.65–0.71 | code_defect/inspect_source/2.43/0.46 |
| judge-20260918T161549Z-0000 | reasoning_event_replay | PASS (27t) | mixed | code_defect | inspect_source | 2.45–2.51 | 0.11–0.20 | 0.48–0.50 | 0.69–0.73 | code_defect/**defer**/2.53/0.49 |
| judge-20260918T170850Z-0000 | go_multifile_registry | PASS (12t) | stochastic | environment_or_dependency,unknown | defer,patch_code,run_validation | 2.29–2.34 | 0.07–0.12 | 0.18–0.20 | 0.21–0.27 | unknown/defer/2.36/0.19 |
| judge-20260918T171044Z-0000 | reasoning_dependency_order | cap (32t) | stochastic | environment_or_dependency,unknown | defer | 2.01–2.28 | 0.17–0.24 | 0.18–0.21 | 0.25–0.34 | environment_or_dependency/defer/2.22/0.18 |
| judge-20260918T171100Z-0001 | reasoning_dependency_order | cap (32t) | mixed | code_defect | defer | 1.63–1.77 | 0.25–0.35 | 0.57–0.62 | 0.45–0.55 | code_defect/defer/1.60/0.61 |
| judge-20260918T171209Z-0002 | reasoning_dependency_order | cap (32t) | mixed | environment_or_dependency | defer | 2.08–2.20 | 0.12–0.21 | 0.23–0.26 | 0.18–0.28 | environment_or_dependency/defer/2.16/0.23 |
| judge-20260918T171241Z-0000 | reasoning_route_specificity | PASS (18t) | stochastic | environment_or_dependency,unknown | defer | 2.01–2.18 | 0.28–0.34 | 0.27–0.30 | 0.24–0.35 | environment_or_dependency/defer/1.99/0.30 |
| judge-20260918T171315Z-0000 | ceil_div | PASS (10t) | mixed | environment_or_dependency | defer | 1.70–1.83 | 0.14–0.21 | 0.24–0.25 | 0.31–0.46 | environment_or_dependency/defer/1.75/0.24 |
| judge-20260918T171551Z-0000 | reasoning_interval_union | cap (32t) | mixed | environment_or_dependency | defer | 2.24–2.37 | 0.15–0.19 | 0.16–0.17 | 0.30–0.36 | environment_or_dependency/defer/2.29/0.15 |
| judge-20260918T171557Z-0001 | reasoning_interval_union | cap (32t) | mixed | code_defect | defer | 2.11–2.17 | 0.28–0.36 | 0.41–0.44 | 0.56–0.60 | code_defect/defer/2.11/0.44 |
| judge-20260918T171837Z-0000 | go_api_pagination | PASS (9t) | mixed | environment_or_dependency | run_validation | 2.02–2.09 | 0.10–0.15 | 0.17–0.20 | 0.43–0.48 | environment_or_dependency/run_validation/2.02/0.20 |

Classification: **deterministic** = all 4 answers deep-equal; **mixed** =
choices identical but numeric values differ; **stochastic** = a choice flips.

- **0 deterministic / 10 mixed / 5 stochastic.** No episode ever produced
  byte-identical answers twice.
- Choice agreement: `failure_family` identical in 10/15 episodes,
  `next_action` identical in 10/15; 5 episodes flip at least one of them.
  The widest flip: `go_multifile_registry` produced three different
  `next_action` values across 4 samples (`defer`, `patch_code`,
  `run_validation`) and two `failure_family` values.
- Sample-vs-shipped agreement: `failure_family` 57/60 (95%), `next_action`
  52/60 (87%). One episode (`judge-20260918T161549Z-0000`, the one A/B run
  that later PASSed) drew `defer` live and `inspect_source` 4/4 on replay —
  the shipped answer was not reproduced once.
- Probability/score spread: within an episode the modal option's probability
  moves by up to 0.12 (`reasoning_route_specificity` `failure_family`
  0.37–0.49); `repair_readiness` spans up to 0.27 (2.01–2.28). Per-field
  within-episode spreads across the 15 episodes: `failure_confidence`
  0.03–0.15, `next_action_confidence` 0.01–0.10,
  `repair_readiness_confidence` 0.00–0.27, `evidence_gap` 0.01–0.05.

**Verdict: the service looks mixed-to-stochastic on this population, not
deterministic.** The choice layer is more stable than the numeric layer (10/15
episodes keep both choices stable), but one in three episodes is not
choice-stable at N=4.

## 2. Confidence distributions and the shipped consumption rule

Distribution over all 60 replayed samples (replayed = live endpoint, same
requests):

| field | min | median | mean | max |
|---|---|---|---|---|
| failure_confidence | 0.18 | 0.44 | 0.4595 | 0.73 |
| evidence_gap (noul) | 0.16 | 0.295 | 0.3812 | 0.84 |
| repair_readiness (score) | 1.12 | 2.135 | 2.0803 | 2.53 |
| repair_readiness_confidence | 0.00 | 0.195 | 0.236 | 0.74 |
| next_action_confidence | 0.07 | 0.17 | 0.2002 | 0.42 |

Shipped consumption rule (`src/core/agent.c:1114–1133`): `uncertain =
next_action_confidence < 0.55 or failure_confidence < 0.45 or
repair_readiness_confidence < 0.45`; the rendered guidance takes the "inspect"
branch when `uncertain or evidence_gap >= 0.60 or failure_family ==
missing_evidence or next_action in {inspect_source, defer}`.

Per-episode classification across the 4 replay samples (1 = rule fires):

| record | uncertain (4 samples) | inspect-adds (eg ≥ 0.60) |
|---|---|---|
| judge-20260918T155317Z-0000 | 1111 | 0000 |
| judge-20260918T155347Z-0001 | 1111 | 0001 |
| judge-20260918T160703Z-0000 | 1111 | 1111 |
| judge-20260918T160821Z-0001 | 1111 | 0000 |
| judge-20260918T160832Z-0002 | 1111 | 0000 |
| judge-20260918T161549Z-0000 | 1111 | 0000 |
| judge-20260918T170850Z-0000 | 1111 | 0000 |
| judge-20260918T171044Z-0000 | 1111 | 0000 |
| judge-20260918T171100Z-0001 | 1111 | 0011 |
| judge-20260918T171209Z-0002 | 1111 | 0000 |
| judge-20260918T171241Z-0000 | 1111 | 0000 |
| judge-20260918T171315Z-0000 | 1111 | 0000 |
| judge-20260918T171551Z-0000 | 1111 | 0000 |
| judge-20260918T171557Z-0001 | 1111 | 0000 |
| judge-20260918T171837Z-0000 | 1111 | 0000 |

- **The rule is saturated.** `uncertain` fires in **60/60** replayed samples
  (and 15/15 shipped): the binding condition is `next_action_confidence <
  0.55`, which holds in 60/60 samples (max observed 0.42); `failure_confidence
  < 0.45` fires in 31/60 and `repair_readiness_confidence < 0.45` in 51/60.
- Consequence: the rendered `JUDGE_FEEDBACK_GUIDANCE` takes the "inspect"
  branch in **75/75** shipped + replayed samples — the `patch_code` and
  `run_validation` branches are unreachable on this population. The choice
  instability measured in §1 does **not** change the treatment text as
  shipped; the campaign would be testing "a constant cautious hint appended
  after failed validations", not typed per-episode guidance.
- The secondary `evidence_gap >= 0.60` gate is *not* stable within-episode in
  2/15 episodes (`0.58–0.60` and `0.57–0.62` around the threshold), though it
  is moot while `uncertain` already routes to the same branch.

## 3. Latency and errors on the live path

| path | n | p50 | p90 | max | errors |
|---|---|---|---|---|---|
| shadow replay (this run) | 60 | 370.9 ms | 430.8 ms | 484.0 ms | 0 (0%) |
| shipped records (2026-09-18) | 15 | 406 ms | 515.6 ms | 547 ms | 0 |

All 60 replayed calls returned HTTP 200 in 317–484 ms — inside the vendor's
70–500 ms band, with a tighter spread than the shipped records (which include
125 ms and 547 ms outliers). No transport error occurred, so the one-retry
path was never exercised. Latency was measured on an otherwise-idle machine.

## 4. Calibration versus episode outcome (descriptive only)

Episodes grouped by the outcome their run later reached (mapping in §Method;
5 records / 20 samples from runs that passed, 10 records / 40 samples from
cap-death runs). All values are replayed samples.

| group | records | rr mean (median) | rrc mean | eg mean | fc mean | next_action counts |
|---|---|---|---|---|---|---|
| passed runs | 5 | 2.13 (2.08) | 0.181 | 0.277 | 0.416 | defer 10, run_validation 5, inspect_source 4, patch_code 1 |
| cap-death runs | 10 | 2.05 (2.15) | 0.264 | 0.433 | 0.482 | defer 28, inspect_source 7, run_validation 5 |

- `repair_readiness` shows **no separation** between episodes from passed and
  cap-death runs (mean 2.13 vs 2.05; medians 2.08 vs 2.15) — it is not a
  usable success predictor on this population.
- Directional differences exist but are small and unpowered: passed-run
  episodes scored lower `evidence_gap` (0.28 vs 0.43), lower
  `failure_confidence` (0.42 vs 0.48), lower `repair_readiness_confidence`
  (0.18 vs 0.26), and drew `defer` less often (50% vs 70% of samples).
- **Limitation:** 5 vs 10 records (20 vs 40 non-independent samples); the
  passed group is 4 different tasks and the cap-death group 4 others, so task
  identity and outcome are confounded; N=4 per episode cannot resolve small
  effects. This section is descriptive pattern-spotting, not a calibration
  result.

## 5. Go / no-go

**Measured facts**

1. 60/60 live calls succeeded, 0 errors, model pinned `jev-1.13.0`; p50
   371 ms / p90 431 ms / max 484 ms; ~$0.005 total. The live path is
   operationally sound and cheap.
2. The judge is **not deterministic** on this population: 0/15 episodes
   reproducible at N=4; 5/15 flip a choice; sample-vs-shipped agreement is
   95% (`failure_family`) / 87% (`next_action`), with one shipped answer
   reproduced 0/4.
3. The shipped conservative rule is **saturated**: `uncertain` fires on
   60/60 samples, so all 75 shipped+replayed judgments render the same
   "inspect" guidance; the typed `patch_code`/`run_validation` branches never
   fire.
4. Calibration against outcome shows no separation on `repair_readiness`;
   other fields differ only directionally at n=5 vs 10 records.

**Judgment**

- The larger campaign does not need a deterministic judge: the judge never
  gates, fails open, and the arm-level A/B measures whether the advisory loop
  changes repair outcomes. Nothing measured here blocks that campaign.
- But two properties must be preregistered rather than discovered afterwards:
  (a) judge outputs are a **stochastic advisory input** — they may not be
  used as labels, strata, or denominators in campaign analysis, and every
  judged call should carry its own retained record (the G01 instrumentation
  prerequisite); (b) with the current thresholds the treatment is effectively
  a **constant conservative hint** on this population, so the campaign cannot
  attribute outcomes to typed per-episode guidance.
- If the goal is for typed guidance to be actionable, the thresholds need
  retuning on a larger shadow set first: `next_action_confidence` never
  reaches 0.55 here (max 0.42) and `evidence_gap` flips across 0.60 within
  episodes, so today's rule cannot discriminate at its own boundaries.

**Recommendation (one line):** GO for the larger A/B on the frozen population
— conditional on preregistering judge output as stochastic advisory input
(never a label), retaining per-call records, and either accepting the
constant-hint treatment or retuning the saturated thresholds beforehand.

## Limitations

- 15 episodes drawn from one day's retained runs (4 tasks + 1 preflight);
  single endpoint, single model version, same-day calls. N=4 per episode is
  small; the deterministic/mixed/stochastic classification is sample-size
  dependent, and "deterministic" here can only mean "no variation observed in
  4 draws".
- Episodes are not independent: several share a run and a task.
- The record→run mapping is a reconstruction from retained artifacts (exact
  response-tuple match + timestamp corroboration), not a recorded join key.
- Replay bodies are the parsed request objects re-serialized compactly:
  semantically verbatim (same fields/values/order), but byte-formatting may
  differ from the original client's, so input-token counts can differ
  slightly from the shipped records' usage.
- The calibration comparison mixes task identity with outcome and is
  underpowered; no inferential claims are made from it.

## Files

- `raw/replay-<record>-<n>.json` — 60 per-attempt records (HTTP status,
  latency_ms, full response JSON, usage, request id/date, request sha256).
- `replay-calls.jsonl` — append-only call log.
- `replay-summary.json` — replay-side counts/latency/tokens.
- `analysis.json` — computed analysis (per-episode stats, distributions,
  rule flags, mapping, calibration, errors).
- `README.md` — this report.

Reproduce: `python benchmark/judge_replay.py replay` (resumable; skips
existing sample files) then `python benchmark/judge_replay.py analyze`.
