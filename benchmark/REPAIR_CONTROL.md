# Repair control experiment

The first experiment compares an earlier development-gate checkpoint, retained
Forge recovery v3, and an opt-in minimal agent on the same known failing cases.
This is a diagnostic experiment on examined development inputs. It cannot
qualify a release, establish preservation on omitted tasks, or decide whether a
different model would perform better.

## Fixed population and budgets

Use the four retraction manifests `original`, `renamed`, `paraphrased`, and
`distractor` from `results/2026-09-08-repair-validation-v1/tasks`, plus the unchanged
`tasks/go_multifile_transfer.json` and `tasks/go_api_pagination.json`. These six
cases are the union of recent repair and broader-gate failures. Run three
repetitions per arm, retaining all 54 outcomes, with a deterministic task order
and interleaved arm order. The passing contrast and window cases are omitted
from this focused comparison; they remain required for a later preservation
gate.

Keep Qwen3-Coder-30B-A3B-Instruct-Q4_K_M, GPU layers -1, embedded native template,
context 16384, output reserve 2048, total generated-token cap 32768, cumulative
prompt-token cap 262144, temperature 0, seed 42, 16 model turns, 600 seconds per
task, cold lifecycle, and order seed
20260831. Use fresh workspaces, the same fixture preparation, protected files,
and independent manifest verifier. Preflight every broken fixture and supplied
oracle before inference. A failure, interruption, or exhausted limit stays in
the population; a successful verifier alone does not override an agent error.

## Arms and interpretation

- **Development-gate checkpoint:** `5222616b2aebeae91d4942fadbc64f9a1d79c371`,
  executable SHA-256
  `9abe126279d9d042a26afaf1983b1df1671092ffcd9ba6e8c0f47740c13d70be`.
  Its recorded development results were 12/12 regression, 60/60 invariants, and
  83/87 overall. The 83/87 figure is hint-inflated: this revision still carries
  four fixture-keyed diagnostic hints that current source has removed, although
  none fired in the repair-control runs. Its later holdout failed. This is a
  descriptive historical comparison: it predates the native inference stability
  fixes, including changes to thinking defaults and forced tool opening.
- **Current Forge:** retained v3 from
  `7660bf175ed4b5a5323ba495a340dc7fabf4d065`, executable SHA-256
  `1ad06811e78c47308713460a4c440c65c854ef4771417e23eb2f60b507698da6`.
  Production source at the start of this experiment (`93faa55`) matched this
  candidate. Its earlier 7/10 repair result did not preserve all prior passes;
  broader gates were 10/12 and 59/60.
- **Minimal control:** the current native model backend and basic tool handlers,
  with an append-only conversation and no recovery/planning/automatic validation
  orchestration. Its source snapshot and runtime are frozen before execution.
  This is an in-repository diagnostic control, not mini-SWE-agent itself.
  The current/minimal comparison is the more direct orchestration experiment.

The minimal mode uses `list_directory`, `read_file`, `apply_patch`,
`run_command`, and `final`. It retains complete model prose in the physical
assistant history, even when the embedded template ignores a separate reasoning
field. It keeps the shared native decoder, permissions, exact-match edit journal,
and bounded tool output. It omits semantic retrieval/output compression,
automatic validation, recovery, staged Go syntax rejection, and last-turn schema
narrowing. It fails when its complete history no longer fits instead of
compacting. These differences define the control; no single omitted feature can
be credited from this comparison alone.

## Running the comparison

Copy the six unchanged manifests into a dedicated task directory. Build and test
the minimal source, then copy its executable and adjacent libraries into a
separate runtime directory. Use a new output directory:

```powershell
python benchmark/repair_control.py --checkpoint-forge CHECKPOINT/forge.exe --checkpoint-revision 5222616 --current-forge CURRENT/forge.exe --current-revision 7660bf1 --minimal-forge MINIMAL/forge.exe --model MODEL.gguf --task-dir TASKS --output OUTPUT
```

The defaults implement the settings above. The runner preflights the fixtures,
locks the schedule and identities, saves the minimal source snapshot (including
new source files), and audits the resulting population. Use the identical
command with `--resume` only to continue an interrupted schedule. Terminal
failures and interrupted cells are retained and are never retried in place.

Report per-task outcomes, terminal reasons, tool/model turns, generated tokens,
and elapsed time. Retain full contexts, raw model responses, tool results,
actual edit diffs, verification output, and failed workspaces. Count correctness
using the independent tests and protected-file check. Do not use fewer loop
warnings as a substitute for successful repair. Timing on this selected small
population does not establish a general latency advantage.

## Follow-up experiments, one change at a time

Choose follow-up work from the control results before changing recovery again.
The next candidate should test an earlier, bounded repair episode: after a
second failed candidate or ineffective replacement, provide current relevant
source, the failing assertion, actual applied changes, and rejected approaches.
Reserve a concrete edit and validation opportunity within the existing turn
budget. Reads, changing tools, and intermediate edits across files may occur
inside the episode; they do not establish recovery. A changed candidate followed
by validation is the checkpoint for assessing the attempt.

Test immediate visibility of the applied difference separately. Existing
exact-match edits already reject byte-identical replacements; a proposed
improvement must demonstrate that the intended change appears in the actual
diff. Test rollback to a saved checkpoint and alternative hypotheses in a later
arm, preserving rejected candidate evidence and sharing the original total
budget. Test another model only as a separately frozen model experiment.

These are hypotheses motivated by the transparent, simple baseline described
by [mini-SWE-agent](https://mini-swe-agent.com/latest/), the editing-interface
experiments described by
[Anthropic](https://www.anthropic.com/engineering/swe-bench-sonnet), and the
separation of localization, repair, and validation in
[Agentless](https://arxiv.org/abs/2407.01489). Their reported results do not
establish that any technique improves Forge or this model.
