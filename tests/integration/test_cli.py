import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())

class ForgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='forge-test-')
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'calc.go').write_text('package calc\n\nfunc Add(a, b int) int { return a - b }\n')
        (self.root / 'caller.go').write_text('package calc\nfunc Use() int { return Add(1, 2) }\n')

    def tearDown(self):
        self.temp.cleanup()

    def cli(self, *args, success=True, **run_options):
        result = subprocess.run([FORGE, *args, '--workspace', str(self.root)], capture_output=True,
                                encoding='utf-8', timeout=35, **run_options)
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
            ], '--allow-exec')
        output = next(e['data']['output'] for e in events if e['type'] == 'tool_result')
        self.assertIn('exit_code=0', output)
        self.assertIn('isolated', output)

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
                              '--allow-exec', **options)
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
