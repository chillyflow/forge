"""Run real agent benchmarks in disposable copies of trusted local task fixtures.

Model weights are never downloaded. A fresh process and workspace are used for
each task/variant. The generated result records model SHA-256, settings, elapsed
time, real session metrics and independent verification. No fabricated baselines.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(16 * 1024 * 1024):
            h.update(data)
    return h.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forge', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--variants', nargs='+', choices=['optimized', 'no-kv', 'no-semantic', 'no-compaction'], default=['optimized'])
    parser.add_argument('--gpu-layers', default='-1')
    parser.add_argument('--context', default='16384')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    forge, model = args.forge.resolve(), args.model.resolve()
    if not forge.is_file() or not model.is_file():
        parser.error('forge and model must exist')
    if not shutil.which('go'):
        parser.error('Go must be on PATH')
    args.output.mkdir(parents=True, exist_ok=True)
    flags = {'optimized': [], 'no-kv': ['--no-kv-reuse'], 'no-semantic': ['--no-semantic'], 'no-compaction': ['--no-compaction']}
    records = []
    metadata = {'schema_version': 1, 'model_file': model.name, 'model_sha256': digest(model), 'gpu_layers': args.gpu_layers,
                'context_tokens': int(args.context), 'platform': platform.platform(), 'forge_version': subprocess.check_output([str(forge), '--version'], text=True).strip(),
                'go_version': subprocess.check_output(['go', 'version'], text=True).strip()}
    try:
        metadata['gpu'] = subprocess.check_output(['nvidia-smi', '--query-gpu=name,driver_version,memory.total', '--format=csv,noheader'], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        metadata['gpu'] = None
    (args.output / 'environment.json').write_text(json.dumps(metadata, indent=2))
    for task_path in sorted((Path(__file__).parent / 'tasks').glob('*.json')):
        task = json.loads(task_path.read_text())
        if args.tasks and task['id'] not in args.tasks:
            continue
        for variant in args.variants:
            name = f'{task["id"]}-{variant}'
            output = args.output / name
            output.mkdir(exist_ok=True)
            with tempfile.TemporaryDirectory(prefix='forge-bench-') as temporary:
                root = Path(temporary).resolve()
                for relative, content in task['files'].items():
                    path = (root / relative).resolve()
                    if not path.is_relative_to(root):
                        raise ValueError('Unsafe fixture path')
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content)
                subprocess.run(['git', 'init', '-q', str(root)], check=True)
                subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
                subprocess.run(['git', '-C', str(root), '-c', 'user.name=Forge benchmark', '-c', 'user.email=benchmark@example.invalid', 'commit', '-qm', 'Fixture baseline'], check=True)
                before_tests = digest(root / 'repair_test.go')
                command = [str(forge), 'bench', str(task_path.resolve()), '--workspace', str(root), '--model', str(model), '--gpu-layers', args.gpu_layers,
                           '--context', args.context, '--allow-write', '--allow-exec', '--json', '--max-turns', '16', '--wall-ms', str(args.timeout * 1000), *flags[variant]]
                start = time.monotonic()
                with (output / 'stdout.jsonl').open('w') as out, (output / 'stderr.txt').open('w') as err:
                    try:
                        result = subprocess.run(command, stdout=out, stderr=err, timeout=args.timeout + 30)
                        code = result.returncode
                    except subprocess.TimeoutExpired:
                        code = 124
                elapsed = time.monotonic() - start
                sessions = list((root / '.forge' / 'sessions').glob('*'))
                metrics, verdict = {}, {}
                if sessions:
                    session = max(sessions, key=lambda p: p.stat().st_mtime_ns)
                    shutil.copytree(session, output / 'session', dirs_exist_ok=True)
                    if (session / 'metrics.json').exists():
                        metrics = json.loads((session / 'metrics.json').read_text())
                    if (session / 'benchmark.json').exists():
                        verdict = json.loads((session / 'benchmark.json').read_text())
                unchanged_tests = (root / 'repair_test.go').exists() and digest(root / 'repair_test.go') == before_tests
                record = {'task': task['id'], 'variant': variant, 'returncode': code, 'wall_seconds': elapsed,
                          'passed': code == 0 and verdict.get('passed', False) and unchanged_tests,
                          'tests_unchanged': unchanged_tests, 'metrics': metrics}
                records.append(record)
                (output / 'result.json').write_text(json.dumps(record, indent=2))
                (args.output / 'results.json').write_text(json.dumps(records, indent=2))
                print(f'{name}: {"PASS" if record["passed"] else "FAIL"} ({elapsed:.1f}s)', flush=True)
    if not records:
        parser.error('No matching tasks')
    return 0 if all(x['passed'] for x in records) else 1

if __name__ == '__main__':
    raise SystemExit(main())
