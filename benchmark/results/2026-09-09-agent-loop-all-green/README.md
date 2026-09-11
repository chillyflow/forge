# Agent-loop all-green execution

Execution started September 9, 2026. Status: **in progress; candidate 2 passed G0
and failed G1, and G2–G6 are unexecuted**.

This campaign executes [the repair plan](../../../docs/plans/agent-loop-all-green.md).
The starting dirty working tree is retained in [starting-state/source.zip](starting-state/source.zip),
with file hashes in [source.json](starting-state/source.json), its tracked diff, and Git status.
All 128 archived source files were reopened and verified against the inventory.
Earlier diagnostic directories and archives remain unchanged.

The gate/profile mapping is frozen before runtime changes: G1 uses the 32-action,
temperature-0.6 loop-pilot profile; G2–G5 use the original 16-action,
temperature-zero historical profiles. G5 retains its original order seed 20260902;
the other gates use 20260831. G1–G5 contain 243 scheduled coding outcomes.
G6 contains 261: six stochastic runs per G1 manifest (42 three times, then 43,
44 and 45 once each), and three original deterministic repetitions per G2–G5
manifest. These 504 outcomes are separate gate populations, not independent
observations to pool. G0 implementation checks and model probes are additional.

## Starting mechanism evidence

The 11 pinned-context failures in the preceding diagnostic reached 14,348–15,214
segment tokens against a 14,336-token input capacity while retaining 2–8 actions,
24,242–26,108 generated tokens and 71,540–119,226 cumulative input tokens.
Assistant history contributes 7,610–9,378 tokens, tool history 3,034–4,561,
and accumulated candidate-state messages 1,334–1,664. All failed terminal programs
still fail independent verification. Bounded history is therefore a capacity
hypothesis, not an assumed correctness fix.

The existing native renderer pairs assistant calls with their tool results and
counts the rendered template. Its ordinary planner remains the control. The
candidate experiment will preserve mandatory current evidence and admit a
bounded suffix of older complete exchanges, with raw history retained in artifacts.
Reflection already has a dedicated small schema; malformed bounded reflection
currently terminates the run and needs an explicit recovery transition.

Defaults and release claims remain unchanged. Missing, failed, or unexecuted
outcomes leave their gate open.

## Implementation progress

The opt-in `--bounded-repair` implementation preserves both original controls.
The bounded context unit regressions, existing minimal/interactive/loop integration
checks, and 12 new bounded-repair integration cases pass. The new cases cover
native pairing and raw history, mandatory input limits, preserved user
clarifications, current source and validation identities, cumulative input/output
reserves, malformed reflection recovery, validation mutation and a single rejected
reserved final. These development checks precede candidate freeze and do not
substitute for G0. The compilation database was regenerated after CMake test
registration. Candidate 2 has since passed frozen G0; coding acceptance remains open.

[The L0 analysis](failure-analysis/README.md) classifies every one of the 23 failed
roots and 15 failed children and verifies 2,353 evidence file hashes. The
[acceptance contract](acceptance/contract.json) freezes the exact populations,
profiles, fixture identities and seed schedule before runtime changes.

