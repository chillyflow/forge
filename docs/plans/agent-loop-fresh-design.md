# Forge agent-loop fresh design

Status: planned, September 9, 2026. This document plans the next campaign after
[the all-green repair plan](agent-loop-all-green.md) stalled at G1. It starts no
model runs and changes no code. It is written to be executed from a fresh session
with no prior conversation context.

Read [the all-green plan](agent-loop-all-green.md) first for the acceptance
ladder, populations, profiles and evidence rules, which remain in force. Read
[the design review](../../benchmark/results/2026-09-09-agent-loop-all-green/DESIGN_REVIEW.md)
second. This document exists because that plan's L5 rule fired: "Three
unproductive experiments trigger a design review and a different hypothesis, not
relaxation of the target."

Every number below was recomputed from retained artifacts. Where a claim is
correlational or unverified it says so.

## Why a fresh design

Five host-side changes have been attempted against the retraction task family.
Their populations and profiles differ and **must not be read as one trend**.

| Attempt | Change class | Result, with its own population and profile |
| --- | --- | --- |
| repair-recovery v2 `c821b5e` | failed states, early runner guidance | 7/10 over the ten repair-validation-v1 retraction **and window** manifests, 1 rep, temp 0, 16 turns; lost paraphrased, gained renamed |
| repair-recovery v3 `7660bf1` | immediate revalidation, hunk bounds | 7/10, same population and profile; lost paraphrased, gained distractor |
| repair-recovery v4 `c5c3f13` | unittest traceback locals; **reverted** | 5/10, same population and profile; lost contrast and paraphrased |
| all-green candidate 1 | checkpoint probe protocol | **no model evidence**; failed G0, never reached a coding gate |
| all-green candidate 2 | bounded repair history | 2/8 over the four G1 Python retraction manifests, 2 of 3 reps executed, temp 0.6, 32 turns |

Three of these were measured on a ten-manifest set that includes five
`generalize_window_*` manifests from a different family and `contrast`, which is
not in G1 at all. One produced no model runs. Only candidate 2 was measured on
the G1 population. The L5 trigger is met by v2, v3 and v4 alone; candidate 2
confirms it on the gate's own population.

What they share is their **class**: every one changed how the host presents
evidence to the model, or how much history it retains. A sixth variation of that
class is not a different hypothesis.

## Carried findings

Established from retained evidence under
`benchmark/results/2026-09-09-agent-loop-all-green/`. Do not re-derive these.

**The stale-diagnostic hypothesis is refuted.** Bounded repair re-pins the last
real failing assertion into the working-state message every turn. In
`renamed-s42-r001` the full `AssertionError` block appears in all 26 context
artifacts from turn 7 to turn 32, and the model quoted it back after receiving
the "already assessed" notice. Diagnostic loss did not cause these failures.

**Every Python failure ended at the action wall.** All six reached exactly 32 of
32 actions and consumed 260,544–261,982 of the 262,144 cumulative input tokens
(99.39%–99.94%). Both walls arrive together because the transcript is re-sent
each turn; the action counter tripped first in all six.

**The anchoring half of `apply_patch` does not fail.** Across all twelve executed
runs, **0 of 73** `apply_patch` calls were rejected with "old_text must match
exactly once". Tree-wide the rate is about 2%. Whatever is broken, it is not the
model's ability to quote the current source.

**The failing half is `new_text` generation.** Counting `apply_patch` calls whose
`new_text` is byte-identical to `old_text`:

| Python run | Outcome | Identical-replacement calls / all `apply_patch` |
| --- | --- | --- |
| `distractor-r001` | passed | 0 / 6 |
| `original-r001` | passed | 1 / 8 |
| `renamed-r001` | failed | 1 / 7 |
| `original-r002` | failed | 2 / 6 |
| `renamed-r002` | failed | 3 / 10 |
| `paraphrased-r002` | failed | 5 / 8 |
| `paraphrased-r001` | failed | 6 / 8 |
| `distractor-r002` | failed | 10 / 14 |

