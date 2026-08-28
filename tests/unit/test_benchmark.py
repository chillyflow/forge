"""Check fixture preparation independently of any model or harness."""
import importlib.util
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


class FixtureTests(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
