# TypeSafe repair-feedback screen — results (2026-09-18)

**Verdict: CONTINUE — engagement is nonzero and the preregistered continue bar is
met. Directional screen only (n=4 per arm over 2 tasks); not a promotion claim.**

First screen of the advisory TypeSafe (`jev-1.13.0`) repair-feedback loop on the
bounded-repair arm: after a failed candidate validation, Forge sends one batched
request to System One and appends the typed answers as advisory repair guidance.
Host validation remains the only authority for candidate success; the judge never
gates.

## Primary results

8 scheduled cells (2 tasks × 2 arms × 2 repetitions), each executed exactly once,
in the preregistered order.

| # | run | result | e2e (s) | turns | generated tokens |
|---|-----|--------|---------|-------|------------------|
| 1 | `reasoning_quota_allocation-loop-repair-judge-r001` | PASS | 111.3 | 21 | 9,722 |
| 2 | `reasoning_event_replay-loop-repair-judge-r002` | FAIL | 150.7 | 32 | 11,563 |
| 3 | `reasoning_quota_allocation-loop-repair-r001` | FAIL | 186.4 | 32 | 17,381 |
| 4 | `reasoning_event_replay-loop-repair-r001` | FAIL | 142.0 | 32 | 11,394 |
| 5 | `reasoning_quota_allocation-loop-repair-judge-r002` | PASS | 33.2 | 9 | 2,099 |
| 6 | `reasoning_event_replay-loop-repair-judge-r001` | PASS | 122.2 | 27 | 9,361 |
| 7 | `reasoning_quota_allocation-loop-repair-r002` | PASS | 34.4 | 10 | 2,250 |
| 8 | `reasoning_event_replay-loop-repair-r002` | FAIL | 139.7 | 32 | 10,839 |

| arm | pass / fail | mean e2e |
|-----|-------------|----------|
| `loop-repair` (control) | 1P / 3F | 125.6 s |
| `loop-repair-judge` (treatment) | 3P / 1F | 104.3 s |

| task | control | treatment |
|------|---------|-----------|
| `reasoning_event_replay` | 0P / 2F | 1P / 1F |
| `reasoning_quota_allocation` | 1P / 1F | 2P / 0F |

Preregistered decision rule (`ab-small/protocol.json`, self-hash
`16b39caf…`): continue only if treatment pass count ≥ control, treatment
engagement nonzero, and no new protected-file violations or harness errors.

- Pass count: 3 ≥ 1 — **met**.
- Engagement: 4 `judge_feedback` events in 2 of 4 treatment cells — **met**
  (the other two treatment cells never produced a failed candidate validation,
  so the mechanism was correctly silent).
- Integrity: protected files unchanged and verification inputs unchanged in all
  8 cells; no harness errors — **met**.

## Mechanism and engagement

- 4 judged calls in the A/B (`event_replay` r001 ×1, r002 ×3) plus 2 in the
  engagement preflight: 6 raw records retained under `raw/`.
- All 6 records: `status=ok`, model `jev-1.13.0`, 11,512 input / 918 output
  tokens, latency 125–516 ms (median 422 ms). Every record carries the server's
  `x-typesafe-request-id` and `date` headers (post-`11e8c822`), individually
  reconcilable. No API errors, no fail-opens, zero calls on the inert cells.

## Population shape (retained observations)

- All 4 failures died at the 32-turn cap; all 4 successes resolved within 9–27
  turns (max successful turn 27 < cap 32). The population is bimodal — resolve
  early or exhaust the budget — so the cap is not censoring successes.
- Half the scheduled population could not exercise the mechanism: both
  `reasoning_quota_allocation` treatment cells produced zero failed candidate
  validations. Only `reasoning_event_replay` carried a treatment-vs-control
  signal; the other half measures the inert case.
- One repetition flip across the preflight/A-B boundary:
  `reasoning_event_replay-loop-repair-judge-r001` FAILed in the engagement
  preflight (32 turns) and PASSed in the A/B (27 turns). The two ran on
  different (relinked, below) binaries and fresh workspaces. This is the
  documented single-run swing class; it is why the screen carries repetitions
  and why it is a screen, not a decision.

## Provenance and integrity

- Preregistration: `ab-small/protocol.json`, self-hash
  `16b39cafcf8ce97ea21bd942dc62240cb3c95e48122aeb4815d1959eb4a74155`
  (recomputed and verified). Schedule 8/8 exact — every scheduled cell ran
  exactly once; none missing, none extra.
- Model: `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`, sha256
  `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad` — matches
  the frozen identity. Temperature 0.0, seed 42, context 16,384, max turns 32,
  GPU layers −1. `judge.toml` sha256
  `92e2e8446a65d5aeacb761f64788827fee66ec2f7a9fea5f535f9d0a4757b66f`.
- **Binary relink between the freeze and the A/B cells.** The frozen protocol
  and the engagement preflight recorded
  `d5984028c9b0a063cdc38cac8ed430de19fff3890aeac3cf3342f9e49ed5ae29`; the A/B
  cells ran on
  `bcf15d4e1dc9a978dfce144445e3b6dc0c1398ec778737163f573cabca037bb7`.
  Cause: a local verification rebuild re-linked `forge.exe` after the preflight
  (CMake re-ran configure because `.git/index` was newer than its generate
  stamp; the rebuild produces a new MSVC link hash). Source unchanged — HEAD was
  `0624acb4` at freeze and at the cells, and the only commit in the session
  touched `benchmark/run.py`, `tests/unit/test_benchmark.py` and `judge.toml`;
  zero `src/`/`include/` changes. Both builds compiled the same source with the
  same configure. The A/B batch is internally consistent: all 8 cells, both
  arms, one binary. No bar, population, schedule, or settings field changed.
- The engagement preflight is evidence for mechanism engagement only and is not
  part of the gate record.
- Retained: `ab-small/` (8 full run directories, including failed-workspace and
  session artifacts), `preflight-engagement/` (1 run directory), `raw/` (6
  records), `protocol.json`, `environment.json`, `judge.toml`, this README.

## Limitations

- n=4 per arm over 2 tasks; repetitions of the same task are not independent
  observations (task-level units = 2). This is a directional screen: it met the
  *continue* bar, and any larger campaign must re-earn its own bar.
- Failures are cap-deaths; the screen cannot say whether the feedback shortens
  the path to failure or improves repair quality — only that the treatment did
  not lose cells and that the mechanism engaged.
- Mean e2e differs (104.3 s vs 125.6 s) but mixes pass/fail compositions; not a
  speed claim.

## Disposition

Continue condition met → a larger campaign is eligible. Recommended next step:
an engagement screen to select the population (tasks that actually produce
failed candidate validations under bounded repair) so full-length cells are
spent where the mechanism can fire, keeping one inert task as a
harm-when-dormant control.

## Follow-up: engagement screen + population manifest (2026-09-18)

Executed. All 29 fixtures screened once under `loop-repair-judge`
(`engagement-screen/`): 6 engaged (`ceil_div`, `go_api_pagination`,
`go_multifile_registry`, `reasoning_dependency_order`,
`reasoning_interval_union`, `reasoning_route_specificity`), 2 inert controls
selected (`range_sum`, `prefix`), 21 excluded; branch `four_plus_engaged` → the
larger campaign is eligible. The frozen population is
[`population-manifest.json`](population-manifest.json) (sha `f0661378…`);
details and caveats (engagement is stochastic at one repetition) in
[`engagement-screen/README.md`](engagement-screen/README.md).
