# Python verification cache audit

Read-only audit of frozen candidate-02, 2026-09-09. No frozen source/runtime edits, build, model inference, or GPU operations were performed. Reproduction fixtures were created in external temporary directories. This note and its executable evidence are outside the frozen source identity.

## Finding and classification

There is a demonstrated correctness hole in both independent benchmark verification and host automatic Python validation. A valid `.pyc` containing passing code can be loaded while the current `.py` still fails. `-B` prevents bytecode writes; it does not prevent bytecode reads. A stable full workspace hash proves which bytes were present, but does not establish that source rather than stale bytecode ran.

This is an acceptance-evidence defect, distinct from the pending quality experiment about historical checkpoint wording. It must be fixed before relying on green results from a new candidate. It does **not** establish that any existing recorded candidate-02 result is wrong. Fresh-source rechecks of the completed workspaces inspected in this audit preserved their outcomes.

The separate omission of `__pycache__` and `.pytest_cache` from benchmark input maps and terminal archives is an evidence-retention defect. `.pyc` is the demonstrated execution issue. There is no evidence here that pytest's result cache produced any observed false pass.

## Implementation evidence

- `benchmark/common.py:446`, `verify_task`, runs the original manifest command through `run_monitored`, passes its optional environment through unchanged, and treats exit code zero as pass. There is no source-freshness mechanism.
- `benchmark/common.py:417`, `run_monitored`, already passes `env` directly to `subprocess.Popen`. A copied per-child environment can fix independent verification without changing process-global environment or the other scheduled runs.
- `benchmark/run.py:209` and `:217` exclude any path component named `.git`, `.forge`, `__pycache__`, or `.pytest_cache` from pre/post verifier maps. `:238` applies the same recursive exclusions to terminal copies. Thus cached execution can pass, the filtered maps can compare equal, and the retained terminal can execute differently.
- `src/core/input_snapshot.c:80` excludes `.git` and `.forge` only when the relative prefix is empty. The names are case-insensitive on Windows and case-sensitive elsewhere. Cache bytes and nested `.git`/`.forge` bytes are otherwise included. The benchmark must match these root-only semantics.
- `src/repo/validation.c:610` compiles source bytes for syntax checks, which is appropriate. Its unittest runner at `:659` imports unittest and application code with `python -B -c ...`; targeted and broad pytest at `:665`/`:699` use `python -B -m pytest -q -p no:cacheprovider`. All test runners may read existing bytecode.
- `src/core/verification.c:292`/`:364` executes the planned argv via `fg_process_at`, then uses input snapshots around validation. Bytecode can be added before validation and remain unchanged throughout it. This was demonstrated end-to-end.
- The locally installed CPython 3.11.9 implementation at `C:/Users/flowc/AppData/Local/Programs/Python/Python311/Lib/importlib/_bootstrap_external.py` validates timestamp/size or hash headers and loads cached code before its `sys.dont_write_bytecode` write guard. This matches the observed behavior.

Inspected Python manifest commands in the task/holdout/repair suites were unittest discovery (22 manifests across the inspected sets, including duplicates): 7 with `-s tests -v`, 15 with `discover -v`. No pytest manifest verifier was found in those sets. Host pytest planning still needs the same fix.

## Reproducible evidence

The executable [test_verification_cache.py](test_verification_cache.py) contains seven tests. `--demonstrate` asserts the frozen defects; its default mode asserts the corrected behavior and agreed metadata. [verification-cache-repro.json](verification-cache-repro.json) retains commands, stdout/stderr, source/cache hashes, input maps, host events, metrics, and the exact scripted actions.

Executed from the repository root:

```powershell
python -B benchmark/results/2026-09-09-agent-loop-all-green/candidate-02/diagnosis/test_verification_cache.py --demonstrate --forge benchmark/results/2026-09-09-agent-loop-all-green/candidate-02/runtime/forge.exe --evidence benchmark/results/2026-09-09-agent-loop-all-green/candidate-02/diagnosis/verification-cache-repro.json
```

Result: seven tests passed in 2.252 seconds. This is a successful reproduction of defects, not a green correctness test. ResourceMonitor alone was replaced with a no-op to avoid GPU/process sampling; real verifier subprocesses ran. The harness retention test mocks its fake Forge process and Git/platform bookkeeping, but calls the real independent verifier. The host test uses the actual frozen executable with the script backend and no model.

| Case | Frozen behavior | Required behavior |
|---|---|---|
| Failing source plus timestamp-valid passing `.pyc` | Independent verifier passes one test | Fail the actual source test |
| Failing source plus unchecked-hash passing `.pyc` | Independent verifier passes one test | Fail the actual source test |
| Same cached fixtures with `python -B` only | Still pass | Demonstrates that `-B` alone is insufficient |
| Same fixtures, fresh external prefix and writes disabled | Fail one test correctly | Preserve |
| Only a passing cache added through `run_command`; source/test unchanged | Host validates and accepts final | Reject checkpoint/final until source passes |
| Cache and nested metadata retained by harness | Omitted from maps and terminal | Preserve exact bytes in both snapshots |
| Caller supplied stale prefix | Propagated to verifier | Override for this Python child only |

