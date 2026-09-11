# Candidate validation and completion — final diagnostic

The opt-in candidate checkpoint arm completed **9/18 runs**, versus **6/18** for
the unchanged minimal control. All three extra passes came from the Python
retraction distractor fixture. Median cold end-to-end time was **27.759 s** versus
**28.351 s**, but total time was higher: **533.140 s** versus **475.279 s**.
This is a six-fixture development diagnostic, with three repetitions per arm.
It does not establish general superiority or satisfy a fresh holdout gate.

| Measure | Candidate checkpoint | Minimal |
| --- | ---: | ---: |
| Successful exit, independent tests and protected files | 9/18 | 6/18 |
| Terminal workspace tests pass | 9/18 | 6/18 |
| Passing terminal workspace without completion | 0 | 0 |
| Accepted model finals | 9 | 7 |
| Accepted finals with failed independent tests | 0 | 1 |
| Median model actions | 15.5 | 16 |
| Total generated tokens | 42,914 | 44,963 |
| Total prompt prefill tokens | 207,307 | 58,430 |
| Total cached prompt tokens | 670,744 | 709,616 |
| Total prefill time | 82.256 s | 28.282 s |
| Host validation executor time | 16.016 s | 0 |
| Host validation commands | 77 | 0 |
| Median cold end-to-end | 27.759 s | 28.351 s |
| Total cold end-to-end | 533.140 s | 475.279 s |

| Fixture | Candidate checkpoint | Minimal |
| --- | ---: | ---: |
| Retractions, distractor | 3/3 | 0/3 |
| Retractions, original | 0/3 | 0/3 |
| Retractions, paraphrased | 0/3 | 0/3 |
| Retractions, renamed | 0/3 | 0/3 |
| Go API pagination | 3/3 | 3/3 |
| Go multi-file transfer | 3/3 | 3/3 |

All 36 scheduled runs used real inference and preserved protected files. The
source, harness, runtime, model and fixture identities remained frozen; the
population audit found no missing, duplicate or unexpected outcomes. All six
broken fixtures failed preflight and all six supplied oracles passed. The 21
primary failures were checked again on copies of their terminal workspaces;
none passed. These secondary checks called no model, repaired nothing, did not
replace primary outcomes and were excluded from the original timing.

## What the intervention establishes

`--minimal-agent --candidate-checkpoint --prompt-protocol native` adds a host
boundary for a complete repair candidate. Net changed workspace inputs plus
stable validation evidence count as an assessed attempt. Reads, tool changes,
no-op edits and arbitrary exit-zero commands do not close a failed repair
episode. Only a passing host validation closes it.

The penultimate action is reserved for `validate_candidate` and the last for
`final`; a passing candidate reserves the next action for `final` immediately.
The tool schema and dispatch both enforce these constraints. Early finals also
require validation. A final can reuse a passing snapshot only after comparison
with current inputs. The host never invents a model final or extends the limits.
The [implementation contract](../../CANDIDATE_CHECKPOINT.md) gives the full bounds.

Every successful candidate run emitted one final after a host-validated changed
workspace. All nine failed candidate runs reached the action cap and had their
final attempt rejected. Minimal had eleven action-cap failures and one accepted
final whose independent tests failed. However, neither arm left a test-passing
terminal workspace unfinished. This population therefore did not reproduce the
specific missing-completion failure that motivated the experiment. The three
extra repairs do not isolate the causal contribution of the completion rule
from the added validation feedback and instructions.

The feature remains opt-in. Candidate identity compares bytes, so comments and
generated input artifacts can count as changes; semantic no-op detection is
still absent. Snapshots are bounded to 100,000 files and 2 GiB and exclude root
`.git` and `.forge`. Host validation supports Go/Python only and uses the existing
broad schedule. It requires process permission and complete, stable evidence;
Python without discovered tests cannot pass. Go follows the existing staged
checks, including packages with no test functions. No-change analytical tasks
should use ordinary mode. There is no best-of-N, rollback policy, symbol-impact
test targeting or failure-conditioned reflection in this change.

