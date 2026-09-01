"""Benchmark the exactly pinned Aider CLI against the shared local GGUF fixtures."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FIXTURE_PREPARATION, check_tools, digest, initialize_git, load_tasks,
                    materialize, platform_metadata, protected_files, protected_unchanged,
                    run_monitored, runtime_bundle, schedule, snapshot_protected, verify_task,
                    write_json)
from opencode import (isolated_environment, metric_record, request, server_command, start_server,
                      stop_server)


PINNED_AIDER_VERSION = '0.86.2'
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.go', '.h', '.hpp', '.java', '.js', '.jsx',
                   '.py', '.rs', '.ts', '.tsx'}


def parse_version(text):
    match = re.search(r'(?<!\d)(\d+\.\d+\.\d+)(?!\d)', text)
    return match.group(1) if match else None


def editable_files(task):
    if 'entry_files' in task:
        return list(task['entry_files'])
    immutable = set(protected_files(task))
    return [name for name in sorted(task['files'])
            if name not in immutable and Path(name).suffix.lower() in SOURCE_SUFFIXES]


def private_aider_files(output, args):
    private = output / 'aider-private'
    private.mkdir(parents=True, exist_ok=True)
    config = private / 'aider.conf.yml'
    config.write_text('{}\n', encoding='utf-8')
    dotenv = private / 'empty.env'
    dotenv.write_text('', encoding='utf-8')
    metadata = private / 'model-metadata.json'
    write_json(metadata, {'openai/forge-local': {
        'max_input_tokens': args.context - args.output_reserve,
        'max_output_tokens': args.output_reserve, 'max_tokens': args.output_reserve,
        'input_cost_per_token': 0, 'output_cost_per_token': 0,
        'litellm_provider': 'openai', 'mode': 'chat'}})
    return config, dotenv, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--aider', type=Path, required=True)
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
    parser.add_argument('--max-turns', type=int, default=16,
                        help='recorded for parity; Aider --message is a single coding turn')
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--chat-template', default=None)
    parser.add_argument('--temperature', type=float, default=0.0)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--repetitions', type=int, default=1)
    parser.add_argument('--order-seed', type=int, default=20260831)
    parser.add_argument('--no-randomize', action='store_true')
    parser.add_argument('--lifecycle', choices=['cold', 'warm'], default='cold')
    parser.add_argument('--gpu-index', type=int, default=0)
    parser.add_argument('--map-tokens', type=int, default=1024)
    args = parser.parse_args()
    aider, server, model = args.aider.resolve(), args.server.resolve(), args.model.resolve()
    for label, path in [('Aider', aider), ('llama-server', server), ('model', model)]:
        if not path.is_file():
            parser.error(f'{label} does not exist: {path}')
    version_text = subprocess.check_output([str(aider), '--version'], text=True,
                                           stderr=subprocess.STDOUT).strip()
    version = parse_version(version_text)
    if version != PINNED_AIDER_VERSION:
        parser.error(f'Aider must be exactly {PINNED_AIDER_VERSION}; found {version_text!r}')
    if args.repetitions < 1 or min(args.timeout, args.verification_timeout,
                                   args.server_timeout) < 1:
        parser.error('repetitions and timeouts must be positive')
    if args.context <= args.output_reserve or args.map_tokens < 0:
        parser.error('context must exceed output reserve and map tokens must be nonnegative')
    try:
        tasks = load_tasks(args.task_dir, args.suite, args.tasks)
        check_tools(tasks)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    base_env = isolated_environment(args.output)
    config, dotenv, model_metadata = private_aider_files(args.output, args)
    records = []
    aider_python = aider.parent / ('python.exe' if os.name == 'nt' else 'python')
    installed_packages = []
    if aider_python.is_file():
        installed_packages = subprocess.check_output(
            [str(aider_python), '-m', 'pip', 'freeze', '--all'], text=True,
            stderr=subprocess.STDOUT, timeout=120).splitlines()
    metadata = {'schema_version': 2, 'harness': 'aider', 'aider_version': version,
                'aider_pin': PINNED_AIDER_VERSION, 'aider_binary_sha256': digest(aider),
                'aider_runtime_bundle': runtime_bundle(aider),
                'aider_installed_packages': installed_packages,
                'fixture_preparation': FIXTURE_PREPARATION,
                'context_tokens': args.context, 'output_reserve': args.output_reserve,
                'max_turns': args.max_turns, 'map_tokens': args.map_tokens,
                'gpu_layers': args.gpu_layers,
                'chat_template': args.chat_template or 'embedded',
                'temperature': args.temperature, 'seed': args.seed,
                'task_suite': args.suite, 'repetitions': args.repetitions,
                'order_seed': args.order_seed, 'randomized_order': not args.no_randomize,
                'lifecycle': args.lifecycle, 'gpu_index': args.gpu_index,
                'server_version': subprocess.check_output(
                    [str(server), '--version'], text=True, stderr=subprocess.STDOUT).strip(),
                'server_binary_sha256': digest(server), 'model_file': model.name,
                'model_sha256': digest(model),
                'timing_policy': {
                    'startup': 'llama-server spawn through healthy response (cold only)',
                    'agent': 'Aider --message process spawn through exit',
                    'teardown': 'llama-server termination (cold only)',
                    'verification': 'independent manifest verifier after task and cold teardown',
                    'end_to_end': 'cold server spawn, or warm task spawn, through verifier exit'},
                'notes': 'Pinned Aider single-message whole-edit protocol. Analytics, updates, '
                         'commits, lint, auto-test, browser integration and prompt caching disabled.',
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
            run_id = f'{task["id"]}-aider-r{repetition:03d}'
            output = args.output / run_id
            if output.exists():
                parser.error(f'Run output already exists: {output}')
            output.mkdir(parents=True)
            with tempfile.TemporaryDirectory(prefix='forge-aider-') as temporary:
                root = Path(temporary).resolve()
                fixture = materialize(root, task)
                initialize_git(root)
                before_protected = snapshot_protected(root, task)
                state = shared_server or start_server(server, model, output, base_env, args)
                state['env']['OPENAI_API_BASE'] = f'http://127.0.0.1:{state["port"]}/v1'
                state['env']['OPENAI_API_KEY'] = state['key']
                startup_seconds = state['startup_seconds'] if shared_server is None else 0.0
                startup_usage = (state['startup_resource_usage'] if shared_server is None else None)
                erased = json.loads(request(state['port'], '/slots/0?action=erase', 'POST',
                                            state['key']))
                if erased.get('id_slot') != 0:
                    raise RuntimeError('Cannot clear benchmark slot')
                before_metrics = request(state['port'], '/metrics', key=state['key'])
                (output / 'server-before.prom').write_text(before_metrics, encoding='utf-8')
                command = [str(aider), '--model', 'openai/forge-local', '--edit-format', 'whole',
                           '--config', str(config), '--env-file', str(dotenv),
                           '--model-metadata-file', str(model_metadata), '--message', task['prompt'],
                           '--yes-always', '--no-stream', '--no-pretty', '--no-fancy-input',
                           '--no-cache-prompts', '--no-suggest-shell-commands',
                           '--no-auto-commits', '--no-dirty-commits',
                           '--no-gitignore', '--no-auto-lint', '--no-auto-test', '--no-check-update',
                           '--no-show-release-notes', '--no-analytics', '--disable-playwright',
                           '--no-show-model-warnings', '--no-check-model-accepts-settings',
                           '--line-endings', 'lf', '--encoding', 'utf-8', '--map-tokens',
                           str(args.map_tokens), '--timeout', str(args.timeout),
                           *editable_files(task)]
                with (output / 'aider.stdout.txt').open('w', encoding='utf-8') as out, \
                        (output / 'stderr.txt').open('w', encoding='utf-8') as err:
                    agent = run_monitored(command, cwd=root, stdout=out, stderr=err,
                                          env=state['env'], timeout=args.timeout,
                                          extra_pids=[state['process'].pid],
                                          gpu_index=args.gpu_index)
                after_metrics = request(state['port'], '/metrics', key=state['key'])
                (output / 'server-after.prom').write_text(after_metrics, encoding='utf-8')
                metrics = metric_record(before_metrics, after_metrics, [])
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
                          'harness': 'aider', 'repetition': repetition,
                          'order_index': order_index, 'order_seed': args.order_seed,
                          'returncode': agent['returncode'], 'passed': passed,
                          'wall_seconds': end_to_end, 'timing': timing,
                          'protected_files_unchanged': unchanged,
                          'protected_files': before_protected,
                          'suite': task.get('suite', 'smoke'),
                          'language': task.get('language', 'go'),
                          'category': task.get('category', 'repair'),
                          'metrics': metrics, 'startup_resource_usage': startup_usage,
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