Every one of these 28 calls is a complete, correctly-escaped, whole-function span
of 21–26 lines and 720–993 bytes whose replacement equals the original byte for
byte. In `distractor-r002` all ten are byte-identical to each other. Nothing is
truncated and nothing is mis-escaped — they matched the file exactly. The model
is not failing to quote; **it is failing to change.**

This is the strongest signal available, but with only two passing runs no measure
can be established as discriminating. Rejected `final` calls separate almost as
well (passes 0 and 1; failures 1, 2, 3, 3, 4, 4). Treat both as
hypothesis-generating, not as established discriminators.

**Bounded compaction damages prefix reuse.** In `renamed-s42-r002` cached tokens
alternated between roughly 10.3–10.9k and 1.1k from turn 23 to turn 30 before
falling to 228 at turn 31, with 112,206 tokens and 44.4 seconds — 16.5% of the
run — spent re-prefilling. The oscillation, not a single collapse, is the
diagnostic fact: it points at a segment whose position changes every other turn.

**Two host defects, neither a pass-rate lever.** In `renamed-s42-r001` the model
authored three scratch test scripts; all three reached the validation planner's
`compile` stage and two reached `broad_tests`, prepending the model's own
reassuring output to the host verdict it read back. Separately, `agent_mode`
advertises `"recovery": true` and `"corrective_prompts": true` while
`--failure-reflection` is not in the profile; `loop_warnings` is 0 in all twelve
runs.

## Rejected before implementation: the edit-channel hypothesis

An earlier draft of this plan proposed exposing the existing `apply_hunk`
line-range editor to the candidate registry, on the theory that exact-span
quotation was the bottleneck. **It was investigated and refuted before any code
was written.** It is recorded here so it is not proposed again.

- The half `apply_hunk` removes is the half that never fails: 0 anchor
  rejections in 73 calls.
- `apply_hunk` cannot express the repair. `src/tools/tools.c:1487` hard-rejects
  any middle-of-file hunk whose replacement line count differs from the selected
  count, and `native_protocol` at line 1441 is computed **without** the
  `!minimal_agent` exclusion that `run()` uses at line 1551, so the guard applies
  to this loop. The dominant observed repair shape changes the line count.
- It has already been run by this model. Across the retained results tree,
  roughly 77–85 `apply_hunk` calls: about 68% accepted, 12.9% rejected by the
  line-count guard, and it still produces identical-span no-ops. On the
  retraction family specifically it is worse than its own average.
- The model does not adopt narrow edits when offered them: about 1 of 77 retained
  hunks is single-line and the median span is 18 lines, so `apply_hunk`
  degenerates into "resend the whole function without the old copy" — the same
  idiom, with the same room for the same degeneracy.
- It costs tokens on a pinned every-turn segment. The six-tool candidate schema
  is 486 tokens; adding `apply_hunk` costs roughly +151 to +308 tokens per turn,
  which removes about one action from every 32-turn Python run — including the
  passing `distractor-r001`, which finished with 229 tokens of headroom.

If any anchored-edit arm is ever revisited, the only version worth testing is an
**enforced single-line** replacement as the sole write path in a declared arm,
where a no-op costs about 40 bytes instead of 750.

## The hypothesis

**H-ECHO: the failing runs are poisoned by their own retained no-op edits.**

When `apply_patch` is rejected because `new_text` equals `old_text`, the host
still retains the full action verbatim in the transcript. That rejected pair then
sits in the model's own conditioning set, where it plausibly acts as a worked
example that the correct continuation of a `new_text` field is a verbatim copy of
`old_text` — and, separately, as a 500–1000 token tax on a cumulative input
budget already 99.4% consumed.

The supporting statistic, recomputed independently from the retained
`events.jsonl` of all twelve runs:

```
P(identical | previous apply_patch applied)   =  9/36 = 25.0%
P(identical | previous apply_patch identical) = 19/25 = 76.0%
odds ratio 9.5, Fisher exact two-sided p = 0.000186
```

