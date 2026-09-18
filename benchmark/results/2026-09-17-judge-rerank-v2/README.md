# Judge rerank screen v2 (post-fix) — results (2026-09-17)

**Verdict: MATERIAL — the preregistered bar was met on all three clauses.**
Corrected-implementation experiment required by the v1 stopping rule; the bar,
query set and stopping rule are identical to v1 — nothing relaxed.

## Primary results

| Arm | MRR | hit@1 | hit@3 | survival |
| --- | --- | --- | --- | --- |
| off (1 rep) | 0.288 | 5.0% | 55.0% | 72.5% (29/40) |
| on r1 | 0.725 | 72.5% | 72.5% | 72.5% |
| on r2 | 0.706 | 70.0% | 70.0% | 72.5% |
| on mean | **0.716** | 71.2% | 71.2% | **72.5%** |

Frozen bar: (1) MRR gain ≥ +0.10 — **met (+0.427)**; (2) survival not lower —
**met** (72.5% both arms); (3) hit@1 not lower — **met**.

## v1 → v2 (informational; the bar is v2's own)

- v1: NOT MATERIAL — MRR +0.424 and hit@1 5.0% → 71.2%, but one judged cell
  died on the snapshot's 5 s scope (hosted latency inside the retrieval
  deadline) and the frozen rule retains failed cells.
- v2 (fix commit `5520c33a`): 120/120 cells exit 0, zero missing; MRR +0.427;
  27/40 queries improved; **zero rank-1 regressions**; one repetition flip
  (`id01`); engagement 79/80 = 98.75%; survival equal in both arms (`id10`,
  v1's survival carrier, kept its covering row in both repetitions).
- The fix proved itself in the live run: `id01-on-r2` hit a transient network
  stall (`WinHttpSendRequest failed (12002)`, ~8 s) — the exact v1 failure
  class — and the call failed open inside the extended budget: exit 0,
  deterministic order preserved, retained as engagement per the frozen rule.
- Residual tuning note: the observed stall took ~8 s (two attempts ×
  connect+send phases), above the 4.5 s `forge_judge_budget_ms()` estimate; the
  9.5 s phase window absorbed it here. A future revision may widen the budget
  to the observed worst case.

## Mechanism

- Judge engagement: 79 of 80 judged cells applied (one network fail-open,
  retained). Model id `jev-1.13.0` on every successful call; latency median
  531 ms, max 984 ms.
- Usage: 291,845 input tokens ≈ **$0.0123** (output free); smoke call 3,906.
- Gains 27/40 (`id01`–`id07`, `id11`–`id30`); regressions 0. The ten concept
  queries remain unmeasurable under the frozen line-level coverage rule (the
  v1 post-hoc note applies: 8/10 targets never enter the candidate output in
  either arm; of the two that do, the judge lifts both).

## Determinism

- Post-run probe with the workspace still in its run state, **before** any
  post-run artifact entered the tree: **8/8 equal** — the v1 confound
  (summarizer outputs entering the indexed workspace) was not repeated.
  `determinism/probe-clean.json`.

## Workspace discipline (lesson from v1, applied)

- 204 query-bearing v1 artifact files were held out under
  `.forge/judge-raw-v2/v1-hold/` for the duration and restored after the probe
  (zero remaining matches verified before launch); all run artifacts stayed
  under `.forge/judge-raw-v2/` until the probe completed.

## Operational note (console reconciliation)

- TypeSafe's console usage export for 2026-09-17 (key "dev") shows 99 requests
  / 245,851 in / 34,202 out; our records for that UTC day are 80 screen calls
  (295,880 in / 11,968 out) plus small probes — same order, not penny-exact
  (the console's counters evidently aggregate differently than the per-response
  `usage` field). Tonight's judged calls (Sep 18 UTC; 291,845 in + probes)
  should appear as the next console day row (~85 requests / ≈305k in).
- The records in this campaign predate request-id capture; commit `11e8c822`
  adds the server's `x-typesafe-request-id` and `date` response headers to every
  raw record going forward, making each call individually reconcilable.

## Provenance

- Binary `build-gpu/Release/forge.exe` sha256
  `4cc9df308e6eaa4bcae516cd66f4408c4c9c9fb2cdee3248ea9abea8108de84c`;
  `judge.toml` sha256
  `3065cf948f2014b6b0720b5e368578272b0e5600789a63e14bd017a48eb62d4e`;
  scripts `d308ee4af12c…`, `037d88cd1e40…`, `20a509b616e5…` (see
  [PREREGISTRATION.md](PREREGISTRATION.md) for full hashes); revision
  `9df0fe2b…` plus commits `4c9a7798`, `f8c9c013`, `5520c33a`, `2b7bf9ac`.
- Schedule: 40 queries × (off, on ×2) = 120 cells; `manifest.json`;
  `progress.log`.

## Verification and retained evidence

- Extractor: 0 rank mismatches across 120 cells (runner metadata vs recomputed
  coverage); survival recounted from records (29/29/29); the fail-open cell
  verified to carry the deterministic order (rank 4, equal to its off cell).
- Retained: `cells/` (120), `raw/` (81 = 80 judged calls + 1 smoke),
  `SUMMARY.md`, `results.json`, `determinism/`, `manifest.json`,
  `progress.log`, `query_set.json`, this preregistration, the frozen scripts
  and `judge.toml`.

## Disposition

Material at the retrieval level. E3 (the agent-level screen,
`PREREGISTRATION-E3.md` in the v1 directory) was conditional on a material
retrieval result — the condition is now satisfied; E3 remains
retained-unexecuted and eligible to run.
