# I1/I2 review-gap test fixups — implementation report

Repo: C:/Users/flowc/dev/forge, branch main, HEAD d16e6736. The uncommitted I1/I2 working tree was
left intact; nothing committed or pushed. Test-evidence fixups only: the only source-code change is
a linkage rename with no behavior change, plus one comment fix. All new assertions live in committed
test files and run in the standard ctest suite with no model and no GPU.

## Build and test results

- Build: `export PYTHONPATH='C:/Users/flowc/dev/forge/.tools'; .tools/bin/cmake.exe --build build-gpu --config Release --parallel`
  → **exit 0**. A forced recompile of the three touched test files produced no warnings or errors.
- Affected binaries run directly:
  - `forge_unit` → "core tests passed", exit 0
  - `forge_process_unit` → "process tests passed", exit 0
  - `forge_agent_changes_unit` → "Agent known-change and edit-evidence tests passed (no watch delivery)", exit 0
- Full suite: `export PATH='<go bin>:$PATH'; .tools/bin/ctest.exe --test-dir build-gpu -C Release --output-on-failure`
  → **100% tests passed out of 36** (`checkpoint_model` Skipped by design: SKIP_RETURN_CODE 77, needs a
  real model argument). Log: `ctest_fixups_final.log` in this directory.
- Environment note (not a code issue): the first ctest attempt ran under a PATH that had lost its
  system entries (the session `$PATH` expansion collapsed), so python/git/go were unresolvable and 5
  PATH-dependent tests failed (`minimal_agent_integration`, `loop_extensions`, `bounded_repair`,
  `repair_control` with `python_executable_unavailable` / git `FileNotFoundError`, and `impact`
  whose process probe could not spawn). Re-run with an explicit full PATH (go bin first, as the task
  env requires) → 36/36 pass; all five are green in the clean run.

## Per review finding

### 1. Path-normalization skip branch (src/core/text.c:168-179) — tests/unit/test_core.c:100-126

Added to `test_workspace_path_normalization`, all with byte-exact `strcmp` expectations:

- (a) Dotless, backslash-free root `C:/work/tmp/forge-bench-ab12cd` with plain text:
  `Traceback: File "<root>/test.py", line 3` → `Traceback: File "./test.py", line 3`.
- (b) The same root with its escaped replica in the text (escaped JSON spelling as command output
  reaches the model; a backslash-free root has nothing to double, so the replica is the literal
  bytes): `File \"<root>/test.py\" and \"<root>/other.py\"` → `File \"./test.py\" and \"./other.py\"`.
  Both occurrences replaced by the single (skipped-second) pass.
- (c) Roots containing '.', pinning the retained two-pass path: `/a/b.c` on `/a/b/a/b.cc` → `"."`
  (pass one re-forms the root: replacement `.` + trailing `c`); and the refuter's relative case
  `./proj` on `./proj/proj` → `"."` (a guard dropping the '.' check would return the un-collapsed
  form).

### 2. Tool-output token invariant (src/core/agent.c:3654-3696) — tests/unit/test_agent_changes.c

The scripted-model harness in test_agent_changes.c drives the real agent loop with no model and no
GPU, so the invariant is asserted there.

- New helper `accumulate_tool_output` (line 281) takes a fresh `fg_model_count` of the final visible
  bytes from each `tool_result` event and of the raw capture read back from the session artifact
  `tool/%06zu.raw`; `on_event` calls it (line 317).
- Byte-identical path, in `run_case` (lines 499-506): `visible_tool_tokens == sum(fresh count of
  event outputs)`, `visible_tool_bytes == sum(lengths)`, `raw_tool_tokens == sum(fresh count of raw
  artifacts)`, `raw_tool_bytes == sum(artifact lengths)`, plus `visible_tool_tokens ==
  raw_tool_tokens` (the reuse path). This runs across all 11 `run_case` variants (indexed /
  backslash / cancel / retrieve).
- Truncated/marker path: new `run_tool_token_metrics_truncated` (line 929), a scripted run with
  `max_tool_bytes = 64` below the read output. Its callback asserts the
  `[truncated; use expand_output]` marker is present, then the same equality assertions (lines
  967-972), plus `visible_tool_tokens < raw_tool_tokens` proving the reduction is real. Called from
  `main` (line 1019), so it runs in the standard `agent_changes` ctest test (GPU-free, no model).

### 3. Signature equivalence (tests/unit/test_core.c:450-456)

`fg_tool_signatures("read_file", args, 4, 7, &paired, &strategy)` must equal the two
single-variant hashes: `paired == fg_tool_signature(name, args, 4, 7)` and `strategy ==
fg_tool_signature(name, args, 0, 0)`.

### 4. quote_arg edge cases — tests/unit/test_process.c:336-374

Ported from the I1 scratch differential test (`quote_expected`/`quote_diff_test.c` in this
directory), byte-exact via a generated table: empty argument (`""` → `"\"\""`), embedded quotes
(`a"b`, `say "hi"`, lone `"`), interior backslashes (Windows path, unchanged), trailing backslash
runs (1, 3 and 4 backslashes → doubled before the closing quote), and backslashes before an embedded
quote (`mid\\"end` → 5 backslashes + escaped quote). Each call fills exactly the capacity
`command_line` reserves (bound `2n+3`) and a 32-byte guard zone after it must stay untouched.

Enabling change: `quote_arg` was file-static and unreachable from the test binary, so it is renamed
`fg_process_quote_arg` and declared in src/internal.h:69-73 under `#ifdef _WIN32`. No behavior
change (same body, same single call site in `command_line`, src/core/process.c:217). The test is
`#ifdef _WIN32` (the function is Win32-only), matching the function's own guard.

### 5. Comment fix (src/core/agent.c:3655-3657)

"The default view is the raw capture itself" → "The default view is a byte-identical copy of the raw
capture; its token count is known, so the budget loop below need not re-count the same bytes."

## Bite-check (test actually fails when the invariant breaks)

Temporarily replaced the reused count with `visible_tokens = raw_tokens` in agent.c (the exact
regression the review suspects), rebuilt, and ran `forge_agent_changes_unit`: it failed at
`tests/unit/test_agent_changes.c:968` — `Assertion failed: metrics->visible_tool_tokens ==
f.visible_tokens_sum && metrics->visible_tool_bytes == f.visible_bytes_sum`, exit 3. The break was
reverted and agent.c verified back to its original SHA-256
(`5c8b22f762bdcd0a44a12b64fc79a30d8b5e491f604eeae871c7fc5f39ca1b76`); rebuild and re-run pass.

## Files modified

- src/core/agent.c (comment only), src/core/process.c (rename), src/internal.h (declaration)
- tests/unit/test_core.c (+33), tests/unit/test_process.c (+42), tests/unit/test_agent_changes.c (+100)
