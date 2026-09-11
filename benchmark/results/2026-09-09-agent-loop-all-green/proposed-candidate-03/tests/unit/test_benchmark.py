"""Check fixture preparation independently of any model or harness."""
import contextlib
import importlib.util
import io
import json
import os
import pathlib
import py_compile
import shutil
import subprocess
import sys
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
import common as COMMON


class FixtureTests(unittest.TestCase):
    def test_generalization_cases_are_deterministic_and_grouped(self):
        from generalization import build_tasks
        tasks = build_tasks()
        self.assertEqual(tasks, build_tasks())
        self.assertEqual(len(tasks), 10)
        self.assertEqual(len({task['id'] for task in tasks}), 10)
        for family in ('retractions', 'window'):
            members = [task for task in tasks if task['generalization']['family'] == family]
            self.assertEqual({task['generalization']['variant'] for task in members},
                             {'original', 'renamed', 'paraphrased', 'distractor', 'contrast'})
            self.assertEqual(sum(task['generalization']['relation'] == 'contrast'
                                 for task in members), 1)
            for task in members:
                self.assertEqual(task['suite'], 'generalization-v1')
                self.assertEqual(len(task['generalization']['source_sha256']), 64)
                self.assertFalse(set(task['oracle_files']) & set(task['protected_files']))

    def test_generalization_broken_and_oracle_states(self):
        from generalization import build_tasks
        if not shutil.which('go') or not shutil.which('gofmt'):
            self.skipTest('Go and gofmt are needed to verify generalization fixtures')
        for task in build_tasks():
            with self.subTest(task=task['id']), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                BENCH.materialize(root, task)
                broken = subprocess.run(task['verify'], cwd=root, capture_output=True, timeout=60)
                self.assertNotEqual(broken.returncode, 0, 'broken fixture unexpectedly passed')
                repaired = dict(task, files={**task['files'], **task['oracle_files']})
                BENCH.materialize(root, repaired)
                oracle = subprocess.run(task['verify'], cwd=root, capture_output=True, timeout=60)
                self.assertEqual(oracle.returncode, 0,
                                 (oracle.stdout + oracle.stderr).decode(errors='replace'))

    def test_generalization_output_cannot_overwrite_existing_evidence(self):
        from generalization import generate
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / 'tasks'
            generate(output)
            before = {path.name: path.read_bytes() for path in output.glob('*.json')}
            with self.assertRaises(FileExistsError):
                generate(output)
            self.assertEqual(before, {path.name: path.read_bytes()
                                      for path in output.glob('*.json')})

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
            for requested in (None, 'flattened'):
                with self.subTest(requested=requested):
                    arm = requested or 'native'
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
            for requested in (None, 'flattened'):
                arm = requested or 'native'
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
                        platform_metadata=mock.Mock(return_value={'gpu': 'test device'}),
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

            argv = ['campaign.py', '--output', str(output), '--task-dir', str(root / 'holdout'),
                    '--require-clean']
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
            for label in ('preflight', 'freeze', 'forge', 'opencode', 'aider'):
                index = commands[label].index('--task-dir')
                self.assertEqual(pathlib.Path(commands[label][index + 1]), (root / 'holdout').resolve())
            self.assertIn('--require-clean', commands['freeze'])
            campaign = json.loads((output / 'campaign.json').read_text(encoding='utf-8'))
            self.assertEqual(campaign['prompt_protocol'], 'native')

    def test_clean_freeze_rejects_dirty_source_before_loading_tasks(self):
        argv = ['freeze.py', '--require-clean', '--output', 'unused.json']
        for name in ('forge', 'opencode', 'aider', 'server', 'model'):
            argv += [f'--{name}', 'unused']
        for status in (' M src/core/agent.c', '?? benchmark/new_task.json'):
            with self.subTest(status=status), \
                    mock.patch.object(FREEZE.sys, 'argv', argv), \
                    mock.patch.object(FREEZE, 'version', side_effect=['abc123', status]), \
                    mock.patch.object(FREEZE, 'load_tasks') as load, \
                    contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as raised:
                FREEZE.main()
            self.assertEqual(raised.exception.code, 2)
            load.assert_not_called()

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