The host executable SHA-256 is `fa3b099c442bba1c422959de98e42b88b4214948e0e0fe2d939617b01cd99de9`. The source remains `def value():\n    return 1\n` (SHA-256 `81c39670c4b19842f26b519fe197fe5406ed038ecfb8d605297076ef3dbda9ba`). Its test expects 2. A scripted `run_command` compiles temporary good code into the input's timestamp-valid bytecode path, followed by `validate_candidate` and `final`. The frozen run returns zero, reports a passing checkpoint, and emits final without repairing source or changing the test. Its fresh-source recheck fails. The retained JSON contains the exact cache fabrication command and final events.

The proposed host wrapper is also executable as `FRESH_RUNNER` in the regression file. It was checked against stale passing bytecode and then repaired source, using both targeted unittest selection and the full explicit unittest file list. Failure and success behave correctly and validation leaves all input bytes unchanged. Pytest is unavailable in this audit interpreter, and its wrapper subcases are explicitly recorded as skipped; do not report them as tested.

## Concrete minimal patch proposal (not applied)

### Independent verifier: keep the original argv and budgets

In `benchmark/common.py`, import `tempfile` and `contextlib.nullcontext`. Scope freshness to declared Python tasks (`task.get('language') == 'python'`), which covers the inspected manifests and preserves non-Python profiles. Keep `run_monitored` unchanged. Replace the environment preparation in `verify_task` with this structure:

```python
python_task = task.get('language') == 'python'
cache_scope = (tempfile.TemporaryDirectory(prefix='forge-python-verification-')
               if python_task else nullcontext(None))
with cache_scope as cache:
    child_env = env
    if python_task:
        child_env = dict(os.environ if env is None else env)
        child_env['PYTHONPYCACHEPREFIX'] = cache
        child_env['PYTHONDONTWRITEBYTECODE'] = '1'
    # Existing stdout/stderr file contexts remain inside this scope.
    result = run_monitored(task['verify'], cwd=root, env=child_env,
                           stdout=stdout, stderr=stderr, timeout=timeout,
                           gpu_index=gpu_index, extra_pids=extra_pids)
result['passed'] = result['returncode'] == 0
result['python_cache_policy'] = ('fresh-external-prefix-no-write'
                                 if python_task else 'not-applicable')
```

The prefix is outside the workspace, fresh for each verification subprocess, and cleaned after the subprocess finishes. Never modify `os.environ` or the caller's `env` dictionary. Do not replace the manifest argv, remove tests, add `-I`, or increase timeout/token budgets. Metadata records the policy, not the caller's full environment. A future profile with a Python verifier but different declared language needs explicit policy coverage; do not infer unrestricted interpreter support from these inspected profiles.

### Host: fresh prefix before importing the test runner

No process-environment API expansion is needed. In `src/repo/validation.c`, use a shared Python test wrapper whose complete runnable text is `FRESH_RUNNER` in the regression file. It imports `sys,tempfile`, enters an external `TemporaryDirectory`, assigns `sys.pycache_prefix`, sets `sys.dont_write_bytecode=True`, and only then imports unittest or pytest. Preserve the existing unittest success and nonzero-tests requirement:

```python
with tempfile.TemporaryDirectory(prefix='forge-python-validation-') as cache:
    sys.pycache_prefix = cache
    sys.dont_write_bytecode = True
    runner = sys.argv.pop(1)
    sys.argv[0] = runner
    if runner == 'pytest':
        import pytest
        status = pytest.main(sys.argv[1:])
    else:
        import unittest
        p = unittest.main(module=None,
                          argv=['unittest', '-v', *sys.argv[1:]], exit=False)
        status = not p.result.wasSuccessful() or p.result.testsRun == 0
sys.exit(status)
```

Generate targeted pytest argv as `[python, '-B', '-c', wrapper, 'pytest', '-q', '-p', 'no:cacheprovider', ...existing_targets]`; prefix count changes from 7 to 8. Broad pytest uses the same eight prefix elements, without targets. Generate unittest argv as `[python, '-B', '-c', wrapper, 'unittest', ...existing_files]`; prefix count changes from 4 to 5. The runner selector is an explicit argv element, so existing plan lookups for the exact `pytest` token remain meaningful. Keep the syntax stage reading/compiling source bytes, existing target batching, all broad files, zero-test rejection, validation timeouts, and validation command count. Do not replace the host's explicit unittest file list with discovery: that changes its namespace/coverage behavior.

### Evidence retention: source freshness and byte retention are separate

In `benchmark/run.py`, replace both map comprehensions and recursive ignore patterns with shared root-only helpers. For a relative path, omit iff its first component equals `.git` or `.forge` (use `.casefold()` on Windows only). Include `__pycache__`, `.pytest_cache`, all other ordinary input bytes, and nested `.git`/`.forge` directories. `copytree`'s ignore callback must return those two names only when called at the workspace root; return an empty set for descendants.

When `--retain-terminal` is active:

