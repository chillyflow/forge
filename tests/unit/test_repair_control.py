"""Exercise repair comparison scheduling, failure retention, and frozen resumption."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import zipfile
from unittest import mock

SOURCE = Path(__file__).resolve().parents[2] / 'benchmark' / 'repair_control.py'
SPEC = importlib.util.spec_from_file_location('repair_control', SOURCE)
CONTROL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROL)


class RepairControlTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.args = SimpleNamespace(
            output=self.root / 'output', task_dir=self.root / 'tasks',
            model=self.root / 'model.gguf', source_dir=self.root,
            checkpoint_forge=self.root / 'checkpoint.exe', current_forge=self.root / 'current.exe',
            minimal_forge=self.root / 'minimal.exe', checkpoint_revision='old', current_revision='current',
            gpu_layers=-1, context=16384, output_reserve=2048, temperature=0.0, seed=42,
            max_turns=16, max_tokens=32768, max_input=262144, timeout=600,
            verification_timeout=120, repetitions=3, order_seed=20260831, gpu_index=0)
        self.identity = {'source': {'files': {}, 'diff_sha256': hashlib.sha256(b'patch').hexdigest()},
                         'model': {'sha256': 'model'},
                         'tasks': {'repair': {}},
                         'runtimes': {arm: {'bundle': {'sha256': arm}} for arm in CONTROL.ARMS}}
        self.protocol = CONTROL.make_protocol(self.args, [(Path('task.json'), {'id': 'repair'})], self.identity)
        self.cell = {'cell_id': 'repair-minimal-r001', 'task': 'repair', 'arm': 'minimal',
                     'repetition': 1, 'order_index': 1}
        self.prepared = {'repair': {'fixture_sha256': 'fixture'}}
        self.environment = {
            'model_sha256': 'model', 'forge_runtime_bundle': {'sha256': 'minimal'},
            'gpu_layers': '-1', 'context_tokens': 16384, 'output_reserve': 2048,
            'max_turns': 16, 'max_tokens': 32768, 'max_input': 262144,
            'temperature': 0.0, 'seed': 42, 'prompt_protocol': 'native', 'chat_template': 'embedded',
            'lifecycle': 'cold', 'fixture_preparation': CONTROL.FIXTURE_PREPARATION}
        self.record = {'task': 'repair', 'variant': 'minimal', 'repetition': 1,
                       'fixture_sha256': 'fixture', 'passed': True, 'returncode': 0,
                       'protected_files_unchanged': True, 'timing': {'end_to_end_seconds': 2.5},
                       'metrics': {'simulated': False, 'generated_tokens': 50, 'prompt_tokens': 100,
                                   'turns': 2, 'loop_warnings': 1}}

    def fake_harness(self, returncode=0, record=True):
        script = self.root / 'run.py'
        script.write_text(
            "import argparse, json, pathlib, sys\n"
            "p=argparse.ArgumentParser(); p.add_argument('--output'); a,_=p.parse_known_args()\n"
            "out=pathlib.Path(a.output); out.mkdir(parents=True)\n"
            f"record={self.record!r}\nenvironment={self.environment!r}\n"
            "(out/'stdout.txt').write_text('retained harness evidence')\n"
            + ("(out/'results.json').write_text(json.dumps([record]))\n"
               "(out/'environment.json').write_text(json.dumps(environment))\n" if record else '')
            + f"sys.exit({returncode})\n", encoding='utf-8')
        return mock.patch.object(CONTROL, 'DIRECTORY', self.root)

    def test_schedule_is_complete_reproducible_and_paired(self):
        ids = ['go_transfer', 'py_retraction', 'go_pagination']
        schedule = CONTROL.make_schedule(ids, 3, 77)
        self.assertEqual(schedule, CONTROL.make_schedule(list(reversed(ids)), 3, 77))
        self.assertNotEqual(schedule, CONTROL.make_schedule(ids, 3, 78))
        self.assertEqual(len(schedule), 27)
        self.assertEqual(len({row['cell_id'] for row in schedule}), 27)
        sequences = [[(row['task'], row['repetition']) for row in schedule if row['arm'] == arm]
                     for arm in CONTROL.ARMS]
        self.assertEqual(sequences[0], sequences[1])
        self.assertEqual(sequences[1], sequences[2])
        for start in range(0, len(schedule), 3):
            block = schedule[start:start + 3]
            self.assertEqual({row['arm'] for row in block}, set(CONTROL.ARMS))
            self.assertEqual(len({(row['task'], row['repetition']) for row in block}), 1)

    def test_commands_share_every_budget_and_only_minimal_changes_variant(self):
        commands = []
        for arm in CONTROL.ARMS:
            command = CONTROL.cell_command(self.args, {**self.cell, 'arm': arm}, self.root / arm)
            for name, value in [('--max-tokens', '32768'), ('--max-input', '262144'),
                                ('--max-turns', '16'), ('--repetitions', '1'),
                                ('--temperature', '0.0'), ('--suite', 'all')]:
                self.assertEqual(command[command.index(name) + 1], value)
            commands.append(command[command.index('--variants') + 1])
        self.assertEqual(commands, ['optimized', 'optimized', 'minimal'])

    def test_exclusive_lock_and_overwrite_requirements(self):
        with CONTROL.exclusive_output(self.args.output, False):
            CONTROL.atomic_json(self.args.output / 'protocol.json', self.protocol)
            with self.assertRaisesRegex(ValueError, 'Another comparison'):
                with CONTROL.exclusive_output(self.args.output, True):
                    pass
        with self.assertRaisesRegex(ValueError, 'not empty'):
            with CONTROL.exclusive_output(self.args.output, False):
                pass
        with CONTROL.exclusive_output(self.args.output, True):
            pass

    def test_resume_refuses_settings_identity_and_population_changes(self):
        CONTROL.validate_resume(self.protocol, copy.deepcopy(self.protocol))
        for key in ('settings', 'identity', 'schedule'):
            changed = copy.deepcopy(self.protocol)
            changed[key] = {} if key != 'schedule' else []
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, 'mismatch'):
                CONTROL.validate_resume(self.protocol, changed)

    def test_harness_crash_is_retained_and_never_rerun(self):
        with self.fake_harness(returncode=17, record=False):
            outcome = CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        self.assertEqual(outcome['status'], 'harness_error')
        self.assertEqual(outcome['harness_returncode'], 17)
        self.assertFalse(outcome['passed'])
        self.assertTrue((self.args.output / 'cells' / self.cell['cell_id'] / 'run' / 'stdout.txt').exists())
        with mock.patch.object(CONTROL.subprocess, 'Popen', side_effect=AssertionError('rerun')):
            self.assertEqual(CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared), outcome)

    def test_failed_agent_result_is_completed_evidence_not_harness_error(self):
        self.record.update(passed=False, returncode=1)
        with self.fake_harness(returncode=1):
            outcome = CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        self.assertEqual(outcome['status'], 'completed')
        self.assertFalse(outcome['passed'])
        self.assertEqual(outcome['record']['timing']['end_to_end_seconds'], 2.5)

    def test_resume_revalidates_completed_evidence(self):
        with self.fake_harness():
            outcome = CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        self.assertTrue(outcome['passed'])
        path = self.args.output / 'cells' / self.cell['cell_id'] / 'run' / 'environment.json'
        changed = {**self.environment, 'model_sha256': 'different'}
        CONTROL.atomic_json(path, changed)
        with self.assertRaisesRegex(ValueError, 'frozen protocol'):
            CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)

    def test_resume_rejects_contradictory_pass_or_unknown_status(self):
        with self.fake_harness():
            outcome = CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        path = self.args.output / 'cells' / self.cell['cell_id'] / 'outcome.json'
        for change in ({'passed': False}, {'status': 'unknown'}, {'status': 'harness_error'}):
            CONTROL.atomic_json(path, {**outcome, **change})
            with self.subTest(change=change), self.assertRaises(ValueError):
                CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)

    def test_interrupted_cell_is_not_retried_and_live_process_blocks_resume(self):
        directory = self.args.output / 'cells' / self.cell['cell_id']
        directory.mkdir(parents=True)
        CONTROL.atomic_json(directory / 'started.json', {'pid': 42})
        with mock.patch.object(CONTROL, 'process_alive', return_value=True):
            with self.assertRaisesRegex(ValueError, 'live harness'):
                CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        with mock.patch.object(CONTROL, 'process_alive', return_value=False):
            outcome = CONTROL.execute_cell(self.args, self.cell, self.protocol, self.prepared)
        self.assertEqual(outcome['status'], 'interrupted')
        self.assertFalse(outcome['passed'])

    def test_preflight_requires_exact_successful_population(self):
        self.args.output.mkdir()
        path = self.args.output / 'preflight.json'
        row = {'task': 'repair', 'broken_returncode': 1, 'oracle_returncode': 0, 'fixture_sha256': 'fixture'}
        for rows, passed in [([row, row], True), ([], True), ([row], False),
                             ([{**row, 'broken_returncode': 0}], True),
                             ([{**row, 'oracle_returncode': 1}], True)]:
            CONTROL.atomic_json(path, {'passed': passed, 'records': rows})
            with self.subTest(rows=rows, passed=passed), self.assertRaises(ValueError):
                CONTROL.preflight(self.args, self.protocol)
        CONTROL.atomic_json(path, {'passed': True, 'records': [row]})
        self.assertEqual(CONTROL.preflight(self.args, self.protocol)['repair'], row)

    def test_result_requires_same_model_fixture_integrity_and_real_bounded_inference(self):
        CONTROL.validate_record(self.cell, self.record, self.environment, self.protocol, self.prepared)
        invalid = [{'fixture_sha256': 'changed'}, {'protected_files_unchanged': False},
                   {'metrics': {**self.record['metrics'], 'simulated': True}},
                   {'metrics': {**self.record['metrics'], 'turns': 17}},
                   {'metrics': {**self.record['metrics'], 'generated_tokens': 32769}},
                   {'metrics': {**self.record['metrics'], 'prompt_tokens': 262145}}]
        for changes in invalid:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                CONTROL.validate_record(self.cell, {**self.record, **changes}, self.environment,
                                        self.protocol, self.prepared)
        self.protocol['identity']['platform'] = {'gpu': 'frozen driver'}
        with self.assertRaisesRegex(ValueError, 'frozen protocol'):
            CONTROL.validate_record(self.cell, self.record, {**self.environment, 'gpu': 'changed driver'},
                                    self.protocol, self.prepared)

    def test_population_audit_and_medians_keep_failures_and_missing_denominators(self):
        outcomes = [{**cell, 'status': 'completed', 'passed': index == 0,
                     'record': {'timing': {'end_to_end_seconds': index + 1}}}
                    for index, cell in enumerate(self.protocol['schedule'])]
        report = CONTROL.summarize(self.protocol, outcomes)
        self.assertTrue(report['population']['complete'])
        self.assertEqual(sum(row['passed'] for row in report['per_task']), 1)
        self.assertTrue(all(row['timed_observations'] == 3 for row in report['per_task']))
        incomplete = CONTROL.summarize(self.protocol, outcomes[:-1])
        self.assertFalse(incomplete['population']['complete'])
        self.assertTrue(all(row['scheduled'] == 3 for row in incomplete['per_task']))
        duplicates = CONTROL.summarize(self.protocol, [*outcomes, outcomes[0]])
        self.assertFalse(duplicates['population']['complete'])
        self.assertEqual(duplicates['population']['unexpected_or_duplicate'], [outcomes[0]['cell_id']])

    def test_trace_notes_do_not_confuse_prompt_with_context_exhaustion(self):
        session = self.root / 'run' / 'case' / 'session'
        session.mkdir(parents=True)
        path = session / 'events.jsonl'
        prompt = {'type': 'context', 'data': 'Context budget exhausted is mentioned in the task'}
        path.write_text(json.dumps(prompt) + '\n', encoding='utf-8')
        self.assertFalse(CONTROL.trace_notes(self.root / 'run', self.record,
                                           self.protocol['settings'])['context_exhaustion_observed'])
        with path.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps({'type': 'error', 'data': {'status': 'limit', 'context_evictions': 7,
                                                              'turns': 16}}) + '\n')
        self.assertFalse(CONTROL.trace_notes(self.root / 'run', self.record,
                                           self.protocol['settings'])['context_exhaustion_observed'])
        with path.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps({'type': 'error', 'data': {'message': 'Context limit exceeded'}}) + '\n')
        self.assertTrue(CONTROL.trace_notes(self.root / 'run', self.record,
                                          self.protocol['settings'])['context_exhaustion_observed'])

    def test_source_identity_retains_untracked_code_and_excludes_result_archives(self):
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root / 'src').mkdir()
        (self.root / 'src' / 'tracked.c').write_text('int value = 1;\n')
        subprocess.run(['git', '-C', str(self.root), 'add', 'src/tracked.c'], check=True)
        subprocess.run(['git', '-C', str(self.root), '-c', 'user.name=Test', '-c',
                        'user.email=test@example.invalid', 'commit', '-qm', 'fixture'], check=True)
        (self.root / 'tests').mkdir()
        (self.root / 'tests' / 'new_test.py').write_text('assert True\n')
        (self.root / 'benchmark' / 'results').mkdir(parents=True)
        (self.root / 'benchmark' / 'results' / 'large.zip').write_bytes(b'excluded')
        identity, _ = CONTROL.source_identity(self.root, {'checkpoint': 'HEAD', 'current': 'HEAD'})
        self.assertIn('tests/new_test.py', identity['files'])
        self.assertNotIn('benchmark/results/large.zip', identity['files'])
        before = identity['files_sha256']
        (self.root / 'tests' / 'new_test.py').write_text('assert False\n')
        after, _ = CONTROL.source_identity(self.root, {'checkpoint': 'HEAD', 'current': 'HEAD'})
        self.assertNotEqual(before, after['files_sha256'])

    def test_source_snapshot_requires_every_frozen_file_and_original_diff(self):
        self.args.output.mkdir()
        (self.args.output / 'minimal-source.diff').write_bytes(b'patch')
        self.identity['source']['files'] = {'tests/untracked.py': {'sha256': hashlib.sha256(b'content').hexdigest()}}
        path = self.args.output / 'minimal-source.zip'
        with self.assertRaises(OSError):
            CONTROL.validate_source_evidence(self.args.output, self.identity)
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('tests/untracked.py', b'content')
        CONTROL.validate_source_evidence(self.args.output, self.identity)
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('tests/untracked.py', b'changed')
        with self.assertRaisesRegex(ValueError, 'differs'):
            CONTROL.validate_source_evidence(self.args.output, self.identity)
        with zipfile.ZipFile(path, 'w'):
            pass
        with self.assertRaisesRegex(ValueError, 'missing'):
            CONTROL.validate_source_evidence(self.args.output, self.identity)

    def test_main_audits_frozen_change_and_preflight_failure_prevents_runs(self):
        self.args.resume = False
        self.identity['platform'] = {}
        tasks = [(self.root / 'repair.json', {'id': 'repair'})]

        def preflight(args, protocol):
            CONTROL.atomic_json(args.output / 'preflight.json', {'passed': True})
            return self.prepared

        def execute(args, cell, protocol, prepared):
            directory = args.output / 'cells' / cell['cell_id']
            directory.mkdir(parents=True)
            outcome = {**cell, 'status': 'completed', 'passed': False, 'record': self.record}
            CONTROL.atomic_json(directory / 'outcome.json', outcome)
            return outcome

        changed = {**self.identity, 'model': {'sha256': 'changed'}}
        with mock.patch.multiple(CONTROL, parse_args=mock.Mock(return_value=self.args),
                                 load_tasks=mock.Mock(return_value=tasks), check_tools=mock.Mock(),
                                 collect_identity=mock.Mock(side_effect=[(self.identity, b'patch'), (changed, b'patch')]),
                                 frozen_stamps=mock.Mock(return_value={}),
                                 preflight=mock.Mock(side_effect=preflight), execute_cell=mock.Mock(side_effect=execute)):
            self.assertEqual(CONTROL.main([]), 2)
        audit = CONTROL.read_json(self.args.output / 'audit.json')
        self.assertFalse(audit['frozen_unchanged'])
        self.assertTrue(audit['population']['complete'])
        self.assertTrue((self.args.output / 'minimal-source.zip').exists())

        self.args.output = self.root / 'preflight-failure'
        executor = mock.Mock()
        with mock.patch.multiple(CONTROL, parse_args=mock.Mock(return_value=self.args),
                                 load_tasks=mock.Mock(return_value=tasks), check_tools=mock.Mock(),
                                 collect_identity=mock.Mock(return_value=(self.identity, b'patch')),
                                 frozen_stamps=mock.Mock(return_value={}),
                                 preflight=mock.Mock(side_effect=ValueError('preflight failed')),
                                 execute_cell=executor):
            self.assertEqual(CONTROL.main([]), 2)
        executor.assert_not_called()


if __name__ == '__main__':
    unittest.main()
