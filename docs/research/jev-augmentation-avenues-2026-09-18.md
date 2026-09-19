# Jev augmentation avenues: full adjudicated catalogue

Status: audited, September 18, 2026. Repository inspected at revision
`d16e6736918808daa9c1f1993093a6dcd6ea0af8` (branch main; the two untracked
evidence directories are untouched). This document extends
[jev-integration-2026-09-17.md](jev-integration-2026-09-17.md); it supersedes
nothing, starts no model runs, changes no code and changes no default.

Method: a 12-domain read-only performance audit produced 59 raw Jev seam
candidates; a reconciliation unit merged them into 53 deduplicated entries
(draft sections A-D) and 12 cross-cutting entries (G01-G12); two independent
refuters then adjudicated all 65 entries against the repository, the retained
benchmark artifacts, the closed axes and the constraint set (no success
authority, offline fallback, TypeSafe MCA 2.3(f)). Verdicts across the 65:
36 survive, 9 partial, 9 duplicates of another entry, 10 downgraded, 1 refuted.
Priorities: 3 adopt-now, 10 experiment, 48 hold, 4 reject. Five further claims
were filed by the refuters themselves (section 5). Every cited anchor was
re-read at HEAD by the refuter that adjudicated it.

## 1. What Jev supplies (fixed by provider documentation)

Jev returns typed answers only: Choice (one of N, with per-option
probabilities and a confidence), Score (ordered descriptive levels), Noul (a
yes/no probability). One request can batch many questions over one shared JSON
state; question ids are host-side. It never generates text, never executes
work, and never gates: any seam below keeps a deterministic host fallback and
the host keeps all authority. Endpoint `POST https://api.typesafe.ai/v1/systemone`,
bearer key from an environment variable at call time; `jev-latest` resolves to
`jev-1.13.0`; vendor-reported 70-500 ms; $0.042 per million input tokens.
Confidence is derived from the answer distribution, not a proven probability of
correctness. See the 2026-09-17 document section "What Jev actually supplies"
for the full provider citations.

## 2. Shipped and screened

### A1 - retrieval rerank (implemented; MATERIAL at retrieval level)

Per-candidate Noul scoring of retrieval candidates against the query, applied
before the output-budget trim. Shipped at `src/tools/tools.c:1823-1827` (the
agent `retrieve_context` path) and `src/repo/retrieval.c:430-437`; worst-case
budget `src/judge/judge.c:130-132` (2 x timeout + 500 ms backoff), fail-open.
Evidence: the first screen was NOT MATERIAL (survival clause failed on one
cell; one snapshot-deadline cell retained); `5520c33a` budgeted the hosted
latency outside the snapshot deadline; the v2 screen is MATERIAL - MRR +0.427,
120/120 cells exit 0, 79/80 engagement, median 531 ms, max 984 ms, 291,845
input tokens (~$0.0123) - `benchmark/results/2026-09-17-judge-rerank-v2/`.
Remaining gap: the agent-level screen (E3) recorded zero engagement - the
measured populations never call `retrieve_context` (0 of 1179 retained
`events.jsonl` files), and the minimal/repair-loop schema does not expose it
(`src/tools/tools.c:279-303`). The arm is sound; its surface is not reached.

### A2 - repair feedback (implemented; continue bar met)

One batched request after a complete, stable failed candidate validation:
failure-family Choice, evidence-gap Noul, repair-readiness Score, next-action
Choice; rendered as advisory guidance. Shipped at `src/core/agent.c:1074`
(guard: validation must be complete), `1100-1126` (rendering and conservative
thresholds), `1266/1268`. Host validation stays the only authority; the judge
never gates; fail-open. Evidence: treatment 3P/1F versus control 1P/3F at
n=4/arm over 2 tasks; 6 raw records, `jev-1.13.0`, 11,512 input / 918 output
tokens, 125-516 ms (median 422); engagement screen over all 29 fixtures found
6 engaged and 2 inert controls, and froze the population manifest
`f0661378...` - `benchmark/results/2026-09-18-typesafe-repair-feedback/`.
The larger campaign on the frozen population is eligible and not yet run.

### B3 - arm reachability (shipped wiring; unscreened combination)

Rerank is unreachable in the minimal/repair loop and feedback is reachable only
inside the bounded-repair candidate path; the two arms' next campaigns must
choose their surfaces deliberately rather than assume both fire.

## 3. Adjudicated catalogue

Entry format: id, seam, anchors, judgment type, what code cannot decide,
verdict and priority. Holds are grouped with one-line reasons; the draft and
refute files in the run directory carry the full 13-field entries.

### 3.1 adopt-now (3)

- **A1 retrieval rerank** - keep shipped; decide the reachable surface (see N1:
  do not move it to `search_text` before that surface has measured engagement)
  or select a retrieval-forcing population.
