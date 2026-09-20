# TypeSafe repair-feedback campaign — 2026-09-19

**Disposition: REFUTED for this population.** The mechanism engaged and the
outcome did not move; per the preregistered rule that is a refutation of the
hypothesis, not evidence of neutrality.

## Question

Does the shipped bounded-repair TypeSafe judge feedback (arm 2) improve
independent task completion on the frozen engaged population?

## Identity (frozen before the first cell)

- Protocol: `protocol.json`, self-hash
  `6c5ce3880dce487af341eff17a15ed20c0eb0970340c55e6118537d456b1f7c3`
  (frozen 2026-09-19T21:38Z; amended 22:30Z — binary identity only, after the
  VRAM diagnostic rebuild; bar, population, schedule and profile fields were
  not changed. See Corrections: the self-hash below is the as-run value; the
  post-errata value is recorded in `protocol.json`).
- Binary: `build-gpu/Release/forge.exe` sha256 `f977c623...`; source `61694349`
  (G01), working tree clean at freeze.
- Model: Qwen3-Coder-30B-A3B-Instruct-Q4_K_M sha256 `fadc3e5f...`.
- Judge: `judge.toml` sha256 `f35fa399...` — api.typesafe.ai, pinned
  `jev-1.13.0`, 2000 ms timeout; raw records under `raw/`.
- Population: `../2026-09-18-typesafe-repair-feedback/population-manifest.json`
  (6 engaged + 2 inert controls, selected by the engagement screen; manifest
  sha256 `f0661378...`).
- Schedule: 64 cells = 8 tasks × 2 arms × 4 repetitions, order seed 20260919,
  each executed exactly once, interleaved (explicit schedule in `protocol.json`).
- Harness: `benchmark/run.py` (resume-safe), `benchmark/common.py`.

## Results — primary: independent completion

| arm | passed | scheduled cells |
|---|---|---|
| `loop-repair` (control) | 26 | 32 |
| `loop-repair-judge-campaign` (treatment) | 25 | 32 |

Per cluster (passed/4 per arm):

| task | role | control | treatment |
|---|---|---|---|
| ceil_div | engaged | 4/4 | 4/4 |
| go_api_pagination | engaged | 4/4 | 4/4 |
| go_multifile_registry | engaged | 4/4 | 4/4 |
| reasoning_dependency_order | engaged | 0/4 | 1/4 |
| reasoning_interval_union | engaged | 2/4 | 0/4 |
| reasoning_route_specificity | engaged | 4/4 | 4/4 |
| range_sum | inert control | 4/4 | 4/4 |
| prefix | inert control | 4/4 | 4/4 |

Engaged clusters: control 18/24, treatment 17/24.

## Engagement

6 of 6 engaged tasks fired (preregistered bar: ≥4). 31 judge calls across 24 of the
32 treatment cells (17 cells ×1 call; 7 cap-death cells ×2, from two failed
candidate validations each). The 2 inert control tasks fired
0 calls in all 8 of their treatment cells. Per-call latency mean 521 ms,
max 781 ms. Raw request/response records retained in `raw/` (36 files: 2 from the
engagement preflight, 3 from the pre-campaign VRAM-diagnostic probes, 31 from
the campaign).

## Latency (secondary)

- Overall e2e: control mean 52.4 s / median 25.0 s; treatment mean 53.0 s /
  median 24.5 s (13 cap-death cells in total — 6 control, 7 treatment — on the two reasoning
  fixtures, which dominate the means).
- Paired e2e on the 24 cells passed by both arms (same task + repetition):
  control 24.5 s vs treatment 24.0 s (−0.5 s, ≈2%, noise-level; the judge call
  itself adds ≈0.5 s where it fires).

## Integrity

- Protected files unchanged in all 64 cells; zero violations.
- No harness/verifier divergence: returncode and independent-verification
  verdict agree in every cell.
- All 64 scheduled cells executed exactly once; no cell lost; no re-runs.

## Decision-rule application (as preregistered)

1. Engagement ≥4 of 6 engaged tasks → **met (6/6)**.
2. Treatment overall pass count > control → **not met (25 vs 26)**.
3. No engaged cluster regresses by ≥2 cells → **not met**
   (`reasoning_interval_union` 2/4 → 0/4).
4. Latency secondary not worse → met (paired −0.5 s, within noise).

Promotion requires (1)–(3): **not promoted.** With (1) met and no material
improvement, the preregistered refutation branch applies: **the mechanism
activates without moving the outcome — a refutation of the hypothesis for this
population.**

## Reading the result honestly

- This refutes "the shipped arm improves completion on this population", not
  "no judge feedback can ever help". The shadow replay (2026-09-18) had already
  shown the shipped consumption thresholds saturate: the rendered guidance is
  today a constant inspect-hint, and Jev's answers are stochastic at N=4
  (0/15 episodes reproduced). The campaign measured that arm as shipped.
- The cluster with the most engagement (`reasoning_interval_union`, 2 calls per
  cell) is the one that regressed by the guardrail margin (2/4 → 0/4): the
  extra inspect guidance plausibly consumed repair turns. With n=4 per cell the
  cluster delta is noisy, but the guardrail fired exactly as written.
- Power: 6 engaged clusters × 4 repetitions detects large effects only. This
  null excludes a large effect; it does not exclude a small one.
- Environment: the batch ran VRAM-idle (1,149 MiB desktop footprint before
  launch); per-cell speeds match the pre-pressure baseline (16.7–45.8 s), so
  the latency comparison is not confounded by the earlier oversubscription
  incident (recorded in `docs/RUN_EFFICIENCY.md` §1.16).

## Artifacts

- `protocol.json` (self-hash above), `judge.toml`, `results.json`,
  `raw/` (36 judge records), 64 run directories with retained sessions and
  terminals, `preflight-engagement/`.

## Corrections (2026-09-19, after the fable-judge pass)

An adversarial verification pass recomputed every claim in this report from the
raw artifacts (verdict: the campaign data fully verified, zero frauds; five
reporting errors found and corrected here; no campaign re-run needed):

| field | as first written | corrected |
|---|---|---|
| judge calls | 24 | 31 (17 cells ×1 + 7 cells ×2) |
| `raw/` files | 26 | 36 (2 preflight + 3 VRAM-diagnostic probes + 31 campaign) |
| paired cells | 26 | 24 |
| cap-death cells | 4 | 13 (6 control, 7 treatment) |
| `protocol.json` harness hash | freeze-time `1e95b1b6` | as-run `8a0c08c0` (commit 8e920922; resume-retry hardening; the resume branch was never taken) |

The protocol was corrected in place with a `corrections` field naming the old
value; its self-hash changed from the as-run `6c5ce388...` to the post-errata
value recorded in `protocol.json`. No bar, population, schedule, profile or
outcome field changed, and the REFUTED classification is unaffected.

Interpretive note: the preregistered rule does not threshold the paired-latency
"advantage"; the observed −0.5 s (≈2%) is treated as noise, and the
classification stands under any reading.

## Follow-ups

- Retune or replace the saturated consumption thresholds before any further
  judge-feedback arm (the replay's "retune" branch); the thresholds were
  preregistered as shipped and are now known to render a constant hint.
- Park arm 2 as shipped: not promoted; keep it behind `--judge` and out of
  default profiles.
- Arm 1 (retrieval rerank) remains untested at agent level (engagement 0 of
  1,179 sessions); this campaign does not change that decision.
