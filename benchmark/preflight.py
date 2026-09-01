"""Validate fixture manifests and prove their broken/oracle states deterministically."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import check_tools, load_tasks, materialize, protected_files, write_json


def run_verify(root, task, timeout):
    start = time.monotonic()
    result = subprocess.run(task['verify'], cwd=root, capture_output=True, text=True,
                            timeout=timeout)
    return {'returncode': result.returncode, 'seconds': time.monotonic() - start,
            'stdout': result.stdout[-4000:], 'stderr': result.stderr[-4000:]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--task-dir', type=Path, default=Path(__file__).parent / 'tasks')
    parser.add_argument('--suite', default='all')
    parser.add_argument('--tasks', nargs='*', default=[])
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        tasks = load_tasks(args.task_dir, args.suite, args.tasks)
        check_tools(tasks)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))
    records = []
    failed = False
    for path, task in tasks:
        record = {'task': task['id'], 'manifest': path.name,
                  'suite': task.get('suite', 'smoke'),
                  'language': task.get('language', 'go'),
                  'category': task.get('category', 'repair')}
        with tempfile.TemporaryDirectory(prefix='forge-preflight-broken-') as temporary:
            fixture = materialize(Path(temporary), task)
            broken = run_verify(temporary, task, args.timeout)
            record.update(fixture, broken_returncode=broken['returncode'],
                          broken_seconds=broken['seconds'])
            if broken['returncode'] == 0:
                record['error'] = 'broken fixture unexpectedly passes'
                record['broken_stdout'] = broken['stdout']
                record['broken_stderr'] = broken['stderr']
                failed = True
        oracle = task.get('oracle_files')
        if oracle:
            overlap = set(oracle) & set(protected_files(task))
            if overlap:
                record['error'] = f'oracle modifies protected files: {sorted(overlap)}'
                failed = True
            repaired = dict(task)
            repaired['files'] = {**task['files'], **oracle}
            with tempfile.TemporaryDirectory(prefix='forge-preflight-oracle-') as temporary:
                materialize(Path(temporary), repaired)
                result = run_verify(temporary, task, args.timeout)
                record.update(oracle_returncode=result['returncode'],
                              oracle_seconds=result['seconds'])
                if result['returncode'] != 0:
                    record['error'] = 'oracle fixture fails verification'
                    record['oracle_stdout'] = result['stdout']
                    record['oracle_stderr'] = result['stderr']
                    failed = True
        else:
            record['oracle_returncode'] = None
        records.append(record)
        print(f'{task["id"]}: {"FAIL" if record.get("error") else "PASS"}', flush=True)
    report = {'schema_version': 1, 'tasks': len(records),
              'oracle_tasks': sum(record['oracle_returncode'] is not None for record in records),
              'passed': not failed, 'records': records}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_json(args.output, report)
    print(json.dumps({key: report[key] for key in ('tasks', 'oracle_tasks', 'passed')}))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