- **A2 repair feedback** - keep shipped; run the larger campaign on the frozen
  population manifest. No code change required.
- **G01 advisory accept/abstain calibration and decision record** - the
  thresholds are hardcoded (`src/core/agent.c:1107-1110`), `judge_feedback`
  events carry no threshold values and no turn/validation id
  (`src/core/agent.c:1047-1059`), and `metrics.json` has no judge counters
  (`src/core/session.c:89-147`); `forge_judge_metrics` has no production
  caller. Record-only instrumentation: counters in `metrics.json` plus event
  linkage, no behavior change. This is the prerequisite for calibrating any
  threshold later.

### 3.2 experiment (10, all replay/shadow first, offline-safe)

- **C1 read-routing** (the 2026-09-17 MVP) - build the replay/shadow decision
  adapter first; at most one host-executed eligible read per failure episode,
  candidate-id-or-defer, abstain on no novel candidate; feasibility report
  before active control. Kept with the reachability amendment from B3.
- **D4 context ranking** - which admitted bundle to drop under budget
  pressure (`src/context/context.c:730-745, 776-784, 895-918`). Shadow only;
  any selection change must preserve prefix stability (the recorded 16.5%
  re-prefill is oscillation evidence, not ranking evidence).
- **D15 failure-class identity** - same failure class versus same text
  (`src/core/agent.c:493-505, 2769-2772`). Detection refinement only; keep it
  clear of the closed bounded-repair presentation family.
- **D22 diagnostic-view ranking** - which dropped lines best explain the
  failure (`src/core/verification.c:409-429`, `src/tools/diagnostics.c:131-150`).
  Replay retained failure excerpts; batchable with the A2 request.
- **D26 retrieval graph seed** - infer a seed when the query has no exact
  symbol (`src/repo/retrieval.c:270-278`). Shadow first; amending
  `docs/RETRIEVAL.md:15-17` is a deliberate decision, not a side effect.
- **D39 incomplete-validation feedback** - the incomplete branch never asks
  (`src/core/agent.c:1074, 1268-1286`). Scope to the incomplete class as a
  separate advisory question inside the same request.
- **D40 repair-evidence excerpt choice** - which source excerpt best explains
  a failed candidate (`src/core/agent.c:1007-1038, 1079`). Same request, no
  extra round trip; read policy and bounds stay host-side.
- **D17 + G11 validation-order prioritization** - which test/command first
  (`src/repo/validation.c:687-716`, `src/core/agent.c:133-140`). Measure the
  validation share of wall time first; ordering-only, the broad stage stays
  authoritative and every planned command still runs.
- **G04 repeated-failure escalation triage** - stop-loss, semantic-loop and
  recovery fire exact detectors with fixed policy (`src/core/agent.c:2156-2173,
  1196-1230, 2774-2781`). Shadow-only; any control use is W4-gated and needs an
  explicit decision.
- **G05 failure-taxonomy residual classification** - the `wrong_fix`
  catch-all (`benchmark/analyze_failures.py:28-38, 113`). Offline analysis
  only; labels never become gates or denominators.
- (G06 was reframed to an optional offline label audit; see section 5.)

### 3.3 hold (48; grouped, with the reason class)

- **Calibration or measurement already recorded** - D1 force-timing (the
  `min(256, max/2)` cap is a recorded calibration; success flat 256..unbounded
  and 256 cheapest), D17-adjacent ordering claims pending a validation-share
  measurement.
- **Scheduled for removal or re-measure by P0.1** - D9 output reserve
  (`src/inference/llama_backend.c:430-431`, `src/core/agent.c:2550-2552`),
  D5 compaction choice, D36 auto-plan accept, D33/D34 hardware metadata
  handling (`src/core/hardware.c:542-559, 597-610`).
- **No measured trigger population** - D19 patch conflict (in minimal mode a
  conflict is a plain error with no excerpt), D24 assert-operand direction
  (deliberate refusal, `src/tools/diagnostics.c:576-592`), D41 Go-syntax
  repair, D44 tool-output retention (truncation never fired in the measured
  population; see N4), D29 self-edit detection (host mutation-identity
  tracking is the prerequisite; `docs/WATCH.md:197-199` records the
  conservative default).
- **Deliberate documented refusals or off-loop surfaces** - D7 semantic raw
  fallback, D8 checkpoint capture budget, D13 semantic cluster, D14 reflection
  salvage (re-scope before pursuing), D16 working-state view, D18
  inputs-changed cause, D20 hunk rebase, D21 read dedup, D23 diagnostic
  eviction, D25 compiler adapter (implement the deterministic emitter
  pass-through first), D27 caller scoring, D28 literal scan, D30/D32/D35
  summary/CLI surfaces (off-loop), D31 repo map, D37 search-text rerank (see
  N1-N3), D43 feedback inclusion (cheap and default-on already).
