# 2026-08-31 tier-1 multi-model sweep: is the thought channel portable?

Every published thought-channel number until now rested on one model
(Qwen3-Coder-30B-A3B-Instruct Q4_K_M, `../2026-08-30-elicited-sweep/`). This
sweep measures four arms on three further **non-thinking instruct models** to
separate the mechanism from that one model. Natively-thinking models are out of
scope: the harness would measure its own artifacts on them, not the model.

One binary for all 120 records
(`93ea9b241eb36f26d874aa3c930b50c54c80cc899680856ee8797f35b6fef09e`, branch
commit `1178cb4`), WSL2 / RTX 5090 Laptop 24 GB, 16,384-token context with
2,048 reserved per response, 16-turn cap, all layers offloaded, greedy decoding
with seed 42. Each model is consolidated separately by
`benchmark/consolidate_arms.py`, which verifies that every arm shares a binary,
model, fixture preparation, context and turn cap; it includes `model_sha256` in
that identity and so structurally refuses to merge models. Every model ran on
its **embedded** chat template — no `--chat-template` override was needed.

The binary differs from the elicited sweep's (`aa5948b7…`) only by a rebuild:
the sole source change between the two commits is a comment in
`src/core/config.c`. Behaviour is identical; the hash is not.

## Cross-model pass rates

Pass rates and turn counts are the only numbers compared across models. Token
counters are tokenizer-relative and are compared **within** a model only.

| model | template | optimized | no-thought | routed decode-only | routed required decode-only |
| --- | --- | --- | --- | --- | --- |
| Qwen3-Coder-30B-A3B Q4_K_M (prior baseline) | ChatML | 10/10 | 10/10 | 8/10 | 7/10 |
| Devstral-Small-2-24B-2512 Q4_K_M | Mistral | 10/10 | 10/10 | 10/10 | 10/10 |
| Qwen3-4B-Instruct-2507 Q8_0 | ChatML | 2/10 | 4/10 | 4/10 | 3/10 |
| Meta-Llama-3.1-8B-Instruct Q4_K_M | Llama-3 | 1/10 | 0/10 | 0/10 | 0/10 |

The prior-baseline row is quoted from `../2026-08-30-elicited-sweep/` for
orientation; it was produced on a different binary hash and is not part of this
sweep's consolidation.

## Devstral-Small-2-24B-Instruct-2512 Q4_K_M

Different lab, different tokenizer (Tekken), different template family. Forty
records, zero failures.

| arm | passed | turns | prompt tokens | generated | actions with thought |
| --- | --- | --- | --- | --- | --- |
| optimized | 10/10 | 52 | 78,888 | 2,733 | 8/52 inline |
| no-thought | 10/10 | 57 | 84,626 | 2,205 | 0/57 |
| thought-routed-decode-only | 10/10 | 48 | 72,518 | 8,303 | 48/48 routed |
| thought-routed-required-decode-only | 10/10 | 49 | 74,681 | 7,294 | 49/49 routed |

Routed thought is **free** here: it holds 10/10 while using fewer turns than
either baseline (48 and 49 vs 52 and 57) and fewer prompt tokens (72,518 and
74,681 vs 78,888 and 84,626). It costs generated tokens — the reasoning itself
— roughly 3x. This is the first model measured on which routed reasoning has no
accuracy cost; on the prior baseline it dropped 10/10 to 8/10.

## Qwen3-4B-Instruct-2507 Q8_0

Chosen for headroom: same template family as the prior baseline, so capability
is isolated from tokenizer and template. Q8_0 removes the quantization
confound.

| arm | passed | turns | prompt tokens | generated | actions with thought |
| --- | --- | --- | --- | --- | --- |
| optimized | 2/10 | 87 | 151,877 | 6,199 | 3/87 inline |
| no-thought | 4/10 | 75 | 121,019 | 4,675 | 0/75 |
| thought-routed-decode-only | 4/10 | 51 | 73,869 | 11,377 | 51/51 routed |
| thought-routed-required-decode-only | 3/10 | 57 | 86,246 | 13,056 | 56/56 routed |

There is headroom, but it is not reasoning-limited: **25 of this model's 27
failures are repeated-action loops**, in which the model re-emits a patch whose
`old_text` never matches (usually spaces where the file has tabs) until the
turn cap. Reasoning does not address exact-match patch discipline, and the
routed arms do not beat `no-thought`. Routed does cut turns sharply, 51 against
87.

## Meta-Llama-3.1-8B-Instruct Q4_K_M

Chosen as the stressor: third template family, and a prediction that its
strict-JSON discipline would break the grammar. **That prediction is refuted.**

| arm | passed | turns | prompt tokens | generated | actions with thought |
| --- | --- | --- | --- | --- | --- |
| optimized | 1/10 | 108 | 533,198 | 119,509 | 0/101 |
| no-thought | 0/10 | 106 | 552,570 | 143,708 | 0/96 |
| thought-routed-decode-only | 0/10 | 20 | 25,973 | 37,464 | 11/11 routed |
| thought-routed-required-decode-only | 0/10 | 70 | 115,119 | 86,068 | 66/66 routed |

