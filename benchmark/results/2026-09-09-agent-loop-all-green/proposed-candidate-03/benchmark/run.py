"""Run real agent benchmarks in disposable copies of trusted local task fixtures.

Model weights are never downloaded. A fresh process and workspace are used for
each task/variant. The generated result records model SHA-256, settings, elapsed
time, real session metrics and independent verification. No fabricated baselines.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FIXTURE_PREPARATION, check_tools, digest, initialize_git, load_tasks,
                    materialize, platform_metadata, protect_protected, protected_unchanged,
                    run_monitored, runtime_bundle, schedule, snapshot_protected, verify_task,
                    write_json)

# Keep experiment policy in one table so the CLI choices, recorded provenance,
# and command line cannot drift apart.  ``thought_cue`` is the host scaffold
# expected in raw model output; an empty value denotes the thinking-safe native
# baseline, which deliberately injects no scaffold.
VARIANTS = {
    'optimized': {'flags': ['--thought-history']},
    'minimal': {'flags': ['--minimal-agent', '--thought-history']},
    'candidate-checkpoint': {'flags': ['--minimal-agent', '--thought-history',
                                       '--candidate-checkpoint']},
    'loop-repair': {'flags': ['--minimal-agent', '--thought-history',
                              '--candidate-checkpoint', '--bounded-repair']},
    'loop-repair-best-of-2': {'flags': ['--minimal-agent', '--thought-history',
                                      '--candidate-checkpoint', '--bounded-repair',
                                      '--candidates', '2']},
    'loop-best-of-2': {'flags': ['--minimal-agent', '--thought-history',
                                '--candidate-checkpoint', '--candidates', '2']},
    'loop-semantic': {'flags': ['--minimal-agent', '--thought-history',
                               '--candidate-checkpoint', '--semantic-loops']},
    'loop-impact': {'flags': ['--minimal-agent', '--thought-history',
                             '--candidate-checkpoint', '--symbol-impact']},
    'loop-reflection': {'flags': ['--minimal-agent', '--thought-history',
                                 '--candidate-checkpoint', '--failure-reflection']},
    'loop-combined': {'flags': ['--minimal-agent', '--thought-history',
                               '--candidate-checkpoint', '--candidates', '2',
                               '--semantic-loops', '--symbol-impact', '--failure-reflection']},
    'no-kv': {'flags': ['--no-kv-reuse']},
    'no-semantic': {'flags': ['--no-semantic']},
    'no-compaction': {'flags': ['--no-compaction']},
    'grammar-first': {'flags': ['--grammar-first']},
    'no-thought': {'flags': ['--no-thought']},
    'thought-optional-decode-only': {'flags': ['--thought-decode-only']},
    'thought-required': {'flags': ['--thought-required', '--thought-history']},
    'thought-required-decode-only': {
        'flags': ['--thought-required', '--thought-decode-only']},
    'thought-routed': {
        'flags': ['--thought-routed', '--thought-history'], 'thought_cue': 'Thought: '},
    'thought-routed-decode-only': {
        'flags': ['--thought-routed', '--thought-decode-only'], 'thought_cue': 'Thought: '},
    'thought-routed-required': {
        'flags': ['--thought-routed', '--thought-required', '--thought-history'],
        'thought_cue': 'Thought: '},
    'thought-routed-required-decode-only': {
        'flags': ['--thought-routed', '--thought-required', '--thought-decode-only'],
        'thought_cue': 'Thought: '},
    'thought-routed-unbounded-decode-only': {
        'flags': ['--thought-routed', '--no-thought-budget', '--thought-decode-only'],
        'thought_cue': 'Thought: ', 'thought_budget': 'unbounded'},
    'thought-routed-budget-256-decode-only': {
        'flags': ['--thought-routed', '--thought-budget', '256', '--thought-decode-only'],
        'thought_cue': 'Thought: ', 'thought_budget': 256},
    'thought-routed-budget-512-decode-only': {
        'flags': ['--thought-routed', '--thought-budget', '512', '--thought-decode-only'],
        'thought_cue': 'Thought: ', 'thought_budget': 512},
    'thought-routed-budget-1024-decode-only': {
        'flags': ['--thought-routed', '--thought-budget', '1024', '--thought-decode-only'],
        'thought_cue': 'Thought: ', 'thought_budget': 1024},
    'thought-routed-budget-1536-decode-only': {
        'flags': ['--thought-routed', '--thought-budget', '1536', '--thought-decode-only'],
        'thought_cue': 'Thought: ', 'thought_budget': 1536},
    'thought-native-decode-only': {
        'flags': ['--thought-native', '--thought-decode-only'],
        'thought_cue': '', 'thought_budget': 'native'},
    'thought-native-disabled-decode-only': {
        'flags': ['--thought-native', '--disable-thinking', '--thought-decode-only'],
        'thought_cue': '', 'thought_budget': 'native-disabled'},
}
PROMPT_PROTOCOLS = ('flattened', 'native')


def terminal_root_ignored(name, is_directory):
    """Match the host input snapshot's root metadata directory exclusions."""
    return is_directory and (name.casefold() if os.name == 'nt' else name) in ('.git', '.forge')