- **Folded into shipped arms** - D12/G02 budget triage (into A2/D38's
  roadmap), D38/D42 (the next-action and failure-family judgments already
  ship in the A2 request).
- **Global holds** - G07 working-memory salvage, G10 cross-run lesson
  admission (A7; overfitting guard applies), G12 ask_user necessity screen.

### 3.4 reject (4) and refuted (1)

- **D6 re-admission floor** - REFUTED: a binding closed axis ("prefix
  stability (admission floor) - no efficiency gain; axis closed",
  `docs/plans/agent-performance-avenues.md` section 1). Not to be pursued.
- **C4 / G03 / E1 choosing among completed passing candidates** - closed axis
  (best-of-N as budget splitting).
- **G08 retrieval-excerpt drop extension** - already in effect: rerank runs
  before render and the trim removes from the end of the judge-ordered array
  (`src/repo/retrieval.c:569-576, 680-686`). The residual issue is the pinning
  gap recorded as new finding 5.

## 4. Constraints every seam inherits

- Fail-open: a transport, service, parse or over-cap failure means "no
  judgment" and the deterministic behavior continues.
- Offline configurations keep working; a fully offline Forge retains its
  current path (non-Windows builds have no transport and every call fails
  open by design).
- No success authority: Jev can never declare tests passed, approve a
  candidate, or bypass host validation; thresholds stay host-side.
- One batched request per decision point; pin `jev-1.13.0` (not the moving
  alias) for experiments and record the returned model id and the server
  request id (the latter shipped in `11e8c822`).
- TypeSafe MCA 2.3(f) restricts publishing benchmark numbers; check the
  agreement or obtain a carve-out before publishing service measurements.
- Payload discipline: measured request sizes are 7.9-25.6 KiB (rerank) and
  3.5-7.8 KiB (feedback) per call in the retained records; per-call payload
  sizes were not recomputed from raw records in this audit and remain
  approximate.

## 5. New findings filed by the refuters

1. **N1 (WRONG, high)** - `search_text` is not "the surface the agent actually
   uses": 0 of 1179 retained `events.jsonl` files contain a `search_text` call
   (`read_file` appears in 1086; `retrieve_context` in 0). Do not wire rerank
   there before that surface has measured engagement.
2. **N2 (WRONG, high)** - `fg_repo_search` is a literal `instr(content, query)`
   scan ordered by path (`src/repo/repo.c:1905-1907`), not BM25; a rerank
   there would replace path order, which strengthens the case but must
   preserve literal-match semantics.
3. **N3 (RISK, high)** - judge rerank fails LIMIT above `max_candidates`
   (default 32) and callers fail open; `search_text` returns up to 50 hits, so
   a naive wiring would silently keep path order on most searches while
   recording rerank failures. Clamp or raise the cap deliberately and test the
   over-cap path.
4. **N4 (COST, high)** - D44's truncation premise is untriggered in the
   measured population (default cap 65,536 bytes versus measured <=2,087
   visible bytes); keep it off until a population with larger outputs exists.
5. **Rerank/trim pinning gap (RISK, needs-verify)** - with rerank armed, the
   output-budget trim removes rows from the end of the judge-ordered array and
   nothing pins mandatory or latest evidence (`src/repo/retrieval.c` has no
   such handling). Before any further rerank-driven inclusion change, add
   explicit mandatory/latest pinning in the trim and record trimmed rows in
   the stage trace; then re-check the retrieval-level survival clause.

## 6. Recommended sequence

1. **G01 record-only instrumentation** (small, behavior-neutral): judge
   counters in `metrics.json` and event linkage. Everything calibration-shaped
   needs this first.
2. **Decide A1's surface and A2's campaign**: either wire rerank where the
   agent actually retrieves (after measuring that surface) or select a
   retrieval-forcing population; run the A2 larger campaign on the frozen
   population manifest.
3. **Shadow/replay experiments in the order listed** (C1 first - it is the
   only seam with an existing feasibility design - then D4, D15, D22, D26,
   D39, D40, D17, G04, G05).
4. **Holds reopen only with new measurement** (a trigger population, a
   calibration set, or a landed P0.1 that changes the premise); rejects stay
   closed.

## 7. Provenance

Audit run: `wf_forge_perf_978e2fd5` (three waves, 27 read-only subagent units:
12 domain audits, 8 refuters, 2 Jev producers, 5 completion/producer units).
The draft catalogue is `jev_enumeration_draft.md`; cross-cutting entries are
`jev_global.md`; adjudications are `refute_jev_a.md` and `refute_jev_b.md`;
per-claim verdicts for the performance audit are `merged_claims.tsv` in the
same run directory. One correction to the audit brief is recorded: the first
rerank screen (v1) was NOT MATERIAL; the MATERIAL result is the post-fix v2
directory.
