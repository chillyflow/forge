# Preregistration — E3 agent screen: does the judge rerank engage in the richer loop, and does it regress?

Written 2026-09-17, **before the first E3 cell including its engagement
preflight**. Conditional on the E2 retrieval screen meeting its frozen bar; if
E2 is not material, E3 does not run and this document is retained unexecuted.
One repetition per cell: this is an examined development diagnostic, not a
superiority, preservation or holdout claim — the same language as the
seven-arm loop pilot.

## Amendment 1 — launch identity (2026-09-18, before the first E3 cell)

This preregistration was frozen 2026-09-17 and then retained unexecuted: E2 v1
returned not material on one retained cell, so the E2 fix cycle ran first (fix
commit `5520c33a`; corrected-implementation screen v2 returned material). E3
launches now; its identity is corrected from the frozen record as follows, and
**nothing else in this document changes** — the bar, arms, schedule, fixtures
and profile are exactly as frozen:

- `forge.exe` sha256 at launch: `6a6b882378095b57c4a412256f8642546cb7df3c0e0027047c7d6365800c28dd`
  (frozen line named `3378441430cdd40d8f835a11a0764f6f3b7f37cc9a20d790e21f363374960e77`).
  The delta is the E2 fix set: `5520c33a` (rerank wall-budget — extends the
  retrieval snapshot's relative deadline by the callback budget; changes no
  scores and no ordering) and `11e8c822` (record-only — captures the server's
  `x-typesafe-request-id`/`date` headers in raw records). No loop, prompt,
  tool or scoring behavior changed.
- Arm labels `rich` / `rich-judge` above are the frozen `benchmark/run.py`
  variants `optimized` / `optimized-judge` (re-verified sha256 `6c0cc041…`,
  matches the frozen line).
- The two engagement-preflight runs are the first two judge-arm cells of the
  12-cell schedule (`go_api_pagination`, `generalize_retractions_original`);
  the full schedule skips them (resume-safe), so the screen remains exactly
  12 cells.
- Re-verified at launch, all matching the frozen lines: `judge-e3.toml`
  `e5f03637…`; the six task manifests (`2bb193ba…`, `8d909f5d…`, `27603cd6…`,
  `3e32c2aa…`, `93bedc92…`, `359e98e0…`); model
  `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` `fadc3e5f…88ad`.

## Question

When the richer agent loop (no `--minimal-agent`) has the optional judge
granted, does the agent's `retrieve_context` tool engage the rerank at all in
these fixtures, and does task outcome regress relative to the identical loop
without the judge?

## Arms

| Arm | Flags | Role |
| --- | --- | --- |
| `rich` | `--thought-history` | control |
| `rich-judge` | `--thought-history --judge --config judge-e3.toml` | treatment |

Same binary, same loop-pilot profile (temperature 0.6, 32 actions, 32768
generated / 262144 input tokens, 600 s wall, 120 s verifier), native protocol,
GPU layers -1.

## Frozen inputs

- Six examined manifests from `benchmark/results/2026-09-08-repair-control/tasks`
  with SHA-256s re-verified today against `benchmark/LOOP_COMPLETION.md:59-66`:
  `2bb193ba…`, `8d909f5d…`, `27603cd6…`, `3e32c2aa…`, `93bedc92…`, `359e98e0…`.
- `forge.exe` sha256 `3378441430cdd40d8f835a11a0764f6f3b7f37cc9a20d790e21f363374960e77`;
  `judge-e3.toml` sha256 `e5f03637fb180a452fe16fce36d3273b94736ef809a44cf4fdcccfcd483a7e2e`;
  `benchmark/run.py` sha256 `6c0cc041620c1ba7678f4420ecd7464a868571fa31ed98df97608dd952fd52cc`
  (adds only the `optimized-judge` variant); model
  `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` sha256 `fadc3e5f…88ad`.
- Schedule: per fixture, `rich` then `rich-judge`; 12 cells; machine otherwise
  idle. Raw judge records under `.forge/judge-raw-e3/` (workspace-excluded);
  rerank metadata also lands in each session's tool results.

## Engagement preflight (before the 12 cells)

Two judge-arm runs (`go_api_pagination`, `generalize_retractions_original`).
Engagement = judge calls recorded (raw records plus rerank metadata in session
tool results). If both runs show zero engagement, the screen does not run; the
recorded result is "the mechanism does not engage in this population".

## Bar (frozen)

This population can show engagement and non-regression only:

1. engagement == 0 in both preflight runs → not run (recorded);
2. otherwise: judge-arm passes ≥ control-arm passes, with pass defined as
   agent exit 0 AND independent verification AND unchanged protected files,
   and no judge-arm cell lost to a protocol or verification defect.

Below that: recorded as a negative result, not reframed. A pass here is a
feasibility signal for a fresh holdout with repetitions, nothing more.

## Stopping rule

No re-runs for a better repetition. Failed or interrupted cells are retained
and count. An operational defect stops the batch without discarding its cells;
a corrected implementation becomes a separate experiment.

## Reporting

Per-cell pass/fail, actions and tokens, end-to-end time, judge engagement per
run (calls, applied, latency, tokens, model id), and the pass comparison.
Egress and the TypeSafe master-agreement publication note are as recorded in
the E2 preregistration.
