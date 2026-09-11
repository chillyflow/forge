"""Audit a completed frozen campaign and export numeric records and failure sources.

Full raw sessions stay in the original directory; private harness state is never
exported. This post-run tool does not alter records, rerun failures, or score edits.
"""
import argparse
import json
from pathlib import Path
import shutil

from common import digest, schedule, write_json
from report import group_records, validate_failure_artifacts, validate_groups, validate_measurements


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def audit(source, source_root):
    lock = read(source / 'protocol-lock.json')
    require(lock['clean_revision_required'] and not lock['git_status_at_freeze'],
            'Protocol was not frozen from a clean revision')
    import hashlib
    identity = {k: v for k, v in lock.items() if k not in ('protocol_sha256', 'created_utc')}
    encoded = json.dumps(identity, sort_keys=True, separators=(',', ':')).encode('utf-8')
    require(hashlib.sha256(encoded).hexdigest() == lock['protocol_sha256'], 'Lock hash mismatch')
    for name, expected in lock['source_files'].items():
        require(digest(source_root / name) == expected['sha256'], f'Source changed: {name}')
    config, tasks = lock['configuration'], lock['tasks']
    expected_order = [(task, rep, index) for task, rep, index in
                      schedule(sorted(tasks), config['repetitions'], config['order_seed'])]
    groups, legs = {}, {}
    for leg in ('forge', 'opencode', 'aider'):
        path = source / leg
        if not (path / 'results.json').exists():
            continue
        env, rows = read(path / 'environment.json'), read(path / 'results.json')
        require(env['harness'] == leg, f'Wrong harness in {leg}')
        require(env['model_sha256'] == lock['model']['sha256'], f'Model mismatch in {leg}')
        for key in ('context_tokens', 'output_reserve', 'max_turns', 'gpu_layers',
                    'chat_template', 'temperature', 'seed', 'repetitions', 'order_seed', 'lifecycle'):
            require(env[key] == config[key], f'{leg}: configuration mismatch: {key}')
        for key in ('platform', 'gpu', 'go_version', 'python_version'):
            require(env.get(key) == lock['hardware'].get(key), f'{leg}: hardware/toolchain mismatch: {key}')
        require(env[f'{leg}_runtime_bundle'] == lock['runtimes'][leg]['bundle'],
                f'Runtime mismatch in {leg}')
        if leg == 'forge':
            require(env['prompt_protocol'] == config['prompt_protocol'], 'Forge protocol mismatch')
        else:
            server = lock['runtimes']['llama_server']['bundle']['files']
            server_hash = next(v['sha256'] for k, v in server.items() if k in ('llama-server.exe', 'llama-server'))
            require(env['server_binary_sha256'] == server_hash, f'{leg}: server mismatch')
        actual_order = [(r['task'], r['repetition'], r['order_index']) for r in rows]
        require(actual_order == expected_order, f'{leg}: incomplete or altered schedule')
        for row in rows:
            task, run = tasks[row['task']], path / row['run_id']
            require(row == read(run / 'result.json'), f'{leg}: per-run record mismatch')
            for key in ('fixture_sha256', 'fixture_files', 'fixture_preparation'):
                require(row[key] == task[key], f'{leg}/{row["run_id"]}: fixture mismatch')
            require(row['protected_files_unchanged'], f'{leg}: protected files changed')
            require(all(task['fixture_files'].get(k) == v for k, v in row['protected_files'].items()),
                    f'{leg}: protected hashes mismatch')
        validate_failure_artifacts(leg, path, rows)
        groups.update(group_records(leg, rows))
        legs[leg] = {'scheduled': len(expected_order), 'runs': len(rows),
                     'passed': sum(r['passed'] for r in rows),
                     'failed': [r['run_id'] for r in rows if not r['passed']]}
    require(legs, 'No result legs found')
    validate_groups(groups)
    validate_measurements(groups)
    return {'git_revision': lock['git_revision'], 'protocol_sha256': lock['protocol_sha256'],
            'audit_tool_sha256': digest(Path(__file__)),
            'source_directory': str(source.resolve()), 'clean_freeze': True,
            'source_hashes_match': True, 'source_file_count': len(lock['source_files']),
            'complete_schedules': True, 'fixture_and_runtime_identities_match': True,
            'reporter_measurement_checks_passed': True, 'legs': legs}


def export(source, destination, checked):
    require(not destination.exists() or not any(destination.iterdir()), 'Destination must be empty')
    destination.mkdir(parents=True, exist_ok=True)
    for name in ('protocol-lock.json', 'preflight.json', 'campaign.json'):
        if (source / name).is_file():
            shutil.copy2(source / name, destination / name)
    for leg in checked['legs']:
        origin, target = source / leg, destination / leg
        target.mkdir()
        for name in ('environment.json', 'results.json'):
            shutil.copy2(origin / name, target / name)
        evidence = {}
        for row in read(origin / 'results.json'):
            old, new = origin / row['run_id'], target / row['run_id']
            new.mkdir()
            shutil.copy2(old / 'result.json', new / 'result.json')
            for name in ('workspace.diff', 'workspace-status.txt', 'verification.stdout', 'verification.stderr'):
                if (old / name).is_file():
                    shutil.copy2(old / name, new / name)
            if not row['passed']:
                failed = new / 'failed-workspace'
                failed.mkdir()
                sources = {}
                for name in row['fixture_files']:
                    relative = Path(name)
                    require(not relative.is_absolute() and '..' not in relative.parts,
                            f'Unsafe fixture path: {name}')
                    original = old / 'failed-workspace' / relative
                    if original.is_file():
                        saved = failed / relative
                        saved.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(original, saved)
                        sources[name] = digest(saved)
                    else:
                        sources[name] = 'missing'
                evidence[row['run_id']] = {'fixture_sources': sources,
                                           'full_workspace': str((old / 'failed-workspace').resolve())}
        write_json(target / 'failure-evidence.json', evidence)
    if (source / 'report').is_dir():
        shutil.copytree(source / 'report', destination / 'analysis')
    write_json(destination / 'audit.json', checked)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    checked = audit(args.source, args.source_root)
    export(args.source, args.output, checked)
    print(json.dumps(checked, indent=2))


if __name__ == '__main__':
    main()
