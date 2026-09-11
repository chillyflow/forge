"""Read-only pilot-layout checks and synthetic evidence-tampering regressions.

All mutations occur in TemporaryDirectory copies. No model process is launched.
"""
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('l3_diagnostic', HERE / 'driver.py')
driver = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(driver)
ROOT = HERE.parents[3]
CONTRACT = driver.read(HERE.parent / 'acceptance/contract.json')
PILOT = ROOT / 'benchmark/results/2026-09-09-agent-loop-v1/cells'


class DiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='forge-l3-selftest-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        original = PILOT / 'go_api_pagination-best-of-2-r001/run/go_api_pagination-loop-best-of-2-r001/session'
        self.session = self.root / 'session'
        shutil.copytree(original, self.session, copy_function=shutil.copyfile)
        self.fixture = CONTRACT['fixtures']['benchmark/results/2026-09-08-repair-control/tasks/go_api_pagination.json']
        self.profile = CONTRACT['profiles']['loop-pilot']
        self.metrics = driver.read(self.session / 'metrics.json')

    def audit(self):
        return driver.audit_children(self.session, self.fixture, self.profile, self.metrics)

    def rewrite(self, path, transform):
        rows = driver.events(path)
        transform(rows)
        path.write_text('\n'.join(json.dumps(row) for row in rows) + '\n', encoding='utf-8')

    def test_exact_18_pair_schedule_preserves_reference_identity(self):
        rows = [row for row in CONTRACT['schedule'] if row['gate'] == 'G1']
        schedule = driver.make_schedule(rows)
        self.assertEqual(len(schedule), 18)
        self.assertEqual(len({row['run_id'] for row in schedule}), 18)
        self.assertEqual([row['baseline_run_id'] for row in schedule], [row['run_id'] for row in rows])
        self.assertEqual({row['seed'] for row in schedule}, {42})
        self.assertEqual([sum(row['repetition'] == rep for row in schedule) for rep in (1, 2, 3)], [6, 6, 6])

    def test_all_six_retained_best_two_pilot_layouts(self):
        results = []
        for cell in sorted(PILOT.glob('*-best-of-2-r001')):
            task = cell.name.removesuffix('-best-of-2-r001')
            session = cell / 'run' / f'{task}-loop-best-of-2-r001' / 'session'
            fixture = CONTRACT['fixtures'][f'benchmark/results/2026-09-08-repair-control/tasks/{task}.json']
            result = driver.audit_children(session, fixture, self.profile, driver.read(session / 'metrics.json'))
            results.append(result)
        self.assertEqual(len(results), 6)
        self.assertEqual(sum(result['started'] for result in results), 12)
        self.assertEqual(sum(result['completed'] for result in results), 4)
        self.assertEqual(sum(result['root_completed'] for result in results), 2)

    def test_missing_child_and_missing_nested_prompt_rejected(self):
        target = (self.session / 'trial-02').resolve()
        self.assertTrue(target.is_relative_to(self.root.resolve()))
        shutil.rmtree(target)
        with self.assertRaisesRegex(ValueError, 'trial workspace'):
            self.audit()
        # A separate retained session copy tests missing prompts without model work.
        original = PILOT / 'go_api_pagination-best-of-2-r001/run/go_api_pagination-loop-best-of-2-r001/session/trial-02'
        shutil.copytree(original, self.session / 'trial-02', copy_function=shutil.copyfile)
        for path in (self.session / 'trial-02').glob('.forge/sessions/*/context/*.txt'):
            path.unlink()
        with self.assertRaisesRegex(ValueError, 'nested child prompts'):
            self.audit()

    def test_protected_child_mutation_rejected(self):
        (self.session / 'trial-02/api/handler_test.go').write_text('changed protected test')
        with self.assertRaisesRegex(ValueError, 'protected'):
            self.audit()

    def test_duplicate_child_event_and_wrong_seed_rejected(self):
        path = self.session / 'events.jsonl'
        original = path.read_text()
        def wrong_seed(rows):
            next(row for row in rows if row['type'] == 'candidate_start')['data']['seed'] = 43
        self.rewrite(path, wrong_seed)
        with self.assertRaisesRegex(ValueError, 'seed'):
            self.audit()
        path.write_text(original)
        def duplicate(rows):
            rows.append(copy.deepcopy(rows[0]))
        self.rewrite(path, duplicate)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.audit()

    def test_child_budget_and_root_aggregate_cannot_be_borrowed(self):
        path = next((self.session / 'trial-01').glob('.forge/sessions/*/metrics.json'))
        metrics = driver.read(path)
        metrics['turns'] = 17
        driver.write(path, metrics)
        def excess(rows):
            next(row for row in rows if row['type'] == 'done')['data']['turns'] = 17
        self.rewrite(path.with_name('events.jsonl'), excess)
        with self.assertRaisesRegex(ValueError, 'shared turns'):
            self.audit()
        metrics['turns'] = 8
        driver.write(path, metrics)
        def restore(rows):
            next(row for row in rows if row['type'] == 'done')['data']['turns'] = 8
        self.rewrite(path.with_name('events.jsonl'), restore)
        self.metrics['generated_tokens'] += 1
        with self.assertRaisesRegex(ValueError, 'aggregate'):
            self.audit()

    def test_root_final_cannot_be_borrowed_from_losing_child(self):
        child_path = next((self.session / 'trial-02').glob('.forge/sessions/*/events.jsonl'))
        other = next(row['data'] for row in driver.events(child_path) if row['type'] == 'final')
        def change(rows):
            next(row for row in rows if row['type'] == 'final')['data'] = other
        self.rewrite(self.session / 'events.jsonl', change)
        with self.assertRaisesRegex(ValueError, 'borrowed'):
            self.audit()

    def test_child_completion_requires_complete_native_call(self):
        path = next((self.session / 'trial-01').glob('.forge/sessions/*/events.jsonl'))
        def change(rows):
            output = [row for row in rows if row['type'] == 'model_output'][-1]
            output['data'] = '<tool_call><function=final>'
        self.rewrite(path, change)
        with self.assertRaisesRegex(ValueError, 'complete native final'):
            self.audit()

    def test_journal_mutation_and_unfinished_restore_rejected(self):
        (self.session / 'candidate-000001.before').write_text('not baseline')
        with self.assertRaisesRegex(ValueError, 'before state'):
            self.audit()

    def test_incomplete_selection_output_rejected(self):
        path = self.session / 'events.jsonl'
        def change(rows):
            next(row for row in rows if row['type'] == 'validation_result')['data']['evidence_complete'] = False
        self.rewrite(path, change)
        with self.assertRaisesRegex(ValueError, 'validation'):
            self.audit()

    def test_cache_bytes_are_retained_in_snapshot_identity(self):
        cache = self.root / 'inputs/__pycache__/source.pyc'
        cache.parent.mkdir(parents=True)
        cache.write_bytes(b'synthetic cached bytes')
        nested = self.root / 'inputs/pkg/.forge/value'
        nested.parent.mkdir(parents=True)
        nested.write_bytes(b'nested input')
        contents = driver.snapshot(self.root / 'inputs')
        self.assertIn('__pycache__/source.pyc', contents)
        self.assertIn('pkg/.forge/value', contents)
        regular = self.root / 'inputs/.git'
        regular.write_bytes(b'root gitfile input')
        self.assertIn('.git', driver.snapshot(self.root / 'inputs'))
        excluded = self.root / 'inputs/.forge/metadata'
        excluded.parent.mkdir()
        excluded.write_bytes(b'root session metadata')
        self.assertNotIn('.forge/metadata', driver.snapshot(self.root / 'inputs'))

    def test_legacy_retention_and_stale_cache_policy_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unsupported-retention'):
            driver.retained_inputs(self.root, {}, self.fixture)
        record = {'terminal_retention_policy': 'complete-except-git-forge', 'language': 'python',
                  'verification': {'python_cache_policy': 'not-applicable'}}
        with self.assertRaisesRegex(ValueError, 'cache policy'):
            driver.retained_inputs(self.root, record, self.fixture)

    def test_complete_before_after_maps_include_cache_bytes(self):
        output = self.root / 'harness'
        for name in ('pre-verification-workspace', 'terminal-workspace'):
            cache = output / name / '__pycache__/x.pyc'
            cache.parent.mkdir(parents=True)
            cache.write_bytes(b'cache input')
        files = driver.snapshot(output / 'terminal-workspace')
        driver.write(output / 'verification-inputs.json', {'before': files, 'after': files})
        result = {'terminal_retention_policy': 'complete-except-git-forge', 'language': 'python',
                  'verification': {'python_cache_policy': 'fresh-external-prefix-no-write'}}
        driver.retained_inputs(output, result, {'protected_files': {}})
        (output / 'pre-verification-workspace/__pycache__/x.pyc').write_bytes(b'changed before bytes')
        with self.assertRaisesRegex(ValueError, 'pre/post bytes'):
            driver.retained_inputs(output, result, {'protected_files': {}})

    def test_report_keeps_missing_denominator_and_rejects_borrowed_execution(self):
        schedule = driver.make_schedule([row for row in CONTRACT['schedule'] if row['gate'] == 'G1'])
        protocol = {'schedule': schedule, 'protocol_sha256': 'synthetic',
                    'candidate': {'identity': {'source_sha256': 'source'}},
                    'arm_configuration_sha256': {'B': 'B-config'},
                    'baseline_records': [{'execution_id': f'A-{i}'} for i in range(18)],
                    'baseline_status': {row['baseline_run_id']: {'status': 'passed'} for row in schedule}}
        directory = self.root / 'report'
        directory.mkdir()
        driver.write(directory / 'protocol.json', protocol)
        driver.write(directory / 'outcomes.json', [])
        with mock.patch.object(driver, 'validate_protocol'):
            summary = driver.report(directory)
            self.assertEqual((summary['A_referenced'], summary['B_scheduled']), (18, 18))
            self.assertEqual(summary['A_passed'], 18)
            self.assertFalse(summary['complete'])
            self.assertFalse(summary['measured_development_benefit'])
            self.assertTrue(all(row['B_status'] == 'missing' for row in summary['pairs']))
            driver.write(directory / 'outcomes.json', [{'run_id': schedule[0]['run_id'], 'execution_id': 'A-0'}])
            with self.assertRaisesRegex(ValueError, 'borrowed an A'):
                driver.report(directory)
            driver.write(directory / 'outcomes.json', [{'run_id': schedule[0]['run_id'], 'execution_id': 'B'}] * 2)
            with self.assertRaisesRegex(ValueError, 'duplicate B'):
                driver.report(directory)

    def test_changed_source_or_runtime_cannot_run(self):
        with mock.patch.object(driver.campaign, 'observe', return_value={'source': 'different'}):
            with self.assertRaisesRegex(ValueError, 'changed since freeze'):
                driver.observed({'candidate': {'identity': {'source': 'frozen'}}})

    def test_archive_verification_rejects_missing_or_changed_members(self):
        path = self.root / 'evidence.txt'
        path.write_text('evidence')
        archive, inventory = driver.campaign.archive_files(self.root, {'evidence.txt': path}, 'sample')
        manifest = driver.read(inventory['path'])
        manifest['files']['evidence.txt']['sha256'] = '0' * 64
        driver.write(inventory['path'], manifest)
        inventory = driver.ref(inventory['path'])
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            driver.evidence.check_archive(self.root, archive, inventory)


if __name__ == '__main__':
    unittest.main()
