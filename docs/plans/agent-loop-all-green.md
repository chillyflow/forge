# Forge agent-loop repair plan

Status: planned, September 9, 2026. This document plans the next implementation
campaign; it does not start model runs or change the loop.

The objective is one frozen Forge configuration that passes every required
implementation check and every scheduled run of the known coding suites. Keep
iterating on demonstrated failure mechanisms until that gate passes. A passing
unit suite, one successful seed, or the union of different candidates' wins does
not satisfy it. Future unseen tasks cannot be guaranteed to pass.

## Starting evidence and scope

The [latest diagnostic](../../benchmark/results/2026-09-09-agent-loop-v1/README.md)
completed 42 real-model runs. Minimal passed 4/6; combined, semantic and reflection
passed 3/6; checkpoint-only, best-of-two and impact passed 2/6. The accepted GPU
suite passed 31 checks with one opt-in model test skipped.

The immediate problems are:

- Checkpoint-only hit the pinned-context limit on all four failures; semantic
  did so on all three failures. `minimal_append` currently pins every appended
  history segment. This is a concrete pressure mechanism to fix and measure,
  not proof that compaction alone will repair the programs.
- All 23 failed root workspaces and all 15 failed child trials failed secondary
  verification. The measured failures require better repairs; none can be fixed
  simply by relabelling an unfinished run as success.
- Reflection activated, but one bounded response never produced a complete
  native call. Repeated-state warnings also activated without reliable recovery.
- Two candidates shared the original budget and often both failed. More
  trajectories have not yet justified their cost or allocation policy.
- All 49 impact plans fell back. Narrowed validation savings were not measured;
  automatic validation was a small part of this pilot's total time.

"All tests" means all maintained unit/integration checks, the applicable real
model probes, and every run in the acceptance ladder below. Model success is
required for the selected deployable policy, not for intentionally weaker
diagnostic controls. Every supported feature still needs its contract tests.
The existing single-candidate restriction on user questions remains explicit.
Resolved cross-language intelligence and other design release features remain in
[ROADMAP.md](../ROADMAP.md); they are not prerequisites invented for this repair.

## Acceptance ladder

Each gate uses one frozen source/runtime/configuration and complete scheduled
denominators. If a code or prompt change follows a failure, it creates a new
candidate; earlier gates must pass on that candidate before acceptance. Results
from overlapping suites are not independent observations and must not be summed
as a larger sample. Exact duplicate executions may be referenced across reports
only when manifest hashes, configuration, seed and execution identity match.

| Gate | Population | Required result |
| --- | --- | --- |
| G0: implementation | Full GPU CTest, new deterministic regressions, native-template and checkpoint-model probes | All applicable checks pass; execute the currently skipped model probe directly with the local GGUF |
| G1: current failures | Six unchanged manifests in `benchmark/results/2026-09-08-repair-control/tasks`, three repetitions | 18/18 independently verified completions |
| G2: variation preservation | Ten unchanged retraction/window manifests in `benchmark/results/2026-09-08-repair-validation-v1/tasks`, three repetitions | 30/30, including contrasts and all rolling-window variants |
| G3: existing gates | Four regression tasks and 20 invariant tasks from the reliability campaign, three repetitions | 12/12 and 60/60 on the current candidate; zero terminal loops or syntax-broken terminal workspaces |
| G4: full known development | All 29 manifests in `benchmark/tasks`, three repetitions | 87/87; the older 80/87 threshold is only an intermediate milestone |
| G5: examined former holdout | All 12 manifests in `benchmark/holdout/2026-09-02/tasks`, three repetitions | 36/36; these examined fixtures now supply regression evidence, not a fresh holdout |
| G6: confirmation | Freeze the final candidate before execution; rerun G1-G5 as a scheduled confirmation campaign | Every scheduled outcome passes with intact evidence; no configuration selection after outcomes |

Every coding pass requires successful agent completion, unchanged protected
files, complete independent test execution and a passing terminal workspace.
No-op success, missing tests, incomplete evidence, exceeded limits, crashes,
timeouts and protected-file changes are non-passing. Separately report passing
terminal inputs without completion; never substitute them for primary success.

G1 development diagnosis uses the existing loop-pilot profile: local Qwen3-Coder
30B-A3B Q4_K_M, native embedded template and thought history, temperature 0.6,
seed 42, 32 actions, 16384 context, 2048 output reserve, 32768 generated tokens,
262144 cumulative input tokens, 600-second task limit and 120-second independent
verification limit. Use order seed 20260831 and three cold repetitions. Preserve
the candidate seed rule and shared total budgets when comparing best-of-N.

Historical preservation must also be rerun under its original 16-action,
temperature-zero profile and recorded limits; a 32-action pass cannot replace
that evidence. L0 records the exact profiles and memberships from the original
protocols before execution. Other shared settings remain fixed. Best-of-N stays
out of a zero-temperature profile; the deployable policy must have an explicit
single-candidate path there. Record both profiles separately, including in G6.
Do not raise budgets or change thinking defaults to turn a failing gate green.

