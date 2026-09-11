# L3 preregistered diagnostic comparison proposal

Status: proposal only, recorded on 2026-09-09T20:24:59.983384+00:00. No source, runtime, build or GPU changes
were made for this proposal. Candidate-02 G1 outcomes were not inspected to select
tasks, repetitions, seeds, comparison membership or the rules below. This document
freezes the proposed diagnostic schedule before any proposed best-of-two outcome;
execution still requires a separate immutable diagnostic protocol and evidence
directory. It does not alter the acceptance contract or authorize an adaptive arm.

The minimum complete comparison uses the **18 already scheduled single-candidate
G1 executions plus 18 newly scheduled best-of-two root executions**: 36 root
outcomes, 18 matched task/repetition pairs, and 18 additional model processes.
The two-child arm has 36 planned child opportunities, counted separately; children
are not extra task successes or independent observations. No seven-arm replay is
needed to decide whether the existing best-of-two policy warrants further work.

## Frozen identity and preconditions

This candidate-02 schedule is an illustrative deferred comparison, not a direction
to compare an obsolete baseline after another repair. The standalone driver must
freeze the supplied later fully measured repaired-single baseline and its exact
18 outcomes before any B run; it must not inherit candidate-02 wins.

The illustrative reference is [bounded-repair-02](../candidate.json), frozen at
`2026-09-09T20:05:48.277007+00:00`, with candidate hash `d2f92ace3d21b0ade8cdb15ee50be67af27a493bda09860368db7e175940d255`.
Its source hash is `39dfdf02f4866edb1becf78247a82b022bd4987a8ebd98ce4794b5e66c00a004`; runtime hash is `2e92a3ef21cbeebbb6858b5278e2348c9d78c65d7ace565592e644e57c87cccb`; model hash is
`fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad`. The model is the existing local
`Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`. The source remains the recorded working
tree; the retained snapshot and diff must not be described as a clean revision.

The [acceptance contract](../contract.json) hash is
`c04b088b0183d5ce68c0db6e4e6ea56d33232cf4832e80fa676cd63b684577b8`.
First finish all 18 single-candidate G1 attempts and their evidence/terminal audits.
Do not launch this comparison while another model run or build is active. If the
single-candidate batch reveals an unresolved L1/L2 mechanism that requires a source
change, record that finding and defer this fixed-source comparison; any replacement
source requires a newly identified diagnostic and a newly measured single-candidate
baseline. Never mix old-source A wins with new-source B wins.

The proposed comparison directory is a separately named sibling such as
`benchmark/results/2026-09-09-agent-loop-all-green/search-compare-02/`. Do not edit
candidate-02's policy mapping, schedules, evidence or outcome records to add B.
Before B runs, record a diagnostic protocol hash, the full 36-outcome schedule,
separate arm configuration hashes, hardware/tool versions, runner source hash,
and source/runtime/model identities. Reference every A execution by its existing
execution ID and artifact hashes. Those are reused observations, not fresh runs.
If any A record lacks its complete evidence or identity, that pair is incomplete;
do not substitute a rerun. This prospective diagnostic is not an acceptance gate.

## Arms and unchanged total profile

| Arm | Existing benchmark variant | Exact flags | Root outcomes |
| --- | --- | --- | ---: |
| A: repaired single | `loop-repair` | `--minimal-agent --thought-history --candidate-checkpoint --bounded-repair` | 18 existing G1 |
| B: repaired two | `loop-repair-best-of-2` | A flags plus `--candidates 2` | 18 new |

These variants are already declared in
[benchmark/run.py](../../../../../benchmark/run.py). The sole intended intervention
is the candidate count. Keep all semantic, reflection, impact and thinking defaults
unchanged. Do not add budget, prompt, fixture-specific or allocation changes.