Per-run outcome strings, `O` applied and `N` identical-rejected:

```
distractor-r001  (pass) OOOOOO
original-r001    (pass) OOONOOOO
renamed-r001            OOOOOON
original-r002           OONNOO
renamed-r002            OONNOOOONO
paraphrased-r001        ONNNONNN
paraphrased-r002        OONNNNNO
distractor-r002         OOOONNNNNNNNNN
```

**Prior evidence that the phenomenon is not specific to bounded repair.** The
`2026-09-08-repair-control` experiment already counted this signal on the same
family at temperature 0 with 16 turns, and recorded 4, 11 and 13 identical
replacement attempts for the checkpoint, current-v3 and minimal arms
respectively. The minimal arm made the most and scored 0/12. So the degeneracy
predates the candidate-checkpoint and bounded-repair machinery and is not created
by it — which strengthens the case that it is worth attacking directly, and also
warns that removing the exemplars may not be sufficient.

**This is correlational and must be stated as such.** A model that has stopped
converging will both emit no-ops and keep emitting them; auto-catalysis through
the transcript is one explanation and "it is simply stuck" is another. The two
are separable by intervention, which is the point.

**The intervention.** For a rejected identical-replacement edit only, do not
retain the action verbatim. Replace the retained `ACTION`/`RESULT` pair with a
compact deterministic host observation that an edit was attempted on the named
path and made no change. Keep the full original in the session artifacts, as
bounded repair already does for evicted history.

**Why this is a different class.** All five prior attempts *added host-authored
evidence* to the prompt. This *removes model-authored text* from the conditioning
set. It is the only proposal considered that reduces tokens per turn, so unlike
the edit-channel arm it cannot cost an action.

**Falsification.** If H-ECHO is right, the identical-replacement rate and the
no-op-after-no-op transition rate both fall sharply. If the transition rate stays
near 76% with the exemplars removed, H-ECHO is wrong and the degeneracy is
internal to the model — a result worth having, and one that points squarely at
W4.

## Mechanical constraints and operational traps

Each of these cost real time to discover. Do not rediscover them.