def snapshot_terminal_inputs(root):
    root = Path(root).resolve()
    result = {}
    for directory, directories, names in os.walk(root):
        if Path(directory) == root:
            directories[:] = [name for name in directories if not terminal_root_ignored(name, True)]
        for name in names:
            path = Path(directory) / name
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def copy_terminal_inputs(root, destination):
    root = Path(root).resolve()

    def ignore(directory, names):
        if Path(directory) != root:
            return []
        return [name for name in names if terminal_root_ignored(name, (root / name).is_dir())]

    shutil.copytree(root, destination, ignore=ignore)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forge', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--task-dir', type=Path, default=Path(__file__).parent / 'tasks')
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--variants', nargs='+', choices=sorted(VARIANTS), default=['optimized'])
    parser.add_argument('--suite', default='smoke',
                        help='fixture family, or all; default preserves the original smoke suite')
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--chat-template', default=None,
                        help='llama.cpp chat template name; default uses the template embedded in the GGUF')
    parser.add_argument('--prompt-protocol', choices=PROMPT_PROTOCOLS, default='native',
                        help='Forge prompt/tool protocol arm; default uses native roles/tools')
    parser.add_argument('--context', default='16384')
    parser.add_argument('--output-reserve', type=int, default=2048)
    parser.add_argument('--temperature', type=float, default=0.0)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--verification-timeout', type=int, default=120)
    parser.add_argument('--max-turns', type=int, default=16)
    parser.add_argument('--max-tokens', type=int, default=32768)
    parser.add_argument('--max-input', type=int, default=262144)
    parser.add_argument('--repetitions', type=int, default=1)
    parser.add_argument('--order-seed', type=int, default=20260831)
    parser.add_argument('--no-randomize', action='store_true')
    parser.add_argument('--retain-terminal', action='store_true',
                        help='retain all terminal workspaces and full verifier metadata')
    parser.add_argument('--gpu-index', type=int, default=0)
    args = parser.parse_args()
    incompatible = [variant for variant in args.variants
                    if any(flag in ('--thought-routed', '--thought-native')
                           for flag in VARIANTS[variant]['flags'])]
    if args.prompt_protocol == 'native' and incompatible:
        parser.error('--prompt-protocol native is incompatible with variants: ' +
                     ', '.join(incompatible))
    forge, model = args.forge.resolve(), args.model.resolve()
    if not forge.is_file() or not model.is_file():
        parser.error('forge and model must exist')
    if not 1 <= args.output_reserve <= 1048576:
        parser.error('--output-reserve must be positive')
    if not 0 <= args.temperature <= 2:
        parser.error('--temperature must be in [0, 2]')
    if not 0 <= args.seed <= 4294967295:
        parser.error('--seed must be in [0, 4294967295]')
    if args.repetitions < 1:
        parser.error('--repetitions must be positive')
    if args.max_tokens < 1 or args.max_input < 1:
        parser.error('--max-tokens and --max-input must be positive')
    if args.timeout < 1 or args.verification_timeout < 1:
        parser.error('timeouts must be positive')
    try:
        tasks = load_tasks(args.task_dir, args.suite, args.tasks)
        check_tools(tasks)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    metadata = {'schema_version': 2, 'harness': 'forge',
                'model_file': model.name, 'model_sha256': digest(model), 'gpu_layers': args.gpu_layers,
                'chat_template': args.chat_template or 'embedded',
                'prompt_protocol': args.prompt_protocol,
                'forge_binary_sha256': digest(forge),
                'forge_runtime_bundle': runtime_bundle(forge),
                'fixture_preparation': FIXTURE_PREPARATION,
                'context_tokens': int(args.context), 'max_turns': int(args.max_turns),
                'max_tokens': args.max_tokens, 'max_input': args.max_input,
                'output_reserve': args.output_reserve,
                'temperature': args.temperature, 'seed': args.seed,
                'task_suite': args.suite, 'repetitions': args.repetitions,
                'order_seed': args.order_seed, 'randomized_order': not args.no_randomize,
                'lifecycle': 'cold', 'gpu_index': args.gpu_index,
                'timing_policy': {
                    'startup': 'process spawn through model load (metrics.load_ms)',
                    'agent': 'remaining Forge process time after model load',
                    'verification': 'independent manifest verify command after Forge exits',
                    'end_to_end': 'Forge process spawn through independent verifier exit'},
                'forge_version': subprocess.check_output([str(forge), '--version'], text=True).strip(),
                **platform_metadata()}
    write_json(args.output / 'environment.json', metadata)
    cases = [(task, variant) for _, task in tasks for variant in args.variants]
    for (task, variant), repetition, order_index in schedule(
            cases, args.repetitions, args.order_seed, not args.no_randomize):
        policy = VARIANTS[variant]
        run_id = f'{task["id"]}-{variant}-r{repetition:03d}'
        output = args.output / run_id
        if output.exists():
            parser.error(f'Run output already exists: {output}')
        output.mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix='forge-bench-') as temporary:
            root = Path(temporary).resolve()
            fixture = materialize(root, task)
            initialize_git(root)
            before_protected = snapshot_protected(root, task)
            protect_protected(root, task, readonly=True)
            command = [str(forge), 'run', task['prompt'], '--workspace', str(root),
                       '--model', str(model), '--gpu-layers', args.gpu_layers,
                       '--prompt-protocol', args.prompt_protocol,
                       '--context', args.context, '--output-reserve', str(args.output_reserve),
                       '--temperature', str(args.temperature), '--seed', str(args.seed),
                       '--allow-write', '--allow-exec', '--json', '--max-turns',
                       str(args.max_turns), '--wall-ms', str(args.timeout * 1000),
                       '--max-tokens', str(args.max_tokens), '--max-input', str(args.max_input),
                       *policy['flags'],
                       *(['--chat-template', args.chat_template] if args.chat_template else [])]
            write_json(output / 'command.json', command)
            with (output / 'stdout.jsonl').open('w', encoding='utf-8') as out, \
                    (output / 'stderr.txt').open('w', encoding='utf-8') as err:
                process_result = run_monitored(command, stdout=out, stderr=err,
                                               timeout=args.timeout + 30,
                                               gpu_index=args.gpu_index)
            sessions = list((root / '.forge' / 'sessions').glob('*'))
            metrics = {}
            if sessions:
                session = max(sessions, key=lambda path: path.stat().st_mtime_ns)
                shutil.copytree(session, output / 'session', dirs_exist_ok=True)
                if (session / 'metrics.json').exists():
                    metrics = json.loads((session / 'metrics.json').read_text(encoding='utf-8'))
            before_verification = snapshot_protected(root, task)
            before_verification_inputs = snapshot_terminal_inputs(root) if args.retain_terminal else None
            if args.retain_terminal:
                copy_terminal_inputs(root, output / 'pre-verification-workspace')
            verification = verify_task(root, task, output, timeout=args.verification_timeout,
                                       gpu_index=args.gpu_index)
            write_json(output / 'verification.json', verification)
            after_verification_inputs = snapshot_terminal_inputs(root) if args.retain_terminal else None
            unchanged = protected_unchanged(root, before_protected)
            protect_protected(root, task, readonly=False)
            startup_seconds = (metrics['load_ms'] / 1000.0) if 'load_ms' in metrics else None
            agent_seconds = (max(0.0, process_result['wall_seconds'] - startup_seconds)
                             if startup_seconds is not None else None)
            end_to_end = process_result['wall_seconds'] + verification['wall_seconds']
            passed = process_result['returncode'] == 0 and verification['passed'] and unchanged
            if args.retain_terminal:
                passed = passed and before_verification_inputs == after_verification_inputs
            with (output / 'workspace-status.txt').open('w', encoding='utf-8') as status:
                subprocess.run(['git', '-C', str(root), 'status', '--short'], text=True,
                               stdout=status, check=False)
            with (output / 'workspace.diff').open('w', encoding='utf-8') as diff:
                subprocess.run(['git', '-C', str(root), 'diff', '--binary'], text=True,
                               stdout=diff, check=False)
            if not passed:
                shutil.copytree(root, output / 'failed-workspace', ignore=shutil.ignore_patterns('.git'))
            if args.retain_terminal:
                copy_terminal_inputs(root, output / 'terminal-workspace')
                write_json(output / 'verification-inputs.json', {
                    'before': before_verification_inputs, 'after': after_verification_inputs,
                    'protected_before': before_verification,
                    'protected_after': snapshot_protected(root, task)})
            timing = {'lifecycle': 'cold', 'startup_seconds': startup_seconds,
                      'agent_seconds': agent_seconds,
                      'agent_process_seconds': process_result['wall_seconds'],
                      'verification_seconds': verification['wall_seconds'],
                      'end_to_end_seconds': end_to_end}
            record = {'schema_version': 2, 'run_id': run_id, 'task': task['id'],
                      'variant': variant, 'repetition': repetition, 'order_index': order_index,
                      'prompt_protocol': args.prompt_protocol,
                      'order_seed': args.order_seed, 'returncode': process_result['returncode'],
                      'wall_seconds': end_to_end, 'timing': timing, 'passed': passed,
                      'protected_files_unchanged': unchanged,
                      'protected_files': before_protected, 'suite': task.get('suite', 'smoke'),
                      'language': task.get('language', 'go'),
                      'category': task.get('category', 'repair'),
                      'thought_cue': policy.get('thought_cue'),
                      'thought_budget': policy.get('thought_budget'), 'metrics': metrics,
                      'resource_usage': process_result['resource_usage'],
                      'verification_resource_usage': verification['resource_usage'], **fixture}
            if args.retain_terminal:
                record.update(verification=verification, terminal_workspace='terminal-workspace',
                              pre_verification_workspace='pre-verification-workspace',
                              terminal_retention_policy='complete-except-git-forge',
                              verification_inputs_unchanged=before_verification_inputs == after_verification_inputs,
                              protected_before_verification=before_verification == before_protected)
            records.append(record)
            write_json(output / 'result.json', record)
            write_json(args.output / 'results.json', records)
            print(f'{run_id}: {"PASS" if passed else "FAIL"} ({end_to_end:.1f}s e2e)',
                  flush=True)
    return 0 if all(x['passed'] for x in records) else 1

if __name__ == '__main__':
    raise SystemExit(main())