Both arms use base seed 42 in all three cold repetitions, temperature 0.6, native
protocol, the GGUF's embedded template, thought history, GPU index 0, GPU layers -1,
16384 context tokens, 2048 output reserve, **32 total actions, 32768 total generated
tokens, 262144 total cumulative input tokens, a 600-second agent deadline and a
120-second independent verifier limit**. Task order derives from the frozen G1
schedule/order seed 20260831 and is listed below. One fresh process/workspace is
used per root. Independent verification remains broad and uses the unchanged
manifest command and protected-file hashes. Startup, agent time, independent
verification, end-to-end time and controller/hash/archive overhead remain separate.

For B, child seeds are exactly `uint32(42 + i * 0x9e3779b9)` for zero-based child
index i: **42 and 2654435811**. They are not two identical trajectories and are not
base seeds 42 and 43. Keep native-call/token-bound checks independent of byte-identical
free-form CUDA output expectations. This diagnostic schedules no base-43/44/45
runs; those remain part of acceptance G6 for the finally selected stochastic policy.
Best-of-N remains absent from the temperature-zero historical profiles.

## Exact membership and execution order

All six paths are the unchanged manifests under
`benchmark/results/2026-09-08-repair-control/tasks`. The contract also retains the
complete per-file fixture and protected-file hashes, including the distractor's
protected `legacy.py`; no current fixture file may be replaced by an oracle.

| Task | Manifest SHA-256 | Prepared fixture SHA-256 |
| --- | --- | --- |
| `generalize_retractions_distractor` | `2bb193bae70032c96e5919787107dead6b8c81ef0aed2c5195678067517b9829` | `81e55124a16662d5f21ac8918048587c8845c8f8b1049f9ad01da96d2c78b777` |
| `generalize_retractions_original` | `8d909f5d803c950d0aa6c2acb7fd6e53ac13b55f348288f37dabde6103fc0472` | `3f4e9ef1c0af1c395fad3cd5ed068907875644874efe2ef9186c9bb3f600a1d2` |
| `generalize_retractions_paraphrased` | `27603cd6d23caf00e7afcfd8714dfbad577f25e407ef9bb901318c810878c0a0` | `114468926283371aefc6cf841001a6786a608ff5c789c3308e5e7e94cfd66523` |
| `generalize_retractions_renamed` | `3e32c2aad5756a0e60823082e619b811aa3bc4e8389052923b86b289b9c91e2f` | `4d74e7ffe7f1e1097a5da0d0805c77ad7b2a0f6e826f0f2156272e28d09ccb92` |
| `go_api_pagination` | `93bedc92229fc6ee76bf2cc78eb788b223bf486b808de730f32b5bc2b6f19ff5` | `f46eca1a16f8a08ce60d5422ae80796ef3e43822f19d3258d491e314e9180a91` |
| `go_multifile_transfer` | `359e98e045a0cade21e50301b1e0ca48f3ee52963b42477a28cc6d5ff82a6589` | `5a218a8a244033ccee61e8c9c7d94f40b212211f0e5f3076c16a2c66a6c6b72b` |

B uses the following 18 rows in order. A links identify the corresponding frozen
G1 observations and must retain their original execution identities and arm hash.
All rows use base seed 42. An unstarted child, crashed root, timeout, invalid
archive, missing measurement, protected mutation, no-op or failed final workspace
remains in the scheduled denominator. Do not run until a favorable repeat appears.