G6 uses a second preregistered sampling seed block for stochastic profiles as well
as the original block: three runs at base seed 42, then one each at base seeds
43, 44 and 45 per manifest. L0 records that schedule before coding starts.
Deterministic profiles retain their original three repetitions and settings.
This is repeated development confirmation, not a
claim that any fixed number of seeds proves universal reliability.

## Work packages and dependency order

### L0 — Freeze acceptance and classify failures

Owner: coordinator with an evidence task. Depends on no code change.

Create a machine-readable suite/arm/seed/budget manifest and an acceptance reporter
covering G0-G6. Resolve exact historical regression/invariant membership, fixture
hashes and profiles rather than reconstructing sets from favorable outcomes.
Retain current minimal and candidate baselines, all prior archives, and the
working-tree source snapshot. Use a separately named result directory per new
candidate and record dirty-source provenance honestly.

For each of the 23 failed roots and 15 failed trials, extract the last changed
candidate, latest complete diagnostics, action and prompt growth, remaining
budgets, tool calls and terminal reason. Distinguish context exhaustion,
incomplete native calls, ineffective logic changes, repetition and budget
allocation. Inspect existing evidence before spending another GPU run.

Acceptance: every known failure has an evidence link and a falsifiable mechanism
hypothesis; the reporter rejects a missing or duplicated scheduled run, mutation
of protected files, swapped runtime, or success borrowed from another candidate.

### L1 — Fit repair history and reserve completion capacity

Owner: context task. Depends on L0's pressure traces.
Primary files: `src/core/agent.c`, `src/context/`, and native conversation tests.

Add bounded history for the candidate policy while preserving the original
minimal control. Retain the task, permissions, current input/validation identities,
latest failure and current source evidence. Evict or compact complete older
assistant/tool exchanges without orphaned tool replies or stale source claims.
Keep raw history in session artifacts. Deterministic host summaries must separate
observations from model hypotheses; they must never invent a passing verdict.

Budget the rendered native prompt, tool schemas and output before generation.
Reserve actual input/output/action capacity for validation feedback and a complete
final call, not only a final action number. A tiny reflection/final registry must
fit without carrying the full edit-tool schema or all historical failures.
Instrument bytes and tokens by segment type so the pressure hypothesis is testable.

Acceptance: replayed long-history failures reach the next repair or final action
within the unchanged limits; current evidence survives compaction; stale validation
is invalidated; native parsed calls remain correct with cold and reused prefixes.
Impossible mandatory input remains an explicit limit error, never silent task loss.

### L2 — Make validation and recovery drive the next repair

Owner: repair task. Depends on L0; integrates after L1's context contract.
Primary files: `src/core/agent.c`, `src/core/verification.c`,
`src/core/semantic_state.c`, `tests/integration/test_loop_extensions.py`.

Represent the repair cycle explicitly: changed candidate, complete validation,
active failure episode, bounded diagnosis, next candidate, and verified completion.
The existing checkpoint already validates candidates and reserves final actions;
improve its evidence delivery and transitions rather than adding a second gate.

Give recovery a compact host evidence record: failing command/assertion, implicated
locations, relevant current source, previous attempted delta and remaining budget.
Request an explanation of the first incorrect operation and a concrete repair
hypothesis using those inputs. Keep unknowns explicit. No fixture names, expected
patches, hidden oracle code or task-specific diagnosis belong in runtime guidance.

Reads, rephrasing and comment edits must not count as successful recovery. Use
canonical failure evidence to direct the next bounded inspection/edit, retaining
legitimate multi-file repairs and intermediate reverts. A different diagnostic is
information, not automatically progress; fewer reported failures can reflect less
test coverage. Preserve passing candidates with journaled snapshots where relevant.

Malformed or exhausted reflection consumes its allowed attempt, records the
failure and returns to ordinary repair when budget permits. It must not grant
unbounded retries or destroy a usable candidate. Final remains host-gated and
requires an actual complete model final. Any new write invalidates prior success.

Acceptance: deterministic regressions for repeated comment changes, distinct
failure operands, multi-file repairs, malformed reflection, validation mutations,
stale passing evidence, and completion under prompt pressure. Then improve G1
without losing the two Go passes or previously solved Python variants.

### L3 — Allocate candidate search according to evidence

Owner: search task. Depends on L1 and L2.
Primary files: `src/core/candidate_search.c`, `src/core/candidate_store.c`.

First measure the repaired single-candidate policy. Compare it with the existing
equal-split best-of-two under identical total limits. Only then test an adaptive
allocation: retain enough capacity for a useful first repair, start another
independent trajectory only when remaining actions/tokens/time cover generation,
real-workspace selection, final verification and guarded restoration.

Keep exact baseline copies, varied seeds, real-workspace validation, deterministic
tie-breaking, and protected-file checks. Do not rank a smaller incorrect patch
above a verified candidate. Do not auto-promote a child that never completed;
reserve a bounded completion opportunity before selection instead. Keep counts
and all losing trajectories in the evidence. User questions remain unsupported
with multiple candidates until a separate shared-clarification design is accepted.