## Earlier iterations and the failed smoke

[Version 2](../2026-09-08-candidate-checkpoint-v2/README.md) completed the same
36-run protocol at **7/18 versus 7/18**, with median time **43.375 s versus
27.869 s**. Changing control metadata was placed in system messages, which the
renderer hoisted ahead of all conversation history. This invalidated the
reusable prefix on each action. Version 3 appends that metadata chronologically
as user messages, keeping the fixed policy in the system message, and adds a
transcript-prefix regression. Candidate rules, task population and budgets
remained the same; no task-specific repair guidance was added.

Version 3 restored substantial reuse, recording 207,307 prefill tokens versus
761,458 in version 2. Checkpoint schema changes still alter the prompt prefix,
and validation and extra prompt processing still cost time. The near-equal
medians must not hide the higher aggregate runtime. Validation executor time
does not include every snapshot, indexing or inference cost. Both complete
matrices remain separate; results are not pooled or replaced. Version 2's
archive also retains the aborted version 1 integration failure and interrupted
control run.

The final implementation's separate [real-model add smoke](validation/real-model-smoke/add-candidate-checkpoint-r001/result.json)
**failed**. The model repaired `Add` and ran passing tests, then added an invalid
`main.go` before requesting host validation. Its attempted `rm` commands were
unavailable on Windows. Host checks failed and its final was rejected at the
16-action cap. The raw trace and failed workspace are retained; this smoke was
not retried or included in the 36-run matrix. An explicit candidate boundary
does not preserve an earlier passing edit if the model damages it before
validation. This failure and the narrow diagnostic population prevent a
preservation or default-promotion claim.

## Protocol, checks and retained evidence

Both arms used the same executable and adjacent DLLs on an RTX 5090 Laptop GPU,
Qwen3-Coder-30B-A3B-Instruct-Q4_K_M, GPU layers -1, the native embedded template,
temperature 0, seed 42, context 16384, output reserve 2048, 16 actions, generated
and cumulative input limits 32768/262144, and a 600-second task limit. Independent
verification had 120 seconds. Runs were cold and interleaved with order seed
20260831. The six unchanged Go/Python manifests were already examined development
diagnostics. Repetitions do not create eighteen independent task families.

The measured source is an uncommitted snapshot on
`07c178ac7707e03b175d3e87b2eb657d19d907d5`, retained with per-file hashes, source
archive and diff. Documentation of final results was added after the completed
identity audit. The GPU Release build passed. CTest passed **26 tests**, with the
opt-in checkpoint-model test skipped (**81.17 s**); this includes 22 scripted
minimal/candidate cases plus native tool-schema/parser and benchmark-protocol
coverage. The scripted cases exercise net no-op and revert rejection, multi-file
repairs, persistent recovery, reservation enforcement, stale evidence, denied
processes, input mutation, missing tests and final snapshot reuse.

- [Frozen protocol](protocol.json), [identity/population audit](audit.json),
  [fixture preflight](preflight.json), [primary outcomes](outcomes.json), and
  [detailed analysis](analysis.json).
- [Analysis and secondary-check script](analyze.py), [source snapshot](minimal-source.zip),
  [source diff](minimal-source.diff), and [build/test evidence](validation/ctest.txt).
- All prompts, raw model output, tool results, validation reports, edit journals,
  metrics and failed workspaces remain under `cells/`. Raw stderr is preserved
  byte-for-byte; analysis tolerates non-UTF-8 bytes when extracting terminal reasons.
- [Complete evidence archive](complete-evidence.zip) and
  [verified file inventory](evidence-inventory.json). Model weights and runtime
  DLLs are identified by protocol hashes rather than bundled.

The next planned loop experiments remain best-of-N with real-workspace
selection, symbol-impact test targeting, and failure-conditioned reflection.
Repair/preservation, regression, invariant and valid fresh holdout gates remain
outstanding before promotion.
