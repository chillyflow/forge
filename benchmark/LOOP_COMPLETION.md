# Remaining agent-loop diagnostic pilot

Preregistered September 9, 2026, before this matrix. This experiment measures
the remaining loop interventions against both the minimal control and the
candidate-checkpoint baseline in one frozen runtime. It is an examined
development diagnostic, not a fresh holdout, preservation gate, or promotion
decision. The earlier candidate-checkpoint results remain separate evidence.

## Arms and hypotheses

Every arm uses native prompting, the embedded template, and thought history.
Every arm except `minimal` also enables `--minimal-agent --candidate-checkpoint`.
`minimal` uses `--minimal-agent` without candidate checkpoints.

| Arm | Additional policy | Diagnostic question |
| --- | --- | --- |
| `minimal` | Original minimal loop | How often does the unchanged control finish and pass? |
| `candidate` | Candidate checkpoints only | What does the completion baseline achieve under these budgets? |
| `best-of-2` | `--candidates 2` | Can two trajectories and real-workspace selection improve repair within the same total budget? |
| `semantic` | `--semantic-loops` | Does canonical failed-state evidence reduce semantic repetition? |
| `impact` | `--symbol-impact` | Does preliminary symbol test selection find failures earlier without weakening final verification? |
| `reflection` | `--failure-reflection` | Does a bounded diagnostic action after failure improve recovery? |
| `combined` | All four policies, including two candidates | How does the complete intervention behave under the shared limit? |

These are comparisons of whole policies. One repetition per fixture cannot
isolate causes, establish statistical superiority, or justify defaults. The
combined arm is not a factorial interaction study. Temperature and action limits
differ from the earlier candidate-checkpoint matrix, so do not pool its results.

Best-of-two generates from independent copies of the initial workspace. Its
candidate selection validates edits in the real workspace, records journaled
apply/revert operations, chooses a passing candidate with the smallest changed
content cost, and keeps the earlier candidate on ties. Selected edits undergo
final real-workspace validation. Candidate seeds are
`uint32(42 + zero_based_candidate_index * 0x9e3779b9)`: 42 and 2654435811.
Remaining actions, generated tokens and cumulative input tokens are divided
among remaining candidates. Trial and selection time share one absolute task
deadline. There is no fresh total budget per candidate.

Reflection uses the default 256-token diagnostic bound, charged to the ordinary
action/token budgets. Symbol impact uses exact indexed Go declaration ranges
and syntactic caller candidates for preliminary tests. The existing broad final
checks remain authoritative. Python emits lexical structural candidates with
explicit conservative fallback; it has no new sound call graph or coverage map.
Trial workspaces use explicit filesystem indexing, independent of any enclosing
Git ignore rules. Its conservative `filesystem_scan` marker means the combined
arm can emit impact evidence while retaining broad preliminary Go checks; the
direct impact arm can narrow those checks in the initialized Git workspace.
Interactive sessions and user questions are covered by separate session and
integration tests; this unattended coding pilot does not measure partnership.

## Fixed population and limits

Use all six unchanged manifests from
`results/2026-09-08-repair-control/tasks`, once per arm: **42 cold runs**.
All seven arms run adjacent within each shuffled task block. The deterministic
order seed is 20260831. No fixture, prompt, protected test or oracle is changed.

| Manifest | SHA-256 |
| --- | --- |
| `generalize_retractions_distractor.json` | `2bb193bae70032c96e5919787107dead6b8c81ef0aed2c5195678067517b9829` |
| `generalize_retractions_original.json` | `8d909f5d803c950d0aa6c2acb7fd6e53ac13b55f348288f37dabde6103fc0472` |
| `generalize_retractions_paraphrased.json` | `27603cd6d23caf00e7afcfd8714dfbad577f25e407ef9bb901318c810878c0a0` |
| `generalize_retractions_renamed.json` | `3e32c2aad5756a0e60823082e619b811aa3bc4e8389052923b86b289b9c91e2f` |
| `go_api_pagination.json` | `93bedc92229fc6ee76bf2cc78eb788b223bf486b808de730f32b5bc2b6f19ff5` |
| `go_multifile_transfer.json` | `359e98e045a0cade21e50301b1e0ca48f3ee52963b42477a28cc6d5ff82a6589` |

Use the local Qwen3-Coder-30B-A3B-Instruct-Q4_K_M model, GPU layers -1,
temperature **0.6**, base seed 42, context 16384, output reserve 2048,
**32 total actions**, 32768 total generated tokens, 262144 total input tokens,
and 600 seconds per task. The independent verifier has 120 seconds. Every arm
uses the same executable and adjacent runtime libraries. No model is downloaded.

```powershell
python benchmark/repair_control.py --experiment loop-completion --forge RUNTIME/forge.exe --model MODEL.gguf --task-dir benchmark/results/2026-09-08-repair-control/tasks --output OUTPUT --repetitions 1 --temperature 0.6 --seed 42 --order-seed 20260831 --max-turns 32 --context 16384 --output-reserve 2048 --max-tokens 32768 --max-input 262144 --timeout 600 --verification-timeout 120 --gpu-layers -1
```

`loop-completion` defaults to one repetition, temperature 0.6 and 32 actions.
The two existing experiment modes retain their original defaults. The single
`--forge` argument prevents accidental per-arm binary differences. The frozen
protocol records every arm's exact feature flags, runtime bundle, source
snapshot, model, task identity, limits, candidate budget scope and schedule.

## Evidence and stopping rules

The existing runner verifies each broken fixture fails and its supplied oracle
passes before inference. It hashes frozen inputs before and after the matrix,
checks input metadata between cells, retains the source snapshot and complete
population, and refuses changed protocols on resume. Failed and interrupted
cells are retained and never retried. Stop for an operational defect without
discarding its outcomes; record any corrected implementation as a separate
experiment. Do not tune on running outcomes or silently replace failures.

Primary success requires successful agent exit, independent verification, and
unchanged protected files. Report each arm's passes out of six, per-fixture
outcomes, terminal completion, total actions/tokens, validation time, cold
end-to-end latency, and failures. Separately inspect test-passing terminal
workspaces that failed to complete; that secondary finding never overwrites the
primary outcome. Best-of-two reports must include both generated candidates,
selection events, rollback evidence and the selected workspace, not just wins.

Retain prompts, model outputs, session events, failed workspaces, validation
plans/reports, diagnostic reflections, semantic-loop evidence, structural impact
reports, process results and independent verification. All 42 scheduled cells
count, including budget exhaustion, invalid finals and process or harness
failures. Candidate count does not multiply the allowed budget during auditing.
No passing result in this pilot removes the need for preservation, realistic
repository, platform and fresh-holdout evidence.
