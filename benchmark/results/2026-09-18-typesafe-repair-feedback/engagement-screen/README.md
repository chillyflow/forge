# Engagement screen — population selection for the larger campaign (2026-09-18)

**Verdict: PROCEED — 6 of 29 fixtures engaged the repair-feedback mechanism; the
preregistered `four_plus_engaged` branch applies. Population frozen in
[`../population-manifest.json`](../population-manifest.json).**

Screen: all 29 fixtures, one repetition each, under `loop-repair-judge` (judge
armed, bounded repair). A run counts as *engaged* when it emitted at least one
`judge_feedback` session event — the direct evidence that a failed candidate
validation triggered the TypeSafe call. Screen evidence selects a population; it
is never part of a gate record. The selection rule was frozen before the first
cell in [`protocol.json`](protocol.json) (self-hash `a052064e…`).

## Engaged fixtures (population core)

| task | suite | judge calls | failed candidate validations | screen result | turns |
|------|-------|-------------|------------------------------|---------------|-------|
| `ceil_div` | smoke | 1 | 1 | PASS | 10 |
| `go_api_pagination` | campaign | 1 | 1 | PASS | 9 |
| `go_multifile_registry` | campaign | 1 | 1 | PASS | 12 |
| `reasoning_dependency_order` | reasoning-gated | 3 | 3 | FAIL | 32 |
| `reasoning_interval_union` | reasoning-gated | 2 | 2 | FAIL | 32 |
| `reasoning_route_specificity` | reasoning-gated | 1 | 1 | PASS | 18 |

Inert controls (frozen rule: zero engagement, zero failed validations, smoke
family preferred, fastest first): **`range_sum`** (17.2 s) and **`prefix`**
(17.3 s).

Selection outcome: **6 engaged + 2 inert controls in the population; 21
excluded**; branch `four_plus_engaged` → proceed to the larger campaign.

## Retained observations

- **Engagement is stochastic at one repetition.** `reasoning_event_replay`
  engaged in 2 of 2 treatment cells of the small A/B (and in its engagement
  preflight, 2 calls) but passed cleanly here in 22 turns with zero failed
  validations. A 1-rep screen therefore under-detects intermittent engagers.
  The manifest is exactly the frozen rule's output; adding a previously-observed
  intermittent engager is a decision for the larger campaign's own
  preregistration, with this evidence cited — not a post-hoc edit here.
- **The smoke family is not uniformly inert.** `ceil_div` engaged once: it
  failed a candidate validation, took the advisory feedback, and passed in 10
  turns. Controls were drawn from the remaining inert smoke tasks per the rule.
- `reasoning_quota_allocation` failed on **input budget** (status `limit`, 5
  turns, zero candidate validations) — the same task that produced zero
  engagement in the small A/B. It never reaches candidate validation at 16k
  context, so no feedback can fire there.
- 9 judged calls in total: all `status=ok`, model `jev-1.13.0`, 15,597 input /
  1,375 output tokens, zero errors, zero fail-opens. Raw records: `raw/`
  (attribution: UTC filename ≥ `2026-09-18T17:06:29Z`, cross-checked 1:1 against
  `judge_feedback` events in the retained session events).
- Failures: 3 of 29 — `reasoning_dependency_order` and
  `reasoning_interval_union` at the 32-turn cap, `reasoning_quota_allocation`
  on input budget. All retained with full run directories.

## Provenance and integrity

- `protocol.json` self-hash `a052064e9702143b7a22f055d21b57d32d326e9c25e683728685009c889fa84b`
  (recomputed and verified by the selector).
- forge `bcf15d4e…`, model `fadc3e5f…`, `judge.toml` `92e2e844…` — the same
  judge configuration as the small A/B. Screen start `2026-09-18T17:06:29Z`;
  all 29 cells ran exactly once; protected files and verification inputs
  unchanged in every cell.
- Selector: `benchmark/engagement_screen.py` (applies the frozen rule; refuses
  partial screens, variant mismatches, and integrity failures). Covered by
  `tests/unit/test_engagement_screen.py`.

## Use

The larger campaign's own preregistration should freeze: population = the 6
engaged fixtures + the 2 inert controls; arms = `loop-repair` vs
`loop-repair-judge`; repetitions sized against observed variance (a 1-rep
screen cannot size them); full-length cells (cap 32 — failures here died at the
cap, successes resolved in 7–22 turns).

## Appendix: full screen table (all 29 fixtures, one repetition each)

| task | suite | result | turns | e2e (s) | judge calls | failed val. | validations |
|------|-------|--------|-------|---------|-------------|-------------|-------------|
| `add` | smoke | PASS | 7 | 17.4 | 0 | 0 | 1 |
| `average` | smoke | PASS | 7 | 17.9 | 0 | 0 | 1 |
| `ceil_div` | smoke | PASS | 10 | 21.1 | 1 | 1 | 2 |
| `clamp` | smoke | PASS | 7 | 18.0 | 0 | 0 | 1 |
| `contains` | smoke | PASS | 7 | 17.4 | 0 | 0 | 1 |
| `go_api_pagination` | campaign | PASS | 9 | 25.6 | 1 | 1 | 2 |
| `go_compiler_interface` | campaign | PASS | 7 | 19.0 | 0 | 0 | 1 |
| `go_explore_precedence` | campaign | PASS | 13 | 32.7 | 0 | 0 | 1 |
| `go_multifile_registry` | campaign | PASS | 12 | 24.9 | 1 | 1 | 2 |
| `go_multifile_transfer` | campaign | PASS | 12 | 23.6 | 0 | 0 | 1 |
| `go_refactor_clock` | campaign | PASS | 10 | 20.8 | 0 | 0 | 1 |
| `last_index` | smoke | PASS | 7 | 17.7 | 0 | 0 | 1 |
| `prefix` | smoke | PASS | 7 | 17.3 | 0 | 0 | 1 |
| `py_api_query` | campaign | PASS | 21 | 42.8 | 0 | 0 | 1 |
| `py_atomic_batch` | campaign | PASS | 8 | 17.6 | 0 | 0 | 1 |
| `py_compiler_syntax` | campaign | PASS | 11 | 22.0 | 0 | 0 | 1 |
| `py_explore_slug` | campaign | PASS | 9 | 18.1 | 0 | 0 | 1 |
| `py_graph_cycle` | campaign | PASS | 7 | 16.8 | 0 | 0 | 1 |
| `py_multifile_invoice` | campaign | PASS | 9 | 17.5 | 0 | 0 | 1 |
| `py_refactor_repository` | campaign | PASS | 11 | 19.9 | 0 | 0 | 1 |
| `range_sum` | smoke | PASS | 7 | 17.2 | 0 | 0 | 1 |
| `reasoning_atomic_transfers` | reasoning-gated | PASS | 7 | 21.7 | 0 | 0 | 1 |
| `reasoning_dependency_order` | reasoning-gated | FAIL | 32 | 142.0 | 3 | 3 | 3 |
| `reasoning_event_replay` | reasoning-gated | PASS | 22 | 75.7 | 0 | 0 | 1 |
| `reasoning_interval_union` | reasoning-gated | FAIL | 32 | 147.5 | 2 | 2 | 2 |
| `reasoning_quota_allocation` | reasoning-gated | FAIL | 5 | 26.0 | 0 | 0 | 0 |
| `reasoning_route_specificity` | reasoning-gated | PASS | 18 | 43.6 | 1 | 1 | 2 |
| `reverse` | smoke | PASS | 7 | 17.8 | 0 | 0 | 1 |
| `unique` | smoke | PASS | 7 | 18.0 | 0 | 0 | 1 |