Across all 40 records not one action failed to parse: every action object this
model emitted was well-formed JSON. It fails by looping — most often repeated
`git_status` or `read_file` calls — and every arm sits at the floor, so **no
lift-or-harm conclusion can be drawn from this model's pass rates**. The
failure taxonomy is still informative, and one part of it is a mechanism
finding (below).

## Findings

1. **The mechanism is portable.** All 281 routed actions across the three
   models carried a nonempty reasoning prefix — 107/107 on Qwen3-4B, 97/97 on
   Devstral, 77/77 on Llama-3.1. The forced `Thought: ` cue, the brace and
   end-of-generation bans, and the lazy action trigger are all derived per
   vocabulary at runtime, and they worked unchanged on ChatML, Mistral and
   Llama-3 templates and on three different tokenizers. Every model's embedded
   template was recognized; the new `--chat-template` escape hatch was never
   needed. Two of three models barely used the optional inline thought field at
   all (Llama 0/101, Qwen3-4B 3/87, Devstral 8/52).

2. **Routed thought can be free, and it is cheapest on the strongest model.**
   Devstral holds 10/10 across all four arms while routed arms use fewer turns
   and fewer prompt tokens than its own baselines. That is a better result than
   the prior baseline model produced, and it argues the 8/10 routed result
   there is a property of that model, not of the mechanism.

3. **The lift question is still unanswered, and this sweep shows why.** A test
   of "does reasoning add passes" needs a model that is *reasoning*-limited on
   these fixtures. Devstral is saturated (no headroom), Llama-3.1 is on the
   floor (no signal), and Qwen3-4B's headroom is consumed by patch-formatting
   loops rather than by reasoning — routed ties `no-thought` at 4/10 there.
   None of the three can answer it. The next attempt needs either fixtures that
   are reasoning-gated or a model whose failures are.

4. **Routed decoding has a per-turn budget failure mode that baselines do not.**
   Ten of Llama-3.1's twenty routed fixtures ended at turn 1 having generated
   exactly 2,048 tokens — the full per-turn budget — with **no `model_output`
   event at all**: the model never opened an action object before the budget
   ran out. Its baseline arms produce no such failure (zero in 20 fixtures);
   they always emit actions and then loop. The cause is structural rather than
   model-specific: `src/inference/llama_backend.c` lifts the
   end-of-generation ban as soon as the action begins and then keeps sampling
   until the model emits an end token or the budget is exhausted. Nothing stops
   generation when the action is already syntactically complete, so a model
   that does not promptly emit its end token after the action pays the whole
   budget. **Phase-2 requirement: stop routed generation at action completion
   rather than waiting for an end-of-generation token.**

5. **The predicted JSON stressor did not appear.** Zero malformed action
   objects in Llama-3.1's 40 records, and zero across all 120 records in this
   sweep. Strict-JSON discipline was not the weak point for any model measured;
   loop avoidance was.

## Caveats

- The rig is a 24 GB RTX 5090 Laptop GPU under WSL2. All arms are greedy with
  seed 42, from Forge's compiled defaults. Devstral's publisher recommends a
  0.15 temperature, which is near-greedy but not identical; the others carry no
  strong low-temperature guidance, so greedy is somewhat off-distribution.
- Forge packs the entire context into a single user-role message
  (`src/inference/llama_backend.c`). This is constant across arms within a
  model but is a genuine cross-model confound, and it plausibly penalizes
  models with strong system-role priors such as Llama-3 more than the others.
  Llama-3.1's floor result should not be read as a general capability claim.
- Prompt tokenization uses `parse_special=true`, so any model's special-token
  strings appearing in repository text tokenize as control tokens. The fixtures
  make this unlikely to matter, but they are therefore not guaranteed to be
  byte-equivalent stimuli across models.
- The fixture set is ten tiny Go repairs. Granularity is 10 percentage points
  per arm, and one run is not statistically robust. Independent runs with the
  same seed are not guaranteed to produce identical action traces
  (`benchmark/README.md`), so single-arm differences of one or two tasks —
  Qwen3-4B's 2/10 versus 4/10, for instance — are weak evidence on their own.
- Wall-clock is not comparable across models and is not published here. For
  scale, Llama-3.1's arms ran roughly six times slower per fixture than
  planned because it loops to the turn cap.

## Files

Per model, under `qwen3-4b/`, `devstral-24b/` and `llama31-8b/`:

- `environment.json` — the identity `consolidate_arms.py` verified across that
  model's four arms, plus the applied `chat_template`.
- `results.json` — all forty per-run records for that model.
- `thought-census.json` — per-arm and per-run count of actions carrying
  reasoning, counted from raw `model_output` events with the host-injected
  `Thought: ` cue stripped, so routed counts are model text and not scaffold.

Raw per-run data (`stdout.jsonl`, sessions) for all twelve arms, the smoke
runs and the harness pass-through test is archived outside the repository under
`dev/forge-bench-raw/2026-08-31-tier1-models-raw/`.