Candidate 1 reached G0: 33 GPU CTest checks, 22 reporter tests and the native
model probe passed, but both direct checkpoint-model probes failed because their
legacy plain-text prompts inherited the native JSON default. G0 remains open;
no coding gate was launched. [Candidate 1's report](candidate-01/README.md) retains
the failure and the next testable correction.

Candidate 2 (`d2f92ace3d21b0ade8cdb15ee50be67af27a493bda09860368db7e175940d255`)
passed all five G0 requirements: full GPU CTest, deterministic acceptance
regressions, native-template model probe, explicit checkpoint-model probe, and
automatic checkpoint-model probe. The physical-checkpoint probes explicitly
retain their original flattened, 16-token contract; the native probe separately
checks complete calls and malformed native input classification. G1's 18-run
schedule started and its recorded result is below. G1–G6 remain open.

Read-only diagnosis during that fixed batch found two evidence-presentation hazards:
the mandatory edit delta includes historical model reasoning, and a prior failed
checkpoint retains operational wording after inputs change. A passing trajectory
actually reverted a correct edit while citing that old failure. The
[two-case comparison](candidate-02/diagnosis/g1-two-python-comparison.md) verifies
the intermediate source states; [context review](candidate-02/diagnosis/context-review.md)
defines the next isolated host-record experiment. Neither is a demonstrated quality fix.

A disposable CPU/scripted audit also demonstrated that Python `-B` suppresses
bytecode writes but can still load existing cached code. That can produce a false
pass in both the independent verifier and host checkpoint despite stable input
hashes. Fresh-source rechecks confirmed the already completed distractor pass and
renamed failure; those outcomes remain recorded. The frozen batch is unchanged;
the next candidate must close this verifier correctness hole and rerun G0.

## Candidate 2 G1 result: failed and incomplete

Candidate 2's G1 batch executed 12 of its 18 scheduled runs and was then
interrupted when the campaign process was terminated at 20:56:15 UTC, immediately
after starting `generalize_retractions_distractor-s42-r003`. That run directory
retains only `command.json` and `started.json`; it produced no outcome and is
reported as missing, not as a result. `run_gate` deliberately refuses to execute a
gate that already has outcomes, so this batch cannot be resumed or retried. The
recorded population stands at 6 passed, 6 failed and 6 missing.

G1 requires 18/18. Six recorded failures already fail it on merit; the six missing
runs are recorded as missing and are not treated as either passes or failures.

Per manifest, executed runs only:

| Manifest | Result |
| --- | --- |
| `go_api_pagination` | 2/2 passed |
| `go_multifile_transfer` | 2/2 passed |
| `generalize_retractions_original` | 1/2 passed |
| `generalize_retractions_distractor` | 1/2 passed |
| `generalize_retractions_paraphrased` | 0/2 passed |
| `generalize_retractions_renamed` | 0/2 passed |

Both Go manifests passed every executed repetition. The Python retraction family
passed 2 of 8. Every one of its four wordings failed at least once, including the
unmodified `original`, so the failures are not explained by renaming or
paraphrasing alone.

Independent CPU reproduction classifies all six failures. Each retained terminal
workspace was copied into a fresh temporary directory, its protected test file was
taken from the task manifest rather than from the run, and the suite was executed
with `python -B` so no cached bytecode could be reused. All six still fail: five
report `{'cash': 7} != {'cash': 0}` and one reports `{} != {'cash': 0}`.

- Five of six left the posting branch unguarded. They repaired retraction
  bookkeeping — duplicate suppression, a `cancelled` set, deleting matched
  postings — but a posting that arrives after its retraction still adds its
  amount, so the protected case still reports `{'cash': 7} != {'cash': 0}`.
  Two of these five changed the source without changing that behaviour at all:
  they removed the `if True:` scaffolding and de-indented the body, which is a
  textual change and a semantic no-op.
- One of six, `renamed-s42-r002`, guarded the posting branch correctly but placed
  the guard before `positions.setdefault(bucket, 0)`, so a cancelled account
  disappears instead of retaining its zero entry. Its final host diagnostic was
  `{} != {'cash': 0}`. Moving that guard down two lines makes it pass; this was
  verified directly on CPU against the unmodified protected test. It reached that
  state on action 28 of 32 and spent its reserved actions on validation and a
  rejected final, so no action remained to act on the new diagnostic.

Bounded history activated and behaved as designed but did not change the outcome.
The three retraction failures examined in detail each rendered 12.5k–14.2k tokens
against a 13.0k–14.3k input capacity, ran to exactly 32 actions, and consumed
260,849–261,941 of the 262,144 cumulative input tokens. Compaction first omitted
segments only at that ceiling; the passing runs never needed it. Capacity was
therefore reached, not spared: the mechanism activated without repairing the
programs, which is the outcome the starting evidence said it might have.

Action accounting on `renamed-s42-r002` shows where the budget went: 3 of 31 tool
calls were `validate_candidate`, 13 were `run_command`, and 11 of those 13 were
near-identical hand-written reproductions of the same protected case whose exact
assertion the host had already returned verbatim. Rejected `final` calls consumed
further actions and triggered full host validations in three of the six failures.

Candidate 2 is closed. Its evidence, including the interrupted run directory,
remains unchanged.

### Comparison with the preceding diagnostic

The [preceding diagnostic](../2026-09-09-agent-loop-v1/README.md) ran the same six
manifests once per arm under the same loop-pilot profile. Its `candidate` arm —
candidate-checkpoint without bounded repair — passed 2/6, both of them Go. Its
plain `minimal` control passed 4/6.

Candidate 2's first repetition passed 4/6: `go_api_pagination`,
`go_multifile_transfer`, `generalize_retractions_distractor` and
`generalize_retractions_original`. That is the same four manifests the plain
`minimal` control already passed. Bounded repair therefore moved the
candidate-checkpoint arm from 2/6 to parity with the control on one repetition;
it has not been shown to exceed the control.

The second repetition passed 2/6, losing `distractor` and `original`, so the
first repetition's 4/6 is not a stable level. The diagnostic's `combined` arm had
passed `generalize_retractions_renamed`, which candidate 2 failed on both of its
executed repetitions; that is a single-run difference between different arms and
is recorded as an observation, not as a regression measurement.

These are one- and two-repetition samples from overlapping suites and different
arms. They are not independent observations and must not be pooled.

## Next step: design review, not candidate 3

The repair plan's L5 rule states that "three unproductive experiments trigger a
design review and a different hypothesis, not relaxation of the target." Counting
the three host-evidence candidates the preceding campaign measured on this same
retraction family (v2, v3 and the reverted v4) plus candidates 1 and 2, that
trigger has fired. The review is in [DESIGN_REVIEW.md](DESIGN_REVIEW.md).

Its findings in short: the stale-diagnostic hypothesis that motivated the prepared
candidate-3 work is refuted by candidate 2's own traces, because bounded repair
already re-pins the last real failing assertion into every prompt. The one measure
that separates passes from failures inside the Python family is the rate of
`apply_patch` calls whose replacement text is a byte-identical copy of the text it
replaces — 0 and 1 in the two passes, 1 to 10 in the six failures. All three
prepared patches carry high-severity defects and none is applied. Removing `final`
from the ordinary repair schema is rejected on mechanics: the native renderer
requires `final` in any registry that is not exactly `[validate_candidate]` or
`[reflect_failure]`.

No runtime source, budget, gate or denominator has been changed. Candidate 3 is
not frozen and no further model runs have been started.

The successor campaign is planned in
[docs/plans/agent-loop-fresh-design.md](../../../docs/plans/agent-loop-fresh-design.md).
Its centrepiece is the baseline calibration that should have preceded all five
attempts: the plain minimal control has never been measured on this family at the
loop-pilot profile at more than one repetition, so candidate 2's 2/8 currently has
no comparator. Its hypothesis is that the failing runs are poisoned by their own
retained no-op edits, which is a different class of change from the five attempts
so far — it removes model-authored text from the conditioning set rather than
adding host-authored evidence. An edit-channel hypothesis was investigated and
refuted before implementation and is recorded there so it is not proposed again.
