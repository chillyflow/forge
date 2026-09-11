# Proposed cache correctness fix

Prepared only in this mirror. The six owned checkout originals were compared byte-for-byte with `originals/` after preparation and remain unchanged. No build, host integration run, model inference, or GPU operation was performed.

Review/apply artifact: `cache-fix.patch` (unified paths rooted at the checkout). `git apply --check` passed against the unchanged checkout. `cache-fix-provenance.json` records each original/proposed SHA-256, byte count, and the patch SHA-256. The initial `provenance.json` also covers unchanged Python dependency copies used to load maintained unit tests. Only the six paths below belong to this patch; other agents' mirror edits are excluded.

| Changed path | Behavior |
|---|---|
| `benchmark/common.py` | Python task verifiers use a fresh external cache prefix and disable bytecode writes through a copied child environment. Original argv and budgets remain unchanged. Policy metadata is explicit for Python and non-Python tasks. |
| `benchmark/run.py` | Root-only `.git`/`.forge` directory exclusions match host case rules; regular files with those names remain. Full cache-inclusive input maps and both pre/post workspace copies are retained. Retain-terminal runs fail when verifier input bytes change. |
| `src/repo/validation.c` | Shared unittest/pytest wrapper sets a fresh external prefix before importing the test runner; exact runner selectors, complete file selection, and zero-test rejection remain. |
| `tests/unit/test_benchmark.py` | Maintained tests cover stale application and test bytecode, timestamp and unchecked-hash modes, both passing/failing current source, per-child environment isolation, root-only retention, and actual verifier mutation with both byte versions retained. |
| `tests/unit/test_validation.c` | Existing Python plan tests verify fresh runner setup while preserving pytest/unittest selectors and command counts. |
| `tests/integration/test_loop_extensions.py` | Three scripted host regressions reject stale passing module/test bytecode and accept repaired source despite stale failing bytecode. Pending a new binary. |

The declared result contract is:

```text
verification.python_cache_policy = fresh-external-prefix-no-write | not-applicable
terminal_retention_policy = complete-except-git-forge
pre_verification_workspace = pre-verification-workspace
terminal_workspace = terminal-workspace
verification_inputs_unchanged = exact full-map equality
```

`complete-except-git-forge` excludes directories with these names only at the workspace root, case-insensitively on Windows and case-sensitively elsewhere. Regular root files named `.git`/`.forge`, cache bytes, and nested metadata directories remain. `verification-inputs.json` retains full before/after maps and protected before/after maps. Source freshness does not depend on deleting any input cache artifact. The acceptance agent was informed of these exact fields and the directory-only refinement.

Executed from the checkout root:

```powershell
python -B benchmark/results/2026-09-09-agent-loop-all-green/proposed-candidate-03/tests/unit/test_benchmark.py VerificationCacheTests FixtureTests.test_prompt_protocol_is_forwarded_and_recorded_per_run FixtureTests.test_prompt_protocol_rejects_unknown_arm FixtureTests.test_native_protocol_rejects_routed_decode_variants FixtureTests.test_protocol_freeze_records_prompt_arm_in_hash
git apply --check benchmark/results/2026-09-09-agent-loop-all-green/proposed-candidate-03/cache-fix.patch
```

Nine tests passed in 2.928 seconds, including the directory-only exclusion refinement and preservation of regular root metadata-named files in both original and uppercase spellings. Verification subprocesses were real; only resource monitoring and fake Forge/platform bookkeeping in harness unit cases were mocked. Python source files were also compiled in memory to check syntax without cache writes.

The exact C string from the mirrored `vp_python_test_script` was extracted and executed directly with Python, without compiling Forge. Targeted and full explicit unittest file selection correctly failed bad source and passed repaired source; zero discovered tests returned failure. No bytecode was written to the fixture. Results are in `cache-wrapper-checks.json`. Pytest is unavailable in this interpreter, so pytest runtime behavior and the three host integration cases remain untested until the full build/test phase. The existing maintained pytest plan tests were extended for that phase; no CMake target was added.

`src/core/agent.c` and `tests/integration/test_bounded_repair.py` are owned by the coordinator and were not edited by this cache-fix task.
