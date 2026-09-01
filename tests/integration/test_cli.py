import hashlib
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


def native_call(name, arguments, reasoning=None, call_id=None):
    call = {'type': 'function',
            'function': {'name': name, 'arguments': json.dumps(arguments,
                                                                separators=(',', ':'))}}
    if call_id:
        call['id'] = call_id
    message = {'role': 'assistant', 'content': '', 'tool_calls': [call]}
    if reasoning is not None:
        message['reasoning_content'] = reasoning
    return message

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

    def require_python(self):
        if not any(shutil.which(name) for name in ('python3', 'python', 'py')):
            self.skipTest('A discoverable Python interpreter is needed for real validation')

    def indexed_rows(self, query):
        with sqlite3.connect(self.root / '.forge' / 'index.db') as connection:
            rows = connection.execute(query).fetchall()
        connection.close()
        return rows

    def test_generated_summary_cache_and_dependency_refresh(self):
        # Explicit simulated backend; this tests CLI/cache control, not quality.
        metadata = self.root / '.forge'
        metadata.mkdir()
        script = metadata / 'summary-fixture.json'
        script.write_text(json.dumps([{'summary': 'fixture description'}]))
        args = ('summarize', 'calc.go', '--script', str(script), '--no-config',
                '--summary-producer', 'simulated-cli-fixture-v1', '--output-reserve', '256')
        original = (self.root / 'calc.go').read_bytes()
        first = json.loads(self.cli(*args).stdout)
        self.assertEqual(first['summary']['text'], '{"summary":"fixture description"}')
        self.assertTrue(first['inference']['simulated'])
        self.assertEqual(first['generation']['model_calls'], 1)
        self.assertTrue(first['generation']['published'])
        self.assertEqual(first['summary']['cache_status'], 'hit')
        hit = json.loads(self.cli(*args).stdout)
        self.assertTrue(hit['generation']['cache_hit'])
        self.assertEqual(hit['generation']['model_calls'], 0)
        self.assertEqual(hit['inference']['generated_tokens'], 0)
        self.assertTrue(hit['inference']['simulated'])
        self.assertEqual(first['summary']['cache_key'], hit['summary']['cache_key'])
        (self.root / 'caller.go').write_text('package calc\nfunc Other() {}\n')
        unrelated = json.loads(self.cli(*args).stdout)
        self.assertTrue(unrelated['generation']['cache_hit'])
        self.assertGreater(unrelated['summary']['generation'], hit['summary']['generation'])
        self.assertEqual(first['summary']['cache_key'], unrelated['summary']['cache_key'])
        self.assertEqual((self.root / 'calc.go').read_bytes(), original)
        (self.root / 'calc.go').write_text('package calc\nfunc New() {}\n')
        changed = json.loads(self.cli(*args).stdout)
        self.assertFalse(changed['generation']['cache_hit'])
        self.assertEqual(changed['generation']['model_calls'], 1)
        self.assertNotEqual(first['summary']['cache_key'], changed['summary']['cache_key'])
        with sqlite3.connect(metadata / 'index.db') as connection:
            connection.execute("UPDATE summary_cache SET content='damaged'")
        connection.close()
        repaired = json.loads(self.cli(*args).stdout)
        self.assertTrue(repaired['generation']['repaired_corruption'])
        self.assertEqual(repaired['summary']['text'], first['summary']['text'])
        self.assertFalse((metadata / 'sessions').exists())

    def test_summary_selectors_and_generation_failure(self):
        self.go_module()
        script = self.root / 'summary-fixture.json'
        script.write_text(json.dumps(['simulated summary text']))
        common = ('--script', str(script), '--no-config', '--summary-producer', 'simulated-v1',
                  '--output-reserve', '256')
        for scope, path in [('repository', '.'), ('module', '.'), ('package', '.'),
                            ('file', 'calc.go'), ('symbol', 'calc.go')]:
            extra = ('--summary-symbol', 'Add') if scope == 'symbol' else ()
            report = json.loads(self.cli('summarize', path, '--summary-scope', scope,
                                         '--summary-full-source', *extra, *common).stdout)
            self.assertEqual(report['summary']['scope'], scope)
            self.assertEqual(report['summary']['evidence'], 'full_source')
            self.assertTrue(report['generation']['published'])
        for bad in [('summarize', 'calc.go'),
                    ('summarize', 'calc.go', '--summary-producer', 'caller'),
                    ('summarize', 'calc.go', '--summary-scope', 'invalid'),
                    ('summarize', 'calc.go', '--summary-scope', 'symbol', *common),
                    ('summarize', 'calc.go', '--summary-symbol', 'Add', *common),
                    ('index', '--summary-full-source')]:
            self.cli(*bad, success=False)
        failed = json.loads(self.cli('summarize', 'calc.go', *common,
                                     '--max-tool-bytes', '1', success=False).stdout)
        self.assertIsNone(failed['summary'])
        self.assertEqual(failed['generation']['model_calls'], 1)
        self.assertFalse(failed['generation']['published'])
        self.assertTrue(failed['inference']['simulated'])

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
        if subprocess.run(['git', '--no-lazy-fetch', 'version'],
                          capture_output=True).returncode != 0:
            self.skipTest('Git 2.45+ is needed for --no-lazy-fetch eligibility rules')
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
                                                 '--output-reserve', '256', '--no-auto-validation')
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

    def test_thought_channel_retention_and_ablation(self):
        # A bounded leading thought rides along with the action. Retention in
        # the ACTION segment is OFF by default (it costs accuracy and prompt
        # tokens once reasoning is actually elicited, and loses no evidence
        # because the raw response is already in the session log);
        # --thought-history opts back in; --no-thought removes the channel from
        # the generated grammar and schema entirely.
        secret = 'Reasoning sentinel that must not reach a later prompt.'
        actions = [{'thought': secret, 'tool': 'read_file',
                    'args': {'path': 'note.txt', 'start': 1, 'end': 1}},
                   {'thought': secret, 'final': 'Inspected the note.'}]
        (self.root / 'note.txt').write_text('original data\n', newline='\n')

        result, _, session = self.run_script(actions, '--no-auto-validation')
        prompts = [path.read_text() for path in sorted((session / 'context').glob('*.txt'))]
        self.assertTrue(prompts)
        self.assertFalse(any(secret in prompt for prompt in prompts),
                         'thought must not re-enter a prompt by default')
        # Stripping the thought must not drop the action it decorated.
        self.assertTrue(any('read_file' in prompt for prompt in prompts))
        # The thought is still recorded as session evidence: dropping it from
        # later prompts must not cost observability.
        self.assertIn(secret, result.stdout)

        _, _, kept = self.run_script(actions, '--no-auto-validation', '--thought-history')
        prompts = [path.read_text() for path in sorted((kept / 'context').glob('*.txt'))]
        self.assertTrue(any(secret in prompt for prompt in prompts),
                        '--thought-history must re-enter the thought')

        # The explicit off switch stays available and agrees with the default.
        _, _, stripped = self.run_script(actions, '--no-auto-validation', '--thought-decode-only')
        prompts = [path.read_text() for path in sorted((stripped / 'context').glob('*.txt'))]
        self.assertTrue(prompts)
        self.assertFalse(any(secret in prompt for prompt in prompts))
        self.assertEqual(json.loads((stripped / 'metrics.json').read_text())['status'], 'ok')

        # The channel is host-gated, not merely grammar-hidden: with it off the
        # schema stops advertising it and a thought-bearing action is refused,
        # so the ablation cannot be defeated by an unconstrained backend.
        result, _, ablated = self.run_script(actions, '--no-auto-validation', '--no-thought',
                                             success=False)
        prompts = [path.read_text() for path in sorted((ablated / 'context').glob('*.txt'))]
        self.assertTrue(prompts)
        self.assertNotIn('"thought" field', prompts[0])
        self.assertFalse(any(secret in prompt for prompt in prompts))
        self.assertIn('invalid action', result.stderr)

        # Routed mode leaves the reasoning prefix unconstrained until the real
        # action object begins, then normalizes it into the same host-validated
        # thought field used above. Raw string script steps model that exact
        # backend output without bypassing the agent parser.
        routed_actions = [
            secret + '\n' + json.dumps({'tool': 'read_file',
                                         'args': {'path': 'note.txt', 'start': 1, 'end': 1}}),
            'The note has been inspected.\n' + json.dumps({'final': 'Inspected the note.'}),
        ]
        _, routed_events, routed = self.run_script(
            routed_actions, '--no-auto-validation', '--thought-routed', '--thought-history')
        tool_call = next(event['data'] for event in routed_events if event['type'] == 'tool_call')
        self.assertEqual(tool_call['thought'], secret)
        prompts = [path.read_text() for path in sorted((routed / 'context').glob('*.txt'))]
        self.assertTrue(any(secret in prompt for prompt in prompts))

        _, _, routed_stripped = self.run_script(
            routed_actions, '--no-auto-validation', '--thought-routed', '--thought-decode-only')
        prompts = [path.read_text() for path in sorted((routed_stripped / 'context').glob('*.txt'))]
        self.assertTrue(prompts)
        self.assertFalse(any(secret in prompt for prompt in prompts))

        # The llama backend force-decodes a "Thought: " cue as scaffold. The
        # host strips it: it never enters the recorded thought and never
        # satisfies --thought-required on its own.
        cued = ['Thought: ' + secret + '\n' + json.dumps(
                    {'tool': 'read_file', 'args': {'path': 'note.txt', 'start': 1, 'end': 1}}),
                json.dumps({'final': 'Done.'})]
        _, cued_events, _ = self.run_script(cued, '--no-auto-validation', '--thought-routed')
        cued_call = next(e['data'] for e in cued_events if e['type'] == 'tool_call')
        self.assertEqual(cued_call['thought'], secret)
        cue_only = ['Thought: \n' + json.dumps({'final': 'No real reasoning.'})]
        result, _, _ = self.run_script(cue_only, '--no-auto-validation', '--thought-routed',
                                       '--thought-required', success=False)
        self.assertIn('required before every action', result.stderr)

        # Required routed thought is enforced by the host even for the scripted
        # backend, which ignores grammar constraints.
        missing = [json.dumps({'final': 'No reasoning prefix.'})]
        result, _, _ = self.run_script(missing, '--no-auto-validation', '--thought-routed',
                                       '--thought-required', success=False)
        self.assertIn('required before every action', result.stderr)
        # An over-long routed prefix is truncated at the byte cap, never
        # run-fatal: the raw text is already session evidence, and killing the
        # run for verbose reasoning was the recorded phase-2 defect. The
        # UTF-8-boundary slice keeps exactly the first 2048 bytes here.
        oversized = ['x' * 2049 + '\n' + json.dumps(
                         {'tool': 'read_file', 'args': {'path': 'note.txt', 'start': 1, 'end': 1}}),
                     json.dumps({'final': 'Read after truncated reasoning.'})]
        _, oversized_events, _ = self.run_script(oversized, '--no-auto-validation',
                                                 '--thought-routed')
        truncated = next(e['data'] for e in oversized_events if e['type'] == 'tool_call')
        self.assertEqual(truncated['thought'], 'x' * 2048)

        # Contradictory ablation settings must fail loudly rather than quietly
        # report one arm's numbers under the other arm's name.
        contradiction = self.cli('run', 'Inspect the note.', '--script', str(self.root / 'script.json'),
                                 '--no-thought', '--thought-required', success=False)
        self.assertIn('contradicts', contradiction.stderr)

        # Inline --thought-required is enforced by the host, not only by the
        # grammar: the scripted backend ignores GBNF, so an action with no
        # thought must still be refused. Without this the flag would be a
        # no-op for any unconstrained backend and the ablation arm it names
        # would silently measure the untreated configuration.
        thoughtless = [json.dumps({'final': 'No reasoning at all.'})]
        refused = self.run_script(thoughtless, '--no-auto-validation', '--thought-required',
                                  success=False)[0]
        self.assertIn('invalid action', refused.stderr)
        # ... and an action that does carry one is still accepted.
        withthought = [{'thought': 'Reasoned first.', 'final': 'Done.'}]
        self.run_script(withthought, '--no-auto-validation', '--thought-required')
        contradiction = self.cli('run', 'Inspect the note.', '--script',
                                 str(self.root / 'script.json'), '--no-thought',
                                 '--thought-routed', success=False)
        self.assertIn('contradict', contradiction.stderr)

        # §32 per-state routing controls. The think budget and cue only mean
        # anything in routed mode; arming them elsewhere is a loud error, not a
        # silent no-op arm.
        rejected = self.cli('run', 'Inspect the note.', '--script',
                            str(self.root / 'script.json'), '--thought-routed',
                            '--thought-budget', '0', success=False)
        self.assertIn('[1, 2147483647]', rejected.stderr)
        rejected = self.cli('run', 'Inspect the note.', '--script',
                            str(self.root / 'script.json'), '--thought-budget', '64',
                            success=False)
        self.assertIn('require --thought-routed', rejected.stderr)
        rejected = self.cli('run', 'Inspect the note.', '--script',
                            str(self.root / 'script.json'), '--thought-routed',
                            '--thought-cue', 'Plan: {', success=False)
        self.assertIn('action-opening brace', rejected.stderr)
        # The unbounded ablation and an explicit budget parse and run; the
        # scripted backend ignores decode routing, so these only pin the CLI
        # and config surface.
        self.run_script(routed_actions, '--no-auto-validation', '--thought-routed',
                        '--no-thought-budget')
        self.run_script(routed_actions, '--no-auto-validation', '--thought-routed',
                        '--thought-budget', '64')
        # A configured cue replaces the default scaffold in host normalization:
        # it is stripped from the recorded thought exactly as "Thought: " is.
        custom_cued = ['Plan: ' + secret + '\n' + json.dumps(
                           {'tool': 'read_file', 'args': {'path': 'note.txt', 'start': 1, 'end': 1}}),
                       json.dumps({'final': 'Done.'})]
        _, custom_events, _ = self.run_script(custom_cued, '--no-auto-validation',
                                              '--thought-routed', '--thought-cue', 'Plan: ')
        custom_call = next(e['data'] for e in custom_events if e['type'] == 'tool_call')
        self.assertEqual(custom_call['thought'], secret)

        # Native template thinking and its thinking-disabled control use the
        # same lazy action grammar and no Forge cue, bans, or forced budget.
        native = ['<think>inspect first</think>\n' + json.dumps(
                      {'tool': 'read_file',
                       'args': {'path': 'note.txt', 'start': 1, 'end': 1}}),
                  json.dumps({'final': 'Done.'})]
        _, native_events, _ = self.run_script(native, '--no-auto-validation',
                                              '--thought-native')
        native_call = next(e['data'] for e in native_events if e['type'] == 'tool_call')
        self.assertEqual(native_call['thought'], '<think>inspect first</think>')
        self.run_script([json.dumps({'final': 'Done.'})], '--no-auto-validation',
                        '--thought-native', '--disable-thinking')

    def test_native_prompt_protocol_roles_tools_and_explicit_failures(self):
        (self.root / 'note.txt').write_text('native evidence\n', encoding='utf-8', newline='\n')
        read = native_call('read_file', {'path': 'note.txt', 'start': 1, 'end': 1},
                           reasoning='inspect natively', call_id='model-call')
        finish = native_call('final', {'answer': 'Inspected the note.'})
        _, events, session = self.run_script(
            [read, finish, finish, finish], '--prompt-protocol', 'native',
            '--no-auto-validation')
        calls = [event['data'] for event in events if event['type'] == 'tool_call']
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]['tool'], 'read_file')
        self.assertEqual(calls[0]['thought'], 'inspect natively')
        self.assertEqual([event['data'] for event in events if event['type'] == 'message'],
                         ['Inspected the note.'])
        prompts = [json.loads(path.read_text(encoding='utf-8'))
                   for path in sorted((session / 'context').glob('*.txt'))]
        self.assertTrue(prompts)
        self.assertTrue(all(prompt['protocol'] == 'forge-native-v1' for prompt in prompts))
        tool_names = {tool['function']['name'] for tool in prompts[0]['tools']}
        self.assertTrue({'read_file', 'final', 'memory'} <= tool_names)
        paired = next(prompt for prompt in prompts
                      if any(message['role'] == 'tool' for message in prompt['messages']))
        messages = paired['messages']
        tool_index = next(i for i, message in enumerate(messages) if message['role'] == 'tool')
        assistant = messages[tool_index - 1]
        tool_message = messages[tool_index]
        self.assertEqual(assistant['role'], 'assistant')
        self.assertEqual(len(assistant['tool_calls']), 1)
        history_call = assistant['tool_calls'][0]
        self.assertEqual(history_call['function']['name'], 'read_file')
        self.assertEqual(history_call['function']['arguments'],
                         {'path': 'note.txt', 'start': 1, 'end': 1})
        self.assertEqual(history_call['id'], tool_message['tool_call_id'])
        self.assertEqual(len(history_call['id']), 9)
        self.assertEqual(tool_message['name'], 'read_file')

        memory = {'facts': ['Native memory fact.'], 'hypotheses': [],
                  'decisions': ['Keep native roles paired.'], 'relevant_files': ['note.txt'],
                  'remaining': ['Finish.']}
        _, _, memory_session = self.run_script(
            [native_call('memory', memory, reasoning='retain state'), finish],
            '--prompt-protocol', 'native', '--no-auto-validation')
        state = json.loads((memory_session / 'working_state.json').read_text(encoding='utf-8'))
        self.assertEqual(state['facts'], memory['facts'])
        memory_prompts = [json.loads(path.read_text(encoding='utf-8'))
                          for path in sorted((memory_session / 'context').glob('*.txt'))]
        paired_memory = next(prompt for prompt in memory_prompts
                             if any(message.get('name') == 'memory'
                                    for message in prompt['messages']))
        memory_messages = paired_memory['messages']
        memory_tool_index = next(i for i, message in enumerate(memory_messages)
                                 if message.get('name') == 'memory')
        self.assertEqual(memory_messages[memory_tool_index - 1]['role'], 'assistant')
        self.assertEqual(
            memory_messages[memory_tool_index - 1]['tool_calls'][0]['function']['name'],
            'memory')
        self.assertEqual(memory_messages[memory_tool_index]['role'], 'tool')

        original = (self.root / 'calc.go').read_bytes()
        malformed = [
            (native_call('not_a_forge_tool', {}), 'unsupported tool'),
            ({'role': 'assistant', 'content': '', 'tool_calls': [
                native_call('git_status', {})['tool_calls'][0],
                native_call('git_diff', {})['tool_calls'][0],
            ]}, 'exactly one tool call'),
            (native_call('read_file', {'path': 'calc.go', 'start': 1}), 'missing end'),
        ]
        for response, detail in malformed:
            with self.subTest(detail=detail):
                result, failed_events, _ = self.run_script(
                    [response], '--prompt-protocol', 'native', success=False)
                self.assertIn(detail, result.stderr.lower())
                self.assertTrue(any(event['type'] == 'model_output'
                                    for event in failed_events))
                self.assertFalse(any(event['type'] == 'tool_call'
                                     for event in failed_events))
                self.assertEqual((self.root / 'calc.go').read_bytes(), original)

        script = self.root / 'script.json'
        invalid = self.cli('run', 'Inspect.', '--script', str(script),
                           '--prompt-protocol', 'neither', success=False)
        self.assertIn('must be flattened or native', invalid.stderr)
        routed = self.cli('run', 'Inspect.', '--script', str(script),
                          '--prompt-protocol', 'native', '--thought-routed', success=False)
        self.assertIn('cannot be combined with routed', routed.stderr)

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

    def test_python_validation_rejects_compiler_only_syntax_errors(self):
        self.require_python()
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'tests').mkdir()
        (self.root / 'worker.py').write_text('return 1\n', encoding='utf-8', newline='\n')
        (self.root / 'tests/test_worker.py').write_text(
            'import unittest\n\nclass WorkerTest(unittest.TestCase):\n'
            '    def test_placeholder(self):\n        self.assertTrue(True)\n',
            encoding='utf-8', newline='\n')
        failed = self.cli('validate', 'worker.py', '--allow-exec', '--json', success=False)
        events = [json.loads(line) for line in failed.stdout.splitlines()]
        report = next(event['data'] for event in events
                      if event['type'] == 'validation_result')
        self.assertFalse(report['passed'])
        self.assertEqual(report['commands_run'], 1)
        self.assertEqual(report['commands'][0]['stage'], 'compile')
        artifact = pathlib.Path(report['session']) / report['commands'][0]['stderr_artifact']
        self.assertIn('outside function', artifact.read_text(encoding='utf-8'))
        self.assertFalse((self.root / '__pycache__').exists())

    def test_staged_retrieval_cli_and_read_only_tool(self):
        (self.root / 'notes.md').write_text('ADD is documented in uppercase.\n', encoding='utf-8')
        report = json.loads(self.cli('retrieve', 'Add', '--depth', '0', '--json').stdout)
        self.assertTrue(report['indexed_snapshot_only'])
        self.assertEqual([stage['stage'] for stage in report['stages']],
                         ['exact_symbol', 'package_graph', 'literal', 'fts5'])
        self.assertEqual(report['results'][0]['stage'], 'exact_symbol')
        self.assertEqual(report['results'][0]['path'], 'calc.go')
        self.assertIn('return a - b', report['results'][0]['snippet'])
        self.assertEqual(next(row for row in report['results'] if row['path'] == 'notes.md')['stage'],
                         'fts5')
        failed = self.cli('retrieve', 'Add', '--max-tool-bytes', '1', success=False)
        self.assertIn('budget', failed.stderr)
        self.cli('retrieve', success=False)
        original = (self.root / 'calc.go').read_bytes()
        _, events, session = self.run_script([
            {'tool': 'retrieve_context', 'args': {'query': 'Add'}},
            {'final': 'Inspected indexed evidence.'},
        ], '--no-auto-validation', '--context', '4096', '--output-reserve', '256',
           '--max-tool-bytes', '8192', fallback_watch=True)
        outputs = [event['data'] for event in events if event['type'] == 'tool_result']
        self.assertEqual([output['name'] for output in outputs], ['retrieve_context'])
        tool_report = json.loads(outputs[0]['output'])
        self.assertEqual(tool_report['results'][0]['stage'], 'exact_symbol')
        self.assertEqual((self.root / 'calc.go').read_bytes(), original)
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertTrue(metrics['simulated'])
        self.assertEqual(metrics['files_modified'], 0)
        self.assertEqual(metrics['validation_commands'], 0)

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

    def test_agent_cannot_finish_when_validation_is_not_applicable(self):
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'note.txt').write_text('before\n', encoding='utf-8', newline='\n')
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'note.txt', 'old_text': 'before', 'new_text': 'after'}},
            {'final': 'Claimed success without an applicable validation plan.'},
        ], '--allow-write', success=False, fallback_watch=True)
        reports = [event['data'] for event in events
                   if event['type'] == 'validation_result']
        self.assertEqual(len(reports), 1)
        self.assertFalse(reports[0]['applicable'])
        self.assertFalse(reports[0]['passed'])
        self.assertFalse(any(event['type'] == 'message' for event in events))
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['validation']['status'], 'not_applicable')

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
        ], '--no-auto-validation')
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

    def test_hash_anchored_hunk_is_narrow_and_rejects_stale_revision(self):
        self.go_module('a - b')
        before = (self.root / 'calc.go').read_bytes()
        anchor = hashlib.sha256(before).hexdigest()
        _, events, session = self.run_script([
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 4, 'end': 4}},
            {'tool': 'apply_hunk', 'args': {
                'path': 'calc.go', 'start': 4, 'end': 4, 'file_sha256': anchor,
                'new_text': '\treturn a + b\n'}},
            {'tool': 'apply_hunk', 'args': {
                'path': 'calc.go', 'start': 4, 'end': 4, 'file_sha256': anchor,
                'new_text': '\treturn 0\n'}},
            {'final': 'Applied one anchored line edit.'},
        ], '--allow-write', '--no-auto-validation')
        outputs = [event['data']['output'] for event in events if event['type'] == 'tool_result']
        self.assertEqual(outputs[0], f'file_sha256:{anchor}\n4: \treturn a - b\n')
        self.assertIn('Patched calc.go', outputs[1])
        self.assertIn('Staged Go syntax validation passed before commit', outputs[1])
        self.assertIn('TOOL_ERROR [conflict]', outputs[2])
        self.assertIn('stale file_sha256', outputs[2])
        self.assertIn('\treturn a + b\n', (self.root / 'calc.go').read_text())
        outcome = json.loads((session / 'tool/000002.edit-result.json').read_text())
        self.assertEqual(outcome['state'], 'applied')
        self.assertFalse((session / 'tool/000003.edit.json').exists())
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['files_modified'], 1)

    def test_hash_anchored_hunk_can_fill_an_existing_empty_file(self):
        target = self.root / 'empty.txt'
        target.write_bytes(b'')
        anchor = hashlib.sha256(b'').hexdigest()
        _, events, _ = self.run_script([
            {'tool': 'apply_hunk', 'args': {
                'path': 'empty.txt', 'start': 1, 'end': 1, 'file_sha256': anchor,
                'new_text': 'first line\n'}},
            {'final': 'Filled the empty file.'},
        ], '--allow-write', '--no-auto-validation')
        output = next(event['data']['output'] for event in events
                      if event['type'] == 'tool_result')
        self.assertIn('Patched empty.txt', output)
        self.assertEqual(target.read_text(), 'first line\n')

    def test_syntax_broken_go_candidate_is_aborted_before_commit(self):
        before = (self.root / 'calc.go').read_bytes()
        source = before.decode()
        candidates = [
            source.replace('return a - b }', 'return a + b'),
            'func Add(a, b int) int { return a + b }\n',
            'package calc\nreturn 1\n',
            'package calc\n' + r'var _ = "\q"' + '\n',
            'package calc\n' + r"var _ = '\x12ab'" + '\n',
            '',
        ]
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'calc.go', 'old_text': source, 'new_text': candidate}}
            for candidate in candidates
        ] + [
            {'final': 'The invalid candidate was rejected.'},
        ], '--allow-write', '--no-auto-validation')
        outputs = [event['data']['output'] for event in events
                   if event['type'] == 'tool_result']
        self.assertEqual(len(outputs), len(candidates))
        for output in outputs:
            self.assertIn('TOOL_ERROR [parse]', output)
            self.assertIn('Proposed Go edit rejected before commit', output)
        self.assertEqual((self.root / 'calc.go').read_bytes(), before)
        self.assertEqual((session / 'tool/000001.before').read_bytes(), before)
        for index, candidate in enumerate(candidates, 1):
            self.assertEqual((session / f'tool/{index:06}.after').read_bytes(),
                             candidate.encode())
            outcome = json.loads((session / f'tool/{index:06}.edit-result.json').read_text())
            self.assertEqual(outcome['state'], 'aborted')
            self.assertEqual(outcome['status'], 'parse')
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['files_modified'], 0)

    def test_valid_go_package_comment_and_coalesced_hex_escape_commit(self):
        before = (self.root / 'calc.go').read_bytes()
        candidate = (b'package /* inline */ calc\n\nvar _ = "\\x12ab"\n\n'
                     b'func Add(a, b int) int { return a - b }\n')
        _, events, _ = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'calc.go', 'old_text': before.decode(),
                'new_text': candidate.decode()}},
            {'final': 'The valid candidate committed.'},
        ], '--allow-write', '--no-auto-validation')
        output = next(event['data']['output'] for event in events
                      if event['type'] == 'tool_result')
        self.assertIn('Patched calc.go', output)
        self.assertIn('Staged Go syntax validation passed before commit', output)
        self.assertEqual((self.root / 'calc.go').read_bytes(), candidate)

    def test_python_first_stall_runs_validation_and_pins_failure(self):
        if not any(shutil.which(name) for name in ('python', 'python3', 'py')):
            self.skipTest('Python executable discovery is needed for automatic validation')
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'expr').mkdir()
        (self.root / 'tests').mkdir()
        (self.root / 'expr/__init__.py').write_text('')
        (self.root / 'expr/evaluate.py').write_text(
            'import ast\n\ndef evaluate(expression)\n'
            '    node = ast.parse(expression, mode="eval").body\n'
            '    if not isinstance(node, ast.BinOp) or not isinstance(node.op, ast.Add):\n'
            '        raise ValueError("unsupported")\n'
            '    if not isinstance(node.left, ast.Constant) or not isinstance(node.right, ast.Constant):\n'
            '        raise ValueError("unsupported")\n'
            '    return node.left.value + node.right.value\n', newline='\n')
        (self.root / 'tests/test_evaluate.py').write_text(
            'import unittest\nfrom expr.evaluate import evaluate\n\n'
            'class EvaluateTests(unittest.TestCase):\n'
            '    def test_addition_only(self):\n'
            '        self.assertEqual(evaluate("2 + 5"), 7)\n'
            '        with self.assertRaises(ValueError): evaluate("2 * 5")\n'
            '        with self.assertRaises(ValueError): evaluate("\'a\' + \'b\'")\n', newline='\n')
        old_guard = ('    if not isinstance(node.left, ast.Constant) or '
                     'not isinstance(node.right, ast.Constant):\n'
                     '        raise ValueError("unsupported")')
        new_guard = ('    if not all(isinstance(value, ast.Constant) and '
                     'isinstance(value.value, int)\n'
                     '               for value in (node.left, node.right)):\n'
                     '        raise ValueError("unsupported")')
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'expr/evaluate.py', 'old_text': 'def evaluate(expression)\n',
                'new_text': 'def evaluate(expression):\n'}},
            {'tool': 'read_file', 'args': {
                'path': 'expr/__init__.py', 'start': 1, 'end': 1}},
            {'tool': 'read_file', 'args': {
                'path': 'expr/__init__.py', 'start': 1, 'end': 1}},
            {'tool': 'apply_patch', 'args': {
                'path': 'expr/evaluate.py', 'old_text': old_guard, 'new_text': new_guard}},
            {'final': 'Python repair validated.'},
        ], '--allow-write', '--allow-exec', fallback_watch=True)
        recoveries = [event['data'] for event in events if event['type'] == 'recovery']
        self.assertEqual(len(recoveries), 1)
        self.assertIn('ValueError not raised', recoveries[0])
        validation_results = [event for event in events if event['type'] == 'validation_result']
        self.assertGreaterEqual(len(validation_results), 2)
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['validation']['status'], 'passed')
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertEqual(metrics['loop_warnings'], 1)
        self.assertGreaterEqual(metrics['validation_commands'], 4)
        self.assertFalse((self.root / '__pycache__').exists())
        self.assertFalse((self.root / '.pytest_cache').exists())
        repaired = (self.root / 'expr/evaluate.py').read_text()
        self.assertIn('isinstance(value.value, int)', repaired)

    def test_blocked_python_stall_enters_recovery_and_can_add_tests(self):
        self.require_python()
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'worker.py').write_text(
            'def value():\n    return 1\n', encoding='utf-8', newline='\n')
        test_source = (
            'import unittest\nfrom worker import value\n\n'
            'class WorkerTest(unittest.TestCase):\n'
            '    def test_value(self):\n        self.assertEqual(value(), 2)\n')
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'worker.py', 'old_text': 'return 1', 'new_text': 'return 2'}},
            {'tool': 'read_file', 'args': {'path': 'worker.py', 'start': 1, 'end': 5}},
            {'tool': 'read_file', 'args': {'path': 'worker.py', 'start': 1, 'end': 5}},
            {'tool': 'apply_patch', 'args': {
                'path': 'test_worker.py', 'old_text': '', 'new_text': test_source}},
            {'final': 'Added the missing Python test and validated the repair.'},
        ], '--allow-write', '--allow-exec', fallback_watch=True)
        recoveries = [event['data'] for event in events if event['type'] == 'recovery']
        self.assertEqual(len(recoveries), 1)
        self.assertIn('python_tests_not_detected', recoveries[0])
        reports = [event['data'] for event in events
                   if event['type'] == 'validation_result']
        self.assertEqual([report['passed'] for report in reports], [False, True])
        self.assertEqual([event['data'] for event in events if event['type'] == 'message'],
                         ['Added the missing Python test and validated the repair.'])
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['validation']['status'], 'passed')
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['loop_warnings'], 1)

    def test_stall_recovery_preserves_rejected_edit_diagnostic(self):
        self.require_python()
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'worker.py').write_text(
            'def value():\n    return 1\n', encoding='utf-8', newline='\n')
        (self.root / 'test_worker.py').write_text(
            'import unittest\nfrom worker import value\n\n'
            'class WorkerTest(unittest.TestCase):\n'
            '    def test_value(self):\n        self.assertEqual(value(), 2)\n',
            encoding='utf-8', newline='\n')
        invalid = {
            'path': 'broken.go',
            'old_text': '',
            'new_text': 'package broken\nfunc Broken() {\n',
        }
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'worker.py', 'old_text': 'return 1', 'new_text': 'return 2'}},
            {'tool': 'apply_patch', 'args': invalid},
            {'tool': 'apply_patch', 'args': invalid},
            {'final': 'Kept the valid committed Python repair.'},
        ], '--allow-write', '--allow-exec', fallback_watch=True)
        recoveries = [event['data'] for event in events if event['type'] == 'recovery']
        self.assertEqual(len(recoveries), 1)
        self.assertIn('Proposed Go edit rejected before commit', recoveries[0])
        self.assertIn('Automatic staged validation passed', recoveries[0])
        self.assertEqual([event['data'] for event in events if event['type'] == 'message'],
                         ['Kept the valid committed Python repair.'])
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['loop_warnings'], 1)

    def test_edit_artifacts_are_exact_and_git_applicable(self):
        if not shutil.which('git'):
            self.skipTest('Git is needed to independently check emitted patches')
        cases = [
            ('crlf space.txt', b'old\r\nnext\r\n', b'new\r\nnext\r\n'),
            ('no-newline.txt', b'old', b'new'),
            ('empty.txt', b'old\n', b''),
            ('new-empty.txt', None, b''),
            ('unicode.txt', 'café\n'.encode(), '雪 🛠\n'.encode()),
        ]
        for name, before, after in cases:
            with self.subTest(name=name):
                target = self.root / name
                if before is not None:
                    target.write_bytes(before)
                _, events, session = self.run_script([
                    {'tool': 'apply_patch', 'args': {
                        'path': name, 'old_text': (before or b'').decode(),
                        'new_text': after.decode()}},
                    {'final': 'Recorded the edit.'},
                ], '--allow-write', '--no-auto-validation')
                self.assertEqual(target.read_bytes(), after)
                self.assertEqual((session / 'tool/000001.before').read_bytes(), before or b'')
                self.assertEqual((session / 'tool/000001.after').read_bytes(), after)
                intent = json.loads((session / 'tool/000001.edit.json').read_text())
                outcome = json.loads((session / 'tool/000001.edit-result.json').read_text())
                self.assertEqual(intent['state'], 'prepared')
                self.assertEqual(intent['before_exists'], before is not None)
                self.assertEqual(intent['path'], name)
                self.assertEqual(outcome['state'], 'applied')
                self.assertEqual(outcome['status'], 'ok')
                self.assertEqual([event['type'] for event in events
                                  if event['type'] in ('edit_prepared', 'edit_result')],
                                 ['edit_prepared', 'edit_result'])
                with tempfile.TemporaryDirectory(prefix='forge-apply-proof-') as proof:
                    checkout = pathlib.Path(proof)
                    if before is not None:
                        (checkout / name).write_bytes(before)
                    patch_path = session / intent['diff_artifact']
                    # The test explicitly authorizes Git only in this private
                    # fixture. Agent finalization must never run this command.
                    for check in (True, False):
                        command = ['git', '-c', 'core.autocrlf=false', '-c', 'core.fsmonitor=false',
                                   'apply', '--whitespace=nowarn']
                        if check:
                            command.append('--check')
                        subprocess.run([*command, str(patch_path)], cwd=checkout, check=True,
                                       capture_output=True, timeout=20)
                    self.assertEqual((checkout / name).read_bytes(), after)

    def test_replay_rejects_corruption(self):
        _, _, session = self.run_script([{'final': 'done'}], '--no-auto-validation')
        (session / 'events.jsonl').write_text('{"sequence":99}\n')
        self.assertNotEqual(self.cli('replay', str(session), success=False).returncode, 0)

    def test_identical_patch_is_a_conflict_and_can_recover(self):
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'not present', 'new_text': 'not present'}},
            {'tool': 'apply_patch', 'args': {'path': 'calc.go', 'old_text': 'return a - b', 'new_text': 'return a + b'}},
            {'final': 'Fixed the expression.'},
        ], '--allow-write', '--no-auto-validation')
        outputs = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('TOOL_ERROR [conflict]', outputs[0])
        self.assertIn('No edit performed', outputs[0])
        self.assertIn('Patched calc.go', outputs[1])
        self.assertFalse((session / 'tool/000001.edit.json').exists())
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['files_modified'], 1)

    def test_loop_detection_uses_canonical_arguments(self):
        actions = [
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 1, 'end': 3}},
            {'tool': 'read_file', 'args': {'end': 3, 'start': 1, 'path': 'calc.go'}},
            {'tool': 'read_file', 'args': {'start': 1, 'path': 'calc.go', 'end': 3}},
            {'final': 'Loop warning observed.'},
        ]
        # This fixture tests canonical loop detection without process permission.
        # Delayed native notifications may legitimately require validation even
        # for read-only actions, so explicitly finish as unverified here.
        _, events, session = self.run_script(actions, '--no-auto-validation')
        outputs = [e['data']['output'] for e in events if e['type'] == 'tool_result']
        self.assertIn('LOOP_DETECTED', outputs[-1])
        self.assertEqual(json.loads((session / 'metrics.json').read_text())['loop_warnings'], 1)
        self.assertFalse(any(event['type'] == 'validation_result' for event in events))
        self.assertEqual(json.loads((session / 'working_state.json').read_text())['validation']['status'],
                         'unverified')

    def test_repeated_process_action_enters_recovery_across_generation_bumps(self):
        command = {'tool': 'run_command', 'args': {
            'argv': [sys.executable, '-c', "print('same command')"]}}
        _, events, session = self.run_script([
            command,
            command,
            {'tool': 'read_file', 'args': {'path': 'calc.go', 'start': 1, 'end': 3}},
            {'final': 'Recovered from the repeated process action.'},
        ], '--allow-exec', '--no-auto-validation')
        outputs = [event['data']['output'] for event in events
                   if event['type'] == 'tool_result']
        self.assertIn('exit_code=0', outputs[0])
        self.assertIn('RECOVERY_MODE', outputs[1])
        self.assertIn('file_sha256:', outputs[2])
        self.assertEqual(len([event for event in events if event['type'] == 'recovery']), 1)
        metrics = json.loads((session / 'metrics.json').read_text())
        self.assertEqual(metrics['loop_warnings'], 1)
        self.assertEqual(metrics['tool_calls'], 3)

    def test_denied_stall_validation_is_recovery_evidence_not_terminal(self):
        self.require_python()
        (self.root / 'calc.go').unlink()
        (self.root / 'caller.go').unlink()
        (self.root / 'worker.py').write_text(
            'def value():\n    return 1\n', encoding='utf-8', newline='\n')
        (self.root / 'test_worker.py').write_text(
            'import unittest\nfrom worker import value\n\n'
            'class WorkerTest(unittest.TestCase):\n'
            '    def test_value(self):\n        self.assertEqual(value(), 2)\n',
            encoding='utf-8', newline='\n')
        _, events, session = self.run_script([
            {'tool': 'apply_patch', 'args': {
                'path': 'worker.py', 'old_text': 'return 1', 'new_text': 'return 2'}},
            {'tool': 'read_file', 'args': {'path': 'worker.py', 'start': 1, 'end': 5}},
            {'tool': 'read_file', 'args': {'path': 'worker.py', 'start': 1, 'end': 5}},
            {'tool': 'apply_patch', 'args': {
                'path': 'worker.py', 'old_text': 'return 2', 'new_text': 'return 3'}},
            {'final': 'This final remains denied.'},
        ], '--allow-write', success=False, fallback_watch=True)
        recoveries = [event['data'] for event in events if event['type'] == 'recovery']
        self.assertEqual(len(recoveries), 1)
        self.assertIn('requires explicit unsandboxed process approval', recoveries[0])
        self.assertIn('return 3', (self.root / 'worker.py').read_text())
        self.assertFalse(any(event['type'] == 'message' for event in events))
        state = json.loads((session / 'working_state.json').read_text())
        self.assertEqual(state['validation']['status'], 'denied')

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
        ], '--no-auto-validation')
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
        empty_hash = hashlib.sha256(b'').hexdigest()
        last_hash = hashlib.sha256('one\ncafé'.encode('utf-8')).hexdigest()
        self.assertEqual(outputs,
                         [f'file_sha256:{empty_hash}\n',
                          f'file_sha256:{last_hash}\n2: café\n'])

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
        ], '--no-auto-validation')
        self.assertIn('policy', next(e['data']['output'] for e in events if e['type'] == 'tool_result'))

if __name__ == '__main__':
    unittest.main()
