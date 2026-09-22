# Jev pursuit candidates — top shortlist

Date: 2026-09-19. Repo HEAD `bfadc7bf`. Basis: the adjudicated usage ranking
(`docs/research/jev-usage-ranking-2026-09-19.md`, built from 115 claims through
refutation + judge passes), the 65-entry seam catalogue
(`docs/research/jev-augmentation-avenues-2026-09-18.md`), and all campaign
results through 2026-09-19. Each candidate states what it is, why it ranks
here, prerequisites, and the kill criterion that would close it. All
candidates keep host authority (no success authority), fail open, keep one
batched request per decision point, pin `jev-1.13.0`, and record server
request ids (`G01` instrumentation is in place at `61694349`).

## 1. A1 — give retrieval rerank a reachable home (surface decision + two wiring fixes) [S-M]

**What.** Decide where rerank lives in the loop the experiments actually run —
the rich loop's retrieval surface (with a retrieval-forcing population) or the
repair loop's reachable selection points (the C1 read menu; D22/D40 excerpt
choices). Before any further inclusion change: clamp or raise the
32-candidate cap and test the over-cap path (N3); add mandatory/latest
pinning to the output-budget trim and record trimmed rows (N5), then
re-check the retrieval-level survival clause.

**Why.** Largest measured quality delta in the whole review — MRR 0.288→0.716,
hit@1 5.0%→71.2%, zero rank-1 regressions, ~$0.0123 per screen — and it is
currently unreached: E3 recorded zero engagement and did not proceed per its
clause 1 (`benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md`).

**Hypothesis.** At a surface the benchmark loop actually hits, rerank improves
retrieval placement in-loop without prefix-stability cost.

**Kill.** No reachable surface within the benchmarked loop, or engagement
stays zero after wiring → A1 stays retrieval-level; stop expanding.

## 2. C1 — read-routing shadow adapter [M]

**What.** The 2026-09-17 MVP, still the only seam with an existing feasibility
design: build the replay/shadow decision adapter first; call only when ≥2
novel eligible reads remain; at most one host-executed read per failure
episode; abstain otherwise; a feasibility report gates any active control.

**Why.** Repair-profile failures are action-selection failures — 28
byte-identical replacement patches across eight runs, six failures consuming
all 32 actions — and detection-only interventions did not convert (the no-op
gate engaged on 11/12 failing runs yet produced 1/12 completions). It is also
the natural reachable home for A1's scoring mechanic.

**Hypothesis.** Retained failure episodes frequently contain useful,
currently-missed reads (feasibility), and selecting one per episode improves
completions more than it costs.

**Kill.** Feasibility report negative (rarely ≥2 novel eligible reads; no
useful missed actions) → drop active control; keep the adapter for offline
analysis.

## 3. A2 — threshold retune before any re-test [M]

**What.** Build a larger shadow set (retained + new episodes) and fit
consumption bands that discriminate. Today's are saturated: `uncertain` fires
60/60, `next_action_confidence` never reaches its 0.55 bound (max 0.42), and
`evidence_gap` flips across the 0.60 gate. If a band discriminates at usable
n: preregister a re-campaign; consider 2-3-sample majority for the one
decision the band trusts.

**Why.** The mechanism engages reliably (6/6 clusters, 31 calls) but the
shipped shape rendered a constant inspect hint — the refutation was of the
shape, not the pattern.

**Kill.** No discriminating band at usable n → retire the typed-guidance
shape for this population; the only alternative shape worth a fresh
preregistration is a failure-class → fixed-advice lookup (the pi-jev
pattern).

## 4. Same-request widening — D39 + D40 + D22 [S, rides on 3]

Incomplete-validation advice (D39), repair-evidence excerpt choice (D40),
diagnostic-view ranking (D22): all batchable into the same request at the
repair decision point, no extra round trip, replayable from retained
excerpts. The shadow-set collection for candidate 3 pays for these too.

## 5. G04 — escalation / stop-loss triage, shadow-only [M]

Forge's failing runs die by repeated failures and cap exhaustion; the
stop-loss, semantic-loop and recovery detectors fire exact policy.
Shadow-test whether jev judgments would improve those decisions; control use
requires an explicit later decision.

**Kill.** Shadow shows jev triage does not separate the detectors' mistake
cases → close.

## 6. G05 — offline failure-taxonomy classification [S]

Classify the `wrong_fix` catch-all offline; labels never become gates or
denominators. Cheap, no runtime risk, improves every future analysis and
every retune dataset.

## Queued — reopen with measurement, in order

- D17+G11 validation ordering (measure the validation share of wall time
  first; ordering-only).
- D4 context ranking (shadow only; preserve prefix stability).
- D26 retrieval graph seed (shadow first; amending `docs/RETRIEVAL.md` is a
  deliberate decision).
- D15 failure-class identity (detection refinement only).
- Permission/action gating experiment (pi-jev shape) only if Forge grows an
  action surface worth screening; fail-open or fail-closed chosen per action;
  shadow-first.
- D44 output pruning only for a population with larger tool outputs.
- System One adapter A/B (interface vs model) if early results look positive.

## Not pursuing

Best-of-N candidate selection (closed axis — sequential shared-budget
candidates); re-admission floor (refuted); constant-hint advisory text as
shipped (parked by its own preregistered refutation); rerank on `search_text`
(wrong surface; literal-scan semantics).

## Suggested next batch

1 + 2 are the must-dos (both offline-heavy, negligible API cost — ~$0.0001–
0.0002 per judge call); then 3's shadow-set collection, with 4 riding on it.
The real budget for any live campaign is GPU time under the machine-idle
discipline in `docs/RUN_EFFICIENCY.md`.