**`run_gate` is not resumable and cannot run a subset.** It refuses any gate that
already has outcomes ("gate already has outcomes; no replacement or result-based
retry is permitted"), and `--gate G1` executes all 18 scheduled rows. Candidate
2's batch died at 12 of 18 and is permanently incomplete. **Never use `run-gate`
for screening**, and always launch a gate batch as a background task that
survives its shell.

**Screening records must never touch `outcomes.json`.**
`agent_loop_acceptance.report()` appends `"unscheduled run: <id>"` to `errors`
for any record not in the frozen schedule, and a non-empty `errors` list sets
`accepted: false` for **every** gate including G0, permanently. Screening runs go
through `benchmark/run.py` directly and are stored at a sibling path such as
`candidate-NN/screening/`. Note that the contract already labels G1 rows
`phase: development` with run ids prefixed `development-G1-`, so do not call
screening output "development evidence" either.

**The frozen tree must not be touched between freeze and gate.** Every `run-g0`
and `run-gate` call asserts `observe(candidate) == candidate['identity']` over
`src/`, `include/`, `cmake/`, `tests/`, `scripts/`, `.github/`, `CMakeLists.txt`,
`benchmark/*.py` and `benchmark/*.md`. Analysis scripts go under
`benchmark/results/...`, never under `benchmark/`.

**The native renderer requires `final` or `memory`.** `validate_native_tools` in
`src/inference/chat_template.cpp` throws for a registry that has neither, unless
it is exactly `[validate_candidate]` or `[reflect_failure]`. It also hardcodes
the permitted registries by name: `minimal` is exactly `read_file, apply_patch,
run_command, list_directory, final`, and `candidate` adds `validate_candidate`.
Any registry change must update it.

**Adding an opt-in flag touches more than the loop.** `include/forge/forge.h`
(a bool beside `minimal_agent`, `candidate_checkpoint`, `bounded_repair`);
`src/cli/main.c` in four places — usage text, the **zero-arity `option_arity`
table** (omitting it silently misparses the flag and can swallow the next
argument), the parse block, and the compatibility validation that currently
rejects loop interventions lacking `--candidate-checkpoint`; and
`benchmark/run.py`'s `VARIANTS`, without which the harness cannot select the arm
at all. Grammar, native parsing, argument validation and capability checks need
no work — the grammar is derived from the passed schemas and the validators are
generic over `definitions[]`.

**Bounded repair's edit memory is `apply_patch`-only.** The `last_patch_path` /
`old` / `new` tracking and the stale-`old_text` re-anchoring in `src/core/agent.c`
key on `apply_patch`. Any change to how edits are recorded must keep the
"previous attempted delta" evidence working.

## Work packages

### W0 — Calibrate the baseline. This is the centrepiece.

Owner: coordinator. No code change. **Nothing else is interpretable until this
exists.**

No comparator exists at the loop-pilot profile. The `2026-09-09-agent-loop-v1`
diagnostic ran one repetition per arm; the `2026-09-08` repair control used
temperature 0 and 16 turns. Candidate 2's 2/8 therefore cannot be compared to
anything, and we do not know whether the candidate-checkpoint machinery helps, is
neutral, or hurts on this family.

A prior does exist at the historical profile and points the other way:
`2026-09-08-candidate-checkpoint-v3` reports checkpoint 9/18 against minimal
6/18 on the same six manifests at n=3. W0 re-measures that under the stochastic
32-action profile. **The two must not be pooled.**

Run three arms on the four Python retraction manifests, three repetitions each,
using the **unchanged candidate-2 binary**, which already supports all three:

| Arm | `--variants` | Flags |
| --- | --- | --- |
| plain control | `minimal` | `--minimal-agent --thought-history` |
| checkpoint only | `candidate-checkpoint` | adds `--candidate-checkpoint` |
| candidate 2 | `loop-repair` | adds `--bounded-repair` |

Profile: temperature 0.6, seed 42, 32 actions, 16384 context, 2048 output
reserve, 32768 generated tokens, 262144 cumulative input, 600 s task, 120 s
verification. Manifests are in
`benchmark/results/2026-09-08-repair-control/tasks/`.

Record for every run, pass or fail: identical-replacement count and denominator,
rejected-final count, `run_command` count and how many were near-duplicate
reproductions, `forced_actions / turns`, cumulative prompt tokens, cached-token
series, action count and terminating reason.

Acceptance: three per-arm rates with intervals and an explicit statement of
whether the checkpoint policy and bounded repair are better than, equal to, or
worse than the plain control on this family. Every later claim that a change
"improved the loop" is measured against this number.

**W0 may well end the campaign.** If the plain control matches or beats candidate
2, the loop machinery is not the problem and W1 should not be built.

### W1 — Test H-ECHO

Owner: loop task. Depends on W0's result, not merely its existence.
Primary files: `src/core/agent.c`, plus the flag sites listed above and
`tests/integration/test_bounded_repair.py`.

Implement the H-ECHO intervention behind a new opt-in flag, leaving `minimal`,
`candidate-checkpoint` and `loop-repair` byte-identical as controls. Suppress
retention only for an edit rejected specifically because `old_text` equals
`new_text`; every other rejection keeps its current handling. The substituted
record must be a host observation of what the host did, never a hypothesis about
why, and must not name a fixture, an oracle or an expected patch.

Prespecify the instrumentation before running, or a null result is
uninterpretable: identical-replacement rate, the applied→identical and
identical→identical transition rates, tokens per turn, and actions used.

Acceptance: deterministic regressions covering a rejected identical edit that is
not retained verbatim, a normal rejection that still is, the raw action surviving
in session artifacts, and the bounded-repair "previous attempted delta" evidence
remaining correct. Full G0 must pass on the new candidate before any screen.

### W2 — Prerequisites and demonstrated defects

Owner: loop task. **Ship as its own candidate, screened separately from W1.**
Two of these act on the same mechanism W1 tests, so bundling them makes the W1
screen unattributable and violates the parent plan's "one policy change at a
time".

**Prerequisite, blocking.** The verification-cache correctness hole must be
closed. Python `-B` suppresses bytecode writes but can still *load* existing
cached code, which can produce a false pass in both the independent verifier and
the host checkpoint despite stable input hashes. The parent campaign record calls
this blocking for the next candidate. `cache-fix.patch` is **not** the fix to
apply — see below — but the hole must be closed some other way before any
screening result is trusted, because every screen depends on it.

Then, claiming nothing for them:

- Exclude `assistant_content` from the deterministic applied-delta record. The
  compressor keeps head and tail, so verbose prose eats the head and the
  **middle** is clipped, losing `old_text` and often the argument envelope while
  the tail of `new_text` survives. Keep the non-fatal path: a compression failure
  must not abort a run whose edit is already on disk.
- Annotate a `run_command` that exits zero with completely empty stdout and
  stderr. All six failures **and both passes** issued at least one such command,
  so this does not discriminate and must not be presented as if it did; whether
  the model then formed a false success belief is not established by retained
  evidence. The annotation is still correct host behaviour.
- The identical-replacement and anchor-mismatch messages are **already distinct**
  today. The addition worth making is to report whether the file already contains
  the intended `new_text`.
- Stop the validation planner from sweeping model-authored scratch scripts into
  the authoritative `broad_tests` argv.
- Stop `agent_mode` advertising `recovery` and `corrective_prompts` when those
  features are not enabled.
- Investigate the compaction prefix-reuse oscillation. Start from the alternating
  pattern, not from a single collapse. If prefix reuse cannot be preserved,
  record the measured re-prefill cost as a known cost of bounded repair.

Acceptance: a deterministic regression per item.

### W3 — Screen before spending a gate batch

Owner: coordinator.

Candidate 2 executed 12 of 18 gate runs and forfeited the rest to learn something
a smaller batch would have shown. Freeze each candidate, pass G0 in full, then
run a **screen**: the four Python manifests at three repetitions plus the two Go
manifests at one repetition — 14 runs. The Go runs are there because a loop
change affects every manifest and Go was 4/4 under candidate 2; screening Python
only would let a Go regression surface after the gate batch is already spent.

The rule below is preregistered here, before any run, and must not be revised
after seeing outcomes.

**Promotion requires both:** at least 11 of 12 Python passes, **and** at least
one pass in each of the four Python manifests, **and** no Go regression.

The manifest-level condition exists because the 12 runs are 4 manifests × 3
repetitions and candidate 2's outcomes were clustered by manifest (paraphrased
0/2, renamed 0/2, distractor 1/2, original 1/2). The effective sample size is
nearer 4 than 12, so the binomial numbers below are optimistic and the parent
plan's ban on treating overlapping suites as independent observations applies.