1. Capture the full pre-verifier map and copy those inputs to `pre-verification-workspace` before running the verifier.
2. Run the original independent verification with the freshness environment above.
3. Capture the full post-verifier map and copy those inputs to `terminal-workspace`.
4. Keep both maps in `verification-inputs.json` and record `verification_inputs_unchanged` from exact map equality.
5. Record `terminal_retention_policy = 'complete-except-git-forge'`. Its defined semantics are root-only exclusion, as above.

Both copies are necessary to retain pre-verifier bytes when a rejected verifier run mutates inputs. A hash alone cannot recover old bytes. Do not strip or delete existing cache artifacts to obtain fresh execution. A fresh prefix plus disabled writes prevents Python cache pollution without altering candidate input bytes.

The acceptance driver must archive both directories and require the policy fields and unchanged input maps for new candidates. Parent and acceptance agent were notified of the agreed contract. Previous frozen artifacts remain unchanged and must not be silently relabeled with the new policy.

## Regression integration and expected fixture impact

After implementation, run the retained regression file without `--demonstrate`, pointing `--forge` at the new executable. Current frozen code should fail that mode. Port its independent/environment/retention cases into `tests/unit/test_benchmark.py`; port its host bytecode-only scripted case into bounded-repair or verification integration coverage. The repro uses an external script file, so script bytes themselves do not satisfy the host's workspace-change requirement.

The separate reusable mutation test runs an actual verifier command that overwrites `.pytest_cache/audit`: before and after maps must differ, `pre-verification-workspace` must retain original bytes, and `terminal-workspace` must retain new bytes. Frozen behavior incorrectly marks the filtered maps unchanged; demonstration mode reproduces that defect. Default regression mode requires the differing maps and both byte versions. Acceptance must reject this case despite verifier exit zero. The unchanged retention case also asserts the pre-copy matches all pre-input bytes, plus exact preservation of both cache categories and nested metadata directories.

Add a genuine host source-repair success case with stale failing bytecode as the reverse control, plus pytest targeted/broad runtime coverage when pytest is available. Preserve the existing zero-test failure test in `tests/unit/test_verification.c` and run it after wrapper changes. Proposed-wrapper unittest targeted/full-list failure/success is already exercised by the retained script.

`tests/unit/test_validation.c` contains affected command-plan assertions in `test_python_pytest_plan` (~596), `test_python_pytest_with_unittest_mock` (~648), `test_python_pytest_configuration_only_pattern` (~672), and `test_python_unittest_plan` (~700). With the explicit runner-selector argv design, exact `pytest`, `-B`, `no:cacheprovider`, `unittest.main`, and `testsRun==0` checks remain valid. Extend the tests to inspect the fresh-prefix wrapper and `-c`; update any fixed prefix length/index checks added elsewhere. Keep command counts and full explicit unittest file targets unchanged. If using a wrapper design that removes the exact `pytest` argv token instead, those `command_for(..., 'pytest')` and `array_has(..., 'pytest')` assertions must be updated deliberately; they use exact token equality, not substring matching.

## Existing candidate-02 outcome scope

Earlier in this audit, exact manifest verification was rerun on external disposable copies of the two then-completed Python terminal workspaces, with a fresh external prefix, writes disabled, and resource monitoring disabled. No candidate input was mutated. The manifest command for both was `['python', '-m', 'unittest', 'discover', '-v']`; the protected test hashes matched their recorded maps.

| Completed run | Recorded outcome | Fresh source | Test count | Canonical terminal file-map SHA-256 |
|---|---|---|---:|---|
| `generalize_retractions_distractor` s42 r001 | PASS | PASS | 2 | `9ee3b4c5c2050683bb58d70da6f6ad02ae81e20b8859a78023a20e8127010408` |
| `generalize_retractions_renamed` s42 r001 | FAIL | FAIL, same `{'cash': 7} != {'cash': 0}` | 2 | `4317f35ca34e1960617a339798a4048e1e5d809b0d470c08371dfba568e018e3` |

Canonical maps encode sorted relative-path/SHA-256 pairs as JSON with `sort_keys=True, separators=(',', ':')`, then hash the UTF-8 bytes. Distractor files were `legacy.py`, `service.py`, and `test_service.py`; its protected test SHA-256 was `f14484d479119112e8476fc2f4cb0f9b658007fad57e1057ad6ca326cd2f75ce`. Renamed's terminal included `ledger.py`, `test_ledger.py`, `debug_test.py`, `debug_test2.py`, and `simple_test.py`; its protected test SHA-256 was `091ae039f2d71ded9ac038e3ea09116a4ebde8f71e1b3d83009bfaa90f671dff`. The failed-workspace copy for renamed retained bytecode, while the terminal copy omitted it. Fresh terminal execution still reproduced failure.

The subsequent `generalize_retractions_original` r001 PASS is being fresh-copy checked by the separate failure-evidence agent; this audit did not duplicate that check. These supplemental executions are diagnostics, not replacements for scheduled G1 runs or evidence that the frozen harness already provided the corrected contract. Keep the actual recorded outcomes and their original provenance.