| B order | Repetition | Task | A execution record | New B run ID |
| ---: | ---: | --- | --- | --- |
| 1 | 1 | `generalize_retractions_renamed` | `development-G1-loop-pilot-generalize_retractions_renamed-s42-r001` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_renamed-s42-r001` |
| 2 | 1 | `generalize_retractions_distractor` | `development-G1-loop-pilot-generalize_retractions_distractor-s42-r001` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_distractor-s42-r001` |
| 3 | 1 | `go_api_pagination` | `development-G1-loop-pilot-go_api_pagination-s42-r001` | `l3-search-02-loop-repair-best-of-2-go_api_pagination-s42-r001` |
| 4 | 1 | `generalize_retractions_paraphrased` | `development-G1-loop-pilot-generalize_retractions_paraphrased-s42-r001` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_paraphrased-s42-r001` |
| 5 | 1 | `go_multifile_transfer` | `development-G1-loop-pilot-go_multifile_transfer-s42-r001` | `l3-search-02-loop-repair-best-of-2-go_multifile_transfer-s42-r001` |
| 6 | 1 | `generalize_retractions_original` | `development-G1-loop-pilot-generalize_retractions_original-s42-r001` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_original-s42-r001` |
| 7 | 2 | `generalize_retractions_paraphrased` | `development-G1-loop-pilot-generalize_retractions_paraphrased-s42-r002` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_paraphrased-s42-r002` |
| 8 | 2 | `generalize_retractions_renamed` | `development-G1-loop-pilot-generalize_retractions_renamed-s42-r002` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_renamed-s42-r002` |
| 9 | 2 | `go_api_pagination` | `development-G1-loop-pilot-go_api_pagination-s42-r002` | `l3-search-02-loop-repair-best-of-2-go_api_pagination-s42-r002` |
| 10 | 2 | `generalize_retractions_distractor` | `development-G1-loop-pilot-generalize_retractions_distractor-s42-r002` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_distractor-s42-r002` |
| 11 | 2 | `go_multifile_transfer` | `development-G1-loop-pilot-go_multifile_transfer-s42-r002` | `l3-search-02-loop-repair-best-of-2-go_multifile_transfer-s42-r002` |
| 12 | 2 | `generalize_retractions_original` | `development-G1-loop-pilot-generalize_retractions_original-s42-r002` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_original-s42-r002` |
| 13 | 3 | `generalize_retractions_distractor` | `development-G1-loop-pilot-generalize_retractions_distractor-s42-r003` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_distractor-s42-r003` |
| 14 | 3 | `go_api_pagination` | `development-G1-loop-pilot-go_api_pagination-s42-r003` | `l3-search-02-loop-repair-best-of-2-go_api_pagination-s42-r003` |
| 15 | 3 | `generalize_retractions_paraphrased` | `development-G1-loop-pilot-generalize_retractions_paraphrased-s42-r003` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_paraphrased-s42-r003` |
| 16 | 3 | `generalize_retractions_renamed` | `development-G1-loop-pilot-generalize_retractions_renamed-s42-r003` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_renamed-s42-r003` |
| 17 | 3 | `generalize_retractions_original` | `development-G1-loop-pilot-generalize_retractions_original-s42-r003` | `l3-search-02-loop-repair-best-of-2-generalize_retractions_original-s42-r003` |
| 18 | 3 | `go_multifile_transfer` | `development-G1-loop-pilot-go_multifile_transfer-s42-r003` | `l3-search-02-loop-repair-best-of-2-go_multifile_transfer-s42-r003` |

The A IDs above are scheduled identities; they are not claims that those runs
have finished. Their eventual evidence path is `../runs/<A-run-id>/outcome.json`.

Use the existing runner once per B row with its own output directory and
`--variants loop-repair-best-of-2 --retain-terminal --repetitions 1 --no-randomize`.
Pass every numeric/profile limit above explicitly, including `--seed 42` and
`--order-seed 20260831`; the enclosing diagnostic schedule owns the repetition
identity. Keep the internal harness repetition 1 distinct from the diagnostic
repetition. Rehash identities before and after each execution and before closing
the diagnostic. Source or configuration drift ends that diagnostic identity; its
started/failing rows remain retained.

A ran before B, so any time differences have a block-order confound. Report that
limitation. This minimal comparison is sufficient to document no observed benefit
or a preservation failure; it is not a causal speed comparison or a superiority
claim. If a later claim needs interleaved fresh A/B runs, preregister that separately
before running them. Do not enlarge this denominator with those future results.

## What the current allocator actually does

[fg_candidate_search](../../../../../src/core/candidate_search.c) shares the total
root budget. At each child it divides the *remaining* action/generated/input budget
by the number of children left. Thus B's first child begins with at most 16 actions,
16384 generated tokens and 131072 input tokens; the second receives the remaining
budget and can receive more than half if the first used less. It is inaccurate to
call these two independent fresh 32-action budgets, or to assume that both always
receive exactly 16 actions.

The current time policy retains a cleanup reserve of
`min(5000ms, wall_timeout_ms/10)`, which is 5 seconds here. Each child then receives
`(remaining search deadline)/(children_left + 1)` after preparation and cloning.
The first allocation is therefore at most roughly 198.3 seconds, reduced by setup,
not 300 seconds. Snapshotting, copying, selection, final real-workspace validation
and guarded restoration consume the same root deadline. They do not justify extra
actions or fresh tokens. Audit actual allocations and consumption, including
remaining capacity when the second child starts or fails to start.

The parent aggregates child turns, generated/input tokens, tool calls and validation
costs. Parent load/arena values use a maximum; they should not be naively summed
with children. Root duration is search-wide. Count root outcomes once and child
work separately to avoid double counting root aggregates plus child metrics.

A child is eligible only after successful completion with an actual native final
and a changed snapshot. Eligible candidates are applied to the real workspace,
validated there, then restored to baseline. Selection ranks passing candidates by
smaller changed-content cost, keeping the first on a tie. The winner is applied
and broadly validated again before the parent emits its selected/final events.
A failed or incomplete child cannot be promoted solely because a later secondary
verifier finds a passing workspace. Record that possibility separately and retain
its final-call evidence. This proposal does not change allocation or ranking.

## Preregistered decision rule

Report A and B primary completions out of 18, plus the four paired categories:
A-pass/B-pass, A-pass/B-fail, A-fail/B-pass, and A-fail/B-fail. A missing/invalid
B outcome belongs to the non-passing side. List every pair and aggregate by the
four retraction variants and the two Go fixtures; four variants are not four
independent problem classes. Report no confidence interval or release claim from
these reused development fixtures.

**Keep best-of-N experimental** if there is any loss of an A passing pair, any
incomplete/integrity failure, or no strict increase in primary completions. In
particular, a tie does not justify enabling a more complicated policy. If A is
18/18, this six-case comparison has no correctness headroom: even B 18/18 is
preservation only. Keeping the simpler supported single-candidate policy is
allowed by the repair plan. The minimum *formal comparison* requires the 18 new B
roots above; it is not necessary to spend those runs merely to assert that no
repaired-B benefit has yet been measured. If deferred, explicitly label L3's
comparison unrun rather than inventing a result.

Only if B retains every A passing pair and strictly increases primary completions
with intact budget/integrity evidence is there a measured development benefit to
investigate. Report the gain and full executor/token/time costs; it still does not
establish acceptance. Decide the deployable mapping before a new candidate's
G0-G6 sequence. Never import B's diagnostic outcomes into candidate-02's A gates,
or pool A/B wins to reach 18/18. Neither result lowers any gate threshold.

An adaptive allocator is out of this comparison. Consider one only after this
fixed A/B evidence and an explicit failure mechanism justify it; preregister a
separate arm, minimum feasible generation/verification/restoration reserves and
complete denominator before execution. A passing single-candidate policy can
finish the plan while best-of-N remains experimental.

## Evidence, source integrity and archive closure

The existing reporter verifies the frozen source manifest and archive inventory,
runtime files, selected configuration hash, per-run before/after identities,
manifest/fixture hashes, and unique scheduled/execution IDs. Each root must retain
commands, preflight, prompt/output/session journals, validation, complete terminal
inputs, metrics and an exact verified archive inventory. Passing verification
requires the unchanged command, a positive test count equal to oracle preflight,
unchanged verification inputs and protected bytes, followed by actual agent
completion. Passing terminal inputs without completion are a separate metric.
G0 additionally binds the full executable/DLL inventory and direct model probes.

Three current-interface limitations matter before a B run:

1. `agent_loop_campaign.freeze` hardcodes `loop-repair` and one candidate in all
   profiles, and `run-gate` consumes the acceptance schedule. Do not spoof B by
   editing that frozen candidate JSON or its result settings. Freeze a separate
   diagnostic driver/protocol, record both arm hashes, and use the existing B
   runner variant. Any source-level driver change creates new source provenance
   and requires a fresh compatible baseline; a separately hashed evidence-side
   driver must also be retained explicitly.
2. The generic root reporter inventories the root prompt manifest and validates
   primary success, but does not itself enforce all multi-candidate child events,
   completion calls, seed/budget allocation, selection/restoration journals or
   nested child prompt inventories. The adapter's raw recursive archive includes
   the retained session tree, but its root `session/context/*.txt` prompt list is
   insufficient as a complete child-prompt index. B therefore requires an explicit
   child audit; do not treat a root reporter pass as proof that L3's contracts passed.
3. The candidate-02 Python verifier can consume cached bytecode, and its terminal
   snapshot omits cache bytes. `-B` alone prevents writes, not reads of stale bytecode.
   The supplied future baseline must record `python_cache_policy` as
   `fresh-external-prefix-no-write` for Python (`not-applicable` otherwise), and
   `terminal_retention_policy` as `complete-except-git-forge`. Retain both full
   pre-verification and terminal snapshots, including cache bytes and nested
   `.git`/`.forge`, excluding only root `.git`/`.forge` directories; root regular files with those names remain inputs. This is a prerequisite for
   a newly measured baseline; legacy records keep their original classification.

The results-only [diagnostic driver](../../l3-comparison/driver.py) and
[operating notes](../../l3-comparison/README.md) implement a separately frozen
18-pair schedule, child audits, full per-root archives and explicit final closure.
They do not launch a comparison during this candidate's measurement.

For every B root, archive both `trial-*` input workspaces and their complete nested
sessions, all winning and losing children, `candidate_start`, `candidate_generated`,
`candidate_selection` and `candidate_selected` events, model finals, all validation
stdout/stderr/plan identities, and the file-operation prepare/outcome journal.
Record candidate count planned/started/completed/eligible/selected and reasons for
missing launches. Check the exact initial baseline copies and protected files,
child seed rule, summed total limits, successful real-workspace selection, final
broad verification and guarded restoration. Distinguish root completion loss,
child completion loss and incorrect code. Independently verify failed roots and
failed/incomplete child terminal inputs on separate copies without modifying the
original evidence; none of those results can replace primary outcomes.

At diagnostic/candidate closure, finish a read-only source/runtime/model/fixture
identity audit after all selected runs. Retain the source archive, original diff,
revision/dirty-state record and build/check command logs associating the snapshot
with the binaries. The source hash identifies a snapshot; by itself it is not a
reproducible-build attestation. Report absent provenance honestly.

Create a final candidate/diagnostic-wide inventory and verified archive including
the protocol, full schedule, every start/outcome/exception, G0 logs and JUnit,
per-root archives, nested trial evidence, preflights, terminal-verification audits,
all comparison summaries and this decision record. Current per-root archives do
not automatically prove that those top-level/final reports are archived. Freeze
the member list before writing the final ZIP, exclude the ZIP and its own inventory
from recursive input, check every member hash/size and duplicate/member count,
then record the archive hash in a separate closure record. Keep an explicit index
of external referenced A executions with their immutable archive hashes if the
comparison archive does not duplicate their bytes. Missing evidence leaves the
diagnostic/candidate incomplete.

## Prior evidence and interpretation

The [42-run pilot](../../../2026-09-09-agent-loop-v1/README.md) measured minimal
4/6, checkpoint-only 2/6, best-of-two 2/6, and combined 3/6. Both Go tasks passed
in every arm. Best-of-two produced all 12 children: four completed and eight failed;
combined completed five and had seven failed children. All 23 failed root inputs
and all 15 failed child inputs failed secondary verification. That pilot found no
hidden passing child lost solely to missing completion. Its shared-budget audit
passed, and it did not demonstrate a correctness benefit for best-of-two.

Those are the earlier unbounded-policy observations, not measurements of
`loop-repair-best-of-2`. They justify keeping search experimental while performing
this bounded comparison, not predicting its outcome. Candidate-02's current G1
results and the proposed B results are intentionally left unreported here.
