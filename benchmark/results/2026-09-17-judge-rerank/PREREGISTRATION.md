# Preregistration — does a hosted judge rerank improve Forge's retrieval evidence?

Written 2026-09-17, **before the first scheduled cell**. Frozen bar; no re-running
for a better repetition; every cell retained, including failures. This is an
examined development screen on one frozen query set over this checkout — not a
holdout, not an agent-loop or completion result, and not a promotion decision
for any default.

## Question

Does TypeSafe's System One model (Jev, pinned `jev-1.13.0`) improve the ranking
of `forge retrieve` evidence for real queries against this repository — i.e.,
does an optional hosted rerank move the target code to earlier positions, and
keep it inside the output budget, versus the deterministic ordering?

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
  recorded under `.forge/judge-raw/` (excluded from the workspace index) and
  archived with the results. The API key is read from `TYPESAFE_API_KEY` and
  never written to configuration or artifacts.
- Binary `build-gpu/Release/forge.exe` sha256
  `3378441430cdd40d8f835a11a0764f6f3b7f37cc9a20d790e21f363374960e77`;
  `judge.toml` sha256
  `2aaf01b91ca63460f530950c0711b8e16141b4e83a04482ca661dda36451cd54`;
  revision `9df0fe2b73f0ef17d9a422daadea26b9fd1d5049` plus the uncommitted
  integration change set (CMakeLists.txt, include/forge/config.h,
  include/forge/forge.h, include/forge/retrieval.h, src/cli/main.c,
  src/core/config.c, src/internal.h, src/repo/retrieval.c, src/tools/tools.c,
  new src/judge/judge.c and include/forge/judge.h, new tests/unit/test_judge.c).

## Population (frozen)

- 40 queries over this workspace, `query_set.json` sha256
  `87f9d7a3ca93d93a66a946c04f28c0e86b2a6afcf19dbc0c4f0e5643082545a6`:
  30 identifier queries (unique column-0 C function definitions whose names
  occur in 4–30 tracked files, selected by a deterministic sorted-name rule;
  the generator is archived with the results) and 10 concept queries
  (hand-picked targets, each located at freeze time by a unique substring).
- Coverage rule (frozen): a result row covers the target when its `path` equals
  the target path and its `snippet` contains the first 60 normalized characters
  of the target line. Rank is the 1-based position of the first covering row;
  uncovered is recorded as no rank (0 contribution to MRR).
- Every query runs against the same workspace with default retrieval options.
  The workspace is not modified during the screen; all run artifacts live
  outside the indexed tree under `.forge/judge-raw/`.

## Arms and schedule

| Arm | Command | Repetitions |
| --- | --- | --- |
| `off` | `forge retrieve QUERY --workspace . --config judge.toml --json` | 1 |
| `on` | the same command plus `--judge` | 2 |

Schedule: per query, `off` then `on` twice, in query-id order — 120 cells
total. The machine is otherwise idle. The off arm is expected deterministic; a
post-run determinism probe (eight queries re-run) is reported as evidence, and
any rank difference is recorded as an off-arm operational defect.

## Bar (frozen before the first cell)

Material benefit requires ALL of:

1. mean MRR (1/rank, zero when uncovered; the judge arm's value is the mean of
   its two repetition MRRs) ≥ judge-off MRR + **0.10 absolute**;
2. target survival (fraction of queries whose target is covered by a returned
   row) not lower under judge-on than off;
3. hit@1 not lower under judge-on than off.

Below that: **not material** — recorded as a negative result, not reframed.

## Stopping rule

No re-runs for a better repetition. Failed, interrupted, or judge-failed cells
are retained and count. Judge failures (fail-open) are reported as engagement,
never re-run. An operational defect stops the batch without discarding its
cells; a corrected implementation would be recorded as a separate experiment.

## Pre-registered expectation (stated so it can be falsified)

Gains are expected on queries whose judge-off rank is greater than 3 (concept
queries, and identifiers with many call sites); queries already at rank 1 are
expected to stay at rank 1. The plausible failure mode is the judge promoting a
file that merely mentions the query over the defining code; the plausible null
is that path/BM25 order was already good enough. Either outcome is recorded as
found.

## Reporting

Per-query ranks for both arms and both judge repetitions; MRR, hit@1, hit@3 and
survival per arm; judge engagement and telemetry (model id, latency, tokens,
per-call records); repetition flips; run wall time. Cost note: ≈120 judged
calls at $0.042/Mtok input (output free) — a few cents. Publication note:
TypeSafe's master agreement §2.3(f) restricts publishing benchmark/performance
information; this record is internal unless a carve-out applies.
