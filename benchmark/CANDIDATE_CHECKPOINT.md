# Candidate validation and completion experiment

This opt-in intervention extends the minimal control with
`--minimal-agent --candidate-checkpoint --prompt-protocol native`. It is a repair
experiment, not the default Forge loop. It requires at least three actions and
cannot be combined with `--no-auto-validation`. The original minimal mode and default
Forge policy remain unchanged.

## Host contract

- `validate_candidate` assesses a net changed workspace with the existing broad
  Go/Python schedule. An early `final` invokes the same check.
- Complete bounded input snapshots, not generation counters or model claims,
  identify candidates. A no-op or return to initial bytes cannot count as an
  attempt. Repeating the last failed candidate does not run validation again.
- A changed candidate with recorded, stable validation counts as an assessed
  attempt, including a failing check. Reads, intermediate edits, tool changes,
  command success, denied checks, and missing tests do not close recovery.
  Only a host-validated pass closes the episode.
- The penultimate action allows only `validate_candidate`; the last allows only
  `final`. A pass restricts the next action to `final` immediately. Both the
  advertised schema and dispatch enforce these reservations. A noncompliant
  action fails without execution. No model final is synthesized by the host.
- A passing input snapshot can be reused by final only after comparison with
  current inputs. Changed inputs invalidate the pass. Validation rejects input
  mutation and incomplete evidence using the existing verifier.
- All original hard limits and process policy apply. Reserving actions does not
  guarantee completion if tokens, context, time, or model compliance run out.
  The transcript stays append-only; changing host control metadata is appended
  as a user message to preserve the reusable prefix. The current tool schema
  changes at checkpoints.

Snapshots use the existing bounded input scanner (100,000 files, 2 GiB of
content, shared deadline), excluding root `.git` and `.forge`. This is byte-level
candidate identity, not semantic change detection: comments and other workspace
input changes are still changes. There is no best-of-N, rollback policy,
symbol-impact selection, or failure-conditioned reasoning in this experiment.
Those separately selectable extensions are now described and measured under the
[remaining-loop protocol](LOOP_COMPLETION.md); they are excluded from the
candidate-checkpoint baseline and its archived results.
Only Go and Python have host validation. Unsupported targets and Python without
discovered tests cannot pass; Go follows the existing staged checks, including
packages with no test functions. No-change analytical tasks should use ordinary mode.

Version 2 tied the control at 7/18 but invalidated most KV reuse by putting
changing control text in the system prefix. Version 3 fixes that placement and
repeats the identical six-task, three-repetition protocol. All version 2 outcomes
remain retained; they are not pooled with or replaced by version 3.

The [completed version 3 diagnostic](results/2026-09-08-candidate-checkpoint-v3/README.md)
recorded 9/18 versus 6/18, with all extra passes on one distractor fixture. Median
time was 27.759 s versus 28.351 s; aggregate time was 533.140 s versus 475.279 s.
Neither arm lost a test-passing terminal workspace to missing completion. A
separate add smoke failed after the model damaged a passing edit before host
validation. The experiment remains opt-in and does not satisfy promotion gates.
This results paragraph was added after the completed frozen-input audit.

## Preregistered measurement

Hypothesis: requiring candidate evidence and reserving completion actions reduces
the loss of test-passing workspaces to unfinished tasks. It may hurt repairs that
need all available edit actions, or add validation latency. Both outcomes count.

Use all six unchanged manifests in
`results/2026-09-08-repair-control/tasks`: four retraction variants, Go pagination,
and Go multi-file transfer. Run three repetitions per arm, 36 cold runs total,
with interleaved deterministic order (seed 20260831). These are examined
development diagnostics, not a fresh holdout or preservation gate.

Both arms use the same executable and adjacent runtime libraries, the local
Qwen3-Coder-30B-A3B-Instruct-Q4_K_M model, GPU layers -1, native embedded template,
temperature 0, seed 42, context 16384, output reserve 2048, 16 actions, generated
tokens 32768, cumulative input tokens 262144, and 600 seconds per task. The
independent verifier has 120 seconds. No model is downloaded.

```powershell
python benchmark/repair_control.py --experiment candidate-checkpoint --candidate-forge RUNTIME/forge.exe --minimal-forge RUNTIME/forge.exe --model MODEL.gguf --task-dir benchmark/results/2026-09-08-repair-control/tasks --output OUTPUT
```

The runner freezes source, runtime, model, harness, task identities and schedule;
retains source snapshots; preflights broken fixtures and supplied oracles; and
audits identities and every scheduled outcome. Resume cannot retry failures or
change the protocol. Do not tune source or policy during the matrix.

Report successful agent exit plus independent verification and protected-file
integrity as the primary outcome. Separately report terminal workspace test
success, final actions, terminal errors, candidate attempts, actions, generated
tokens, validation time, and cold end-to-end time. Retain all failed workspaces,
raw model output, prompts, edit journals, checkpoint events and verification
logs. A passing terminal workspace does not overwrite a failed primary result.

Promotion requires later repair/preservation, regression, invariant, and fresh
holdout evidence. This experiment alone cannot establish general superiority.
