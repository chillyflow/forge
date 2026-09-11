"""Synthetic evidence attacks against acceptance accounting; no model runs."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('agent_loop_acceptance',
                                             ROOT / 'benchmark/agent_loop_acceptance.py')
acceptance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(acceptance)
import sys
sys.path.insert(0, str(ROOT / 'benchmark'))
import agent_loop_campaign as campaign
CONTRACT = ROOT / 'benchmark/results/2026-09-09-agent-loop-all-green/acceptance/contract.json'


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge-acceptance-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.contract = acceptance.read_json(CONTRACT)
        self.row = self.contract['schedule'][0]
        self.fixture = self.contract['fixtures'][self.row['manifest']]
        self.profile = self.contract['profiles'][self.row['profile']]
        self.contract['model']['sha256'] = self.digest(b'synthetic model')
        self.seal_contract()
        source_files = {'src/example.c': self.entry(b'synthetic source')}
        source_identity = {'files': source_files, 'files_sha256': acceptance.json_digest(source_files)}
        source_archive, source_inventory = self.bundle('source', {'src/example.c': b'synthetic source'})
        runtime_dir = self.root / 'runtime'
        runtime_dir.mkdir()
        (runtime_dir / 'forge.exe').write_bytes(b'synthetic binary')
        runtime_files = {'forge.exe': self.entry(b'synthetic binary')}
        policies = {name: {'candidate_count': 1, 'variant': 'loop-repair', 'flags': ['--thought-history']}
                    for name in self.contract['profiles']}
        self.candidate = {
            'candidate_id': 'synthetic-unit-test-only',
            'contract_sha256': self.contract['contract_sha256'],
            'frozen_utc': '2026-09-09T00:00:00+00:00',
            'identity': {'source_sha256': source_identity['files_sha256'],
                         'runtime_sha256': acceptance.json_digest(runtime_files),
                         'model_sha256': self.contract['model']['sha256'],
                         'configuration_sha256': acceptance.json_digest({
                             'profiles': self.contract['profiles'], 'profile_policies': policies})},
            'profile_policies': policies,
            'source_manifest': self.artifact('source-manifest.json', source_identity),
            'source_archive': source_archive, 'source_inventory': source_inventory,
            'runtime_directory': str(runtime_dir),
            'runtime_manifest': self.artifact('runtime.json', {
                'files': runtime_files, 'sha256': acceptance.json_digest(runtime_files)})}
        self.seal_candidate()
        self.record = {
            'run_id': self.row['run_id'], 'execution_id': 'synthetic-execution-1',
            'candidate_id': self.candidate['candidate_id'],
            'candidate_sha256': self.candidate['candidate_sha256'],
            'identity_before': copy.deepcopy(self.candidate['identity']),
            'identity_after': copy.deepcopy(self.candidate['identity']),
            'started_utc': '2026-09-09T00:01:00+00:00',
            'finished_utc': '2026-09-09T00:02:00+00:00',
            'schedule': self.row, 'fixture_sha256': self.fixture['fixture_sha256'],
            'manifest_sha256': self.fixture['manifest_sha256'],
            'settings': {**self.profile, 'seed': self.row['seed']},
            'protected_files_unchanged': True}
        task = acceptance.read_json(ROOT / self.row['manifest'])
        self.terminal_contents = {name: content.encode() for name, content in task['files'].items()}
        self.source_file = next(name for name in self.terminal_contents
                                if name not in self.fixture['protected_files'])
        self.terminal_contents[self.source_file] += b'\n# synthetic changed contents\n'
        terminal_files = {name: self.digest(content) for name, content in self.terminal_contents.items()}
        self.payloads = {
            'execution': {**{name: self.record[name] for name in (
                'run_id', 'execution_id', 'candidate_id', 'candidate_sha256',
                'identity_before', 'identity_after', 'started_utc', 'finished_utc')},
                'agent_completed': True, 'returncode': 0, 'terminal_loop': False},
            'command': {'settings': self.record['settings']},
            'preflight': {'fixture_sha256': self.fixture['fixture_sha256'],
                          'baseline_failed': True, 'oracle_passed': True, 'oracle_test_count': 2},
            'prompts': 'synthetic prompt', 'outputs': 'synthetic model output',
            'journal': 'synthetic journal',
            'validation': {'command': self.fixture['verify'], 'returncode': 0, 'complete': True,
                           'terminal_sha256': acceptance.json_digest(terminal_files), 'test_count': 2},
            'terminal': {'files': terminal_files, 'syntax_valid': True},
            'metrics': {'simulated': False, 'turns': 2, 'generated_tokens': 20, 'prompt_tokens': 30,
                        'agent_seconds': 10., 'verification_seconds': .2},
            'verification_stdout': '', 'verification_stderr': 'Ran 2 tests in 0.001s\n\nOK\n'}
        self.seal_record()

    @staticmethod
    def digest(content):
        return hashlib.sha256(content).hexdigest()

    @classmethod
    def entry(cls, content):
        return {'sha256': cls.digest(content), 'bytes': len(content)}

    def artifact(self, name, content):
        if not isinstance(content, (str, bytes)):
            content = json.dumps(content, sort_keys=True)
        if isinstance(content, str):
            content = content.encode()
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return {'path': name, 'sha256': self.digest(content)}

    def bundle(self, name, contents):
        path = self.root / (name + '.zip')
        with zipfile.ZipFile(path, 'w') as archive:
            for member, content in contents.items():
                archive.writestr(member, content)
        inventory = self.artifact(name + '-inventory.json', {
            'files': {member: self.entry(content) for member, content in contents.items()}})
        return {'path': path.relative_to(self.root).as_posix(), 'sha256': acceptance.file_digest(path)}, inventory

    def seal_contract(self):
        self.contract.pop('contract_sha256', None)
        self.contract['contract_sha256'] = acceptance.json_digest(self.contract)

    def seal_candidate(self):
        self.candidate.pop('candidate_sha256', None)
        self.candidate['candidate_sha256'] = acceptance.json_digest(self.candidate)

    def seal_record(self, prefix='run'):
        contents = {'terminal/' + name: data for name, data in self.terminal_contents.items()}
        evidence = {}
        for role, content in self.payloads.items():
            reference = self.artifact(prefix + '/' + role, content)
            reference['member'] = role
            evidence[role] = reference
            contents[role] = (self.root / reference['path']).read_bytes()
        self.record['evidence'] = evidence
        self.record['archive'], self.record['inventory'] = self.bundle(prefix, contents)

    def validate_record(self):
        return acceptance.validate_coding_record(self.contract, self.candidate, self.row,
                                                 self.record, self.root)

    def test_frozen_schedule_and_denominators(self):
        contract = acceptance.read_json(CONTRACT)
        acceptance.validate_contract(contract, ROOT)
        self.assertEqual(len(contract['schedule']), 504)
        self.assertEqual(contract['denominators']['coding_total'], 504)
        for phase, expected in [('development', [42, 42, 42]),
                                ('confirmation', [42, 42, 42, 43, 44, 45])]:
            rows = [row for row in contract['schedule'] if row['population'] == 'G1'
                    and row['phase'] == phase and row['task'] == self.row['task']]
            self.assertEqual([row['seed'] for row in rows], expected)
        self.assertEqual(contract['profiles']['historical-holdout']['order_seed'], 20260902)

    def test_missing_and_duplicated_schedule_rejected_even_after_rehash(self):
        for change in ('missing', 'duplicate'):
            with self.subTest(change=change):
                contract = copy.deepcopy(self.contract)
                if change == 'missing':
                    contract['schedule'].pop()
                else:
                    contract['schedule'].append(contract['schedule'][0])
                contract.pop('contract_sha256')
                contract['contract_sha256'] = acceptance.json_digest(contract)
                with self.assertRaisesRegex(acceptance.EvidenceError, 'schedule'):
                    acceptance.validate_contract(contract)

    def test_empty_campaign_keeps_complete_denominators_open(self):
        report = acceptance.report(acceptance.read_json(CONTRACT), None, [], [], ROOT)
        self.assertFalse(report['accepted'])
        self.assertEqual(report['gates']['G6']['missing'], 261)
        self.assertEqual(sum(g['missing'] for g in report['gates'].values()), 509)

    def test_valid_synthetic_evidence_is_recognized(self):
        acceptance.validate_candidate(self.contract, self.candidate, self.root)
        self.assertEqual(self.validate_record(), {'passed': True,
                         'terminal_passing_without_completion': False})

    def test_completion_cannot_be_replaced_by_passing_terminal_inputs(self):
        self.payloads['execution']['agent_completed'] = False
        self.payloads['execution']['returncode'] = 1
        self.seal_record()
        self.assertEqual(self.validate_record(), {'passed': False,
                         'terminal_passing_without_completion': True})

    def test_other_candidate_or_swapped_runtime_rejected(self):
        for field in ('candidate_id', 'identity_after'):
            with self.subTest(field=field):
                original = copy.deepcopy(self.record[field])
                if field == 'candidate_id':
                    self.record[field] = 'another-candidate'
                else:
                    self.record[field]['runtime_sha256'] = '0' * 64
                with self.assertRaises(acceptance.EvidenceError):
                    self.validate_record()
                self.record[field] = original

    def test_actual_runtime_swap_rejected(self):
        (self.root / 'runtime/forge.exe').write_bytes(b'swapped binary')
        with self.assertRaisesRegex(acceptance.EvidenceError, 'swapped runtime'):
            acceptance.validate_candidate(self.contract, self.candidate, self.root)

    def test_protected_mutation_rejected_even_in_rehashed_archive(self):
        name = next(iter(self.fixture['protected_files']))
        self.terminal_contents[name] += b'\n# mutated\n'
        self.payloads['terminal']['files'][name] = self.digest(self.terminal_contents[name])
        self.payloads['validation']['terminal_sha256'] = acceptance.json_digest(
            self.payloads['terminal']['files'])
        self.seal_record()
        with self.assertRaisesRegex(acceptance.EvidenceError, 'protected'):
            self.validate_record()

    def test_missing_terminal_input_and_missing_evidence_rejected(self):
        self.record['evidence'].pop('prompts')
        with self.assertRaisesRegex(acceptance.EvidenceError, 'missing prompts'):
            self.validate_record()
        self.seal_record()
        self.payloads['terminal']['files'].pop(self.source_file)
        self.seal_record()
        with self.assertRaisesRegex(acceptance.EvidenceError, 'terminal inputs'):
            self.validate_record()

    def test_empty_partial_or_skipped_tests_rejected(self):
        for output in ('', 'Ran 1 test in 0.001s\nOK\n',
                       'Ran 2 tests in 0.001s\nOK (skipped=1)\n'):
            with self.subTest(output=output):
                self.payloads['verification_stderr'] = output
                self.seal_record()
                with self.assertRaises(acceptance.EvidenceError):
                    self.validate_record()

    def test_changed_verifier_command_rejected(self):
        self.payloads['validation']['command'] = ['python', '-c', 'print("OK")']
        self.seal_record()
        with self.assertRaisesRegex(acceptance.EvidenceError, 'verification command'):
            self.validate_record()

    def test_missing_metrics_and_exceeded_budgets_rejected(self):
        for value in (None, self.profile['max_tokens'] + 1):
            self.payloads['metrics']['generated_tokens'] = value
            self.seal_record()
            with self.assertRaisesRegex(acceptance.EvidenceError, 'budget'):
                self.validate_record()

    def test_evidence_and_archive_tampering_rejected(self):
        path = self.root / self.record['evidence']['journal']['path']
        path.write_text('changed')
        with self.assertRaisesRegex(acceptance.EvidenceError, 'hash mismatch'):
            self.validate_record()
        self.seal_record()
        inventory = acceptance.read_json(self.root / self.record['inventory']['path'])
        inventory['files'].pop('journal')
        self.record['inventory'] = self.artifact('run-inventory.json', inventory)
        with self.assertRaisesRegex(acceptance.EvidenceError, 'membership mismatch'):
            self.validate_record()

    def test_duplicate_runs_are_not_best_of_successes(self):
        # Keep the fixture checks real while using synthetic execution evidence.
        contract = copy.deepcopy(self.contract)
        for manifest in contract['fixtures']:
            target = self.root / manifest
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / manifest).read_bytes())
        result = acceptance.report(contract, self.candidate,
                                   [self.record, copy.deepcopy(self.record)], [], self.root)
        self.assertEqual(result['runs'][self.row['run_id']]['status'], 'invalid')
        self.assertIn('duplicated', result['runs'][self.row['run_id']]['reason'])

    def test_adapter_preregisters_exact_limits_and_seed(self):
        row = next(row for row in self.contract['schedule'] if row['seed'] == 45)
        candidate = copy.deepcopy(self.candidate)
        candidate['model_path'] = 'local-model.gguf'
        candidate['profile_policies'][row['profile']]['variant'] = 'loop-repair'
        command = campaign.command_for(candidate, row, self.contract['profiles'][row['profile']],
                                       ROOT, self.root / 'output')
        self.assertIn('--retain-terminal', command)
        self.assertIn('--max-turns=32', command)
        self.assertIn('--max-input=262144', command)
        self.assertIn('--max-tokens=32768', command)
        self.assertEqual(command[command.index('--seed') + 1], '45')
        self.assertEqual(command[command.index('--repetitions') + 1], '1')

    def test_adapter_real_python_preflight(self):
        path = campaign.preflight(self.root, ROOT, self.row, self.fixture, 30)
        result = acceptance.read_json(path)
        self.assertTrue(result['baseline_failed'])
        self.assertTrue(result['oracle_passed'])
        self.assertEqual(result['oracle_test_count'], 2)

    def test_zero_exit_verifier_with_mutated_inputs_stays_nonpassing(self):
        target = self.root / 'adapter-run'
        output = target / 'harness' / f'{self.row["task"]}-loop-repair-r001'
        terminal = output / 'terminal-workspace'
        terminal.mkdir(parents=True)
        for name, content in self.terminal_contents.items():
            path = terminal / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        harness = {'returncode': 0, 'passed': True, 'protected_files_unchanged': True,
                   'protected_before_verification': True, 'verification_inputs_unchanged': False,
                   'verification': {'returncode': 0}, 'language': 'python',
                   'timing': {'agent_seconds': 10., 'verification_seconds': .2},
                   'metrics': {**self.payloads['metrics'], 'status': 'ok'}}
        campaign.write(output / 'result.json', harness)
        observed = {key: self.profile[key] for key in ('output_reserve', 'max_turns', 'max_tokens',
                    'max_input', 'temperature', 'prompt_protocol', 'lifecycle', 'gpu_index', 'order_seed')}
        observed.update(context_tokens=self.profile['context'], gpu_layers=str(self.profile['gpu_layers']),
                        chat_template=self.profile['chat_template'], seed=self.row['seed'],
                        forge_runtime_bundle={'sha256': self.candidate['identity']['runtime_sha256']},
                        model_sha256=self.candidate['identity']['model_sha256'])
        campaign.write(target / 'harness/environment.json', observed)
        campaign.write(target / 'command.json', ['synthetic harness'])
        campaign.write(output / 'command.json', ['synthetic Forge'])
        campaign.write(output / 'stdout.jsonl', {'synthetic': True})
        campaign.write(output / 'session/events.jsonl', {'synthetic': True})
        prompt = output / 'session/context/0001.txt'
        prompt.parent.mkdir()
        prompt.write_text('synthetic prompt')
        (output / 'verification.stdout').write_text('')
        (output / 'verification.stderr').write_text(self.payloads['verification_stderr'])
        preflight = self.root / 'preflight-fixture/preflight.json'
        campaign.write(preflight, self.payloads['preflight'])
        with mock.patch.object(campaign, 'observe', return_value=self.candidate['identity']):
            record = campaign.package_run(self.root, target, self.row, self.contract, self.candidate,
                self.candidate['identity'], self.record['started_utc'], self.record['finished_utc'], preflight)
        result = acceptance.validate_coding_record(self.contract, self.candidate, self.row, record, self.root)
        self.assertFalse(result['passed'])
        harness['verification_inputs_unchanged'] = True
        campaign.write(output / 'result.json', harness)
        with mock.patch.object(campaign, 'observe', return_value=self.candidate['identity']):
            record = campaign.package_run(self.root, target, self.row, self.contract, self.candidate,
                self.candidate['identity'], self.record['started_utc'], self.record['finished_utc'], preflight)
        result = acceptance.validate_coding_record(self.contract, self.candidate, self.row, record, self.root)
        self.assertTrue(result['passed'])

    def test_adapter_junit_preserves_skips_and_failures(self):
        junit = self.root / 'junit.xml'
        junit.write_text('<testsuite><testcase name="unit"/>'
                         '<testcase name="checkpoint_model"><skipped/></testcase>'
                         '<testcase name="broken"><failure/></testcase></testsuite>')
        log = self.root / 'check.log'
        log.write_text('test output')
        check = next(c for c in self.contract['g0_checks'] if c['check_id'] == 'full-gpu-ctest')
        result = campaign.g0_summary(check, 1, log, junit)
        self.assertEqual([test['status'] for test in result['tests']], ['passed', 'skipped', 'failed'])
        self.assertEqual(result['status'], 'failed')

    def test_g0_command_exit_tracks_full_gate_or_selected_check(self):
        result = {'gates': {'G0': {'accepted': False}},
                  'runs': {'G0-unit': {'status': 'passed'}}}
        args = SimpleNamespace(candidate_dir=self.root, check_id=None)
        with mock.patch.object(campaign, 'load', return_value=(
                {'g0_checks': []}, {'source_root': str(ROOT)})), \
                mock.patch.object(campaign, 'make_report', return_value=result):
            self.assertEqual(campaign.run_g0(args), 1)
            args.check_id = 'unit'
            self.assertEqual(campaign.run_g0(args), 0)
            result['runs']['G0-unit']['status'] = 'failed'
            self.assertEqual(campaign.run_g0(args), 1)
            args.check_id = None
            result['gates']['G0']['accepted'] = True
            self.assertEqual(campaign.run_g0(args), 0)

    def test_complete_synthetic_campaign_requires_separate_confirmation(self):
        # A small contract exercises aggregation; the production contract's exact
        # 504-run membership and seeds are checked separately above.
        contract = copy.deepcopy(self.contract)
        contract['suites'] = {'G1': {**contract['suites']['G1'], 'count': 1,
                                   'manifests': [self.row['manifest']], 'tasks': [self.row['task']]}}
        contract['fixtures'] = {self.row['manifest']: self.fixture}
        contract['schedule'] = acceptance.expected_schedule(contract)
        contract['denominators'] = {'development': {'G1': 3}, 'confirmation': {'G1': 6}, 'coding_total': 9}
        contract['g0_checks'] = [{'check_id': 'new-deterministic-regressions',
                                 'required_tests': ['test_agent_loop_acceptance']}]
        contract.pop('contract_sha256')
        contract['contract_sha256'] = acceptance.json_digest(contract)
        fixture_path = self.root / self.row['manifest']
        fixture_path.parent.mkdir(parents=True)
        fixture_path.write_bytes((ROOT / self.row['manifest']).read_bytes())
        self.candidate['contract_sha256'] = contract['contract_sha256']
        binary = self.artifact('synthetic-g0.exe', b'synthetic executable')
        self.candidate['g0_runtime'] = {'files': {'synthetic-g0.exe': binary}}
        self.seal_candidate()
        self.candidate['confirmation_frozen_utc'] = '2026-09-09T00:04:00+00:00'
        records = []
        for index, row in enumerate(contract['schedule']):
            record = copy.deepcopy(self.record)
            record.update(run_id=row['run_id'], execution_id=f'execution-{index}', schedule=row,
                          candidate_sha256=self.candidate['candidate_sha256'])
            minute = index + 1 if index < 3 else index + 2
            record.update(started_utc=f'2026-09-09T00:{minute:02}:00+00:00',
                          finished_utc=f'2026-09-09T00:{minute:02}:30+00:00',
                          settings={**self.profile, 'seed': row['seed']})
            self.record = record
            self.payloads['execution'].update({name: record[name] for name in (
                'run_id', 'execution_id', 'candidate_sha256', 'started_utc', 'finished_utc')})
            self.payloads['command']['settings'] = record['settings']
            self.seal_record(f'cases/{index}')
            records.append(copy.deepcopy(self.record))
        g0 = copy.deepcopy(records[0])
        g0.update(run_id='G0-new-deterministic-regressions', execution_id='g0-execution',
                  started_utc='2026-09-09T00:00:01+00:00', finished_utc='2026-09-09T00:00:30+00:00')
        hashes = {'synthetic-g0.exe': binary['sha256']}
        g0['evidence'] = self.artifact('g0-evidence.json', {
            **g0, 'returncode': 0, 'status': 'passed', 'log': self.artifact('g0.log', 'Ran 21 tests\nOK\n'),
            'g0_runtime_before': hashes, 'g0_runtime_after': hashes,
            'tests': [{'name': 'test_agent_loop_acceptance', 'status': 'passed'}]})
        result = acceptance.report(contract, self.candidate, records, [g0], self.root)
        self.assertTrue(result['accepted'], result)
        self.candidate.pop('confirmation_frozen_utc')
        result = acceptance.report(contract, self.candidate, records, [g0], self.root)
        self.assertFalse(result['accepted'])
        self.assertTrue(any('confirmation freeze' in error for error in result['errors']))

    def test_native_g0_requires_direct_supported_probe(self):
        check = next(check for check in self.contract['g0_checks']
                     if check['check_id'] == 'checkpoint-model')
        record = {**self.record, 'run_id': 'G0-checkpoint-model'}
        payload = {**record, 'command': check['command'], 'returncode': 77, 'status': 'unsupported'}
        record['evidence'] = self.artifact('g0.json', payload)
        with self.assertRaisesRegex(acceptance.EvidenceError, 'unsupported'):
            acceptance.validate_g0(self.candidate, check, record, self.root)

    def test_go_test_parser_requires_actual_tests_and_final_package(self):
        def events(rows):
            return '\n'.join(json.dumps(row) for row in rows)
        complete = [{'Action': 'pass', 'Package': 'fixture', 'Test': 'TestBehavior'},
                    {'Action': 'pass', 'Package': 'fixture'}]
        self.assertEqual(acceptance.test_count('go', events(complete)), 1)
        with self.assertRaises(acceptance.EvidenceError):
            acceptance.test_count('go', events(complete[:1]))
        with self.assertRaises(acceptance.EvidenceError):
            acceptance.test_count('go', events(complete + [{'Action': 'skip'}]))


if __name__ == '__main__':
    unittest.main()
