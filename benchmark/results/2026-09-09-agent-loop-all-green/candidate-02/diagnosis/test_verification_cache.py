"""Executable cache-freshness evidence and proposed behavioral regressions.

Run with Python -B. Default mode asserts the fixed contract; --demonstrate asserts
the frozen candidate-02 defects. All fixture/model placeholders live in external
temporary directories. Only ResourceMonitor and the harness's fake Forge process
are mocked: independent verification and optional scripted Forge really execute.
No model or GPU is used. This file is intentionally outside frozen source.
"""

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import py_compile
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
PARSER = argparse.ArgumentParser(description=__doc__)
PARSER.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[5])
PARSER.add_argument('--forge', type=Path)
PARSER.add_argument('--evidence', type=Path)
PARSER.add_argument('--demonstrate', action='store_true')
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
sys.path.insert(0, str(ARGS.repo / 'benchmark'))
import common  # noqa: E402

SPEC = importlib.util.spec_from_file_location('cache_audit_benchmark_run', ARGS.repo / 'benchmark/run.py')
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)
EVIDENCE = {'mode': 'demonstrate' if ARGS.demonstrate else 'regression',
            'python': sys.version, 'cases': {}}
BAD = b'def value():\n    return 1\n'
GOOD = b'def value():\n    return 2\n'
TEST = ('import unittest\nfrom value import value\n'
        'class ValueTests(unittest.TestCase):\n'
        '    def test_value(self):\n        self.assertEqual(value(), 2)\n')
FRESH_RUNNER = """import sys,tempfile
with tempfile.TemporaryDirectory(prefix='forge-python-validation-') as cache:
    sys.pycache_prefix=cache
    sys.dont_write_bytecode=True
    runner=sys.argv.pop(1)
    sys.argv[0]=runner
    if runner=='pytest':
        import pytest
        status=pytest.main(sys.argv[1:])
    else:
        import unittest
        p=unittest.main(module=None,argv=['unittest','-v',*sys.argv[1:]],exit=False)
        status=not p.result.wasSuccessful() or p.result.testsRun==0
sys.exit(status)
"""


