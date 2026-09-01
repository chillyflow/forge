"""Check fixture preparation independently of any model or harness."""
import contextlib
import importlib.util
import io
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

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
FREEZE_SPEC = importlib.util.spec_from_file_location(
    'forge_benchmark_freeze', SOURCE.parent / 'freeze.py')
FREEZE = importlib.util.module_from_spec(FREEZE_SPEC)
FREEZE_SPEC.loader.exec_module(FREEZE)
CAMPAIGN_SPEC = importlib.util.spec_from_file_location(
    'forge_benchmark_campaign', SOURCE.parent / 'campaign.py')
CAMPAIGN = importlib.util.module_from_spec(CAMPAIGN_SPEC)
CAMPAIGN_SPEC.loader.exec_module(CAMPAIGN)


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
                            'wall_seconds': seconds, 'language': 'python', 'category': 'api',
                            'timing': {'end_to_end_seconds': seconds,
                                       'agent_seconds': seconds - .2,
                                       'startup_seconds': .1, 'verification_seconds': .1},
                            'metrics': {'prompt_tokens': 100,
                                        'generated_tokens': 10 * repetition},
                            'fixture_preparation': 'v2', 'fixture_sha256': 'same',
                            'fixture_files': {'a.py': 'hash'},
                            'protected_files': {'test_a.py': 'protected'},
                            'resource_usage': {'peak_process_tree_rss_bytes': 100 * repetition,
                                               'peak_gpu_used_bytes': 200 * repetition}})
        summary = REPORT.summarize(records)
        self.assertEqual(summary['passed'], 2)
        self.assertEqual(summary['end_to_end_seconds']['p50'], 2.0)
        self.assertGreater(summary['end_to_end_seconds']['stdev'], 0)
        self.assertEqual(summary['pass_rate_ci']['estimate'], 2 / 3)
        self.assertIn('python', summary['per_language'])
        self.assertIn('api', summary['per_category'])
        comparisons = REPORT.pairwise_comparisons(
            {'one': records, 'two': [dict(record) for record in records]})
        self.assertEqual(comparisons['two minus one']['both_passed_pairs'], 2)
        self.assertEqual(
            comparisons['two minus one']['matched_end_to_end_difference_seconds'], 0.0)
        REPORT.validate_groups({'one': records, 'two': [dict(record) for record in records]})
        changed = [dict(record) for record in records]
        changed[0]['fixture_sha256'] = 'different'
        with self.assertRaises(ValueError):
            REPORT.validate_groups({'one': records, 'two': changed})


    def test_measurement_gate_rejects_zero_tokens_and_order_drift(self):
        records = []
        for task, repetition, order_index in BENCH.schedule(['a', 'b'], 2, 7):
            records.append({
                'schema_version': 2, 'run_id': f'{task}-r{repetition:03d}',
                'task': task, 'repetition': repetition, 'order_index': order_index,
                'order_seed': 7, 'passed': True, 'protected_files_unchanged': True,
                'timing': {'lifecycle': 'cold', 'startup_seconds': .1,
                           'agent_seconds': .2, 'verification_seconds': .1,
                           'end_to_end_seconds': .4},
                'metrics': {'prompt_tokens': 10, 'generated_tokens': 2},
                'resource_usage': {'peak_process_tree_rss_bytes': 100,
                                   'peak_gpu_used_bytes': 200},
            })
        REPORT.validate_measurements({'harness': records})
        zero = json.loads(json.dumps(records))
        zero[0]['metrics']['generated_tokens'] = 0
        with self.assertRaises(ValueError):
            REPORT.validate_measurements({'harness': zero})
        reordered = list(records)
        reordered[0], reordered[1] = reordered[1], reordered[0]
        with self.assertRaises(ValueError):
            REPORT.validate_measurements({'harness': reordered})

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

    def test_native_tool_call_marker_preserves_reasoning_census(self):
        native_call = '<tool_call>\n{"name":"final","arguments":{"answer":"ok"}}'
        self.assertEqual(CONSOLIDATE.classify(native_call, ''), 'none')
        self.assertEqual(CONSOLIDATE.classify('inspect the result\n' + native_call, ''),
                         'routed_prefix')

    def test_prompt_protocol_is_forwarded_and_recorded_per_run(self):
        task = {'id': 'one', 'prompt': 'repair it', 'files': {'answer.txt': 'broken\n'},
                'protected_files': ['answer.txt']}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            forge, model = root / 'forge.exe', root / 'model.gguf'
            forge.write_bytes(b'forge')
            model.write_bytes(b'model')
            for requested in (None, 'native'):
                with self.subTest(requested=requested):
                    arm = requested or 'flattened'
                    output = root / f'output-{arm}'
                    commands = []

                    def monitored(command, **_kwargs):
                        commands.append(command)
                        return {'returncode': 0, 'wall_seconds': 1.0, 'resource_usage': {}}

                    argv = ['run.py', '--forge', str(forge), '--model', str(model),
                            '--output', str(output), '--no-randomize']
                    if requested:
                        argv += ['--prompt-protocol', requested]
                    with mock.patch.multiple(
                            BENCH,
                            load_tasks=mock.Mock(return_value=[(root / 'one.json', task)]),
                            check_tools=mock.Mock(),
                            initialize_git=mock.Mock(),
                            runtime_bundle=mock.Mock(return_value={'files': []}),
                            platform_metadata=mock.Mock(return_value={'platform': 'test'}),
                            run_monitored=monitored,
                            verify_task=mock.Mock(return_value={
                                'passed': True, 'wall_seconds': .2, 'resource_usage': {}})), \
                            mock.patch.object(BENCH.subprocess, 'check_output',
                                              return_value='forge test'), \
                            mock.patch.object(BENCH.subprocess, 'run'):
                        with mock.patch.object(BENCH.sys, 'argv', argv), \
                                contextlib.redirect_stdout(io.StringIO()):
                            self.assertEqual(BENCH.main(), 0)

                    self.assertEqual(len(commands), 1)
                    protocol_index = commands[0].index('--prompt-protocol')
                    self.assertEqual(commands[0][protocol_index + 1], arm)
                    environment = json.loads(
                        (output / 'environment.json').read_text(encoding='utf-8'))
                    result = json.loads((output / 'one-optimized-r001' / 'result.json')
                                        .read_text(encoding='utf-8'))
                    aggregate = json.loads(
                        (output / 'results.json').read_text(encoding='utf-8'))
                    self.assertEqual(environment['prompt_protocol'], arm)
                    self.assertEqual(result['prompt_protocol'], arm)
                    self.assertEqual(aggregate[0]['prompt_protocol'], arm)

    def test_prompt_protocol_rejects_unknown_arm(self):
        argv = ['run.py', '--forge', 'missing', '--model', 'missing', '--output', 'unused',
                '--prompt-protocol', 'unsupported']
        with mock.patch.object(BENCH.sys, 'argv', argv), \
                contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as error:
            BENCH.main()
        self.assertEqual(error.exception.code, 2)

    def test_native_protocol_rejects_routed_decode_variants(self):
        argv = ['run.py', '--forge', 'unused', '--model', 'unused', '--output', 'unused',
                '--prompt-protocol', 'native', '--variants', 'thought-routed']
        with mock.patch.object(BENCH.sys, 'argv', argv), \
                contextlib.redirect_stderr(io.StringIO()) as stderr, \
                self.assertRaises(SystemExit) as error:
            BENCH.main()
        self.assertEqual(error.exception.code, 2)
        self.assertIn('incompatible', stderr.getvalue())

    def test_protocol_freeze_records_prompt_arm_in_hash(self):
        task = {'id': 'one', 'files': {'answer.txt': 'broken\n'}}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            binaries = {}
            for name in ('forge', 'opencode', 'aider', 'server', 'model'):
                binaries[name] = root / name
                binaries[name].write_bytes(name.encode())
            task_path = root / 'one.json'
            task_path.write_text('{}\n', encoding='utf-8')
            freezes = {}
            for requested in (None, 'native'):
                arm = requested or 'flattened'
                output = root / f'protocol-{arm}.json'
                argv = ['freeze.py']
                for name, path in binaries.items():
                    argv += [f'--{name}', str(path)]
                argv += ['--output', str(output)]
                if requested:
                    argv += ['--prompt-protocol', requested]
                with mock.patch.multiple(
                        FREEZE,
                        load_tasks=mock.Mock(return_value=[(task_path, task)]),
                        check_tools=mock.Mock(),
                        source_identity=mock.Mock(return_value={}),
                        runtime_bundle=mock.Mock(return_value={'files': []}),
                        version=mock.Mock(return_value=FREEZE.PINNED_AIDER_VERSION)):
                    with mock.patch.object(FREEZE.sys, 'argv', argv), \
                            contextlib.redirect_stdout(io.StringIO()):
                        self.assertIsNone(FREEZE.main())
                freezes[arm] = json.loads(output.read_text(encoding='utf-8'))
                self.assertEqual(freezes[arm]['configuration']['prompt_protocol'], arm)
            self.assertNotEqual(freezes['flattened']['protocol_sha256'],
                                freezes['native']['protocol_sha256'])

    def test_campaign_routes_prompt_protocol_only_to_forge(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            binaries = {}
            for name in ('forge', 'opencode', 'aider', 'server', 'model'):
                binaries[name] = root / name
                binaries[name].write_bytes(name.encode())
            output = root / 'campaign'
            commands = {}

            def execute(label, command, _env):
                commands[label] = command
                return 0

            argv = ['campaign.py', '--output', str(output), '--prompt-protocol', 'native']
            for name, path in binaries.items():
                argv += [f'--{name}', str(path)]
            with mock.patch.object(CAMPAIGN, 'execute', execute), \
                    mock.patch.object(CAMPAIGN.sys, 'argv', argv):
                self.assertEqual(CAMPAIGN.main(), 0)

            for label in ('freeze', 'forge'):
                index = commands[label].index('--prompt-protocol')
                self.assertEqual(commands[label][index + 1], 'native')
            self.assertNotIn('--prompt-protocol', commands['opencode'])
            self.assertNotIn('--prompt-protocol', commands['aider'])
            campaign = json.loads((output / 'campaign.json').read_text(encoding='utf-8'))
            self.assertEqual(campaign['prompt_protocol'], 'native')

    def test_consolidation_checks_complete_fixture_sets(self):
        environment: dict = {key: 'same' for key in CONSOLIDATE.IDENTITY}
        environment['repetitions'] = 1
        base = {'task': 'a', 'variant': 'optimized', 'repetition': 1,
                'order_index': 1, 'fixture_preparation': 'v1',
                'fixture_sha256': 'a', 'fixture_files': {'a.go': 'hash'},
                'suite': 'smoke'}
        arms = {
            'flattened': ([base], environment),
            'native': ([dict(base)], environment),
        }
        tasks, mismatches = CONSOLIDATE.check_records(arms)
        self.assertEqual(tasks, ['a'])
        self.assertEqual(mismatches, {})
        arms['native'][0].append(dict(base))
        self.assertIn('native', CONSOLIDATE.check_records(arms)[1])

    def test_consolidation_records_protocol_as_treatment(self):
        environments = {
            'flattened': ([], {'prompt_protocol': 'flattened'}),
            'native': ([], {'prompt_protocol': 'native'}),
        }
        protocol, by_arm = CONSOLIDATE.prompt_protocol_provenance(environments)
        self.assertEqual(protocol, 'mixed')
        self.assertEqual(by_arm, {'flattened': 'flattened', 'native': 'native'})
        self.assertNotIn('prompt_protocol', CONSOLIDATE.IDENTITY)

        records = {'flattened': ([{'prompt_protocol': 'flattened'}],
                                 {'prompt_protocol': 'flattened'}),
                   'native': ([{'prompt_protocol': 'native'}],
                              {'prompt_protocol': 'native'})}
        self.assertEqual(CONSOLIDATE.check_prompt_protocol_provenance(records)[2], {})
        records['native'][0][0]['prompt_protocol'] = 'flattened'
        self.assertIn('native',
                      CONSOLIDATE.check_prompt_protocol_provenance(records)[2])

    def test_consolidation_supports_repeated_prompt_protocol_ab(self):
        tasks = ['gap-a', 'gap-b', 'gap-c', 'gap-d']
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            run_root, output = root / 'runs', root / 'consolidated'
            for arm in ('flattened', 'native'):
                arm_root = run_root / arm
                arm_root.mkdir(parents=True)
                environment: dict = {key: 'same' for key in CONSOLIDATE.IDENTITY}
                environment.update({'prompt_protocol': arm, 'repetitions': 3,
                                    'order_seed': 17, 'randomized_order': True,
                                    'lifecycle': 'cold'})
                records = []
                order_index = 0
                for repetition in range(1, 4):
                    for task in tasks:
                        order_index += 1
                        records.append({
                            'task': task, 'variant': 'optimized',
                            'repetition': repetition, 'order_index': order_index,
                            'prompt_protocol': arm, 'fixture_preparation': 'v1',
                            'fixture_sha256': task + '-hash',
                            'fixture_files': {task + '.py': 'hash'},
                            'suite': 'reasoning-gated'})
                (arm_root / 'environment.json').write_text(
                    json.dumps(environment), encoding='utf-8')
                (arm_root / 'results.json').write_text(
                    json.dumps(records), encoding='utf-8')

            argv = ['consolidate_arms.py', str(run_root), str(output),
                    '--expect-arms', 'flattened', 'native',
                    '--expect-tasks', *tasks]
            with mock.patch.object(CONSOLIDATE.sys, 'argv', argv), \
                    contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(CONSOLIDATE.main(), 0)

            consolidated = json.loads((output / 'results.json').read_text(encoding='utf-8'))
            environment = json.loads(
                (output / 'environment.json').read_text(encoding='utf-8'))
            self.assertEqual(len(consolidated), 24)
            self.assertEqual({record['variant'] for record in consolidated}, {'optimized'})
            self.assertEqual(environment['prompt_protocol'], 'mixed')
            self.assertEqual(environment['prompt_protocol_by_arm'],
                             {'flattened': 'flattened', 'native': 'native'})
            self.assertEqual(environment['record_identity_verified'],
                             list(CONSOLIDATE.RECORD_IDENTITY))

    def test_run_identity_covers_decode_configuration(self):
        for key in ('gpu_layers', 'chat_template', 'task_suite', 'output_reserve',
                    'temperature', 'seed', 'repetitions', 'order_seed',
                    'randomized_order', 'lifecycle', 'platform', 'go_version', 'gpu'):
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
