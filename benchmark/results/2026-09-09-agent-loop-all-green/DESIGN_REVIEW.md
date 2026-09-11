# Design review: G1 after candidate 2

September 9, 2026. Triggered by the repair plan's own rule in L5: "Three
unproductive experiments trigger a design review and a different hypothesis, not
relaxation of the target."

This review is read-only analysis of retained evidence. It changes no runtime
code, no budget, no gate and no denominator. It records what the fourth and fifth
consecutive attempts on one task family measured, and what the plan's rules imply
about continuing.

## Why the trigger has fired

Five host-side experiments have now targeted the Python retraction family.

| Experiment | Change class | Retraction family result |
| --- | --- | --- |
| [v2](../2026-09-08-repair-recovery/README.md) `c821b5e` | failed states, early runner guidance | 7/10 overall; lost paraphrased, gained renamed |
| [v3](../2026-09-08-repair-recovery/README.md) `7660bf1` | immediate revalidation, hunk bounds | 7/10 overall; lost paraphrased, gained distractor |
| [v4](../2026-09-08-repair-recovery/README.md) `c5c3f13` | unittest traceback locals; **reverted** | 5/10 overall; lost contrast and paraphrased |
| candidate 1 | checkpoint probe protocol | never reached a coding gate; failed G0 |
| candidate 2 | bounded repair history | 2/8 on the Python family; failed G1 |

v2, v3 and v4 are recorded in the preceding campaign's own words: each "shuffled
which retraction variant passed without raising the total." v4 fed the model the
actual failing loop-variable values — the most direct evidence intervention yet
attempted — and was the worst of the three at 5/10. It was reverted.

Candidate 2 is the fourth and fifth data points. All five changed how the host
presents evidence or how much history it retains. None raised the count.

## What candidate 2's batch actually measured

Twelve of eighteen scheduled runs executed before the campaign process was
terminated; `run_gate` refuses to resume a started gate, so the batch is closed
at 6 passed, 6 failed, 6 missing. Details and the per-manifest table are in the
[campaign record](README.md).

Verified mechanism findings, each reproduced directly from retained artifacts:

**The action wall is where every Python failure ended.** All six failures reached
exactly 32 of 32 actions and consumed 260,849–261,982 of the 262,144 cumulative
input tokens — between 99.5% and 99.94%. Both walls arrive together because the
transcript is re-sent every turn; the action counter tripped first in all six.

**Bounded history activated and did not repair anything.** Compaction first
omitted segments only at turns 23–29, when the rendered prompt had already grown
to 12.5k–14.2k against a 13.0k–14.3k adaptive budget. It also cost prefix reuse:
in `renamed-s42-r002` cached tokens collapsed from 10,395 at turn 22 to 1,088 at
turn 23 and 228 by turn 31, with 112,206 tokens and 44.4 seconds — 16.5% of the
run — spent re-prefilling. That is a measured regression introduced by this
candidate, not a benefit.

**The stale-diagnostic hypothesis is refuted as a cause.** The "already assessed"
feedback does omit the failing assertion, and it fired in three of six failures.
But bounded repair re-pins the last real failure into the working-state message
on every turn: in `renamed-s42-r001` the full `AssertionError: {'cash': 7} !=
{'cash': 0}` block is present in all 26 context artifacts from turn 7 to turn 32,
and the model quoted it back after receiving the stale notice. Diagnostic loss did
not cause these failures. This removes the change that three of the five prior
experiments were variations of.

**Rejected `final` calls cost actions but not validations.** Between one and four
per failing run. They do not trigger a validation: `candidate_validate`
short-circuits when the workspace is unchanged, so all such checkpoints record
`commands: 0`. The action cost is real; the validation cost is not.

**The no-op `apply_patch` is the one thing that separates passes from failures
inside the Python family.** Counting `apply_patch` calls whose `old_text` and
`new_text` are byte-identical:

| Python run | Outcome | No-op patches | Forced openers / turn |
| --- | --- | --- | --- |
| `distractor-r001` | passed | 0 | 0.44 |
| `original-r001` | passed | 1 | 0.44 |
| `renamed-r001` | failed | 1 | 0.31 |
| `original-r002` | failed | 2 | 0.34 |
| `renamed-r002` | failed | 3 | 0.50 |
| `paraphrased-r002` | failed | 5 | 0.50 |
| `paraphrased-r001` | failed | 6 | 0.59 |
| `distractor-r002` | failed | 10 | 0.31 |

In these calls the model's prose states the correct repair and its replacement
text is a verbatim copy of the text it replaces. `distractor-r002` did this ten
times across 32 actions while stating the right fix eight times, then blamed the
tool and stopped repairing.

**Forced tool-call openers do not discriminate.** The `min(256, max_tokens/2)`
cap in `fg_native_force_due` injects `<tool_call>` mid-sentence after 256 tokens
of prose. Every Python run is heavily affected (0.31–0.59 per turn) and every Go
run is barely affected (0.00–0.04), but the two Python passes sit at 0.44, inside
the failures' range. Forced opening tracks the task family, not the outcome. It
remains a plausible background condition for the copy degeneracy and cannot be
separated with two passing samples — and the plan forbids acting on it anyway:
"Do not raise budgets or change thinking defaults to turn a failing gate green."

