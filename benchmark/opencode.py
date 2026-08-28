"""Optional comparison adapter for a locally installed OpenCode and llama-server.

Uses the exact same task fixtures and GGUF as Forge. Nothing is downloaded.
Server startup is measured separately; do not compare this wall time directly
with Forge's cold-process wall time without accounting for model load.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request

from run import digest

def request(port, path, method='GET'):
    query = urllib.request.Request(f'http://127.0.0.1:{port}{path}', method=method,
                                   data=b'{}' if method == 'POST' else None,
                                   headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(query, timeout=5) as response:
        return response.read().decode()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opencode', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    opencode, server, model = args.opencode.resolve(), args.server.resolve(), args.model.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    env = {k: v for k, v in os.environ.items() if k.upper() in {
        'PATH', 'SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP', 'COMSPEC', 'PATHEXT',
        'USERPROFILE', 'HOME', 'LOCALAPPDATA', 'APPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)'}}
    # Isolate application data without changing HOME or any user configuration.
    for variable, folder in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')]:
        path = args.output / 'opencode-state' / folder
        path.mkdir(parents=True, exist_ok=True)
        env[variable] = str(path)
    for key in ['OPENCODE_DISABLE_AUTOUPDATE', 'OPENCODE_DISABLE_DEFAULT_PLUGINS', 'OPENCODE_DISABLE_LSP_DOWNLOAD',
                'OPENCODE_DISABLE_CLAUDE_CODE', 'OPENCODE_DISABLE_MODELS_FETCH']:
        env[key] = 'true'
    env['OPENCODE_AUTO_SHARE'] = 'false'
    config = {
        '$schema': 'https://opencode.ai/config.json', 'share': 'disabled', 'autoupdate': False,
        'enabled_providers': ['llama.cpp'], 'model': 'llama.cpp/forge-local', 'small_model': 'llama.cpp/forge-local',
        'provider': {'llama.cpp': {'npm': '@ai-sdk/openai-compatible', 'name': 'Local benchmark',
            'options': {'baseURL': f'http://127.0.0.1:{port}/v1', 'apiKey': 'local'},
            'models': {'forge-local': {'name': 'Qwen3 Coder', 'limit': {'context': 16384, 'output': 2048}}}}},
        'permission': {'read': 'allow', 'edit': 'allow', 'glob': 'allow', 'grep': 'allow', 'bash': 'allow',
                       'task': 'deny', 'webfetch': 'deny', 'websearch': 'deny', 'external_directory': 'deny',
                       'skill': 'deny', 'lsp': 'deny', 'question': 'deny'},
        'agent': {'build': {'temperature': 0, 'steps': 16}}, 'lsp': False, 'formatter': False,
    }
    config_path = args.output / 'opencode.json'
    config_path.write_text(json.dumps(config, indent=2))
    env['OPENCODE_CONFIG'] = str(config_path)
    command = [str(server), '--model', str(model), '--alias', 'forge-local', '--ctx-size', '16384',
               '--gpu-layers', '-1', '--parallel', '1', '--seed', '42', '--temp', '0', '--metrics', '--cache-ram', '0',
               '--host', '127.0.0.1', '--port', str(port), '--no-webui']
    records = []
    metadata = {'opencode_version': subprocess.check_output([str(opencode), '--version'], text=True).strip(),
                'model_file': model.name, 'model_sha256': digest(model), 'server_command': [model.name if x == str(model) else Path(x).name if x == str(server) else x for x in command],
                'notes': 'Model stays loaded but slot KV is erased before each task; cross-task RAM prompt cache is disabled. Model load is excluded from per-task wall time. Full OpenCode tool protocol retained; remote tools and subagents disabled.'}
    with (args.output / 'server.log').open('w') as log:
        start = time.monotonic()
        process = subprocess.Popen(command, stdout=log, stderr=log, env=env,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            while True:
                if process.poll() is not None:
                    raise RuntimeError('llama-server exited; inspect server.log')
                if time.monotonic() - start > 180:
                    raise TimeoutError('llama-server did not become healthy')
                try:
                    if json.loads(request(port, '/health')).get('status') == 'ok':
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.5)
            metadata['server_startup_seconds'] = time.monotonic() - start
            (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2))
            for task_path in sorted((Path(__file__).parent / 'tasks').glob('*.json')):
                task = json.loads(task_path.read_text())
                if args.tasks and task['id'] not in args.tasks:
                    continue
                output = args.output / task['id']
                output.mkdir(exist_ok=True)
                with tempfile.TemporaryDirectory(prefix='forge-opencode-') as temporary:
                    root = Path(temporary).resolve()
                    for relative, content in task['files'].items():
                        path = (root / relative).resolve()
                        if not path.is_relative_to(root):
                            raise ValueError('Unsafe fixture path')
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(content)
                    subprocess.run(['git', 'init', '-q', str(root)], check=True)
                    before = digest(root / 'repair_test.go')
                    erased = json.loads(request(port, '/slots/0?action=erase', 'POST'))
                    if erased.get('id_slot') != 0:
                        raise RuntimeError('Cannot clear benchmark slot')
                    (output / 'server-before.prom').write_text(request(port, '/metrics'))
                    start = time.monotonic()
                    with (output / 'events.jsonl').open('w') as out, (output / 'stderr.txt').open('w') as err:
                        try:
                            result = subprocess.run([str(opencode), 'run', '--pure', '--dir', str(root), '--model',
                                'llama.cpp/forge-local', '--format', 'json', task['prompt']], stdout=out, stderr=err, env=env, timeout=args.timeout)
                            code = result.returncode
                        except subprocess.TimeoutExpired:
                            code = 124
                    elapsed = time.monotonic() - start
                    (output / 'server-after.prom').write_text(request(port, '/metrics'))
                    verification = subprocess.run(task['verify'], cwd=root, capture_output=True, text=True, timeout=120, env=env)
                    (output / 'verification.stdout').write_text(verification.stdout)
                    (output / 'verification.stderr').write_text(verification.stderr)
                    events = []
                    for line in (output / 'events.jsonl').read_text(encoding='utf-8').splitlines():
                        try:
                            events.append(json.loads(line))
                        except ValueError:
                            pass
                    steps = [e['part'] for e in events if e.get('type') == 'step_finish' and 'part' in e]
                    unchanged = (root / 'repair_test.go').exists() and digest(root / 'repair_test.go') == before
                    record = {'task': task['id'], 'passed': code == 0 and verification.returncode == 0 and unchanged,
                              'returncode': code, 'tests_unchanged': unchanged, 'wall_seconds': elapsed,
                              'step_usage': [s.get('tokens', {}) for s in steps]}
                    records.append(record)
                    (output / 'result.json').write_text(json.dumps(record, indent=2))
                    (args.output / 'results.json').write_text(json.dumps(records, indent=2))
                    print(f'{task["id"]}: {"PASS" if record["passed"] else "FAIL"} ({elapsed:.1f}s)', flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return 0 if records and all(r['passed'] for r in records) else 1

if __name__ == '__main__':
    raise SystemExit(main())
