# Repair control comparison — September 8, 2026

The minimal control did not improve benchmark completion: **6/18**, versus
**7/18** for current Forge and **9/18** for the historical checkpoint. It left
**8/18 test-passing final workspaces**, including two repairs that exhausted their
actions without finishing. The results support separating candidate correctness
from completion, while showing that ineffective model proposals persist with a
simple interface. They do not establish a winning agent or model.

All **54 scheduled runs** are retained. The final audit found unchanged source,
model, runtime, settings and fixtures, real inference in every run, and no
protected-file mutation, process crash, task timeout, or budget overrun.

## Results

| Arm | Benchmark completion | Tests pass at stopping point | Median cold end-to-end time |
| --- | ---: | ---: | ---: |
| Development-gate checkpoint, `5222616` | 9/18 | 9/18 | 41.813 s |
| Current Forge, retained v3 `7660bf1` | 7/18 | 7/18 | 48.227 s |
| Minimal control | 6/18 | 8/18 | 28.774 s |

Benchmark completion is the original composite requirement: successful agent
exit, successful independent manifest verifier, and unchanged protected files.
The secondary column assesses the final code without changing those primary
outcomes. For the 32 primary failures, the retained workspace was copied and
checked with the unchanged manifest verifier after all model runs finished. No
model was called and no repair was made during this check. All secondary checks
had intact protected files. Original verifier logs already report passing tests
for both additional minimal-control cases.

| Case | Checkpoint completion | Current completion | Minimal completion | Minimal terminal tests |
| --- | ---: | ---: | ---: | ---: |
| Retractions / original | 0/3 | 0/3 | 0/3 | 0/3 |
| Retractions / renamed | 0/3 | 0/3 | 0/3 | 0/3 |
| Retractions / distractor | 0/3 | 2/3 | 0/3 | 1/3 |
| Retractions / paraphrased | 3/3 | 1/3 | 0/3 | 1/3 |
| Go pagination | 3/3 | 3/3 | 3/3 | 3/3 |
| Go multi-file transfer | 3/3 | 1/3 | 3/3 | 3/3 |

The two additional passing workspaces are minimal paraphrased repetition 1 and
minimal distractor repetition 2. Both reached 16 turns without a final answer.
Their original primary failures remain unchanged. Thirty-one of the 32 primary
failures reached 16 turns; the historical original repetition 1 stopped at turn
13 when its generation budget ended before a complete native call. No run
reached the cumulative 32,768 generated-token or 262,144 prompt-token cap.

Times include cold process/model startup and the original independent verifier,
across successes and failures. Harness hashing and the secondary checks are
outside these times. These measurements on six selected development cases do
not establish a general latency advantage.

## What the traces show

- **Ineffective edits originate in the sampled raw model outputs.** In the
  original minimal run, the model isolated the failing order and described the
  needed condition, then supplied identical old/new text twice. In a sampled
  current run, the proposed edit changed only a comment. Raw native parameters,
  parsed arguments, and edit journals agree; the parser did not discard a
  correct change. Exact-match no-op rejection already worked.
- **A correct candidate can be lost as a completed task.** The minimal
  paraphrased run produced passing code and ran the real tests, then spent its
  final two actions on unnecessary execution and reading. Explicit candidate
  validation and completion checkpoints deserve an isolated test.
- **The simpler interface helped the selected multi-file task.** Minimal
  repaired both transfer defects in all three repetitions; current did so once.
  In the audited first pair, current never proposed the second required fix and
  never had a passing candidate. Its intermediate revert was allowed. Extra
  validation and context-processing costs are visible, but this experiment
  cannot identify which omitted feature caused the different proposals.
- **Warning counts are not repair scores.** Checkpoint/current/minimal recorded
  6/8/0 loop warnings and 4/11/13 identical replacement attempts. The minimal
  control has no recovery warning mechanism by design.

See the [trace review](first-repetition-review.md), including exact action,
event, raw-output, and patch references. Those paths resolve within the complete
evidence archive.

## Protocol and implementation

The population is the four unchanged retraction variants with recent failures,
plus transfer and pagination, with three repetitions per arm. It excludes known
passing contrast/window variants and the remaining development tasks, so it
cannot establish preservation or pass the broader gates. All six broken
baselines fail preflight and all six supplied oracles pass.

All arms use the same Qwen3-Coder-30B-A3B-Instruct-Q4_K_M file and adjacent runtime
libraries, native embedded template, GPU layers -1, context 16384, output reserve
2048, temperature 0, seed 42, 16 turns, 600 seconds, cold lifecycle, and fixed
order seed 20260831. The cumulative limits are explicit. Arm order is interleaved
within a common task/repetition schedule.

The historical checkpoint passed its earlier development gates (12/12, 60/60,
83/87) but failed its holdout. The 83/87 figure is hint-inflated: revision
`5222616` still carries four fixture-keyed diagnostic hints that current source
has removed. None of those hints fired in any of the 54 runs here. The checkpoint
also predates native decoding fixes and has different thinking/tool-choice
behavior despite the same benchmark arguments. Its arm is a descriptive
historical reference. **Current versus minimal is the direct comparison using
the same current decoder.**

The opt-in `--minimal-agent` uses list/read/exact-edit/command/final tools and
complete appended history, including model prose in the physical Qwen prompt.
It shares permissions, native decoding, and edit evidence with Forge. It omits
recovery, automatic validation, semantic retrieval/compression, staged syntax
rejection, and last-turn schema narrowing. This is an in-repository control,
not mini-SWE-agent itself. Its source is a frozen uncommitted snapshot on
`93faa55`, retained in [minimal-source.zip](minimal-source.zip) and
[minimal-source.diff](minimal-source.diff); executable SHA-256 is
`6285a5fdcd349d752ba156c5072c20d972b07dbb37ee195fc654573e332e88c3`.

The new comparison runner records and checks identities, retains every failure,
and refuses mismatched resume or missing source evidence. The GPU Release build
passed. CTest passed 26 tests in 80.52 seconds and skipped the opt-in checkpoint
model test. This includes 11 minimal-control regressions and 16 comparison
checks. The separate Qwen GPU probe passed, including forced opening,
cancellation, budget exhaustion, recovery, cached tool-call equivalence, and
generation without a callback. No source or harness changed during the matrix.

## Next experiment and evidence

Test one bounded candidate/checkpoint intervention at a time. Keep a repair
episode active through reads and legitimate multi-file edits, require an actual
changed candidate and validation to assess progress, and reserve a completion
opportunity within the existing budget. Test compact alternative attempts and
immediate applied-diff feedback separately. The persistent original/renamed
failures also justify a separate model comparison using the same simple control;
these results do not prove that another model will improve them.

No recovery technique or model change has been promoted. The repair/preservation,
regression, invariant, and fresh comparative holdout requirements remain open.

- [Frozen protocol](protocol.json), [population and identity audit](audit.json),
  [primary results](summary.json), and [per-run analysis](analysis.json).
- [Secondary terminal verification](terminal-verification.json) and
  [independent final review](final-audit-review.md).
- [Complete evidence](complete-evidence.zip) retains all raw model output,
  contexts, tool results, edit journals, verification output, failed workspaces,
  source snapshots, build/probe logs, historical identities, and analysis tools.
  Every archive member was reopened and checked against the
  [hash inventory](evidence-inventory.json).
- [Experiment design and reproduction](../../REPAIR_CONTROL.md).
