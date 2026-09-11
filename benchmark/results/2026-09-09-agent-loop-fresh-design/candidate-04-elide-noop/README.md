# candidate-04 `elide-noop-04` — W1 tests H-ECHO, and refutes it

Frozen and screened September 10, 2026. G0 accepted 5/5. Screened under W3's
preregistered rule. **H-ECHO is refuted.**

Source identity is byte-identical to the retained, aborted `candidate-03`
(see its README): same `source_sha256`, `runtime_sha256`, `model_sha256` and
`configuration_sha256`.

## The change

`--elide-noop-edits`, opt-in, requires `--candidate-checkpoint`. When an
`apply_patch` is rejected **specifically** because its `new_text` equalled its
`old_text`, the retained action keeps the model's own prose and the named path
and drops the patch text, and the retained result becomes a deterministic host
observation. `minimal`, `candidate-checkpoint` and `loop-repair` are unchanged.

The discriminator keys on the rejection **branch**, not the status code: the
identical-replacement and anchor-mismatch rejections both raise
`FORGE_ERR_CONFLICT`, so a code-based test would have caught both. The
post-splice byte-equality rejection is deliberately excluded, because its
argument strings differ.

The ACTION/RESULT pair is preserved structurally rather than removed. Dropping
it would fail `validate_native_history` (`src/inference/chat_template.cpp:54`)
and the native renderer (`src/context/context.c:519`), both of which require
every assistant tool call to keep its matching tool result.

## G0

| Check | Result |
| --- | --- |
| `full-gpu-ctest` | passed (34/34) |
| `new-deterministic-regressions` | passed |
| `native-template-model` | passed |
| `checkpoint-model` | passed |
| `checkpoint-model-automatic` | passed |

`acceptance-report.json` records `G0 accepted: true`, 5 required, 5 passed,
0 invalid, 0 missing, and an empty `errors` list.

Six new deterministic regressions cover W1's acceptance items: a rejected
identical edit that is not retained verbatim, a normal rejection that still is,
the raw action surviving in session artifacts, the bounded-repair applied-delta
evidence remaining correct, the unflagged control retaining verbatim, and the
flag's own compatibility validation.

## Screen

14 runs: four Python retraction manifests at three repetitions, two Go manifests
at one. Screening evidence only — never written to `outcomes.json`, never pooled
with gate outcomes, not re-run.

| Population | Result |
| --- | --- |
| Python | **2/12** (distractor 2/3; original, renamed, paraphrased 0/3 each) |
| Go | 2/2 — no regression |
| Manifests with ≥1 pass | 1/4 |

The preregistered rule requires ≥11/12 Python **and** ≥1 pass in each of the four
Python manifests **and** no Go regression. None of the first two is met.

W0's matched baseline for the same arm, population, profile and frozen runtime
is **4/12**. The screen is **2/12** — below it, not a doubling. The preregistered
branch is therefore *"≤7/12 with no material lift over W0's baseline: not a
lever. Record it, keep any defect fixes, escalate to W4."*

That branch is recorded as it was written, before any outcome was seen. 2/12
versus 4/12 is not a statistically meaningful decrease at this sample size
(Fisher exact two-sided p = 0.64); the honest reading is "no lift", not "harm".

## The falsification test — the actual result

The plan states the criterion in advance: *"If H-ECHO is right, the
identical-replacement rate and the no-op-after-no-op transition rate both fall
sharply. If the transition rate stays near [the baseline] with the exemplars
removed, H-ECHO is wrong and the degeneracy is internal to the model."*

**The mechanism provably activated.** 43 `noop_edit_elided` events across the
screen, exactly equal to the 43 identical replacements counted independently
from `tool_call` events. Every no-op was removed from the conditioning set.

Comparison against W0's matched candidate-2 arm — same manifests, same profile,
same frozen runtime, differing only in the flag:

| Measure | candidate 2 (W0) | + elision (screen) | Fisher p |
| --- | --- | --- | --- |
| identical / `apply_patch` | 33/82 = 40.2% | 43/101 = 42.6% | 0.77 |
| P(identical \| previous applied) | 15/40 = 37.5% | 20/50 = 40.0% | — |
| **P(identical \| previous identical)** | **18/30 = 60.0%** | **23/39 = 59.0%** | **1.00** |

**Neither rate fell.** The repeat rate is unchanged to within one percentage
point with the exemplars provably absent from the prompt. H-ECHO predicted a
sharp fall in exactly this number and it did not move.

**Conclusion: H-ECHO is wrong.** The identical-replacement degeneracy is not
auto-catalysed by the model's own retained no-ops. It is internal to the model.
This is the "result worth having" the plan anticipated, and it points squarely
at W4.

This is a stronger refutation than a null pass-rate result would have been,
because the intervention is confirmed to have done exactly what it was designed
to do. The exemplars were removed; the behaviour did not change.

## A claim the plan made that did not hold

The plan argues the intervention "is the only proposal considered that reduces
tokens per turn. It cannot cost an action." Mean cumulative prompt tokens rose,
221,049 → 232,852. The per-turn saving is real, but it does not reduce
cumulative consumption: freed budget is spent on further turns rather than
banked. The claim should not be carried forward.

## Caveats

- Screening evidence, not gate evidence. Never pooled with a gate.
- 12 Python runs cluster by manifest; the effective sample is nearer 4 than 12.
- Execution order is this screen's fixed order, not G1's shuffled order.
- The transition statistic remains correlational in both arms. What this screen
  establishes is that removing the exemplars does not change it — which is an
  interventional result, and the reason the experiment was worth running.
- **The W2 verification-cache prerequisite was not closed before this screen.**
  The plan calls it blocking for trusting a screening result. It is demonstrated
  real in `../W2-CACHE-PREREQUISITE.md`. It could in principle admit a false
  *pass*; this screen's finding is a null on the transition rate, which that
  hole cannot manufacture, and the pass count is low rather than inflated. The
  caveat is recorded rather than argued away.
