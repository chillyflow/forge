# Judge rerank screen — results (2026-09-17)

**Verdict: NOT MATERIAL — the preregistered bar was not met.** Recorded as a
negative result; not reframed. The ranking gain was large and the mechanism
engaged on every judged cell; the bar failed on the target-survival guardrail,
violated by a single judge-arm cell (1 of 40 queries, 1 of 2 repetitions) that
died on a deadline interaction between the judge's in-snapshot latency and the
retrieval call's 5-second snapshot scope. That cell is retained and counts, per
the frozen stopping rule.

Preregistration: [PREREGISTRATION.md](PREREGISTRATION.md) (bar, stopping rule
and expectation frozen before the first cell). Machine-generated metrics:
[SUMMARY.md](SUMMARY.md), [results.json](results.json).

## Primary results

| Arm | MRR | hit@1 | hit@3 | survival |
| --- | --- | --- | --- | --- |
| off (1 rep) | 0.288 | 5.0% | 55.0% | 72.5% (29/40) |
| on r1 | 0.725 | 72.5% | 72.5% | 72.5% (29/40) |
| on r2 | 0.700 | 70.0% | 70.0% | 70.0% (28/40) |
| on mean | **0.712** | 71.2% | 71.2% | **71.25%** |

Frozen bar: (1) MRR gain ≥ +0.10 — **met** (+0.424); (2) survival not lower —
**not met** (71.25% vs 72.5%, one cell); (3) hit@1 not lower — met. All three
must hold; the bar is not met.

## Work and mechanism evidence

- Judge engagement: all 80 judged cells issued a judge call (80 records
  archived); 79 applied to a returned output. Returned model id: `jev-1.13.0`
  (pinned) on every call.
- Gains: 27 of 40 queries improved; **zero regressions** among the two
  queries the control already ranked first (`id09`, `id10`; `id10`'s second
  repetition is the failed cell above); one repetition flip (`id10`).
- Judge latency: median 468 ms, p90 578 ms, max 1109 ms per call.
- Cost: 293,136 input tokens ≈ **$0.0123** for the whole screen (output free).
- Cell wall time: median 32.4 s (index refresh dominates; range 28.3–35.6 s).
- Ranking example: `fg_compress_output` moved from rank 8 to rank 1;
  `fg_edit_prepare` concept query from rank 11 to rank 2–3.

## The failed cell (attribution)

`id10-on-r2` exited 1 with `FORGE_ERR_LIMIT "Repository indexing deadline
exceeded"` and produced no output. The message is from `index_stopped`
(`src/repo/repo.c:38-47`), reached through `fg_repo_snapshot_stopped` during
snapshot progress callbacks. The retrieval snapshot enters its scope with the
retrieval deadline `min(options.deadline_ms, now + options.timeout_ms)` — the
default `timeout_ms` is 5,000 ms. The judge call runs inside that snapshot
window (before `render()`), so its latency counts against the 5-second budget;
this cell's phase exceeded it. The judge call itself succeeded (its raw record
exists); the call was rejected for lateness.

**Designed fix (not shipped here, to keep the measured artifact intact):**
budget the rerank outside the snapshot scope — e.g. extend the effective
deadline by the judge's per-call budget when a rerank callback is configured,
or move the rerank after snapshot end. Per the campaign rules a corrected
implementation is a separate experiment.

## Determinism probes

- Probe 1 (invalidated by a confound, recorded as such): run after the
  summarizer's `results.json`/`SUMMARY.md` entered the indexed workspace; the
  new candidate rows were exactly those two files, and all 8 probes differed.
- Probe 2 (workspace restored to its screen-time content state): **8/8 equal**
  — the judge-off arm is deterministic; the screen's control ordering is not a
  noise source. Artifacts: `determinism/probe1-confounded.json`,
  `determinism/probe2-clean.json`.

## Post-hoc supplement (not part of the bar)

The frozen coverage rule (first 60 normalized characters of the target line
inside a returned snippet) cannot see FTS token windows, so the ten concept
queries contributed zero in both arms. A post-hoc file-level view: 8 of 10
concept targets were never in the candidate output at all (retrieval recall,
either arm — unmeasurable at any level); of the two that were, the judge lifted
both (`cq01` 5→1, `cq09` 11→2/3).

## Frozen method and provenance

- Preregistered 2026-09-17 before the first cell; 40 queries (30 identifier,
  10 concept) frozen in `query_set.json` sha256
  `87f9d7a3ca93d93a66a946c04f28c0e86b2a6afcf19dbc0c4f0e5643082545a6`;
  generator `build_query_set.py` archived.
- Binary `build-gpu/Release/forge.exe` sha256
  `3378441430cdd40d8f835a11a0764f6f3b7f37cc9a20d790e21f363374960e77`;
  `judge.toml` sha256
  `2aaf01b91ca63460f530950c0711b8e16141b4e83a04482ca661dda36451cd54`;
  revision `9df0fe2b…` plus the uncommitted judge integration change set.
- Schedule: per query `off` then `on` twice — 120 cells; run manifest in
  `manifest.json`; progress log in `progress.log`.

## Verification and retained evidence

- Extractor: ranks recomputed from every `output.json` independently of the
  runner metadata — **0 mismatches across 120 cells**; survival recounted by
  hand (29/29/28) matches the summarizer; cell-level inspection performed for
  `id01`, `id10` and `cq01`.
- Retained: `cells/` (120), `raw/` (80 judge request/response records),
  `query_set.json`, `build_query_set.py`, `determinism/`, `manifest.json`,
  `progress.log`, `SUMMARY.md`, `results.json`, and the two preregistrations.
- E3 (agent screen) was conditional on E2 materiality and is **retained
  unexecuted** ([PREREGISTRATION-E3.md](PREREGISTRATION-E3.md)).

## Disposition

Negative per the frozen bar. The ranking mechanism itself is large, cheap
(≈$0.00015 per judged call) and fast (≈0.5 s), with one known robustness defect
(snapshot-scope deadline) that the next experiment should fix and then
re-measure — on this query set and, for the agent-level question, with the E3
screen once a material retrieval result exists.
