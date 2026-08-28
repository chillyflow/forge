import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())

class ForgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge-test-')
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'calc.go').write_text('package calc\n\nfunc Add(a, b int) int { return a - b }\n')
        (self.root / 'caller.go').write_text('package calc\nfunc Use() int { return Add(1, 2) }\n')

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *args, success=True):
        result = subprocess.run([FORGE, *args, '--workspace', str(self.root)], capture_output=True, text=True, timeout=35)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def run_script(self, actions, *options, success=True):
        path = self.root / 'script.json'
        path.write_text(json.dumps(actions))
        result = self.cli('run', 'Fix Add and verify it.', '--script', str(path), '--json', *options, success=success)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        sessions = sorted((self.root / '.forge' / 'sessions').iterdir(), key=lambda p: p.stat().st_mtime_ns)
        return result, events, sessions[-1]

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
        _, events, session = self.run_script(actions, '--allow-write', '--allow-exec')
        self.assertIn('return a + b', (self.root / 'calc.go').read_text())
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertTrue(metrics['simulated'])
        self.assertEqual(metrics['tool_calls'], 3)
        self.assertGreater(metrics['cached_tokens'], 0)
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
        ], '--allow-exec', '--timeout-ms', '300', '--max-tool-bytes', '1024', '--no-semantic')
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
        ], '--allow-write')
        results = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('conflict', results[0])
        self.assertEqual((self.root / 'new.go').read_text(), 'package calc\n')

    def test_replay_rejects_corruption(self):
        _, _, session = self.run_script([{'final': 'done'}])
        (session / 'events.jsonl').write_text('{"sequence":99}\n')
        self.assertNotEqual(self.cli('replay', str(session), success=False).returncode, 0)

    def test_no_implicit_simulation(self):
        self.assertNotEqual(self.cli('run', 'task', success=False).returncode, 0)
        self.assertNotEqual(self.cli('run', 'task', '--context', '-1', success=False).returncode, 0)

if __name__ == '__main__':
    unittest.main()
