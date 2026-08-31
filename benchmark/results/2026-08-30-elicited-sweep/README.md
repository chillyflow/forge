# 2026-08-30 elicited sweep: all thought arms on the cue-elicitation binary

Nine arms, ten fixtures, one binary
(`aa5948b72de847e8180b7c71f5d5782bdbe2e69550f3972478ca993dabdbcf20`, branch
commit `cba84f2`), Qwen3-Coder-30B-A3B-Instruct Q4_K_M, WSL2/RTX 5090, 16-turn
cap, greedy decoding. This is the first measurement of routed thought that
actually elicits reasoning: the earlier
[routed sweep](../2026-08-30-routed-sweep/README.md) ran a binary
(`24bf9a18…`) whose lazy trigger armed on the model's first token, so every
routed arm measured zero prefixes. Consolidated by
`benchmark/consolidate_arms.py` (identity-verified across arms); the retention
analysis is `benchmark/analyze_retention.py`.

## Pass rates and reasoning census

| arm | passed | turns | prompt tokens | actions with thought |
| --- | --- | --- | --- | --- |
| optimized | 10/10 | 63 | 91,233 | 0/63 |
| no-thought | 10/10 | 62 | 87,908 | 0/62 |
| thought-optional-decode-only | 10/10 | 63 | 91,233 | 0/63 |
| thought-required (retained) | 4/10 | 114 | 280,976 | 114/114 inline |
| thought-required-decode-only | 7/10 | 76 | 123,355 | 76/76 inline |
| thought-routed | 8/10 | 54 | 99,180 | 53/53 routed |
| thought-routed-decode-only | 8/10 | 52 | 80,195 | 51/51 routed |
| thought-routed-required | 8/10 | 65 | 157,453 | 64/64 routed |
| thought-routed-required-decode-only | 7/10 | 38 | 53,349 | 35/35 routed |

The census strips the host-injected `Thought: ` cue before counting, so routed
numbers are model reasoning, not scaffold. Two prior findings replicate
exactly: an optional inline thought field is never used (0/63, was 0/63), and
forced inline retained-vs-stripped lands 4/10 vs 7/10 (was 4/10 vs 7/10).

## Findings

1. **Elicitation works.** Every routed action across all four routed arms
   carried a nonempty reasoning prefix (203/203 total). `--thought-routed
   --thought-required`, a guaranteed 0/10 parse-death trap on the old binary,
   now passes 8/10.
2. **Reasoning still does not pay on these fixtures.** Both baselines and the
   optional arm are 10/10; every arm that actually reasons loses 2–6 tasks.
   Routed reasoning is the cheapest and best form measured: 8/10 at roughly
   baseline prompt cost, versus forced inline retained at 4/10 and 3.1x the
   prompt tokens. Routed arms also use fewer turns than baseline (52–54 vs
   62–63): reasoning turns are individually more productive, but the arm
   loses tasks to within-turn limit deaths (below).
3. **Retention is now decisively harmful, not directionally.** Across five
   replicates of the required pair (50 fixture pairs,
   `retention-analysis.json`), 15 pairs were discordant and all 15 favored
   stripping: exact two-sided sign test p = 6.1e-05 (was 3 pairs, p = 0.25).
   The decode-only retention default stands on this.
4. **Routed failures are not turn starvation.** The doubled-cap control
   (`turncap-control.json`, same binary) passes the identical task set with
   identical turn counts for both routed arms (54=54, 52=52 turns): at 32
   turns nothing changes. The failures are within-turn: three of four routed
   arm failures are `Generation limit reached before a complete action` (the
   model reasons past the 2048-token turn budget without starting its action;
   end-of-generation stays banned until an action begins by design), one is an
   ordinary repeated-action loop. Bounding the reasoning phase per state is
   the phase-2 (§32 state-router) motivation.
5. **The bare prefix ban was refuted before this sweep.** The first
   re-measurement attempt (binary `850c52b3…`, archived as
   `bench-phase1-v1-aborted`) banned `{` for 32 tokens without a cue: greedy
   decoding fell into prompt echo and all ten routed tasks died at turns 1–4
   without a single action. The forced `Thought: ` cue is what makes
   elicitation work; the aborted run is the evidence for why it exists.

## Files

- `environment.json`, `results.json` — identity-verified consolidation of the
  nine 16-turn arms.
- `thought-census.json` — per-arm and per-run reasoning census (cue-stripped).
- `turncap-control.json`, `turncap-control-results.json` — the two primary
  routed arms re-run at a 32-turn cap on the same binary.
- `retention-analysis.json` — 50 paired fixture x replicate outcomes for
  retained vs stripped required thought, with the exact sign test.

Raw per-run data (stdout.jsonl, sessions) for all arms, the four replicates,
and the aborted v1 attempt is archived outside the repository under
`dev/forge-bench-raw/` (roots `bench-phase1-v2` and `bench-phase1-v1-aborted`).
