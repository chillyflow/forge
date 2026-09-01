"""Benchmark a pinned local OpenCode against the shared trusted fixtures.

The adapter supports explicit cold and warm lifecycles. Every task is verified
outside OpenCode, after the task command exits, using the manifest's verifier.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (CREATE_NO_WINDOW, FIXTURE_PREPARATION, ResourceMonitor, check_tools,
                    digest, initialize_git, load_tasks, materialize, platform_metadata,
                    protected_unchanged, run_monitored, runtime_bundle, schedule, snapshot_protected,
                    verify_task, write_json)


def request(port, path, method='GET', key=''):
    query = urllib.request.Request(f'http://127.0.0.1:{port}{path}', method=method,
                                   data=b'{}' if method == 'POST' else None,
                                   headers={'Content-Type': 'application/json',
                                            'Authorization': f'Bearer {key}'})
    with urllib.request.urlopen(query, timeout=5) as response:
        return response.read().decode()


def counters(text):
    return {parts[0]: float(parts[1]) for line in text.splitlines()
            if line and not line.startswith('#') and len(parts := line.split()) == 2}


def isolated_environment(output):
    env = {key: value for key, value in os.environ.items() if key.upper() in {
        'PATH', 'SYSTEMROOT', 'WINDIR', 'TEMP', 'TMP', 'COMSPEC', 'PATHEXT',
        'USERPROFILE', 'HOME', 'LOCALAPPDATA', 'APPDATA', 'PROGRAMFILES',
        'PROGRAMFILES(X86)'}}
    for variable, folder in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                             ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')]:
        path = output / 'opencode-state' / folder
        path.mkdir(parents=True, exist_ok=True)
        env[variable] = str(path)
    for key in ['OPENCODE_DISABLE_AUTOUPDATE', 'OPENCODE_DISABLE_DEFAULT_PLUGINS',
                'OPENCODE_DISABLE_LSP_DOWNLOAD', 'OPENCODE_DISABLE_CLAUDE_CODE',
                'OPENCODE_DISABLE_MODELS_FETCH']:
        env[key] = 'true'
    env['OPENCODE_AUTO_SHARE'] = 'false'
    return env


def reserve_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def server_command(server, model, port, slot_path, args):
    command = [str(server), '--model', str(model), '--alias', 'forge-local',
               '--ctx-size', str(args.context), '--gpu-layers', args.gpu_layers,
               '--parallel', '1', '--seed', str(args.seed), '--temp', str(args.temperature),
               '--metrics', '--cache-ram', '0', '--host', '127.0.0.1', '--port', str(port),
               '--no-webui', '--slot-save-path', str(slot_path), '--batch-size', '512',
               '--ubatch-size', '256', '--threads', '4', '--threads-batch', '4']
    if args.chat_template:
        command += ['--chat-template', args.chat_template]
    return command


def write_config(path, port, local_key, args):
    config = {
        '$schema': 'https://opencode.ai/config.json', 'share': 'disabled',
        'autoupdate': False, 'enabled_providers': ['llama.cpp'],
        'model': 'llama.cpp/forge-local', 'small_model': 'llama.cpp/forge-local',
        'provider': {'llama.cpp': {'npm': '@ai-sdk/openai-compatible',
            'name': 'Local benchmark',
            'options': {'baseURL': f'http://127.0.0.1:{port}/v1', 'apiKey': local_key},
            'models': {'forge-local': {'name': 'Pinned local GGUF',
                'limit': {'context': args.context, 'output': args.output_reserve}}}}},
        'permission': {'read': 'allow', 'edit': 'allow', 'glob': 'allow',
                       'grep': 'allow', 'bash': 'allow', 'task': 'deny',
                       'webfetch': 'deny', 'websearch': 'deny',
                       'external_directory': 'deny', 'skill': 'deny', 'lsp': 'deny',
                       'question': 'deny'},
        'agent': {'build': {'temperature': args.temperature, 'steps': args.max_turns}},
        'lsp': False, 'formatter': False,
    }
    write_json(path, config)


def start_server(server, model, output, env, args):
    output.mkdir(parents=True, exist_ok=True)
    port = reserve_port()
    local_key = secrets.token_urlsafe(32)
    env = dict(env)
    env['LLAMA_API_KEY'] = local_key
    config_path = output / 'opencode-private.json'
    write_config(config_path, port, local_key, args)
    env['OPENCODE_CONFIG'] = str(config_path)
    slot_path = output / 'slots'
    slot_path.mkdir(exist_ok=True)
    command = server_command(server, model, port, slot_path, args)
    log = (output / 'server.log').open('w', encoding='utf-8')
    monitor = ResourceMonitor(gpu_index=args.gpu_index)
    start = time.monotonic()
    process = subprocess.Popen(command, stdout=log, stderr=log, env=env,
                               creationflags=CREATE_NO_WINDOW if os.name == 'nt' else 0)
    monitor.add_root(process.pid)
    monitor.start()
    try:
        while True:
            if process.poll() is not None:
                raise RuntimeError(f'llama-server exited; inspect {output / "server.log"}')
            if time.monotonic() - start > args.server_timeout:
                raise TimeoutError('llama-server did not become healthy')
            try:
                if json.loads(request(port, '/health')).get('status') == 'ok':
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.25)
    except Exception:
        process.terminate()
        process.wait(timeout=10)
        log.close()
        monitor.stop()
        raise
    return {'process': process, 'port': port, 'key': local_key, 'env': env,
            'command': command, 'log': log, 'startup_seconds': time.monotonic() - start,
            'startup_resource_usage': monitor.stop()}


def stop_server(state):
    start = time.monotonic()
    state['process'].terminate()
    try:
        state['process'].wait(timeout=10)
    except subprocess.TimeoutExpired:
        state['process'].kill()
        state['process'].wait()
    state['log'].close()
    return time.monotonic() - start


def metric_record(before_text, after_text, events):
    old, new = counters(before_text), counters(after_text)
    delta = {key: value - old.get(key, 0) for key, value in new.items()}
    required = ['llamacpp:prompt_tokens_total', 'llamacpp:prompt_tokens_cached_total',
                'llamacpp:tokens_predicted_total', 'llamacpp:prompt_seconds_total',
                'llamacpp:tokens_predicted_seconds_total']
    missing = [key for key in required if key not in delta]
    if missing:
        raise RuntimeError(f'llama-server metrics are missing: {missing}')
    result = {
        'prefill_tokens': int(delta['llamacpp:prompt_tokens_total']),
        'cached_tokens': int(delta['llamacpp:prompt_tokens_cached_total']),
        'generated_tokens': int(delta['llamacpp:tokens_predicted_total']),
        'prefill_ms': delta['llamacpp:prompt_seconds_total'] * 1000,
        'decode_ms': delta['llamacpp:tokens_predicted_seconds_total'] * 1000,
        'tool_calls': sum(event.get('type') == 'tool_use' for event in events),
    }
    result['prompt_tokens'] = result['prefill_tokens'] + result['cached_tokens']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opencode', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--task-dir', type=Path, default=Path(__file__).parent / 'tasks')
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--suite', default='smoke')
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--verification-timeout', type=int, default=120)
    parser.add_argument('--server-timeout', type=int, default=180)
    parser.add_argument('--context', type=int, default=16384)
    parser.add_argument('--output-reserve', type=int, default=2048)
    parser.add_argument('--max-turns', type=int, default=16)
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--chat-template', default=None)
    parser.add_argument('--temperature', type=float, default=0.0)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--repetitions', type=int, default=1)
    parser.add_argument('--order-seed', type=int, default=20260831)
    parser.add_argument('--no-randomize', action='store_true')
    parser.add_argument('--lifecycle', choices=['cold', 'warm'], default='cold')
    parser.add_argument('--gpu-index', type=int, default=0)
    args = parser.parse_args()
    opencode, server, model = (args.opencode.resolve(), args.server.resolve(),
                               args.model.resolve())
    for label, path in [('OpenCode', opencode), ('llama-server', server), ('model', model)]:
        if not path.is_file():
            parser.error(f'{label} does not exist: {path}')
    if args.repetitions < 1 or min(args.timeout, args.verification_timeout,
                                   args.server_timeout) < 1:
        parser.error('repetitions and timeouts must be positive')
    try:
        tasks = load_tasks(args.task_dir, args.suite, args.tasks)
        check_tools(tasks)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    base_env = isolated_environment(args.output)
    records = []
    metadata = {'schema_version': 2, 'harness': 'opencode',
                'opencode_version': subprocess.check_output(
                    [str(opencode), '--version'], text=True).strip(),
                'fixture_preparation': FIXTURE_PREPARATION,
                'context_tokens': args.context, 'output_reserve': args.output_reserve,
                'max_turns': args.max_turns, 'gpu_layers': args.gpu_layers,
                'chat_template': args.chat_template or 'embedded',
                'temperature': args.temperature, 'seed': args.seed,
                'task_suite': args.suite, 'repetitions': args.repetitions,
                'order_seed': args.order_seed, 'randomized_order': not args.no_randomize,
                'lifecycle': args.lifecycle, 'gpu_index': args.gpu_index,
                'server_version': subprocess.check_output(
                    [str(server), '--version'], text=True, stderr=subprocess.STDOUT).strip(),
                'server_binary_sha256': digest(server),
                'server_runtime_bundle': runtime_bundle(server),
                'opencode_binary_sha256': digest(opencode),
                'opencode_runtime_bundle': runtime_bundle(opencode), 'model_file': model.name,
                'model_sha256': digest(model),
                'timing_policy': {
                    'startup': 'llama-server spawn through healthy response (cold only)',
                    'agent': 'OpenCode task process spawn through exit',
                    'teardown': 'llama-server termination (cold only)',
                    'verification': 'independent manifest verifier after task and cold teardown',
                    'end_to_end': 'cold server spawn, or warm task spawn, through verifier exit'},
                'notes': 'Slot KV is erased before each task; cross-task RAM prompt cache is disabled. '
                         'Remote tools, subagents, plugins, sharing and model downloads are disabled.',
                **platform_metadata()}
    template = server_command(server, model, '<PORT>', Path('<SLOT_PATH>'), args)
    metadata['server_command'] = [model.name if value == str(model) else server.name
                                  if value == str(server) else value for value in template]
    write_json(args.output / 'environment.json', metadata)
    shared_server = None
    if args.lifecycle == 'warm':
        shared_server = start_server(server, model, args.output / 'warm-server', base_env, args)
        metadata['warm_server_startup_seconds'] = shared_server['startup_seconds']
        metadata['warm_server_startup_resource_usage'] = shared_server['startup_resource_usage']
        write_json(args.output / 'environment.json', metadata)
    try:
        for (_, task), repetition, order_index in schedule(
                tasks, args.repetitions, args.order_seed, not args.no_randomize):
            run_id = f'{task["id"]}-opencode-r{repetition:03d}'
            output = args.output / run_id
            if output.exists():
                parser.error(f'Run output already exists: {output}')
            output.mkdir(parents=True)
            with tempfile.TemporaryDirectory(prefix='forge-opencode-') as temporary:
                root = Path(temporary).resolve()
                fixture = materialize(root, task)
                initialize_git(root)
                before_protected = snapshot_protected(root, task)
                state = shared_server or start_server(server, model, output, base_env, args)
                startup_seconds = state['startup_seconds'] if shared_server is None else 0.0
                startup_usage = (state['startup_resource_usage'] if shared_server is None else None)
                erased = json.loads(request(state['port'], '/slots/0?action=erase', 'POST',
                                            state['key']))
                if erased.get('id_slot') != 0:
                    raise RuntimeError('Cannot clear benchmark slot')
                before_metrics = request(state['port'], '/metrics', key=state['key'])
                (output / 'server-before.prom').write_text(before_metrics, encoding='utf-8')
                command = [str(opencode), 'run', '--pure', '--dir', str(root), '--model',
                           'llama.cpp/forge-local', '--format', 'json', task['prompt']]
                with (output / 'events.jsonl').open('w', encoding='utf-8') as out, \
                        (output / 'stderr.txt').open('w', encoding='utf-8') as err:
                    agent = run_monitored(command, stdout=out, stderr=err, env=state['env'],
                                          timeout=args.timeout, extra_pids=[state['process'].pid],
                                          gpu_index=args.gpu_index)
                after_metrics = request(state['port'], '/metrics', key=state['key'])
                (output / 'server-after.prom').write_text(after_metrics, encoding='utf-8')
                events = []
                for line in (output / 'events.jsonl').read_text(encoding='utf-8').splitlines():
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
                steps = [event['part'] for event in events
                         if event.get('type') == 'step_finish' and 'part' in event]
                metrics = metric_record(before_metrics, after_metrics, events)
                teardown_seconds = 0.0
                if shared_server is None:
                    teardown_seconds = stop_server(state)
                verification_extra = [state['process'].pid] if shared_server is not None else []
                verification = verify_task(root, task, output, env=state['env'],
                                           timeout=args.verification_timeout,
                                           gpu_index=args.gpu_index,
                                           extra_pids=verification_extra)
                unchanged = protected_unchanged(root, before_protected)
                end_to_end = (startup_seconds + agent['wall_seconds'] + teardown_seconds +
                              verification['wall_seconds'])
                passed = agent['returncode'] == 0 and verification['passed'] and unchanged
                with (output / 'workspace-status.txt').open('w', encoding='utf-8') as status:
                    subprocess.run(['git', '-C', str(root), 'status', '--short'], text=True,
                                   stdout=status, check=False)
                with (output / 'workspace.diff').open('w', encoding='utf-8') as diff:
                    subprocess.run(['git', '-C', str(root), 'diff', '--binary'], text=True,
                                   stdout=diff, check=False)
                if not passed:
                    shutil.copytree(root, output / 'failed-workspace',
                                    ignore=shutil.ignore_patterns('.git'))
                timing = {'lifecycle': args.lifecycle, 'startup_seconds': startup_seconds,
                          'agent_seconds': agent['wall_seconds'],
                          'teardown_seconds': teardown_seconds,
                          'verification_seconds': verification['wall_seconds'],
                          'end_to_end_seconds': end_to_end}
                record = {'schema_version': 2, 'run_id': run_id, 'task': task['id'],
                          'harness': 'opencode', 'repetition': repetition,
                          'order_index': order_index, 'order_seed': args.order_seed,
                          'returncode': agent['returncode'], 'passed': passed,
                          'wall_seconds': end_to_end, 'timing': timing,
                          'protected_files_unchanged': unchanged,
                          'protected_files': before_protected,
                          'suite': task.get('suite', 'smoke'),
                          'language': task.get('language', 'go'),
                          'category': task.get('category', 'repair'),
                          'metrics': metrics,
                          'step_usage': [step.get('tokens', {}) for step in steps],
                          'startup_resource_usage': startup_usage,
                          'resource_usage': agent['resource_usage'],
                          'verification_resource_usage': verification['resource_usage'], **fixture}
                records.append(record)
                write_json(output / 'result.json', record)
                write_json(args.output / 'results.json', records)
                print(f'{run_id}: {"PASS" if passed else "FAIL"} ({end_to_end:.1f}s e2e, '
                      f'{args.lifecycle})', flush=True)
    finally:
        if shared_server is not None:
            metadata['warm_server_teardown_seconds'] = stop_server(shared_server)
            write_json(args.output / 'environment.json', metadata)
    return 0 if records and all(record['passed'] for record in records) else 1


if __name__ == '__main__':
    raise SystemExit(main())
