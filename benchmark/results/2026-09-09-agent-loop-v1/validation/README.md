# Implementation verification

The accepted source build completed the full Windows GPU CTest suite with
**31 passed, one opt-in checkpoint-model test skipped**, in 87.71 seconds.
`loop-accepted-tests.txt` and `build-loop-accepted.txt` are the final build's
logs. Earlier build/test failures and intermediate passing suites are retained
under their original filenames; they are not additional benchmark observations.

The separate Qwen3-Coder native-template probe passed on the local 30B-A3B Q4
model with GPU layers -1 (`loop-qwen-probe.txt`). It exercised forced opening,
cancellation, token exhaustion, recovery, cached parsed-tool equivalence and
generation without a callback. It ran before the final recovery/index/diagnostic
review corrections, which did not change the inference/template implementation.

New contracts cover independent trial contents and real-workspace selection,
protected files, shared budgets, selecting the smaller passing change, rejecting
an isolated-only pass, all-candidate failure, ignored `.forge` directories inside
a real Git repository, bounded reflection, reserved completion, semantic
comment loops and literal changes, persistent native conversations, questions,
EOF/refusal/cancellation, and rejecting questions with multiple candidates.

The content-store unit checks include initially dirty/untracked/binary/empty
files, read-only protection, create/delete, metadata exclusion, policy and full
journal preflight, stale writers, type conflicts, limits and cancellation.
Review added regressions for enough budget to apply but not restore, cancellation
after writes, guarded recovery, and refusal to overwrite external/partial state.
Structured diagnostic tests preserve failure operands while ignoring parsed
source coordinates; unknown diagnostics cannot establish equivalence.

The review found and corrected:

- Missing rollback journal capacity and cleanup after a cancelled final scan.
- Unsigned deadline subtraction after a slow host event callback.
- Parent Git ignore rules hiding source files in trial workspaces.
- Diagnostic hashes derived from clipped summaries and source coordinates.
- User clarification existing only in one discarded candidate's conversation.
  This combination is explicitly rejected until clarifications can be shared.

`loop-serena-configure.txt` records successful compilation database maintenance
after the new CMake sources and test targets were added. Existing Serena
configuration was preserved. The copied runtime used by the pilot is identified
in `../protocol.json`; model weights and runtime DLLs are not bundled here.

The separate pre-review combined `add` smoke passed in 56.641 seconds and is
retained under `../../2026-09-09-agent-loop-smoke`. It used an earlier executable
and is not pooled with the frozen seven-arm matrix.
