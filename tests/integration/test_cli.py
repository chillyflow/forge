import json
import os
import pathlib
import queue
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())
FALLBACK_FORGE = None
if '--fallback-forge' in sys.argv:
    index = sys.argv.index('--fallback-forge')
    FALLBACK_FORGE = str(pathlib.Path(sys.argv.pop(index + 1)).resolve())
    sys.argv.pop(index)

class ForgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge-test-')
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'calc.go').write_text('package calc\n\nfunc Add(a, b int) int { return a - b }\n')
        (self.root / 'caller.go').write_text('package calc\nfunc Use() int { return Add(1, 2) }\n')

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *args, success=True, executable=FORGE, **run_options):
        run_options.setdefault('timeout', 90)
        result = subprocess.run([executable, *args, '--workspace', str(self.root)], capture_output=True,
                                encoding='utf-8', **run_options)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def run_script(self, actions, *options, success=True, fallback_watch=False):
        path = self.root / 'script.json'
        # Strict validation-order fixtures use the real bounded snapshot
        # fallback. Other scripts retain native monitoring. Native
        # notifications (notably FSEvents) may arrive after a known edit
        # was already indexed. The runtime conservatively requests fresh
        # generation. These successful fixtures can repeat their terminal
        # response; never repeat mutation actions or extend failure fixtures.
        fallback = FALLBACK_FORGE if fallback_watch else None
        scripted = list(actions)
        if not fallback and success and scripted and 'final' in scripted[-1]:
            scripted.extend([scripted[-1]] * 4)
        path.write_text(json.dumps(scripted))
        result = self.cli('run', 'Fix Add and verify it.', '--script', str(path), '--json',
                          *options, success=success, executable=fallback or FORGE)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        if fallback:
            scans = [event['data'] for event in events if event['type'] == 'repository_scan']
            self.assertTrue(scans, result.stdout + result.stderr)
            self.assertTrue(all(not scan['watch_available'] for scan in scans))
            self.assertIn('fallback test fixture', scans[0]['watch_fallback'])
        sessions = sorted((self.root / '.forge' / 'sessions').iterdir(), key=lambda p: p.stat().st_mtime_ns)
        return result, events, sessions[-1]

    def go_module(self, expression='a + b'):
        (self.root / 'caller.go').unlink(missing_ok=True)
        (self.root / 'go.mod').write_text('module example.test/calc\n\ngo 1.22\n', newline='\n')
        (self.root / 'calc.go').write_text(
            f'package calc\n\nfunc Add(a, b int) int {{\n\treturn {expression}\n}}\n', newline='\n')
        (self.root / 'calc_test.go').write_text(
            'package calc\n\nimport "testing"\n\nfunc TestAdd(t *testing.T) {\n'
            '\tif got := Add(2, 3); got != 5 {\n'
            '\t\tt.Fatalf("got %d, want 5", got)\n\t}\n}\n', newline='\n')

    def require_go(self):
        if not shutil.which('go') or not shutil.which('gofmt'):
            self.skipTest('Go and gofmt are needed for real automatic validation')

    def indexed_rows(self, query):
        with sqlite3.connect(self.root / '.forge' / 'index.db') as connection:
            rows = connection.execute(query).fetchall()
        connection.close()
        return rows

    def test_delta_index_only_refreshes_named_files_and_rolls_back(self):
        result = self.cli('index', '--json')
        initial = json.loads(result.stdout)['generation']
        (self.root / 'calc.go').write_text('package calc\nfunc Renamed() {}\n')
        (self.root / 'caller.go').write_text('package calc\nfunc UnindexedChange() {}\n')
        update = json.loads(self.cli('index', 'calc.go', '--json').stdout)
        self.assertEqual(update, {'index_mode': 'delta', 'generation': initial + 1, 'paths': 1})
        self.assertEqual(set(row[0] for row in self.indexed_rows('SELECT name FROM symbols')),
                         {'Renamed', 'Use'})
        (self.root / 'calc.go').write_text('package calc\nfunc ShouldRollback() {}\n')
        with sqlite3.connect(self.root / '.forge' / 'index.db') as connection:
            connection.execute("CREATE TRIGGER deny_update BEFORE DELETE ON files "
                               "BEGIN SELECT RAISE(ABORT, 'fixture transaction failure'); END")
        connection.close()
        self.cli('index', 'calc.go', success=False)
        self.assertEqual(set(row[0] for row in self.indexed_rows('SELECT name FROM symbols')),
                         {'Renamed', 'Use'})
        self.assertEqual(self.indexed_rows("SELECT value FROM meta WHERE key='generation'"),
                         [(initial + 1,)])
        with sqlite3.connect(self.root / '.forge' / 'index.db') as connection:
            connection.execute('DROP TRIGGER deny_update')
        connection.close()
        (self.root / 'calc.go').unlink()
        self.cli('index', 'calc.go')
        self.assertEqual(self.indexed_rows('SELECT name FROM symbols'), [('Use',)])

    def test_delta_index_respects_git_ignored_and_forced_tracked_paths(self):
        if not shutil.which('git'):
            self.skipTest('Git is needed for eligibility rules')
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True, capture_output=True)
        (self.root / '.gitignore').write_text('ignored.go\n.forge/\n', newline='\n')
        (self.root / 'ignored.go').write_text('package calc\nfunc IgnoreMe() {}\n')
        first = json.loads(self.cli('index', '--json').stdout)
        delta = json.loads(self.cli('index', 'ignored.go', '--json').stdout)
        self.assertEqual(delta['generation'], first['generation'])
        self.assertNotIn(('IgnoreMe',), self.indexed_rows('SELECT name FROM symbols'))
        subprocess.run(['git', '-C', str(self.root), 'add', '-f', '--', 'ignored.go'],
                       check=True, capture_output=True)
        delta = json.loads(self.cli('index', 'ignored.go', '--json').stdout)
        self.assertEqual(delta['generation'], first['generation'] + 1)
        self.assertIn(('IgnoreMe',), self.indexed_rows('SELECT name FROM symbols'))
        (self.root / 'ignored.go').unlink()
        self.cli('index', 'ignored.go')
        self.assertNotIn(('IgnoreMe',), self.indexed_rows('SELECT name FROM symbols'))

    def test_native_watch_updates_named_source_and_excludes_session_metadata(self):
        process = subprocess.Popen(
            [FORGE, 'watch', '--workspace', str(self.root), '--wall-ms', '2500', '--json'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding='utf-8')
        lines = queue.Queue()
        def read_lines():
            for line in process.stdout:
                lines.put(line)
        reader = threading.Thread(target=read_lines, daemon=True)
        reader.start()
        try:
            ready_line = lines.get(timeout=10)
            ready = json.loads(ready_line)
            self.assertEqual(ready['index_mode'], 'full')
            self.assertTrue(ready['watch_available'])
            (self.root / 'calc.go').write_text('package calc\nfunc ChangedByEditor() {}\n')
            (self.root / '.forge' / 'private.go').write_text('package private\n')
            self.assertEqual(process.wait(timeout=10), 0, process.stderr.read())
            reader.join(timeout=2)
            self.assertFalse(reader.is_alive())
            batches = []
            while not lines.empty():
                batches.append(json.loads(lines.get_nowait()))
            self.assertTrue(any(batch['index_mode'] == 'delta' for batch in batches), batches)
            observed = [event['path'] for batch in batches for event in batch['events']]
            self.assertIn('calc.go', observed)
            self.assertFalse(any(path.startswith('.forge/') for path in observed))
            self.assertIn(('ChangedByEditor',), self.indexed_rows('SELECT name FROM symbols'))
            self.assertTrue(any(batch['generation'] > ready['generation'] for batch in batches))
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            reader.join(timeout=2)
            process.stdout.close()
            process.stderr.close()

    def test_typed_memory_preserves_claims_without_forging_evidence(self):
        memory = {'facts': ['The model claims all tests pass.'], 'hypotheses': [],
                  'decisions': ['Use addition.'], 'relevant_files': ['calc.go'],
                  'remaining': ['Run independent verification.']}
        _, _, session = self.run_script([
            {'memory': memory},
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b'}},
            {'final': 'Edited without verifying.'},
        ], '--allow-write', '--no-auto-validation')
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['facts'], memory['facts'])
        self.assertEqual(state['decisions'], memory['decisions'])
        self.assertTrue(state['model_fields_stale'])
        self.assertEqual(state['validation']['status'], 'unverified')
        self.assertEqual(state['observed_changes'][0]['path'], 'calc.go')
        _, _, rejected = self.run_script([
            {'memory': {**memory, 'validation': {'status': 'passed'}}},
        ], success=False)
        self.assertEqual(json.loads((rejected / 'working_state.json').read_text())['validation']['status'],
                         'unverified')

    def test_compaction_keeps_structured_decisions(self):
        (self.root / 'large.txt').write_text(''.join(f'line {i}: ' + 'x' * 100 + '\n' for i in range(500)))
        memory = {'facts': ['Sentinel fact survives compaction.'], 'hypotheses': [],
                  'decisions': ['Keep the original task.'], 'relevant_files': ['large.txt'],
                  'remaining': ['Inspect remaining lines.']}
        actions = [{'memory': memory}]
        actions += [{'tool': 'read_file', 'args': {'path': 'large.txt', 'start': i + 1, 'end': i + 20}}
                    for i in range(0, 400, 20)]
        actions.append({'final': 'Inspection finished.'})
        for capacity in (8192, 4096):
            with self.subTest(context=capacity):
                _, _, session = self.run_script(actions, '--context', str(capacity),
                                                 '--output-reserve', '256')
                metrics = json.loads((session / 'metrics.json').read_text())
                self.assertGreater(metrics['context_evictions'], 0)
                final_prompt = sorted((session / 'context').glob('*.txt'))[-1].read_text()
                self.assertIn('Sentinel fact survives compaction.', final_prompt)
                self.assertIn('Keep the original task.', final_prompt)
                snapshots = [(path, json.loads(path.read_text()))
                             for path in sorted((session / 'context').glob('[0-9][0-9][0-9][0-9].json'))]
                path, compacted = next((path, snapshot) for path, snapshot in snapshots
                                       if snapshot['planned_evicted'])
                memory_segment = next(segment for segment in compacted['segments']
                                      if segment['kind'] == 4)
                view = json.loads(memory_segment['text'])
                self.assertEqual(view['facts'], memory['facts'])
                # The first compacted prompt already contains the last completed
                # tool outcome; it must not lag until the following turn.
                if view['recent_outcomes']:
                    self.assertEqual(view['recent_outcomes'][0]['tool_call_id'],
                                     int(path.stem) - 2)

    def test_validation_plan_and_permission_denial(self):
        self.go_module()
        plan = json.loads(self.cli('validation-plan', 'calc.go', '--json').stdout)
        self.assertEqual([stage['name'] for stage in plan['stages']],
                         ['format', 'compile', 'affected_tests', 'dependent_tests', 'vet', 'broad_tests'])
        result = self.cli('validate', 'calc.go', '--json', success=False)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        report = next(e['data'] for e in events if e['type'] == 'validation_result')
        self.assertFalse(report['passed'])
        self.assertEqual(report['status'], 'policy')
        self.assertEqual(report['commands_run'], 0)
        self.assertFalse(any(e['type'] == 'validation_command_start' for e in events))

    def test_real_staged_validation_and_format_failure(self):
        self.require_go()
        self.go_module()
        result = self.cli('validate', 'calc.go', '--allow-exec', '--json')
        events = [json.loads(line) for line in result.stdout.splitlines()]
        report = next(e['data'] for e in events if e['type'] == 'validation_result')
        self.assertTrue(report['passed'])
        self.assertGreaterEqual(report['commands_run'], 4)
        self.assertEqual(report['commands'][0]['stage'], 'format')
        self.assertEqual(report['commands'][-1]['stage'], 'broad_tests')
        self.assertTrue(all(command['exit_code'] == 0 for command in report['commands']))
        for command in report['commands']:
            self.assertTrue((pathlib.Path(report['session']) / command['stdout_artifact']).exists())
            self.assertTrue((pathlib.Path(report['session']) / command['stderr_artifact']).exists())
        with (self.root / 'calc.go').open('a') as stream:
            stream.write('\nfunc Ugly( )int{return 0}\n')
        failed = self.cli('validate', 'calc.go', '--allow-exec', '--json', success=False)
        failure_events = [json.loads(line) for line in failed.stdout.splitlines()]
        failure = next(e['data'] for e in failure_events if e['type'] == 'validation_result')
        self.assertFalse(failure['passed'])
        self.assertEqual(failure['commands_run'], 1)
        self.assertEqual(failure['commands'][0]['stage'], 'format')
        self.assertEqual(failure['commands'][0]['exit_code'], 0)

    def test_agent_repairs_after_automatic_verification_failure(self):
        self.require_go()
        self.go_module('a - b')
        original_test = (self.root / 'calc_test.go').read_bytes()
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b + 1'}},
            {'final': 'This premature success claim must not be accepted.'},
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a + b + 1', 'new_text': 'return a + b'}},
            {'final': 'The corrected result passed verification.'},
        ], '--allow-write', '--allow-exec', fallback_watch=True)
        reports = [e['data'] for e in events if e['type'] == 'validation_result']
        self.assertEqual([report['passed'] for report in reports], [False, True])
        self.assertEqual([e['data'] for e in events if e['type'] == 'message'],
                         ['The corrected result passed verification.'])
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['validation']['status'], 'passed')
        self.assertEqual(state['observed_changes'][0]['observations'], 2)
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertEqual(metrics['files_modified'], 1)
        self.assertEqual(metrics['validation_failures'], 1)
        self.assertGreater(metrics['validation_commands'], 4)
        self.assertEqual((self.root / 'calc_test.go').read_bytes(), original_test)

    def test_agent_cannot_finish_edits_when_validation_is_denied(self):
        self.go_module('a - b')
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b'}},
            {'final': 'Claimed success without process permission.'},
        ], '--allow-write', success=False, fallback_watch=True)
        self.assertFalse(any(e['type'] == 'message' for e in events))
        self.assertEqual(json.loads((session / 'working_state.json').read_text())['validation']['status'],
                         'denied')

    def test_unindexed_command_changes_require_fresh_validation(self):
        self.require_go()
        self.go_module()
        (self.root / 'fixture.txt').write_text('good', encoding='utf-8')
        (self.root / 'fixture_test.go').write_text(
            'package calc\n\nimport (\n\t"os"\n\t"testing"\n)\n\n'
            'func TestFixture(t *testing.T) {\n'
            '\tdata, err := os.ReadFile("fixture.txt")\n'
            '\tif err != nil || string(data) != "good" {\n'
            '\t\tt.Fatalf("fixture must be good: %q (%v)", data, err)\n\t}\n}\n',
            encoding='utf-8', newline='\n')
        original_source = (self.root / 'calc.go').read_bytes()
        original_test = (self.root / 'fixture_test.go').read_bytes()
        _, events, session = self.run_script([
            {'tool': 'run_command', 'args': {
                'argv': [sys.executable, '-c',
                         "from pathlib import Path; Path('fixture.txt').write_text('bad')"]}},
            {'final': 'This command-only change must not bypass validation.'},
            {'tool': 'apply_patch', 'args': {
                'path': 'fixture.txt', 'old_text': 'bad', 'new_text': 'good'}},
            {'final': 'Restored the fixture and verified it.'},
        ], '--allow-exec', '--allow-write', fallback_watch=True)
        reports = [event['data'] for event in events if event['type'] == 'validation_result']
        self.assertEqual([report['passed'] for report in reports], [False, True])
        self.assertTrue(all(report['inputs_checked'] for report in reports))
        self.assertEqual([event['data'] for event in events if event['type'] == 'message'],
                         ['Restored the fixture and verified it.'])
        self.assertEqual((self.root / 'calc.go').read_bytes(), original_source)
        self.assertEqual((self.root / 'fixture_test.go').read_bytes(), original_test)
        self.assertEqual(json.loads((session / 'working_state.json').read_text())['validation']['status'],
                         'passed')

    def test_passing_tests_that_mutate_inputs_do_not_pass_validation(self):
        self.require_go()
        self.go_module()
        (self.root / 'fixture.bin').write_bytes(b'before')
        (self.root / 'mutation_test.go').write_text(
            'package calc\n\nimport (\n\t"os"\n\t"testing"\n)\n\n'
            'func TestMutation(t *testing.T) {\n'
            '\tif err := os.WriteFile("fixture.bin", []byte("after"), 0600); err != nil {\n'
            '\t\tt.Fatal(err)\n\t}\n}\n', encoding='utf-8', newline='\n')
        failed = self.cli('validate', '--allow-exec', '--json', success=False)
        events = [json.loads(line) for line in failed.stdout.splitlines()]
        report = next(event['data'] for event in events if event['type'] == 'validation_result')
        self.assertFalse(report['passed'])
        self.assertTrue(report['inputs_changed'])
        self.assertTrue(report['inputs_checked'])
        self.assertEqual(report['status'], 'conflict')
        self.assertNotEqual(report['input_hash_before'], report['input_hash_after'])
        self.assertTrue(all(command['exit_code'] == 0 for command in report['commands']))
        self.assertEqual((self.root / 'fixture.bin').read_bytes(), b'after')


    def test_index_and_incremental_update(self):
        self.assertIn('generation=1', self.cli('index').stdout)
        self.assertIn('generation=1', self.cli('index').stdout)
        self.assertIn('return a - b', self.cli('inspect', 'Add').stdout)
        self.assertIn('caller.go:2', self.cli('references', 'Add').stdout)
        (self.root / 'calc.go').write_text('package calc\nfunc Sum(a, b int) int { return a + b }\n')
        self.assertIn('generation=2', self.cli('index').stdout)
        self.assertIn('No matching', self.cli('inspect', 'Add').stdout)
        self.assertIn('return a + b', self.cli('search', 'return a + b').stdout)
        (self.root / 'caller.go').unlink()
        self.assertNotIn('caller.go:2', self.cli('references', 'Add').stdout)

    def test_edit_command_metrics_replay(self):
        actions = [
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 1, 'end': 20}},
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b'}},
            {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', "from pathlib import Path; assert 'return a + b' in Path('calc.go').read_text(); print('PASS')"]}},
            {'final': 'Fixed Add; verification passed.'},
        ]
        _, events, session = self.run_script(actions, '--allow-write', '--allow-exec', '--no-auto-validation')
        self.assertIn('return a + b', (self.root / 'calc.go').read_text())
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertTrue(metrics['simulated'])
        self.assertEqual(metrics['tool_calls'], 3)
        self.assertGreater(metrics['cached_tokens'], 0)
        self.assertGreater(metrics['generation_arena_peak_bytes'], 0)
        self.assertLessEqual(metrics['generation_arena_peak_bytes'], 64 * 1024 * 1024)
        self.assertEqual(metrics['prefill_tokens'] + metrics['cached_tokens'], metrics['prompt_tokens'])
        replay = self.cli('replay', str(session), '--json')
        self.assertEqual([json.loads(x) for x in replay.stdout.splitlines()], events)
        self.assertTrue((session / 'tool' / '000003.stdout').exists())

    def test_policy_and_traversal(self):
        _, events, _ = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return 0'}},
            {'tool': 'read_file', 'args': {'path': '../outside', 'start': 1, 'end': 10}},
            {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', 'print(123)']}},
            {'final': 'Operations denied.'},
        ])
        results = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertTrue(all('TOOL_ERROR [policy]' in x for x in results))
        self.assertIn('return a - b', (self.root / 'calc.go').read_text())

    def test_timeout_and_output_limit(self):
        _, events, _ = self.run_script([
            {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', 'import time; print("started", flush=True); time.sleep(10)']}},
            {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', 'print("x" * 20000)']}},
            {'final': 'Done.'},
        ], '--allow-exec', '--timeout-ms', '300', '--max-tool-bytes', '1024', '--no-semantic',
           '--no-auto-validation')
        results = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('timeout=true', results[0])
        self.assertIn('truncated=true', results[1])

    def test_invalid_action_and_budget(self):
        result, _, _ = self.run_script([{'tool': 'run_command', 'args': {'argv': 'echo bad'}}], success=False)
        self.assertNotEqual(result.returncode, 0)
        result, _, _ = self.run_script([{'final': 'No room'}], '--max-input', '1', success=False)
        self.assertNotEqual(result.returncode, 0)

    def test_patch_conflict_and_create(self):
        _, events, _ = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'not present', 'new_text': 'bad'}},
            {'tool': 'apply_patch', 'args': {'path': 'new.go', 'old_text': '', 'new_text': 'package calc\n'}},
            {'final': 'Done.'},
        ], '--allow-write', '--no-auto-validation')
        results = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('conflict', results[0])
        self.assertEqual((self.root / 'new.go').read_text(), 'package calc\n')

    def test_replay_rejects_corruption(self):
        _, _, session = self.run_script([{'final': 'done'}])
        (session / 'events.jsonl').write_text('{"sequence":99}\n')
        self.assertNotEqual(self.cli('replay', str(session), success=False).returncode, 0)

    def test_identical_patch_is_a_conflict_and_can_recover(self):
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a - b'}},
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b'}},
            {'final': 'Fixed the expression.'},
        ], '--allow-write', '--no-auto-validation')
        outputs = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('TOOL_ERROR [conflict]', outputs[0])
        self.assertIn('No edit performed', outputs[0])
        self.assertIn('Patched calc.go', outputs[1])
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['files_modified'], 1)

    def test_loop_detection_uses_canonical_arguments(self):
        actions = [
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 1, 'end': 3}},
            {'tool': 'read_file', 'args': {'end': 3, 'start': 1, 'path': 'calc.go'}},
            {'tool': 'read_file', 'args': {'start': 1, 'path': 'calc.go', 'end': 3}},
            {'final': 'Loop warning observed.'},
        ]
        _, events, session = self.run_script(actions)
        outputs = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('LOOP_DETECTED', outputs[-1])
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['loop_warnings'], 1)

    def test_no_implicit_simulation(self):
        self.assertNotEqual(self.cli('run', 'task', success=False).returncode, 0)
        self.assertNotEqual(self.cli('run', 'task', '--context', '-1', success=False).returncode, 0)

    def test_memory_only_exhaustion_is_not_success(self):
        result, _, _ = self.run_script([{'memory': 'Still investigating.'}], '--max-turns', '1', success=False)
        self.assertNotEqual(result.returncode, 0)

    def test_streams_unicode_and_preserves_evidence(self):
        _, events, session = self.run_script([
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 1, 'end': 3}},
            {'final': 'Checked: café, 日本語, 🛠'},
        ])
        pieces = ''.join(e['data'] for e in events if e['type'] == 'token')
        self.assertIn('café', pieces)
        self.assertIn('日本語', pieces)
        result_text = next(e['data']['output'] for e in events if e['type'] == 'tool_result')
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertEqual(metrics['visible_tool_bytes'], len(result_text.encode()))

    def test_read_views_support_empty_files_and_unterminated_lines(self):
        (self.root / 'empty.txt').write_bytes(b'')
        (self.root / 'last.txt').write_bytes('one\ncafé'.encode('utf-8'))
        _, events, _ = self.run_script([
            {'tool': 'read_file', 'args': {'path': 'empty.txt', 'start': 1, 'end': 2}},
            {'tool': 'read_file', 'args': {'path': 'last.txt', 'start': 2, 'end': 2}},
            {'final': 'Read bounded source slices.'},
        ])
        outputs = [event['data']['output'] for event in events if event['type'] == 'tool_result']
        self.assertEqual(outputs, ['', '2: café\n'])

    def test_hardlink_patch_denied(self):
        outside = self.root / 'original.txt'
        outside.write_text('original')
        os.link(outside, self.root / 'linked.txt')
        _, events, _ = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'linked.txt', 'old_text': 'original', 'new_text': 'modified'}},
            {'final': 'Denied.'},
        ], '--allow-write')
        self.assertIn('policy', next(e['data']['output'] for e in events if e['type'] == 'tool_result'))
        self.assertEqual(outside.read_text(), 'original')

    def test_linked_metadata_denied(self):
        metadata = self.root / '.forge'
        metadata.mkdir()
        original = self.root / 'original.db'
        original.write_bytes(b'untouched')
        os.link(original, metadata / 'index.db')
        self.assertNotEqual(self.cli('index', success=False).returncode, 0)
        self.assertEqual(original.read_bytes(), b'untouched')
        (metadata / 'index.db').unlink()
        try:
            (metadata / 'index.db-wal').symlink_to(original)
        except OSError:
            self.skipTest('Symlink creation is not permitted on this host')
        self.assertNotEqual(self.cli('index', success=False).returncode, 0)
        self.assertEqual(original.read_bytes(), b'untouched')

    def test_command_environment_is_allowlisted(self):
        code = "import os,sys; assert 'FORGE_TEST_PRIVATE_VALUE' not in os.environ; assert sys.stdin.read() == ''; print('isolated')"
        with patch.dict(os.environ, {'FORGE_TEST_PRIVATE_VALUE': 'non-secret-sentinel'}):
            _, events, _ = self.run_script([
                {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', code]}},
                {'final': 'Checked.'},
            ], '--allow-exec', '--no-auto-validation')
        output = next(e['data']['output'] for e in events if e['type'] == 'tool_result')
        self.assertIn('exit_code=0', output)
        self.assertIn('isolated', output)

    def test_binary_command_output_is_preserved(self):
        code = "import sys; sys.stdout.buffer.write(b'begin\\x00after\\xff\\n'); sys.stderr.buffer.write(b'error\\x00tail')"
        _, events, session = self.run_script([
            {'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', code]}},
            {'final': 'Recorded output.'},
        ], '--allow-exec', '--no-auto-validation')
        self.assertEqual((session / 'tool' / '000001.stdout').read_bytes(), b'begin\x00after\xff\n')
        self.assertEqual((session / 'tool' / '000001.stderr').read_bytes(), b'error\x00tail')
        output = next(e['data']['output'] for e in events if e['type'] == 'tool_result')
        self.assertIn(r'begin\x00after\xff', output)
        self.assertIn(r'error\x00tail', output)

    def test_command_does_not_inherit_parent_descriptors(self):
        with (self.root / 'inherit-only.txt').open('w') as stream:
            fd = stream.fileno()
            if os.name == 'nt':
                import msvcrt
                handle = msvcrt.get_osfhandle(fd)
                os.set_handle_inheritable(handle, True)
                startup = subprocess.STARTUPINFO()
                startup.lpAttributeList = {'handle_list': [handle]}
                options = {'startupinfo': startup}
                code = ("import ctypes\nk=ctypes.WinDLL('kernel32'); b=ctypes.create_unicode_buffer(4096)\n"
                        f"n=k.GetFinalPathNameByHandleW(ctypes.c_void_p({handle}),b,4096,0)\n"
                        "assert n == 0 or not b.value.endswith('inherit-only.txt')\nprint('closed')")
            else:
                inode = os.fstat(fd).st_ino
                options = {'pass_fds': (fd,)}
                code = ("import os\ntry:\n s=os.fstat(" + str(fd) + ")\n"
                        "except OSError:\n pass\nelse:\n assert s.st_ino != " + str(inode) + "\nprint('closed')")
            actions = [{'tool': 'run_command', 'args': {'argv': [sys.executable, '-c', code]}}, {'final': 'Checked.'}]
            path = self.root / 'script.json'
            path.write_text(json.dumps(actions), encoding='utf-8')
            result = self.cli('run', 'Check inherited descriptors', '--script', str(path), '--json',
                              '--allow-exec', '--no-auto-validation', **options)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        output = next(e['data']['output'] for e in events if e['type'] == 'tool_result')
        self.assertIn('exit_code=0', output)
        self.assertIn('closed', output)

    def test_symlink_reads_denied(self):
        try:
            (self.root / 'linked.go').symlink_to(self.root / 'calc.go')
        except OSError:
            self.skipTest('Symlink creation is not permitted on this host')
        _, events, _ = self.run_script([
            {'tool': 'read_file', 'args': {'path': 'linked.go', 'start': 1, 'end': 2}},
            {'final': 'Denied.'},
        ])
        self.assertIn('policy', next(e['data']['output'] for e in events if e['type'] == 'tool_result'))

if __name__ == '__main__':
    unittest.main()