class NoMonitor:
    def __init__(self, **_kwargs):
        pass

    def add_root(self, _pid):
        pass

    def start(self):
        pass

    def stop(self):
        return {'audit_monitor': 'disabled; no GPU or process sampling'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def files(root):
    return {p.relative_to(root).as_posix(): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}


def seed_stale(root, mode=py_compile.PycInvalidationMode.TIMESTAMP):
    """A valid passing cache paired with equally sized failing current source."""
    source = root / 'value.py'
    source.write_bytes(GOOD)
    (root / 'test_value.py').write_text(TEST, encoding='utf-8', newline='\n')
    timestamp = source.stat()
    pyc = Path(py_compile.compile(str(source), doraise=True, invalidation_mode=mode))
    source.write_bytes(BAD)
    os.utime(source, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
    return pyc


def task():
    return {'id': 'cache_audit', 'language': 'python', 'prompt': 'Repair value without changing tests.',
            'files': {'value.py': BAD.decode(), 'test_value.py': TEST},
            'protected': ['test_value.py'],
            'verify': [sys.executable, '-m', 'unittest', 'discover', '-v']}


def verify(root, output, manifest=None, env=None):
    output.mkdir()
    with mock.patch.object(common, 'ResourceMonitor', NoMonitor):
        result = common.verify_task(root, manifest or task(), output, env=env, timeout=15)
    return {**result, 'stdout': (output / 'verification.stdout').read_text(encoding='utf-8'),
            'stderr': (output / 'verification.stderr').read_text(encoding='utf-8')}


def fresh_env(cache):
    return {**os.environ, 'PYTHONPYCACHEPREFIX': str(cache), 'PYTHONDONTWRITEBYTECODE': '1'}


def call(name, arguments=None):
    return {'role': 'assistant', 'content': '', 'tool_calls': [
        {'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments or {})}}]}


class CacheFreshnessTests(unittest.TestCase):
    def test_proposed_runner_uses_current_source_for_targeted_and_broad_tests(self):
        for runner in ('unittest', 'pytest'):
            if runner == 'pytest' and importlib.util.find_spec('pytest') is None:
                EVIDENCE['cases']['proposed_wrapper_pytest'] = {'skipped': 'pytest is not installed in audit Python'}
                continue
            for broad in (False, True):
                with self.subTest(runner=runner, broad=broad), \
                        tempfile.TemporaryDirectory(prefix='forge-cache-wrapper-audit-') as tmp:
                    root = Path(tmp)
                    seed_stale(root)
                    (root / 'test_other.py').write_text(
                        'import unittest\nclass Other(unittest.TestCase):\n    def test_other(self):\n        self.assertTrue(True)\n',
                        encoding='utf-8')
                    before = files(root)
                    targets = ['./test_value.py', *(['./test_other.py'] if broad else [])] if runner == 'unittest' \
                        else ['-q', '-p', 'no:cacheprovider', *([] if broad else ['./test_value.py'])]
                    command = [sys.executable, '-B', '-c', FRESH_RUNNER, runner, *targets]
                    failing = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=15)
                    self.assertNotEqual(failing.returncode, 0, failing.stdout + failing.stderr)
                    self.assertEqual(files(root), before, 'Validation must not alter any workspace input')
                    # Keep the existing cache and make current source pass.
                    (root / 'value.py').write_bytes(GOOD)
                    passing = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=15)
                    self.assertEqual(passing.returncode, 0, passing.stdout + passing.stderr)
                    EVIDENCE['cases'][f'proposed_wrapper_{runner}_{broad}'] = {
                        'command': command, 'stale_cache_current_failure': {
                            'returncode': failing.returncode, 'stdout': failing.stdout, 'stderr': failing.stderr},
                        'current_source_pass': {'returncode': passing.returncode,
                            'stdout': passing.stdout, 'stderr': passing.stderr}}

    def test_independent_verifier_rejects_stale_timestamp_and_unchecked_hash(self):
        for mode in (py_compile.PycInvalidationMode.TIMESTAMP,
                     py_compile.PycInvalidationMode.UNCHECKED_HASH):
            with self.subTest(mode=mode.name), tempfile.TemporaryDirectory(prefix='forge-cache-audit-') as tmp:
                base = Path(tmp)
                root = base / 'input'
                root.mkdir()
                pyc = seed_stale(root, mode)
                before = files(root)
                ordinary = verify(root, base / 'ordinary')
                with tempfile.TemporaryDirectory(prefix='forge-cache-fresh-') as cache:
                    fresh = verify(root, base / 'fresh', env=fresh_env(cache))
                plain_b = subprocess.run([sys.executable, '-B', '-m', 'unittest', 'discover', '-v'],
                                         cwd=root, capture_output=True, text=True, timeout=15)
                case = {'command': task()['verify'], 'inputs_before': before,
                        'cache': pyc.relative_to(root).as_posix(),
                        'ordinary': ordinary, 'fresh_external_prefix': fresh,
                        'B_only': {'returncode': plain_b.returncode, 'stderr': plain_b.stderr}}
                EVIDENCE['cases'][f'independent_{mode.name.lower()}'] = case
                self.assertRegex(ordinary['stderr'], r'Ran 1 test\b')
                self.assertEqual(ordinary['passed'], ARGS.demonstrate, ordinary)
                if not ARGS.demonstrate:
                    self.assertEqual(ordinary['python_cache_policy'], 'fresh-external-prefix-no-write')
                self.assertFalse(fresh['passed'], fresh)
                self.assertRegex(fresh['stderr'], r'Ran 1 test\b')
                self.assertEqual(plain_b.returncode, 0, plain_b.stderr)
                self.assertEqual((root / 'value.py').read_bytes(), BAD)
                self.assertEqual(sha(pyc), before[pyc.relative_to(root).as_posix()])

    def test_child_environment_is_fresh_external_and_caller_is_unchanged(self):
        with tempfile.TemporaryDirectory(prefix='forge-cache-env-audit-') as tmp:
            base = Path(tmp)
            root = base / 'input'
            root.mkdir()
            code = ('import json,os,sys;print(json.dumps(dict(prefix=sys.pycache_prefix,'
                    'disabled=sys.dont_write_bytecode,sentinel=os.environ["CACHE_AUDIT_SENTINEL"])))')
            manifest = {'language': 'python', 'verify': [sys.executable, '-c', code]}
            caller = {**os.environ, 'PYTHONPYCACHEPREFIX': str(root / 'old-cache'),
                      'PYTHONDONTWRITEBYTECODE': '', 'CACHE_AUDIT_SENTINEL': 'preserved'}
            original_caller, original_process = caller.copy(), os.environ.copy()
            result = verify(root, base / 'output', manifest, caller)
            child = json.loads(result['stdout'])
            EVIDENCE['cases']['child_environment'] = {'command': manifest['verify'], **result, 'child': child}
            self.assertTrue(result['passed'])
            self.assertEqual(caller, original_caller)
            self.assertEqual(dict(os.environ), original_process)
            self.assertEqual(child['sentinel'], 'preserved')
            if ARGS.demonstrate:
                self.assertEqual(child['prefix'], caller['PYTHONPYCACHEPREFIX'])
                self.assertFalse(child['disabled'])
            else:
                self.assertEqual(result['python_cache_policy'], 'fresh-external-prefix-no-write')
                prefix = Path(child['prefix']).resolve()
                self.assertFalse(prefix.is_relative_to(root.resolve()))
                self.assertNotEqual(str(prefix), caller['PYTHONPYCACHEPREFIX'])
                self.assertTrue(child['disabled'])
                self.assertFalse(prefix.exists(), 'Temporary verifier cache must be cleaned up')

    def test_non_python_verifier_keeps_environment(self):
        with tempfile.TemporaryDirectory(prefix='forge-cache-other-audit-') as tmp:
            base = Path(tmp)
            # Python is just an env reporter here; the declared task is non-Python.
            manifest = {'language': 'go', 'verify': [sys.executable, '-c',
                'import os;print(os.environ["PYTHONPYCACHEPREFIX"]);print(os.environ["CACHE_AUDIT_SENTINEL"])']}
            env = {**os.environ, 'PYTHONPYCACHEPREFIX': str(base / 'caller-cache'),
                   'CACHE_AUDIT_SENTINEL': 'unchanged'}
            original = env.copy()
            result = verify(base, base / 'output', manifest, env)
            EVIDENCE['cases']['non_python_environment'] = result
            self.assertEqual(result['stdout'].splitlines(), [env['PYTHONPYCACHEPREFIX'], 'unchanged'])
            self.assertEqual(env, original)
            if not ARGS.demonstrate:
                self.assertEqual(result['python_cache_policy'], 'not-applicable')

    def test_terminal_evidence_preserves_cache_and_nested_metadata_bytes(self):
        self._terminal_case(mutate=False)

    def test_terminal_evidence_preserves_both_versions_of_verifier_mutation(self):
        self._terminal_case(mutate=True)

    def _terminal_case(self, mutate):
        with tempfile.TemporaryDirectory(prefix='forge-cache-harness-audit-') as tmp:
            base = Path(tmp)
            forge, model = base / 'fake-forge.exe', base / 'fake.gguf'
            forge.write_bytes(b'fake forge: never executed')
            model.write_bytes(b'fake model: never loaded')
            output = base / 'output'
            seeded = {}
            manifest = task()
            if mutate:
                manifest['verify'] = [sys.executable, '-c',
                    "from pathlib import Path;Path('.pytest_cache/audit').write_bytes(b'changed by verifier')"]

            def fake_forge(command, **_kwargs):
                root = Path(command[command.index('--workspace') + 1])
                # Seed only application code; the real protected fixture remains untouched.
                source = root / 'value.py'
                source.write_bytes(GOOD)
                timestamp = source.stat()
                pyc = Path(py_compile.compile(str(source), doraise=True))
                source.write_bytes(BAD)
                os.utime(source, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
                for name in ('.pytest_cache/audit', 'nested/.forge/audit', 'nested/.git/audit'):
                    p = root / name
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(b'pre-verification input bytes\x00\xff')
                seeded.update({name: digest for name, digest in files(root).items()
                               if name.startswith(('.pytest_cache/', 'nested/'))})
                seeded[pyc.relative_to(root).as_posix()] = sha(pyc)
                return {'returncode': 0, 'wall_seconds': .01, 'resource_usage': {}}

            argv = ['run.py', '--forge', str(forge), '--model', str(model), '--output', str(output),
                    '--no-randomize', '--retain-terminal']
            with mock.patch.multiple(BENCH, load_tasks=mock.Mock(return_value=[(base / 'task.json', manifest)]),
                                     check_tools=mock.Mock(), initialize_git=mock.Mock(),
                                     runtime_bundle=mock.Mock(return_value={'files': []}),
                                     platform_metadata=mock.Mock(return_value={'platform': 'audit'}),
                                     run_monitored=fake_forge), \
                    mock.patch.object(common, 'ResourceMonitor', NoMonitor), \
                    mock.patch.object(BENCH.subprocess, 'check_output', return_value='forge audit'), \
                    mock.patch.object(BENCH.subprocess, 'run'), \
                    mock.patch.object(BENCH.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()):
                returncode = BENCH.main()
            case = output / 'cache_audit-optimized-r001'
            maps = json.loads((case / 'verification-inputs.json').read_text(encoding='utf-8'))
            record = json.loads((case / 'result.json').read_text(encoding='utf-8'))
            terminal = files(case / 'terminal-workspace')
            EVIDENCE['cases'][f'terminal_retention_mutation_{mutate}'] = {'command': argv, 'seeded': seeded,
                    'input_maps': maps, 'terminal': terminal, 'result': record}
            if not mutate:
                self.assertEqual(returncode, 0 if ARGS.demonstrate else 1)
                self.assertEqual(record['passed'], ARGS.demonstrate)
            else:
                self.assertTrue(record['verification']['passed'])
            self.assertTrue(record['protected_files_unchanged'])
            self.assertEqual(record['verification_inputs_unchanged'], ARGS.demonstrate or not mutate)
            if not ARGS.demonstrate:
                self.assertEqual(record['terminal_retention_policy'], 'complete-except-git-forge')
                self.assertEqual(record['verification']['python_cache_policy'], 'fresh-external-prefix-no-write')
                self.assertEqual(files(case / 'pre-verification-workspace'), maps['before'])
            for name, digest in seeded.items():
                for stage, mapping in (('before', maps['before']), ('after', maps['after']), ('terminal', terminal)):
                    if ARGS.demonstrate:
                        self.assertNotIn(name, mapping)
                    else:
                        expected = hashlib.sha256(b'changed by verifier').hexdigest() \
                            if mutate and stage != 'before' and name == '.pytest_cache/audit' else digest
                        self.assertEqual(mapping.get(name), expected, name)

    @unittest.skipUnless(ARGS.forge, 'pass --forge for actual script-backend host validation')
    def test_host_rejects_passing_bytecode_without_source_repair(self):
        with tempfile.TemporaryDirectory(prefix='forge-cache-host-audit-') as tmp:
            base = Path(tmp)
            root = base / 'input'
            root.mkdir()
            (root / 'value.py').write_bytes(BAD)
            (root / 'test_value.py').write_text(TEST, encoding='utf-8', newline='\n')
            before = files(root)
            poison = (
                "import pathlib,os,py_compile,importlib.util,tempfile;"
                "p=pathlib.Path('value.py');t=tempfile.NamedTemporaryFile(suffix='.py',delete=False);"
                "q=pathlib.Path(t.name);t.close();q.write_bytes(p.read_bytes().replace(b'return 1',b'return 2'));"
                "s=p.stat();os.utime(q,ns=(s.st_atime_ns,s.st_mtime_ns));"
                "py_compile.compile(str(q),cfile=importlib.util.cache_from_source(str(p)),dfile=str(p),"
                "doraise=True,invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP);q.unlink()")
            script = base / 'script.json'
            script.write_text(json.dumps([
                call('run_command', {'argv': [sys.executable, '-c', poison]}),
                call('validate_candidate'), call('final', {'answer': 'Current workspace validated.'})]),
                encoding='utf-8')
            command = [str(ARGS.forge.resolve()), 'run', 'Repair value without changing tests; validate and finish.',
                       '--workspace', str(root), '--script', str(script), '--no-config', '--json',
                       '--minimal-agent', '--candidate-checkpoint', '--bounded-repair', '--allow-write',
                       '--allow-exec', '--context', '16384', '--output-reserve', '2048',
                       '--max-turns', '4', '--wall-ms', '30000']
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=45)
            events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            checkpoints = [e['data'] for e in events if e['type'] == 'candidate_checkpoint']
            finals = [e for e in events if e['type'] == 'final']
            sessions = list((root / '.forge/sessions').glob('*'))
            metrics = json.loads((sessions[0] / 'metrics.json').read_text(encoding='utf-8')) if sessions else {}
            with tempfile.TemporaryDirectory(prefix='forge-cache-fresh-') as cache:
                fresh = verify(root, base / 'fresh', env=fresh_env(cache))
            EVIDENCE['cases']['host'] = {'command': command, 'script': json.loads(script.read_text()),
                'forge_sha256': sha(ARGS.forge), 'returncode': result.returncode, 'stdout': result.stdout,
                'stderr': result.stderr, 'checkpoints': checkpoints, 'metrics': metrics,
                'source_before': before, 'source_after': {name: sha(root / name) for name in before},
                'fresh_source': fresh}
            self.assertEqual({name: sha(root / name) for name in before}, before)
            self.assertFalse(fresh['passed'], fresh)
            self.assertRegex(fresh['stderr'], r'Ran 1 test\b')
            self.assertTrue(checkpoints, result.stdout + result.stderr)
            self.assertEqual(any(c['passed'] for c in checkpoints), ARGS.demonstrate, checkpoints)
            self.assertEqual(bool(finals), ARGS.demonstrate, result.stdout)
            self.assertEqual(result.returncode == 0, ARGS.demonstrate, result.stdout + result.stderr)


if __name__ == '__main__':
    runner = unittest.main(argv=[sys.argv[0], *UNITTEST_ARGS], exit=False, verbosity=2)
    if ARGS.evidence:
        ARGS.evidence.write_text(json.dumps(EVIDENCE, indent=2) + '\n', encoding='utf-8')
    raise SystemExit(not runner.result.wasSuccessful())
