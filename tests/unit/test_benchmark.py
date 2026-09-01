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
FAILURE_SPEC = importlib.util.spec_from_file_location(
    'forge_benchmark_failures', SOURCE.parent / 'analyze_failures.py')
FAILURES = importlib.util.module_from_spec(FAILURE_SPEC)
FAILURE_SPEC.loader.exec_module(FAILURES)
REPORT_SPEC = importlib.util.spec_from_file_location(
    'forge_benchmark_report', SOURCE.parent / 'report.py')
REPORT = importlib.util.module_from_spec(REPORT_SPEC)
REPORT_SPEC.loader.exec_module(REPORT)


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

    def test_campaign_has_bounded_multilanguage_category_coverage(self):
        tasks = [json.loads(path.read_text(encoding='utf-8'))
                 for path in SOURCE.parent.joinpath('tasks').glob('*.json')]
        self.assertGreaterEqual(len(tasks), 25)
        self.assertLessEqual(len(tasks), 50)
        campaign = [task for task in tasks if task.get('suite') == 'campaign']
        self.assertEqual({task.get('language') for task in campaign}, {'go', 'python'})
        self.assertTrue({'multi-file', 'api', 'refactor', 'compiler-failure', 'exploration'}
                        <= {task.get('category') for task in campaign})
        self.assertTrue(all(task.get('protected_files') for task in campaign))

    def test_repeated_schedule_is_seeded_and_complete(self):
        cases = ['a', 'b', 'c']
        first = BENCH.schedule(cases, repetitions=3, seed=7)
        second = BENCH.schedule(cases, repetitions=3, seed=7)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 9)
        self.assertEqual({(case, repetition) for case, repetition, _ in first},
                         {(case, repetition) for case in cases for repetition in range(1, 4)})
        self.assertEqual([index for _, _, index in first], list(range(1, 10)))

    def test_protected_file_hashes_generalize_beyond_repair_test(self):
        task = {'files': {'src/a.py': 'value = 1\n', 'tests/test_a.py': 'sentinel = 1\n'},
                'protected_files': ['tests/test_a.py']}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            BENCH.materialize(root, task)
            before = BENCH.snapshot_protected(root, task)
            self.assertTrue(BENCH.protected_unchanged(root, before))
            (root / 'tests' / 'test_a.py').write_text('sentinel = 2\n', encoding='utf-8')
            self.assertFalse(BENCH.protected_unchanged(root, before))

    def test_campaign_report_distributions_and_fixture_validation(self):
        records = []
        for repetition, seconds in enumerate((1.0, 2.0, 6.0), 1):
            records.append({'task': 'a', 'repetition': repetition, 'passed': repetition != 3,
                            'wall_seconds': seconds,
                            'timing': {'end_to_end_seconds': seconds, 'agent_seconds': seconds - .2,
                                       'startup_seconds': .1, 'verification_seconds': .1},
                            'metrics': {'generated_tokens': 10 * repetition},
                            'fixture_preparation': 'v2', 'fixture_sha256': 'same',
                            'fixture_files': {'a.py': 'hash'},
                            'resource_usage': {'peak_process_tree_rss_bytes': 100 * repetition,
                                               'peak_gpu_used_bytes': 200 * repetition}})
        summary = REPORT.summarize(records)
        self.assertEqual(summary['passed'], 2)
        self.assertEqual(summary['end_to_end_seconds']['p50'], 2.0)
        self.assertGreater(summary['end_to_end_seconds']['stdev'], 0)
        REPORT.validate_groups({'one': records, 'two': [dict(record) for record in records]})
        changed = [dict(record) for record in records]
        changed[0]['fixture_sha256'] = 'different'
        with self.assertRaises(ValueError):
            REPORT.validate_groups({'one': records, 'two': changed})

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

    def test_failure_classifier_parses_inline_thought_envelopes(self):
        event = {'type': 'model_output', 'data': json.dumps({
            'thought': 'reason', 'tool': 'read_file',
            'args': {'path': 'x', 'start': 1, 'end': 1}})}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'stdout.jsonl').write_text(json.dumps(event) + '\n', encoding='utf-8')
            self.assertEqual(list(FAILURES.actions(root)),
                             [(json.loads(event['data']), True)])


if __name__ == '__main__':
    unittest.main()