Acceptance: cancellation, external mutation, failed apply/restore and shared-budget
contracts remain green. Enable multi-candidate search in the selected profile only
if it preserves all current passes and supplies measured benefit. A successful
single-candidate policy can satisfy this plan while best-of-N remains experimental.

### L4 — Reduce demonstrated validation cost

Owner: impact task. Depends on passing repair gates; not on the critical path to
the first correctness improvement.
Primary files: `src/repo/impact.c`, `src/repo/validation.c`.

Measure indexing, snapshots, executor time and duplicate validations separately.
Use unchanged input/command/toolchain identities before considering evidence reuse;
never treat a different plan or incomplete command output as the same check.
Keep authoritative broad final verification.

Exercise Go narrowing on a fixture where declaration/test relationships really
permit it, including reverse dependencies. Investigate trial-index fallback and
method/type relationships independently. Unknown relations must retain broad
checks. Do not claim resolved coverage or remove conservative safeguards merely
to make a targeted-plan counter nonzero.

Acceptance: the same broken behavior is detected, final checks remain broad,
correctness gates are preserved, and measured validation cost falls. If no saving
is shown, retain the broad policy and report that result; speed work must not hold
correctness hostage or weaken its gate.

### L5 — Integrate, verify, and select the simplest passing policy

Owner: coordinator. Depends on L1-L3; L4 may follow later.

Run applicable deterministic checks, then the six-case diagnostic. Keep the
original minimal control and checkpoint-only baseline as controls; add one policy
change at a time. Do not repeat the full seven-arm matrix after every edit.
Use new experiments when changing an arm, budget or protocol.

Choose one configuration before each gate series. Advance through G1-G5 only
after the preceding gate passes. Any failure returns to its owning work package,
produces a new candidate and repeats the affected checks plus preservation gates.
Do not run until a favorable repetition appears. After every failed batch, record
what changed, whether the mechanism activated, which previously passing inputs
regressed, and the next testable hypothesis. Three unproductive experiments trigger
a design review and a different hypothesis, not relaxation of the target.

Before G6, freeze a reproducible final source revision/runtime and the selected
profile mapping. Do not silently include unrelated dirty changes or claim a clean
revision for a source snapshot. Any G6 failure leaves the plan incomplete; repair
it and start a newly identified confirmation campaign. Update the guide and
roadmap only with observed results. Defaults change only after the gates pass.

## Orchestration rules

L0 prepares the common evidence contract. L1 and L2 can then run as separate tasks
against disjoint helpers/tests, with the coordinator owning shared `agent.c`
integration, public headers, schemas and build files. L3 starts after those
contracts settle. An evidence task can analyze completed runs and prepare the
gate reporter alongside code work. L4 runs only when useful profiling evidence
exists. Persistent chat/ordinary-agent tests run throughout to catch integration
regressions even when unattended repair policies are being tuned.

Limit implementation concurrency to the coordinator plus three bounded tasks.
Serialize edits to shared files, builds and GPU inference; do not let competing
model runs distort latency or exhaust VRAM. Each handoff includes source identity,
owned files, regression tests, retained failures and acceptance status. These are
planned implementation work packages, not Forge runtime multi-agent support.

## Local verification commands

```powershell
$env:PYTHONPATH="$PWD/.tools"
$env:PATH="$PWD/.tools/go/bin;$env:PATH"
.tools/bin/cmake.exe --build build-gpu --config Release --parallel
.tools/bin/ctest.exe --test-dir build-gpu -C Release --output-on-failure
build-gpu/Release/forge_chat_template_unit.exe C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1
build-gpu/Release/forge_checkpoint_model.exe C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
build-gpu/Release/forge_checkpoint_model.exe --automatic C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
```

A real unsupported backend is recorded as unsupported, never called a pass or
hidden as a skip. The local target's required supported probes must pass to close
G0. Compare parsed native calls and token bounds rather than byte-identical CUDA
prose. Re-run `scripts/configure-serena.ps1` only after relevant CMake/toolchain
changes, as required by `AGENTS.md`.

## Evidence and completion

Every candidate retains source/runtime/model hashes, complete manifests, preflight,
commands, prompts, outputs, journals, validation reports, terminal inputs, all
outcomes, token/time metrics and an inventory-verified archive. Separate model
quality, mechanism activation and executor cost. Keep the original failed
experiments immutable. Missing evidence leaves its gate open.

This plan is complete only when G0-G6 pass for the frozen supported profiles and
the report records every scheduled failure as well as every pass. Perfect scores
on examined fixtures still do not establish superiority or a release. After this
repair plan, a separately authored untouched holdout and the existing comparative
latency/task-cluster gates in the [reliability campaign](beat-opencode-reliability.md)
remain required for those claims. A holdout used for another repair becomes
development evidence; it cannot remain the fresh promotion set.
