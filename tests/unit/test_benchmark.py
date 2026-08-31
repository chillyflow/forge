"""Check fixture preparation independently of any model or harness."""
import importlib.util
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SOURCE = pathlib.Path(__file__).resolve().parents[2] / 'benchmark' / 'run.py'
SPEC = importlib.util.spec_from_file_location('forge_benchmark_run', SOURCE)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)
SUMMARY_SPEC = importlib.util.spec_from_file_location('forge_benchmark_summary',
                                                     SOURCE.parent / 'summarize.py')
SUMMARY = importlib.util.module_from_spec(SUMMARY_SPEC)
SUMMARY_SPEC.loader.exec_module(SUMMARY)
CONSOLIDATE_SPEC = importlib.util.spec_from_file_location(
    'forge_benchmark_consolidate', SOURCE.parent / 'consolidate_arms.py')
CONSOLIDATE = importlib.util.module_from_spec(CONSOLIDATE_SPEC)
CONSOLIDATE_SPEC.loader.exec_module(CONSOLIDATE)


class FixtureTests(unittest.TestCase):
    def test_reasoning_gated_fixtures_fail_before_and_pass_oracle(self):
        go = shutil.which('go')
        gofmt = shutil.which('gofmt')
        if not go or not gofmt:
            self.skipTest('Go and gofmt are needed to verify reasoning fixtures')
        task_paths = sorted(SOURCE.parent.joinpath('tasks').glob('reasoning_*.json'))
        self.assertGreaterEqual(len(task_paths), 6)
        seen = set()
        for task_path in task_paths:
            task = json.loads(task_path.read_text(encoding='utf-8'))
            with self.subTest(task=task['id']):
                self.assertEqual(task.get('suite'), 'reasoning-gated')
                self.assertNotIn(task['id'], seen)
                seen.add(task['id'])
                self.assertIn('repair_test.go', task['files'])
                self.assertTrue(task.get('oracle_files'))
                with tempfile.TemporaryDirectory() as temporary:
                    root = pathlib.Path(temporary)
                    BENCH.materialize(root, task)
                    broken = subprocess.run([go, 'test', './...'], cwd=root,
                                            capture_output=True, timeout=60)
                    self.assertNotEqual(broken.returncode, 0,
                                        'broken fixture unexpectedly passed')
                    for name, content in task['oracle_files'].items():
                        (root / name).write_text(content, encoding='utf-8', newline='\n')
                    go_files = [str(path) for path in root.glob('*.go')]
                    subprocess.run([gofmt, '-w', *go_files], cwd=root, check=True,
                                   capture_output=True, timeout=30)
                    repaired = subprocess.run([go, 'test', './...'], cwd=root,
                                              capture_output=True, timeout=60)
                    self.assertEqual(repaired.returncode, 0,
                                     repaired.stdout.decode(errors='replace') +
                                     repaired.stderr.decode(errors='replace'))

    def test_go_inputs_are_formatted_before_baseline_hashing(self):
        if not shutil.which('gofmt'):
            self.skipTest('gofmt is needed to prepare real Go inputs')
        task = {'files': {
            'repair.go': 'package repair\nfunc Add(a,b int)int{return a-b}\n',
            'repair_test.go': 'package repair\n// test sentinel\n',
            'notes.txt': 'keep this\n',
        }}
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            a, b = pathlib.Path(first), pathlib.Path(second)
            prepared = BENCH.materialize(a, task)
            self.assertEqual(prepared, BENCH.materialize(b, task))
            self.assertEqual(prepared['fixture_preparation'], BENCH.FIXTURE_PREPARATION)
            self.assertEqual(prepared['fixture_files']['repair_test.go'],
                             BENCH.digest(a / 'repair_test.go'))
            self.assertIn(b'return a - b', (a / 'repair.go').read_bytes())
            self.assertNotIn(b'\r', (a / 'repair.go').read_bytes())
            formatted = subprocess.run([shutil.which('gofmt'), '-l', './repair.go', './repair_test.go'],
                                       cwd=a, check=True, capture_output=True, timeout=30)
            self.assertEqual(formatted.stdout, b'')
            self.assertEqual((a / 'notes.txt').read_bytes(), b'keep this\n')

    def test_unsafe_paths_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name in ('../escape.txt', '.'):
                with self.subTest(path=name), self.assertRaises(ValueError):
                    BENCH.materialize(root, {'files': {name: 'bad'}})

    def test_hash_changes_with_content_or_path(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            a, b = pathlib.Path(first), pathlib.Path(second)
            baseline = BENCH.materialize(a, {'files': {'a.txt': 'one'}})
            edited = BENCH.materialize(b, {'files': {'a.txt': 'two'}})
            renamed = BENCH.materialize(b, {'files': {'b.txt': 'one'}})
            self.assertNotEqual(baseline['fixture_sha256'], edited['fixture_sha256'])
            self.assertNotEqual(baseline['fixture_sha256'], renamed['fixture_sha256'])

    def test_comparison_rejects_mismatched_or_mixed_preparation(self):
        record = {'task': 'test', 'fixture_preparation': BENCH.FIXTURE_PREPARATION,
                  'fixture_sha256': 'fixture-a', 'fixture_files': {'a.go': 'file-a'}}
        SUMMARY.verify_fixtures([record], [dict(record)])
        SUMMARY.verify_fixtures([{'task': 'legacy'}], [{'task': 'legacy'}])
        for other in ({'task': 'test'}, {**record, 'fixture_sha256': 'fixture-b'},
                      {**record, 'fixture_files': {'a.go': 'file-b'}}):
            with self.subTest(other=other), self.assertRaises(ValueError):
                SUMMARY.verify_fixtures([record], [other])

    def test_variant_and_cue_provenance(self):
        self.assertIn('thought-routed-unbounded-decode-only', BENCH.VARIANTS)
        self.assertEqual(BENCH.VARIANTS['thought-routed-budget-512-decode-only'][
            'thought_budget'], 512)
        self.assertEqual(BENCH.VARIANTS['thought-native-decode-only']['thought_cue'], '')
        self.assertIn('--disable-thinking', BENCH.VARIANTS[
            'thought-native-disabled-decode-only']['flags'])
        action = '{"tool":"read_file","args":{"path":"x","start":1,"end":1}}'
        self.assertEqual(CONSOLIDATE.classify('Plan: reason\n' + action, 'Plan: '),
                         'routed_prefix')
        self.assertEqual(CONSOLIDATE.classify('Plan: \n' + action, 'Plan: '), 'none')
        self.assertEqual(CONSOLIDATE.classify('<think>x</think>\n' + action, ''),
                         'routed_prefix')

    def test_consolidation_checks_complete_fixture_sets(self):
        environment = {key: 'same' for key in CONSOLIDATE.IDENTITY}
        base = {'task': 'a', 'variant': 'one', 'fixture_preparation': 'v1',
                'fixture_sha256': 'a', 'fixture_files': {'a.go': 'hash'}, 'suite': 'smoke'}
        arms = {
            'one': ([base], environment),
            'two': ([{**base, 'variant': 'two'}], environment),
        }
        tasks, mismatches = CONSOLIDATE.check_records(arms)
        self.assertEqual(tasks, ['a'])
        self.assertEqual(mismatches, {})
        arms['two'][0].append({**base, 'variant': 'two'})
        self.assertIn('two', CONSOLIDATE.check_records(arms)[1])

    def test_run_identity_covers_decode_configuration(self):
        for key in ('gpu_layers', 'chat_template', 'task_suite', 'output_reserve',
                    'temperature', 'seed', 'platform', 'go_version', 'gpu'):
            self.assertIn(key, CONSOLIDATE.IDENTITY)


if __name__ == '__main__':
    unittest.main()