**Two host defects worth fixing on their own merits, neither a pass-rate lever.**
In `renamed-s42-r001` the model wrote three scratch test scripts and the
validation planner swept them into the authoritative `broad_tests` argv, so its
own reassuring output was prepended to the host verdict it read back. Separately,
the emitted `agent_mode` event advertises `"recovery": true` and
`"corrective_prompts": true` while `--failure-reflection` is not in the profile;
`loop_warnings` is 0 in all twelve runs and no reflection ever fired.

## Review of the three prepared candidate-3 patches

All three were reviewed against the current tree. **None should be applied as
written.** Each carries at least one high-severity defect:

- `agent-record.patch` — two of its three new integration tests are dead on
  Windows: they are the first bounded-repair tests to use a multi-line `old_text`
  against CRLF fixtures, so `apply_patch` can never match. It also retains "All
  applicable validation commands passed for the recorded inputs" on a path where
  no command ran, which the plan forbids outright, and drops `SEMANTIC_LOOP`
  evidence from persistent memory. Its accompanying review attests a test file
  that is not the one on disk. Its diff is also CRLF against an LF tree and does
  not apply.
- `cache-fix.patch` — sets `sys.pycache_prefix` to an empty directory *before*
  importing `unittest`/`pytest`, so the standard library is recompiled from
  source on every validation command inside a 120-second cap, never timed. Its
  pytest half was never executed (`"pytest": "not run: package unavailable"`)
  while its user-visible `limitations` string asserts the bypass works.
- `acceptance-policy.patch` — adds a top-level report key without bumping the
  report schema version, which breaks `close_candidate.py`'s stored-versus-
  recomputed equality and makes candidate 1's sealed archive non-reproducible.
  Its raw-archive exclusion filter matches bare filenames anywhere in the tree.

The separately considered idea of removing `final` from the ordinary repair
schema is **rejected on mechanics**: `validate_native_tools` in
`src/inference/chat_template.cpp` throws for any registry lacking `final` unless
it is exactly `[validate_candidate]` or `[reflect_failure]`, so a five-tool
no-final registry would abort while rendering the first prompt. Eleven of twelve
ordinary rejected finals also had `novel=false`, so the substituted action would
have been a validation on a frozen workspace — the same wasted action.

## What the target now requires

G1 requires 18/18, of which twelve runs are the Python retraction family.
Candidate 2 measured that family at 2/8. Treating repetitions as independent
Bernoulli trials at the observed rate:

| Per-run Python pass rate | P(12 consecutive passes) |
| --- | --- |
| 0.25 (observed) | 0.00000006 |
| 0.90 | 0.28 |
| 0.95 | 0.54 |
| 0.98 | 0.78 |

The observed rate has a wide confidence interval at n=8, so the point estimate is
not precise. The structural point does not depend on precision: G1 as written
requires near-deterministic behaviour from a temperature-0.6 sampler, and G6 then
repeats G1–G5 across four seed blocks. The variance is not seed-controllable —
`--seed 42` is byte-identical across repetitions, yet `distractor` and `original`
each passed at r001 and failed at r002.

The failure is a reasoning gap, not a plumbing gap. The oracle changes one line:
`if True:` becomes `if event['id'] not in cancelled:`, with
`totals.setdefault(account, 0)` left above it. The model must notice that a
posting arriving after its own retraction still creates a zero-valued account
entry while not adding its amount. Five of six failures deleted the hook and left
the credit unconditional. The sixth wrote the guard but placed it above the
`setdefault`, destroying the zero entry — one line from passing, reached at
action 28 of 32.

## Conclusion and options

**On this evidence I do not expect any host-side change to move the Python
retraction family from 2/8 to 12/12.** Five attempts have now been measured; the
most direct evidence intervention was the worst and was reverted; and the
hypothesis that motivated the newest one is refuted by its own trace.

The plan's rules point to a decision that is not mine to make unilaterally,
because each option changes the shape of the work:

1. **Fix the demonstrated defects without predicting a pass-rate gain.** The
   corrected applied-delta record, the empty-output annotation, the informative
   identical-patch rejection, the scratch-file contamination of validation, the
   false `agent_mode` capability claims, and the compaction-versus-prefix-reuse
   regression are all real and all worth repairing. None is a G1 lever, and they
   should not be sold as one.
2. **Change the profile or the model.** G1's stochastic loop-pilot profile is the
   binding difficulty. This is the option the evidence most supports and the one
   the plan does not currently authorise.
3. **Test L3's best-of-N under identical total budgets**, which the plan already
   marks experimental.
4. **Record that this model on these manifests does not support G1 at the
   required denominator.** The plan provides this shape of honest exit at L4:
   "If no saving is shown, retain the broad policy and report that result."

What should not happen is a sixth round of host evidence rewording presented as
progress. The target itself stays where it is; the plan is explicit that the
response to repeated failure is a different hypothesis, not a relaxed gate.
