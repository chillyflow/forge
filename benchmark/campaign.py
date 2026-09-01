"""Run the parity pilot or full repeated campaign across all pinned harnesses."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import write_json


PILOT_TASKS = ['go_multifile_registry', 'go_compiler_interface', 'py_api_query']


def execute(label, command, env):
    print(f'\n[{label}] {" ".join(map(str, command))}', flush=True)
    result = subprocess.run(command, env=env, check=False)
    print(f'[{label}] exit {result.returncode}', flush=True)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['pilot', 'campaign'], default='pilot')
    parser.add_argument('--forge', type=Path, required=True)
    parser.add_argument('--opencode', type=Path, required=True)
    parser.add_argument('--aider', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tasks', nargs='*')
    parser.add_argument('--repetitions', type=int)
    parser.add_argument('--order-seed', type=int, default=20260831)
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--verification-timeout', type=int, default=120)
    parser.add_argument('--context', type=int, default=16384)
    parser.add_argument('--output-reserve', type=int, default=2048)
    parser.add_argument('--max-turns', type=int, default=16)
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--chat-template')
    parser.add_argument('--prompt-protocol', choices=['flattened', 'native'], default='flattened')
    parser.add_argument('--temperature', type=float, default=0.0)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--gpu-index', type=int, default=0)
    args = parser.parse_args()
    repetitions = args.repetitions if args.repetitions is not None else (
        1 if args.mode == 'pilot' else 3)
    tasks = args.tasks if args.tasks is not None else (PILOT_TASKS if args.mode == 'pilot' else [])
    if repetitions < 1:
        parser.error('--repetitions must be positive')
    binaries = {'forge': args.forge, 'opencode': args.opencode, 'aider': args.aider,
                'server': args.server, 'model': args.model}
    for label, path in binaries.items():
        if not path.resolve().is_file():
            parser.error(f'{label} does not exist: {path}')
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error(f'output directory is not empty: {output}')
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    bundled_go = Path(__file__).resolve().parents[1] / '.tools' / 'go' / 'bin'
    if bundled_go.is_dir():
        env['PATH'] = str(bundled_go) + os.pathsep + env.get('PATH', '')
    python = sys.executable
    directory = Path(__file__).resolve().parent
    common = ['--suite', 'all', '--repetitions', str(repetitions),
              '--order-seed', str(args.order_seed), '--timeout', str(args.timeout),
              '--verification-timeout', str(args.verification_timeout),
              '--context', str(args.context), '--output-reserve', str(args.output_reserve),
              '--max-turns', str(args.max_turns), '--gpu-layers', args.gpu_layers,
              '--temperature', str(args.temperature), '--seed', str(args.seed),
              '--gpu-index', str(args.gpu_index)]
    if tasks:
        common += ['--tasks', *tasks]
    if args.chat_template:
        common += ['--chat-template', args.chat_template]
    codes = {}
    preflight = [python, str(directory / 'preflight.py'), '--suite', 'all',
                 '--output', str(output / 'preflight.json')]
    if tasks:
        preflight += ['--tasks', *tasks]
    codes['preflight'] = execute('preflight', preflight, env)
    if codes['preflight'] != 0:
        write_json(output / 'campaign.json', {'mode': args.mode,
                                              'prompt_protocol': args.prompt_protocol,
                                              'codes': codes})
        return codes['preflight']
    freeze = [python, str(directory / 'freeze.py'), '--forge', str(args.forge.resolve()),
              '--opencode', str(args.opencode.resolve()), '--aider', str(args.aider.resolve()),
              '--server', str(args.server.resolve()), '--model', str(args.model.resolve()),
              '--suite', 'all', '--output', str(output / 'protocol-lock.json'),
              '--context', str(args.context), '--output-reserve', str(args.output_reserve),
              '--max-turns', str(args.max_turns), '--gpu-layers', args.gpu_layers,
              '--chat-template', args.chat_template or 'embedded',
              '--prompt-protocol', args.prompt_protocol,
              '--temperature', str(args.temperature), '--seed', str(args.seed),
              '--repetitions', str(repetitions), '--order-seed', str(args.order_seed),
              '--lifecycle', 'cold']
    if tasks:
        freeze += ['--tasks', *tasks]
    codes['freeze'] = execute('freeze', freeze, env)
    if codes['freeze'] != 0:
        write_json(output / 'campaign.json', {'mode': args.mode,
                                              'prompt_protocol': args.prompt_protocol,
                                              'codes': codes})
        return codes['freeze']
    commands = {
        'forge': [python, str(directory / 'run.py'), '--forge', str(args.forge.resolve()),
                  '--model', str(args.model.resolve()), '--output', str(output / 'forge'),
                  '--variants', 'optimized', '--prompt-protocol', args.prompt_protocol, *common],
        'opencode': [python, str(directory / 'opencode.py'), '--opencode',
                     str(args.opencode.resolve()), '--server', str(args.server.resolve()),
                     '--model', str(args.model.resolve()), '--output', str(output / 'opencode'),
                     '--lifecycle', 'cold', *common],
        'aider': [python, str(directory / 'aider.py'), '--aider', str(args.aider.resolve()),
                  '--server', str(args.server.resolve()), '--model', str(args.model.resolve()),
                  '--output', str(output / 'aider'), '--lifecycle', 'cold', *common],
    }
    for label, command in commands.items():
        codes[label] = execute(label, command, env)
    if all((output / name / 'results.json').is_file() for name in commands):
        report = [python, str(directory / 'report.py'),
                  '--run', f'Forge={output / "forge"}',
                  '--run', f'OpenCode={output / "opencode"}',
                  '--run', f'Aider={output / "aider"}', '--output', str(output / 'report')]
        codes['report'] = execute('report', report, env)
    write_json(output / 'campaign.json', {'schema_version': 1, 'mode': args.mode,
                                          'tasks': tasks or 'all',
                                          'prompt_protocol': args.prompt_protocol,
                                          'repetitions': repetitions,
                                          'order_seed': args.order_seed, 'codes': codes})
    return 0 if all(code == 0 for code in codes.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