class VerificationCacheTests(unittest.TestCase):
    SOURCE = 'def value():\n    return 1\n'
    TEST = ('import unittest\nfrom value import value\n'
            'class ValueTests(unittest.TestCase):\n'
            '    def test_value(self):\n        self.assertEqual(value(), 2)\n')

    @classmethod
    def task(cls):
        return {'id': 'cache', 'language': 'python', 'prompt': 'Repair value; do not change tests.',
                'files': {'value.py': cls.SOURCE, 'test_value.py': cls.TEST},
                'protected_files': ['test_value.py'],
                'verify': [sys.executable, '-m', 'unittest', 'discover', '-v']}

    def verify(self, root, output, task, env=None):
        output.mkdir()
        # Verifier subprocesses are real; this unit test does not sample GPUs.
        with mock.patch.object(COMMON, 'ResourceMonitor') as monitor:
            monitor.return_value.stop.return_value = {}
            result = COMMON.verify_task(root, task, output, env=env, timeout=15)
        return result, (output / 'verification.stdout').read_text(encoding='utf-8'), \
            (output / 'verification.stderr').read_text(encoding='utf-8')

    def test_source_and_tests_override_timestamp_and_unchecked_bytecode(self):
        for mode in (py_compile.PycInvalidationMode.TIMESTAMP,
                     py_compile.PycInvalidationMode.UNCHECKED_HASH):
            for target in ('value.py', 'test_value.py'):
                for current_passes in (False, True):
                    with self.subTest(mode=mode, target=target, current_passes=current_passes), \
                            tempfile.TemporaryDirectory(prefix='forge-cache-test-') as temporary:
                        base = pathlib.Path(temporary)
                        root = base / 'input'
                        root.mkdir()
                        task = self.task()
                        source = self.SOURCE.replace('return 1', 'return 2') if current_passes else self.SOURCE
                        (root / 'value.py').write_text(source, encoding='utf-8', newline='\n')
                        (root / 'test_value.py').write_text(self.TEST, encoding='utf-8', newline='\n')
                        path = root / target
                        current = path.read_bytes()
                        if target == 'value.py':
                            cached = self.SOURCE if current_passes else self.SOURCE.replace('return 1', 'return 2')
                        else:
                            cached = self.TEST.replace('value(), 2', 'value(), 1')
                        path.write_text(cached, encoding='utf-8', newline='\n')
                        stamp = path.stat()
                        py_compile.compile(str(path), doraise=True, invalidation_mode=mode)
                        path.write_bytes(current)
                        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                        before = BENCH.snapshot_terminal_inputs(root)
                        result, _, stderr = self.verify(root, base / 'output', task)
                        self.assertEqual(result['passed'], current_passes, stderr)
                        self.assertRegex(stderr, r'Ran 1 test\b')
                        self.assertEqual(result['python_cache_policy'], 'fresh-external-prefix-no-write')
                        self.assertEqual(BENCH.snapshot_terminal_inputs(root), before)

    def test_fresh_prefix_is_per_python_child_and_does_not_mutate_caller_env(self):
        with tempfile.TemporaryDirectory(prefix='forge-cache-env-test-') as temporary:
            root = pathlib.Path(temporary)
            code = ('import json,os,sys;print(json.dumps([sys.pycache_prefix,'
                    'sys.dont_write_bytecode,os.environ["FORGE_TEST_SENTINEL"]]))')
            command = [sys.executable, '-c', code]
            caller = {**os.environ, 'PYTHONPYCACHEPREFIX': str(root / 'old-cache'),
                      'PYTHONDONTWRITEBYTECODE': '', 'FORGE_TEST_SENTINEL': 'preserved'}
            original_caller, original_process = caller.copy(), os.environ.copy()
            prefixes = []
            for index in range(2):
                result, stdout, _ = self.verify(root, root / f'python-{index}',
                                                {'language': 'python', 'verify': command}, caller)
                prefix, disabled, sentinel = json.loads(stdout)
                self.assertTrue(result['passed'])
                self.assertEqual(result['python_cache_policy'], 'fresh-external-prefix-no-write')
                self.assertFalse(pathlib.Path(prefix).is_relative_to(root))
                self.assertFalse(pathlib.Path(prefix).exists())
                self.assertTrue(disabled)
                self.assertEqual(sentinel, 'preserved')
                prefixes.append(prefix)
            self.assertNotEqual(*prefixes)
            result, stdout, _ = self.verify(root, root / 'other', {'language': 'go', 'verify': command}, caller)
            self.assertEqual(json.loads(stdout), [caller['PYTHONPYCACHEPREFIX'], False, 'preserved'])
            self.assertEqual(result['python_cache_policy'], 'not-applicable')
            self.assertEqual(caller, original_caller)
            self.assertEqual(dict(os.environ), original_process)

    def test_retention_excludes_only_root_metadata_with_host_case_rules(self):
        with tempfile.TemporaryDirectory(prefix='forge-retention-test-') as temporary:
            base = pathlib.Path(temporary)
            root = base / 'input'
            root.mkdir()
            names = ('value.py', '.git/config', '.forge/state', '.GIT/upper', '.FORGE/upper',
                     '__pycache__/value.pyc', '.pytest_cache/state',
                     'nested/.git/config', 'nested/.forge/state')
            for name in names:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())
            before = BENCH.snapshot_terminal_inputs(root)
            expected = {name for name in names if not name.startswith(('.git/', '.forge/'))}
            if os.name == 'nt':
                expected -= {'.GIT/upper', '.FORGE/upper'}
            self.assertEqual(set(before), expected)
            BENCH.copy_terminal_inputs(root, base / 'copy')
            self.assertEqual(BENCH.snapshot_terminal_inputs(base / 'copy'), before)
            self.assertFalse((base / 'copy/.git').exists())
            self.assertFalse((base / 'copy/.forge').exists())

    def test_terminal_copies_retain_bytecode_and_both_sides_of_verifier_mutation(self):
        for mutate in (False, True):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory(prefix='forge-retention-run-') as temporary:
                base = pathlib.Path(temporary)
                forge, model = base / 'forge.exe', base / 'model.gguf'
                forge.write_bytes(b'not executed')
                model.write_bytes(b'not loaded')
                task = self.task()
                if mutate:
                    task['verify'] = [sys.executable, '-c',
                        "from pathlib import Path;Path('.pytest_cache/state').write_bytes(b'after')"]
                seeded = {}

                def fake_forge(command, **_kwargs):
                    root = pathlib.Path(command[command.index('--workspace') + 1])
                    source = root / 'value.py'
                    source.write_text(self.SOURCE.replace('return 1', 'return 2'), encoding='utf-8')
                    stamp = source.stat()
                    py_compile.compile(str(source), doraise=True)
                    source.write_text(self.SOURCE, encoding='utf-8')
                    os.utime(source, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
                    cache = root / '.pytest_cache/state'
                    cache.parent.mkdir()
                    cache.write_bytes(b'before')
                    seeded.update(BENCH.snapshot_terminal_inputs(root))
                    return {'returncode': 0, 'wall_seconds': .01, 'resource_usage': {}}

                output = base / 'output'
                argv = ['run.py', '--forge', str(forge), '--model', str(model), '--output', str(output),
                        '--no-randomize', '--retain-terminal']
                with mock.patch.multiple(BENCH, load_tasks=mock.Mock(return_value=[(base / 'task.json', task)]),
                                         check_tools=mock.Mock(), initialize_git=mock.Mock(),
                                         runtime_bundle=mock.Mock(return_value={'files': []}),
                                         platform_metadata=mock.Mock(return_value={'platform': 'test'}),
                                         run_monitored=fake_forge), \
                        mock.patch.object(COMMON, 'ResourceMonitor') as monitor, \
                        mock.patch.object(BENCH.subprocess, 'check_output', return_value='forge test'), \
                        mock.patch.object(BENCH.subprocess, 'run'), \
                        mock.patch.object(BENCH.sys, 'argv', argv), \
                        contextlib.redirect_stdout(io.StringIO()):
                    monitor.return_value.stop.return_value = {}
                    returncode = BENCH.main()
                case = output / 'cache-optimized-r001'
                record = json.loads((case / 'result.json').read_text(encoding='utf-8'))
                maps = json.loads((case / 'verification-inputs.json').read_text(encoding='utf-8'))
                self.assertEqual(returncode, 1)
                self.assertFalse(record['passed'])
                self.assertEqual(record['verification']['passed'], mutate)
                self.assertTrue(record['protected_files_unchanged'])
                self.assertEqual(record['verification_inputs_unchanged'], not mutate)
                self.assertEqual(record['terminal_retention_policy'], 'complete-except-git-forge')
                self.assertEqual(record['pre_verification_workspace'], 'pre-verification-workspace')
                self.assertEqual(record['verification']['python_cache_policy'], 'fresh-external-prefix-no-write')
                self.assertEqual(maps['before'], seeded)
                self.assertEqual(BENCH.snapshot_terminal_inputs(case / 'pre-verification-workspace'), maps['before'])
                self.assertEqual(BENCH.snapshot_terminal_inputs(case / 'terminal-workspace'), maps['after'])
                self.assertEqual((case / 'pre-verification-workspace/.pytest_cache/state').read_bytes(), b'before')
                self.assertEqual((case / 'terminal-workspace/.pytest_cache/state').read_bytes(),
                                 b'after' if mutate else b'before')

    def test_retention_keeps_regular_root_metadata_files(self):
        for names in (('.git', '.forge'), ('.GIT', '.FORGE')):
            with self.subTest(names=names), tempfile.TemporaryDirectory(prefix='forge-retention-files-') as temporary:
                base = pathlib.Path(temporary)
                root = base / 'input'
                root.mkdir()
                for name in names:
                    (root / name).write_bytes(b'regular metadata-named input file')
                    self.assertFalse(BENCH.terminal_root_ignored(name, False))
                    self.assertEqual(BENCH.terminal_root_ignored(name, True),
                                     os.name == 'nt' or name in ('.git', '.forge'))
                before = BENCH.snapshot_terminal_inputs(root)
                self.assertEqual(set(before), set(names))
                BENCH.copy_terminal_inputs(root, base / 'copy')
                self.assertEqual(BENCH.snapshot_terminal_inputs(base / 'copy'), before)
                for name in names:
                    self.assertEqual((base / 'copy' / name).read_bytes(), (root / name).read_bytes())


if __name__ == '__main__':
    unittest.main()