Operating characteristic, exact binomial at n=12, stated so the threshold is not
merely asserted. Null 0.25 (candidate 2's observed rate), alternative 0.95:

| True per-run rate | P(≥11) | P(8–10) | P(≤7) |
| --- | --- | --- | --- |
| 0.25 | 0.0000022 | 0.003 | 0.997 |
| 0.50 | 0.003 | 0.191 | 0.806 |
| 0.75 | 0.158 | 0.684 | 0.158 |
| 0.90 | 0.659 | 0.337 | 0.004 |
| 0.95 | 0.882 | 0.118 | 0.0002 |

False-promotion rate against the null is 2.2e-6 and power against 0.95 is 0.88,
so the bar separates *those two* well. It separates 0.75 or 0.90 from 0.95 poorly
— likelihood ratios of about 5.6 and 1.3 — and that is the comparison that
actually governs the decision. Act accordingly:

| Screen result | Preregistered action |
| --- | --- |
| ≥11/12 with all four manifests represented, no Go regression | Attempt the full G1 gate batch on this frozen candidate |
| 8–10/12 | Real improvement by default, but not a certainty: a true 0.90 candidate lands here 34% of the time and has a defensible 28% chance at G1's Python subset. Record the lift; one G1 attempt is permitted if the manifest condition is met, otherwise escalate to W4 |
| ≤7/12 with at least a doubling over W0's baseline | **Inconclusive.** A true 0.75 candidate lands here about 16% of the time and would otherwise be wrongly discarded while re-runs are forbidden. Record it, keep the candidate, and do not claim it failed |
| ≤7/12 with no material lift over W0's baseline | Not a lever. Record it, keep any defect fixes, escalate to W4 |

The bar is not a prediction of success. G1 requires **18/18**, not twelve Python
passes: at 0.95 throughout, P(G1) = 0.95^18 = 0.40, and G6 repeats the population
across four seed blocks for 0.95^36 = 0.16. Screening exists to avoid burning
gate batches and to measure direction.

**Any G1 attempt must report how many screens on how many candidates preceded
it**, and a passing screen followed by a G1 pass is two draws from the same
population, not a confirmation.

### W4 — Escalation when the loop is not the ceiling

Owner: coordinator. **These change the shape of the deliverable and need an
explicit decision before execution. No W4 arm may be substituted for a G1
outcome; G1's profile stays fixed and a W4 result is a separate declared arm.**

1. **Profile.** Measuring this family at temperature 0 under the *same 32-action
   budget* separates sampler variance from capability. Note the contradicting
   prior: `2026-09-08-repair-control` already measured this family at temperature
   0 and got 3/12 for current Forge and 0/12 for the minimal control — not better
   than 2/8 at temperature 0.6. The confound is the 16- versus 32-action budget,
   which is exactly what this arm would remove. The design review's conclusion
   ("a reasoning gap, not a plumbing gap"; the variance is not seed-controllable)
   points away from sampler variance, so this arm is expected to *confirm* the
   ceiling rather than lift it.
2. **Model.** A different or larger local model on identical manifests and
   profile. Changes what the campaign can claim about the deployable
   configuration; record as a separate arm, never merged.
3. **L3 best-of-N** under identical total budgets, which the parent plan already
   marks experimental and which must not be given extra budget to win.
4. **Record the limit.** State that this model on these manifests does not
   support G1 at the required denominator, and report it. The parent plan
   provides this shape of honest exit at L4.

## Operations

Ordering, which the campaign tooling enforces: **write tests → build → freeze →
`run-g0` → screen → `run-gate`**, with no edits to the identity-covered paths in
between.

Screening and W0 batches go through `benchmark/run.py` directly, one invocation
per manifest, in the shape recorded in `candidate-02/runs/*/command.json`:
`--forge <frozen binary> --model <gguf> --task-dir
benchmark/results/2026-09-08-repair-control/tasks --tasks <id> --variants <arm>
--repetitions 1 --no-randomize --retain-terminal --output <fresh dir>` plus the
profile flags. Note that `run.py` ignores `--order-seed` under `--no-randomize`,
so W0's ordering will not match G1's shuffled order; that is acceptable for a
control measurement but must be recorded rather than claimed as identical.
`run.py` errors on an existing `--output` directory; `benchmark/run_missing.py`
exists for resumption.

Launch every batch as a background task with a log path, and poll the log — not
as a foreground shell call, which dies with its session and permanently burns
started run identifiers.

Budget the time before starting. Candidate 2 averaged about 200 seconds per run
plus verification. W0 as specified is 36 runs, roughly 2–3 hours; each screen is
14 runs, roughly 50 minutes; a G1 gate batch is 18 runs, roughly 1–1.5 hours.

Metric definitions, so two sessions compute the same number:

- **Identical-replacement count**: `tool_call` events with `data.tool ==
  "apply_patch"` and non-empty `data.args.old_text == data.args.new_text`, read
  from `<run>/harness/<task>-<variant>-r00N/session/events.jsonl`. This
  reproduces every row of the table above. `mechanism-analysis.json` does **not**
  — it was generated mid-batch and holds only 9 of 12 runs.
- **Forced-opener rate**: `metrics.forced_actions / metrics.turns`.
- Always report a count with its denominator; the raw counts have very different
  denominators.

Build and probe commands:

```powershell
$env:PYTHONPATH="$PWD/.tools"
$env:PATH="$PWD/.tools/go/bin;$env:PATH"
.tools/bin/cmake.exe --build build-gpu --config Release --parallel
.tools/bin/ctest.exe --test-dir build-gpu -C Release --output-on-failure
build-gpu/Release/forge_chat_template_unit.exe C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1
build-gpu/Release/forge_checkpoint_model.exe C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
build-gpu/Release/forge_checkpoint_model.exe --automatic C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
```

Re-run `scripts/configure-serena.ps1` after CMake target, source-list or
toolchain changes, as `AGENTS.md` requires.

## Evidence rules

The parent plan's "Evidence and completion" section applies. In addition:

- Every code change creates a **new candidate** with its own result directory,
  frozen source/runtime/model hashes, and a full G0 pass before any model runs.
  Earlier candidates, including failures and interrupted batches, stay immutable.
- Screens are never substituted for a gate, never pooled with gate outcomes, and
  never re-run to find a better repetition.
- Record the instrumentation set named in W0 for every run in every batch. A
  batch without it is not interpretable.
- Do not recompute candidate 1's or candidate 2's reports under a changed
  reporter. If the reporter changes, bump its schema version and make the closing
  tooling schema-aware.

## What not to do

**Do not raise any budget or change thinking defaults.** Not `--max-turns` past
32, not `--max-input` past 262,144, not the output reserve, not the temperature
within a profile, and not the `max(1, min(256, max_tokens/2))` forced-opener cap
in `fg_native_force_due`. That cap correlates with the Python *family* — every
Python run is heavily affected and every Go run is barely affected — but **not
with the outcome**: both passing Python runs sit inside the failures' range. It
may be measured as a separate declared W4 arm; it may not be quietly raised.

**Do not weaken the final gate.** In `paraphrased-r002` it blocked four separate
false completion claims, each asserting behaviour the workspace did not have.
That it cost four actions is the gate working.

**Do not change the six G1 manifests.** The protected test loops three event
lists over a single `assertEqual` with no `subTest`, so no traceback can name the
failing ordering. Adding `subTest` would help the model and would be changing the
exam.

**Do not apply the three prepared candidate-3 patches as written.** Each carries
a high-severity defect documented in the design review: two integration tests
that are dead on Windows, a synthesized "validation commands passed" claim on a
path where no command ran, the standard library recompiled inside a 120-second
cap, and a report key added without a schema bump that breaks a sealed archive.
Salvage the individual repairs listed in W2.

**Do not attempt to remove `final` from the ordinary repair schema**, and do not
re-propose the `apply_hunk` arm. Both are refuted above.

**Do not sum arms or pool overlapping suites.**

## Stopping rule and completion

The parent plan's three-unproductive-experiments counter has already fired once.
**This plan permits at most two screened candidates** — one for W1 and one for
W2 — before W4 must be chosen and executed. If W0 shows the plain control
matching or beating candidate 2, W1 is not built and W4 fires immediately.

This plan is complete when W0 has produced a calibrated baseline, W1 and W2 have
been screened under W3's preregistered rule or explicitly not built, and either a
G1 gate batch has been attempted on a candidate that met the promotion bar, or
W4's chosen axis has been executed and reported.

It is explicitly acceptable for this plan to conclude that the loop is not the
ceiling. That outcome must be reported with its evidence rather than absorbed
into another round of changes.
