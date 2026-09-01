"""Freeze exact campaign source, runtime, model, configuration and fixture identity."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (check_tools, digest, load_tasks, materialize, runtime_bundle, write_json)
from aider import PINNED_AIDER_VERSION, parse_version


def version(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT,
                                   timeout=120).strip()


def source_identity(root):
    benchmark = root / 'benchmark'
    paths = sorted([*benchmark.glob('*.py'), benchmark / 'README.md',
                    benchmark / 'requirements-aider.txt'])
    return {path.relative_to(root).as_posix(): {'sha256': digest(path),
                                                'bytes': path.stat().st_size}
            for path in paths if path.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forge', type=Path, required=True)
    parser.add_argument('--opencode', type=Path, required=True)
    parser.add_argument('--aider', type=Path, required=True)
    parser.add_argument('--server', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--task-dir', type=Path, default=Path(__file__).parent / 'tasks')
    parser.add_argument('--suite', default='all')
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--context', type=int, default=16384)
    parser.add_argument('--output-reserve', type=int, default=2048)
    parser.add_argument('--max-turns', type=int, default=16)
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--chat-template', default='embedded')
    parser.add_argument('--temperature', type=float, default=0.0)
    parser.add_argument('--seed', type=int, default=42)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--order-seed', type=int, default=20260831)
    parser.add_argument('--lifecycle', choices=['cold', 'warm'], default='cold')
    args = parser.parse_args()
    paths = {name: path.resolve() for name, path in {
        'forge': args.forge, 'opencode': args.opencode, 'aider': args.aider,
        'server': args.server, 'model': args.model}.items()}
    for name, path in paths.items():
        if not path.is_file():
            parser.error(f'{name} does not exist: {path}')
    tasks = load_tasks(args.task_dir, args.suite, args.tasks)
    check_tools(tasks)
    root = Path(__file__).resolve().parents[1]
    task_identity = {}
    for path, task in tasks:
        with tempfile.TemporaryDirectory(prefix='forge-freeze-') as temporary:
            prepared = materialize(Path(temporary), task)
        task_identity[task['id']] = {'manifest_sha256': digest(path),
                                     'manifest_file': path.name, **prepared}
    aider_version = version([str(paths['aider']), '--version'])
    if parse_version(aider_version) != PINNED_AIDER_VERSION:
        parser.error(f'Aider must be exactly {PINNED_AIDER_VERSION}: {aider_version}')
    aider_python = paths['aider'].parent / ('python.exe' if os.name == 'nt' else 'python')
    packages = (version([str(aider_python), '-m', 'pip', 'freeze', '--all']).splitlines()
                if aider_python.is_file() else [])
    git_revision = version(['git', '-C', str(root), 'rev-parse', 'HEAD'])
    git_status = version(['git', '-C', str(root), 'status', '--short'])
    frozen = {
        'schema_version': 1,
        'git_revision': git_revision,
        'git_status_at_freeze': git_status.splitlines(),
        'source_files': source_identity(root),
        'configuration': {'suite': args.suite, 'tasks': args.tasks or 'all',
                          'context_tokens': args.context,
                          'output_reserve': args.output_reserve,
                          'max_turns': args.max_turns, 'gpu_layers': args.gpu_layers,
                          'chat_template': args.chat_template,
                          'temperature': args.temperature, 'seed': args.seed,
                          'repetitions': args.repetitions, 'order_seed': args.order_seed,
                          'lifecycle': args.lifecycle},
        'runtimes': {
            'forge': {'version': version([str(paths['forge']), '--version']),
                      'bundle': runtime_bundle(paths['forge'])},
            'opencode': {'version': version([str(paths['opencode']), '--version']),
                         'bundle': runtime_bundle(paths['opencode'])},
            'aider': {'version': aider_version, 'pin': PINNED_AIDER_VERSION,
                      'bundle': runtime_bundle(paths['aider']),
                      'installed_packages': packages},
            'llama_server': {'version': version([str(paths['server']), '--version']),
                             'bundle': runtime_bundle(paths['server'])},
        },
        'model': {'file': paths['model'].name, 'bytes': paths['model'].stat().st_size,
                  'sha256': digest(paths['model'])},
        'tasks': task_identity,
    }
    encoded = json.dumps(frozen, sort_keys=True, separators=(',', ':')).encode('utf-8')
    frozen['protocol_sha256'] = hashlib.sha256(encoded).hexdigest()
    frozen['created_utc'] = datetime.now(timezone.utc).isoformat()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_json(args.output, frozen)
    print(f'{len(tasks)} tasks frozen: {frozen["protocol_sha256"]}')


if __name__ == '__main__':
    main()
