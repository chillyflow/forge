# Capability stage — model arms on the reasoning-gated and retraction suites

Executed September 11, 2026, on one frozen binary (candidate-09 runtime,
`9960ad04741e14f8…`) with the loop-pilot profile (temperature 0.6, seed 42,
32 actions, 16384 context, 600 s, native embedded templates). Protocol and
bars were frozen in `protocol.json` before the first run. These are declared
screening arms: no gate batch, no default change, no `outcomes.json` writes.

## Models (downloaded, hashes verified)

| Arm label | Model | SHA-256 (verified) |
| --- | --- | --- |
| Qwen coder | Qwen3-Coder-30B-A3B-Instruct Q4_K_M | `fadc3e5f8d42…` |
| Devstral | Devstral-Small-2-24B-Instruct-2512 Q4_K_M | `d14ba9edee1b…` (matches `docs/MODEL.md` pin) |
| Thinking | Qwen3-30B-A3B-Thinking-2507 Q4_K_M | `b7380c816fca…` (matches HF LFS oid) |

## Configuration finding: Devstral cannot run the minimal-native loop

Devstral's Mistral template enforces strict user/assistant alternation
(`raise_exception` when the sequence does not alternate). The minimal loop
appends a control `user` message after tool results, which the template
rejects; the native prompt counter then falls back to a sentinel and the run
dies with "Rendered mandatory prompt exceeds the input budget". Reproduced
directly with a native request containing one tool exchange. Recorded as a
Forge/renderer limitation, not a capability result. Devstral therefore ran in
the ordinary flattened agent (variant `optimized`, `--prompt-protocol
flattened`), a declared configuration.

The Thinking model runs both ways; in the minimal loop its per-turn output
reserve must be 4096 (2048 died before one complete native call in the pilot),
so Group A used 4096 for both arms to keep the single variable the model.

## Stage 1 — six reasoning-gated Go fixtures, 3 repetitions per arm

**Group A — deployable minimal-native loop (reserve 4096):**

| Arm | Runs | Fixtures with a pass | Per-fixture P/. |
| --- | ---: | ---: | --- |
| a0 Qwen coder | 6/18 | 4/6 | `...` `...` `..P` `..P` `.P.` `PPP` |
| a1 Thinking | 3/18 | 1/6 | `...` `...` `...` `PPP` `...` `...` |

The thinking arm did not clear G-Model and is directionally below the coder
baseline. **Cluster-level (fixture-level) comparison: 4/6 vs 1/6,
Fisher exact two-sided p = 0.24.** The axis closes: enabling the model's
native thinking path does not lift success on this suite within the fixed
budgets.

**Group B — ordinary flattened agent (reserve 2048), internal control:**

| Arm | Runs | Fixtures with a pass | Per-fixture P/. |
| --- | ---: | ---: | --- |
| b0 Qwen coder | 0/18 | 0/6 | all `...` |
| b1 Devstral | 10/18 | 4/6 | `...` `...` `PPP` `PPP` `PP.` `.PP` |

**Cluster-level comparison: 4/6 vs 0/6, Fisher exact two-sided p = 0.061.**
This is a strong directional model effect; it is *not* statistically
significant at six clusters. Do not report the per-run contingency
(`[[10,8],[0,18]]`) as the headline: the three repetitions of one fixture are
not independent observations.

## Stage 2 — retraction family (Groups A and B), 12 runs per entitled arm

**Group B pair:**

| Arm | Runs | Fixtures with a pass | Bar: ≥4/12, ≥2 fixtures |
| --- | ---: | ---: | --- |
| b1 Devstral | 5/12 | 4/4 | **cleared** |
| b0 Qwen coder | 0/12 | 0/4 | not cleared |

**Group A addendum — `a0` coder, the only Group A arm entitled to Stage 2:**

| Arm | Runs | Fixtures with a pass | Bar: ≥4/12, ≥2 fixtures |
| --- | ---: | ---: | --- |
| a0 Qwen coder | 2/12 | 2/4 | not cleared |

`a1` Thinking was excluded: it did not clear G-Model and is therefore not
entitled to the retraction stage. The decision, the schedule and the profile
were frozen in `stage2/A-deployable/protocol-addendum.json` before execution
(its declared `frozen_utc` has been corrected to the file's true mtime,
`22:39:42Z`, which precedes run 1 by 36 s). The `--arms` subset flag this stage
required was added to `run_models.py` and is committed with this record.

Every scheduled run in both groups reports `protected_files_unchanged` and
`verification_inputs_unchanged` true; zero protected-file violations across all
36 Stage 2 runs.

**Secondary finding, reported separately and never substituted for a pass:**
three further Devstral runs (`original` r001, `original` r003, `renamed`
r003) left a *passing terminal workspace* without a successful agent run. In
total 8 of 12 Devstral runs left a test-passing workspace; 5 completed
successfully. A passing terminal workspace does not overwrite a failed
primary result.

## Interpretation and next steps

1. **Devstral-Small-2-24B is the capable model on both populations** in the
   configuration it can run (ordinary flattened): 10/18 and 5/12 against its
   same-loop control's 0/18 and 0/12. The Qwen coder result is not
   comparable across loops (it is much better on the minimal loop, 6/18,
   than on flattened, 0/18).
2. **Native thinking buys nothing here** on the deployable loop under fixed
   budgets; its axis closes.
3. **The retraction family separates the models cleanly.** Devstral clears at
   5/12 across all four manifests; the Qwen coder scores 2/12 on the deployable
   native loop and 0/12 on the flattened agent. The recorded retraction-family
   exit for Qwen3-Coder is therefore reproduced on a second configuration, and
   the exit's stated reopen condition — a different hypothesis class, model
   capability — is met by Devstral rather than asserted.
4. The highest-value engineering task the stage identifies is a renderer fix
   so Mistral-family templates can run the deployable minimal-native loop,
   where Devstral's capability and the loop's strengths would combine. Until
   that exists, a flattened deployable policy for Mistral-family models is
   the only working configuration, and it needs its own measurement before
   any default changes.
5. No promotion claim and no default change follows from this stage. A fresh
   holdout remains the promotion instrument.

Corrections applied from the Hermes session handoff (`HANDOFF.md`): the
per-run Fisher figure was removed, cluster-level p reported instead; the
passing-terminal-workspace runs are separated; no significance language.

Stage 2 for Group A was executed after this section was first written and is
reported above; the `protocol-addendum.json` timestamp correction is also
recorded here rather than silently in the file.
