"""Execute frozen acceptance gates serially and retain every scheduled attempt.

Only the explicit run-g0 and run-gate commands execute checks/model work. Freeze,
report and attach-g0 are provenance operations. No model is downloaded.
"""
import argparse
import copy
import datetime
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET
import zipfile

import agent_loop_acceptance as acceptance
from common import materialize, runtime_bundle, verify_task
from repair_control import source_identity
from run import VARIANTS


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    temporary.replace(path)


def reference(path):
    return {'path': str(Path(path).resolve()), 'sha256': acceptance.file_digest(path)}


def archive_files(directory, paths, prefix):
    """Archive an explicit member mapping and verify its inventory immediately."""
    inventory = {}
    archive = directory / (prefix + '.zip')
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as bundle:
        for name, path in sorted(paths.items()):
            bundle.write(path, name)
            inventory[name] = {'sha256': acceptance.file_digest(path), 'bytes': path.stat().st_size}
    manifest = directory / (prefix + '-inventory.json')
    write(manifest, {'files': inventory})
    acceptance.check_archive(directory, reference(archive), reference(manifest))
    return reference(archive), reference(manifest)


def environment(root):
    result = os.environ.copy()
    result['PYTHONPATH'] = str(root / '.tools') + os.pathsep + result.get('PYTHONPATH', '')
    result['PATH'] = str(root / '.tools/go/bin') + os.pathsep + result.get('PATH', '')
    return result


def run_process(command, root, log, timeout):
    """Bound the complete child tree so a timed-out harness cannot keep inferring."""
    process = subprocess.Popen(command, cwd=root, env=environment(root), stdout=log,
        stderr=subprocess.STDOUT, start_new_session=os.name != 'nt',
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           capture_output=True, timeout=30, check=False,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        else:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=30)
        return 124


def freeze(args):
    root, output = args.root.resolve(), args.output.resolve()
    acceptance.require(not output.exists(), 'candidate directory already exists; use a new candidate identity')
    contract = acceptance.read_json(args.contract)
    acceptance.validate_contract(contract, root)
    acceptance.require('loop-repair' in VARIANTS, 'loop-repair variant is unavailable')
    model_hash = acceptance.file_digest(args.model)
    acceptance.require(model_hash == contract['model']['sha256'], 'model differs from preregistration')
    source, diff = source_identity(root, {})
    runtime = runtime_bundle(args.forge)
    output.mkdir(parents=True)
    write(output / 'contract.json', contract)
    write(output / 'source.json', source)
    (output / 'source.diff').write_bytes(diff)
    source_archive, source_inventory = archive_files(output, {
        name: root / name for name, entry in source['files'].items() if entry is not None}, 'source')
    runtime_dir = output / 'runtime'
    runtime_dir.mkdir()
    for name in runtime['files']:
        shutil.copy2(args.forge.resolve().parent / name, runtime_dir / name)
    write(output / 'runtime.json', runtime)
    policies = {name: {'candidate_count': 1, 'variant': 'loop-repair',
                       'flags': VARIANTS['loop-repair']['flags']}
                for name in contract['profiles']}
    probe_binaries = {}
    for check in contract['g0_checks']:
        if 'command' in check:
            executable = root / check['command'][0]
            acceptance.require(executable.is_file(), 'required G0 executable is missing: ' + str(executable))
            retained = output / 'g0-binaries' / executable.name
            retained.parent.mkdir(exist_ok=True)
            shutil.copy2(executable, retained)
            probe_binaries[check['command'][0]] = reference(retained)
    g0_directory = root / 'build-gpu/Release'
    g0_files = {}
    for executable in sorted(g0_directory.iterdir()):
        if executable.suffix.lower() not in ('.exe', '.dll') or not executable.is_file():
            continue
        retained = runtime_dir / executable.name
        if not retained.exists():
            retained = output / 'g0-binaries' / executable.name
            shutil.copy2(executable, retained)
        g0_files[executable.name] = reference(retained)
    candidate = {
        'schema_version': 2, 'verification_policy': copy.deepcopy(acceptance.VERIFICATION_POLICY),
        'candidate_id': args.candidate_id, 'contract_sha256': contract['contract_sha256'],
        'frozen_utc': now(), 'source_root': str(root), 'model_path': str(args.model.resolve()),
        'runtime_directory': str(runtime_dir), 'source_manifest': reference(output / 'source.json'),
        'source_archive': source_archive, 'source_inventory': source_inventory,
        'runtime_manifest': reference(output / 'runtime.json'), 'profile_policies': policies,
        'g0_binaries': probe_binaries,
        'g0_runtime': {'directory': str(g0_directory), 'files': g0_files},
        'identity': {'source_sha256': source['files_sha256'], 'runtime_sha256': runtime['sha256'],
                     'model_sha256': model_hash, 'configuration_sha256': acceptance.configuration_digest(
                         contract['profiles'], policies, acceptance.VERIFICATION_POLICY)}}
    candidate['candidate_sha256'] = acceptance.json_digest(candidate)
    write(output / 'candidate.json', candidate)
    write(output / 'outcomes.json', [])
    write(output / 'g0.json', [])
    acceptance.validate_candidate(contract, candidate, root)
    make_report(output)
    print(json.dumps({'candidate': str(output / 'candidate.json'),
                      'candidate_sha256': candidate['candidate_sha256']}))


def load(directory):
    directory = directory.resolve()
    return (acceptance.read_json(directory / 'contract.json'),
            acceptance.read_json(directory / 'candidate.json'))


def observe(candidate):
    source, _ = source_identity(Path(candidate['source_root']), {})
    runtime = runtime_bundle(Path(candidate['runtime_directory']) / 'forge.exe')
    return {'source_sha256': source['files_sha256'], 'runtime_sha256': runtime['sha256'],
            'model_sha256': acceptance.file_digest(candidate['model_path']),
            'configuration_sha256': candidate['identity']['configuration_sha256']}


def observe_g0(candidate):
    runtime = candidate['g0_runtime']
    root = Path(runtime['directory'])
    names = {path.name for path in root.iterdir()
             if path.is_file() and path.suffix.lower() in ('.exe', '.dll')}
    acceptance.require(names == set(runtime['files']), 'G0 executable/runtime membership changed')
    hashes = {name: acceptance.file_digest(root / name) for name in sorted(names)}
    acceptance.require(hashes == {name: value['sha256'] for name, value in runtime['files'].items()},
                       'G0 executable/runtime changed since freeze')
    return hashes


def bind(candidate, run_id, before, started, finished):
    return {'run_id': run_id, 'execution_id': str(uuid.uuid4()),
            'candidate_id': candidate['candidate_id'], 'candidate_sha256': candidate['candidate_sha256'],
            'identity_before': before, 'identity_after': observe(candidate),
            'started_utc': started, 'finished_utc': finished}


def make_report(directory):
    contract, candidate = load(directory)
    result = acceptance.report(contract, candidate,
        acceptance.read_json(directory / 'outcomes.json'), acceptance.read_json(directory / 'g0.json'),
        Path(candidate['source_root']))
    write(directory / 'acceptance-report.json', result)
    return result


def append_record(directory, filename, record):
    records = acceptance.read_json(directory / filename)
    acceptance.require(not any(r['run_id'] == record['run_id'] for r in records),
                       'scheduled run already has a retained outcome')
    records.append(record)
    write(directory / filename, records)


def g0_summary(check, code, log, junit=None):
    result = {'returncode': code, 'status': 'passed' if code == 0 else 'failed',
              'log': reference(log)}
    if check['check_id'] == 'full-gpu-ctest':
        acceptance.require(junit is not None and junit.is_file(), 'full CTest JUnit report is required')
        tree = ET.parse(junit)
        result['tests'] = [{'name': test.attrib['name'], 'status':
                            'skipped' if test.find('skipped') is not None else
                            'failed' if test.find('failure') is not None or test.find('error') is not None
                            else 'passed'} for test in tree.iter('testcase')]
        result['junit'] = reference(junit)
    elif check['check_id'] == 'new-deterministic-regressions':
        text = log.read_text(encoding='utf-8', errors='replace')
        passed = code == 0 and '\nOK' in text and 'Ran ' in text
        result['tests'] = [{'name': name, 'status': 'passed' if passed else 'failed'}
                           for name in check['required_tests']]
    else:
        result['command'] = check['command']
    return result


def attach_g0(args):
    directory = args.candidate_dir.resolve()
    contract, candidate = load(directory)
    check = next(c for c in contract['g0_checks'] if c['check_id'] == args.check_id)
    target = directory / 'g0' / args.check_id
    acceptance.require(not target.exists(), 'G0 evidence already exists; do not replace attempts')
    before = observe(candidate)
    acceptance.require(before == candidate['identity'], 'candidate changed since freeze')
    g0_before = observe_g0(candidate)
    target.mkdir(parents=True)
    shutil.copy2(args.log, target / 'check.log')
    junit = None
    if args.junit:
        junit = target / 'ctest.xml'
        shutil.copy2(args.junit, junit)
    record = bind(candidate, 'G0-' + args.check_id, before, args.started_utc, args.finished_utc)
    summary = {**record, **g0_summary(check, args.exit_code, target / 'check.log', junit)}
    summary.update(g0_runtime_before=g0_before, g0_runtime_after=observe_g0(candidate))
    if 'command' in check:
        summary['binary_sha256'] = acceptance.file_digest(Path(candidate['source_root']) / check['command'][0])
    write(target / 'evidence.json', summary)
    record['evidence'] = reference(target / 'evidence.json')
    append_record(directory, 'g0.json', record)
    make_report(directory)


def run_g0(args):
    directory = args.candidate_dir.resolve()
    contract, candidate = load(directory)
    root = Path(candidate['source_root'])
    for check in contract['g0_checks']:
        if args.check_id and args.check_id != check['check_id']:
            continue
        target = directory / 'g0' / check['check_id']
        acceptance.require(not target.exists(), 'G0 already attempted; retain the result and use a new candidate')
        before = observe(candidate)
        acceptance.require(before == candidate['identity'], 'candidate changed since freeze')
        g0_before = observe_g0(candidate)
        target.mkdir(parents=True)
        junit = None
        if check['check_id'] == 'full-gpu-ctest':
            junit = target / 'ctest.xml'
            command = [str(root / '.tools/bin/ctest.exe'), '--test-dir', 'build-gpu', '-C', 'Release',
                       '--output-on-failure', '--output-junit', str(junit)]
        elif check['check_id'] == 'new-deterministic-regressions':
            command = [sys.executable, 'tests/unit/test_agent_loop_acceptance.py']
        else:
            command = check['command']
            expected = candidate['g0_binaries'][command[0]]['sha256']
            acceptance.require(acceptance.file_digest(root / command[0]) == expected,
                               'direct probe executable changed since freeze')
        started = now()
        write(target / 'started.json', {'started_utc': started, 'identity': before, 'command': command})
        with (target / 'check.log').open('w', encoding='utf-8') as log:
            code = run_process(command, root, log, 1200)
        finished = now()
        record = bind(candidate, 'G0-' + check['check_id'], before, started, finished)
        try:
            summary = {**record, **g0_summary(check, code, target / 'check.log', junit)}
        except (OSError, ValueError, ET.ParseError) as exc:
            summary = {**record, 'returncode': code, 'status': 'incomplete-evidence',
                       'evidence_error': str(exc), 'log': reference(target / 'check.log')}
        if 'command' in check:
            summary['binary_sha256'] = acceptance.file_digest(root / check['command'][0])
        try:
            summary.update(g0_runtime_before=g0_before, g0_runtime_after=observe_g0(candidate))
        except (OSError, ValueError) as exc:
            summary.update(status='incomplete-evidence', evidence_error=str(exc))
        write(target / 'evidence.json', summary)
        record['evidence'] = reference(target / 'evidence.json')
        append_record(directory, 'g0.json', record)
        make_report(directory)
        print(check['check_id'] + ': ' + summary['status'], flush=True)
    final_report = make_report(directory)
    if args.check_id:
        state = final_report['runs'].get('G0-' + args.check_id, {}).get('status')
        return 0 if state == 'passed' else 1
    return 0 if final_report['gates']['G0']['accepted'] else 1


def preflight(directory, root, row, fixture, timeout):
    target = directory / 'preflight' / fixture['manifest_sha256']
    if (target / 'preflight.json').exists():
        return target / 'preflight.json'
    target.mkdir(parents=True)
    task = acceptance.read_json(root / row['manifest'])
    results = {}
    for label in ('baseline', 'oracle'):
        prepared_task = copy.deepcopy(task)
        if label == 'oracle':
            acceptance.require(bool(task.get('oracle_files')), 'fixture lacks a preflight oracle')
            prepared_task['files'].update(task['oracle_files'])
        with tempfile.TemporaryDirectory(prefix='forge-acceptance-preflight-') as tmp:
            prepared = materialize(Path(tmp), prepared_task)
            if label == 'baseline':
                acceptance.require(prepared['fixture_sha256'] == fixture['fixture_sha256'],
                                   'fixture materialization differs from preregistration')
            (target / label).mkdir()
            result = verify_task(Path(tmp), prepared_task, target / label,
                                 env=environment(root), timeout=timeout)
            for stream in ('stdout', 'stderr'):
                shutil.copyfile(target / label / ('verification.' + stream), target / (label + '.' + stream))
            results[label] = result
    count = acceptance.test_count(task.get('language', 'go'),
                                  (target / 'oracle.stdout').read_text(encoding='utf-8') + '\n' +
                                  (target / 'oracle.stderr').read_text(encoding='utf-8'))
    summary = {'fixture_sha256': fixture['fixture_sha256'],
               'baseline_failed': results['baseline']['returncode'] != 0,
               'oracle_passed': results['oracle']['returncode'] == 0,
               'oracle_test_count': count, 'command': task['verify'],
               'verification': {label: {'returncode': result['returncode'],
                   'python_cache_policy': result.get('python_cache_policy')} for label, result in results.items()}}
    acceptance.require(summary['baseline_failed'] and summary['oracle_passed'] and count > 0,
                       'fixture preflight failed; no model attempt launched')
    write(target / 'preflight.json', summary)
    return target / 'preflight.json'


def command_for(candidate, row, profile, root, output):
    command = [sys.executable, str(root / 'benchmark/run.py'), '--forge',
               str(Path(candidate['runtime_directory']) / 'forge.exe'), '--model', candidate['model_path'],
               '--task-dir', str((root / row['manifest']).parent), '--tasks', row['task'],
               '--suite', 'all', '--variants', candidate['profile_policies'][row['profile']]['variant'],
               '--retain-terminal', '--output', str(output), '--repetitions', '1', '--no-randomize']
    flags = {'gpu_layers': '--gpu-layers', 'context': '--context', 'output_reserve': '--output-reserve',
             'temperature': '--temperature', 'max_turns': '--max-turns', 'max_tokens': '--max-tokens',
             'max_input': '--max-input', 'timeout': '--timeout',
             'verification_timeout': '--verification-timeout', 'order_seed': '--order-seed',
             'gpu_index': '--gpu-index', 'prompt_protocol': '--prompt-protocol'}
    for name, flag in flags.items():
        command += [flag + '=' + str(profile[name])]
    command += ['--seed', str(row['seed'])]
    return command


def package_run(directory, target, row, contract, candidate, before, started, finished, preflight_path):
    record = bind(candidate, row['run_id'], before, started, finished)
    fixture, profile = contract['fixtures'][row['manifest']], contract['profiles'][row['profile']]
    record.update(schedule=row, fixture_sha256=fixture['fixture_sha256'],
                  manifest_sha256=fixture['manifest_sha256'], settings={**profile, 'seed': row['seed']})
    if candidate.get('verification_policy') is not None:
        record['verification_policy'] = candidate['verification_policy']
    variant = candidate['profile_policies'][row['profile']]['variant']
    output = target / 'harness' / f'{row["task"]}-{variant}-r001'
    harness = acceptance.read_json(output / 'result.json')
    observed = acceptance.read_json(target / 'harness/environment.json')
    acceptance.require(observed['forge_runtime_bundle']['sha256'] == candidate['identity']['runtime_sha256']
                       and observed['model_sha256'] == candidate['identity']['model_sha256'],
                       'harness observed another runtime or model')
    expected_settings = {key: profile[key] for key in ('output_reserve', 'max_turns', 'max_tokens',
        'max_input', 'temperature', 'prompt_protocol', 'lifecycle', 'gpu_index', 'order_seed')}
    expected_settings.update(context_tokens=profile['context'], gpu_layers=str(profile['gpu_layers']),
                             chat_template=profile['chat_template'], seed=row['seed'])
    acceptance.require(all(observed.get(key) == value for key, value in expected_settings.items()),
                       'harness observed another profile, seed or budget')
    terminal_root = output / 'terminal-workspace'
    files = {path.relative_to(terminal_root).as_posix(): acceptance.file_digest(path)
             for path in terminal_root.rglob('*') if path.is_file()}
    protected_ok = harness.get('protected_files_unchanged') is True and \
                   harness.get('protected_before_verification') is True
    record['protected_files_unchanged'] = protected_ok
    execution = {**record, 'returncode': harness['returncode'],
                 'agent_completed': harness['returncode'] == 0 and
                 harness.get('metrics', {}).get('status') == 'ok',
                 'terminal_loop': harness['returncode'] != 0 and
                 bool(harness.get('metrics', {}).get('loop_warnings', 0))}
    verification = harness['verification']
    output_text = (output / 'verification.stdout').read_text(encoding='utf-8') + '\n' + \
                  (output / 'verification.stderr').read_text(encoding='utf-8')
    try:
        count = acceptance.test_count(harness.get('language', 'go'), output_text)
    except acceptance.EvidenceError:
        count = 0
    metrics = {**harness.get('metrics', {}), 'agent_seconds': harness['timing']['agent_seconds'],
               'verification_seconds': harness['timing']['verification_seconds']}
    terminal = {'files': files, 'syntax_valid': None}
    if row['population'].startswith('G3'):
        if harness.get('language') == 'python':
            try:
                for name in files:
                    if name.endswith('.py'):
                        compile((terminal_root / name).read_text(encoding='utf-8'), name, 'exec')
                terminal['syntax_valid'] = True
            except (SyntaxError, UnicodeError):
                terminal['syntax_valid'] = False
        else:
            go_files = [str(terminal_root / name) for name in files if name.endswith('.go')]
            syntax = subprocess.run([str(Path(candidate['source_root']) / '.tools/go/bin/gofmt.exe'),
                                     '-e', *go_files], capture_output=True, timeout=30, check=False)
            terminal['syntax_valid'] = syntax.returncode == 0
    values = {'execution': execution, 'command': {'argv': acceptance.read_json(output / 'command.json'),
              'harness_argv': acceptance.read_json(target / 'command.json'), 'settings': record['settings']},
              'preflight': acceptance.read_json(preflight_path), 'metrics': metrics, 'terminal': terminal,
              'validation': {'command': fixture['verify'], 'returncode': verification['returncode'],
                             'complete': verification['returncode'] == 0 and count > 0 and
                             harness.get('verification_inputs_unchanged') is True,
                             'terminal_sha256': acceptance.json_digest(files), 'test_count': count}}
    evidence = {}
    archive_paths = {'terminal/' + name: terminal_root / name for name in files}
    if candidate.get('verification_policy') is not None:
        pre_root = output / 'pre-verification-workspace'
        acceptance.require(pre_root.is_dir(), 'missing pre-verification workspace')
        before_files = {}
        for path in pre_root.rglob('*'):
            acceptance.require(not path.is_symlink(), 'symlink in retained verification workspace')
            if path.is_file():
                before_files[path.relative_to(pre_root).as_posix()] = acceptance.file_digest(path)
        for path in terminal_root.rglob('*'):
            acceptance.require(not path.is_symlink(), 'symlink in retained terminal workspace')
        values['pre_verification'] = {'files': before_files}
        values['validation'].update(
            python_cache_policy=verification.get('python_cache_policy'),
            terminal_retention_policy=harness.get('terminal_retention_policy'),
            verification_inputs_unchanged=harness.get('verification_inputs_unchanged'))
        archive_paths.update({'pre-verification/' + name: pre_root / name for name in before_files})
    for role, value in values.items():
        path = target / 'envelope' / (role + '.json')
        write(path, value)
        evidence[role] = {**reference(path), 'member': 'envelope/' + role + '.json'}
        archive_paths[evidence[role]['member']] = path
    roles = {'outputs': output / 'stdout.jsonl', 'verification_stdout': output / 'verification.stdout',
             'verification_stderr': output / 'verification.stderr', 'journal': output / 'session/events.jsonl'}
    if candidate.get('verification_policy') is not None:
        roles.update(verification_inputs=output / 'verification-inputs.json', harness_result=output / 'result.json')
    # The manifest enumerates every raw prompt; actual files are retained below.
    prompts = sorted((output / 'session/context').glob('*.txt'))
    prompt_manifest = target / 'envelope/prompts.json'
    write(prompt_manifest, {'files': [reference(path) for path in prompts]})
    roles['prompts'] = prompt_manifest
    for role, path in roles.items():
        if path.is_file():
            member = 'evidence/' + role
            evidence[role] = {**reference(path), 'member': member}
            archive_paths[member] = path
    for path in target.rglob('*'):
        if path.is_file() and path.name not in ('evidence.zip', 'evidence-inventory.json', 'outcome.json'):
            archive_paths['raw/' + path.relative_to(target).as_posix()] = path
    for path in preflight_path.parent.rglob('*'):
        if path.is_file():
            archive_paths['preflight/' + path.relative_to(preflight_path.parent).as_posix()] = path
    record['evidence'] = evidence
    record['archive'], record['inventory'] = archive_files(target, archive_paths, 'evidence')
    return record


def run_gate(args):
    directory = args.candidate_dir.resolve()
    contract, candidate = load(directory)
    root = Path(candidate['source_root'])
    # Fixture formatting uses the same explicitly selected Go toolchain as runs.
    os.environ.update(environment(root))
    previous = {'G1': 'G0', 'G2': 'G1', 'G3': 'G2', 'G4': 'G3', 'G5': 'G4', 'G6': 'G5'}[args.gate]
    current = make_report(directory)
    acceptance.require(current['gates'][previous]['accepted'], 'preceding gate has not passed: ' + previous)
    existing = acceptance.read_json(directory / 'outcomes.json')
    rows = [row for row in contract['schedule'] if row['gate'] == args.gate]
    acceptance.require(not any(record['run_id'] in {row['run_id'] for row in rows} for record in existing),
                       'gate already has outcomes; no replacement or result-based retry is permitted')
    if args.gate == 'G6':
        candidate['confirmation_frozen_utc'] = now()
        write(directory / 'candidate.json', candidate)
        write(directory / 'confirmation-freeze.json', {
            'candidate_sha256': candidate['candidate_sha256'], 'identity': candidate['identity'],
            'frozen_utc': candidate['confirmation_frozen_utc']})
    prepared = {}
    for row in rows:
        if row['manifest'] not in prepared:
            prepared[row['manifest']] = preflight(directory, root, row,
                contract['fixtures'][row['manifest']], contract['profiles'][row['profile']]['verification_timeout'])
    for row in rows:
        target = directory / 'runs' / row['run_id']
        acceptance.require(not target.exists(), 'scheduled run already started; retained attempt cannot be replaced')
        before = observe(candidate)
        acceptance.require(before == candidate['identity'], 'candidate changed since freeze')
        target.mkdir(parents=True)
        profile = contract['profiles'][row['profile']]
        command = command_for(candidate, row, profile, root, target / 'harness')
        write(target / 'command.json', command)
        started = now()
        write(target / 'started.json', {'schedule': row, 'identity_before': before, 'started_utc': started})
        with (target / 'harness.log').open('w', encoding='utf-8') as log:
            harness_exit = run_process(command, root, log,
                profile['timeout'] + profile['verification_timeout'] + 180)
        finished = now()
        try:
            record = package_run(directory, target, row, contract, candidate, before, started, finished,
                                 prepared[row['manifest']])
        except (OSError, KeyError, ValueError, subprocess.SubprocessError) as exc:
            record = bind(candidate, row['run_id'], before, started, finished)
            record.update(schedule=row, harness_exit=harness_exit, evidence_error=str(exc),
                          status='incomplete-evidence')
        write(target / 'outcome.json', record)
        append_record(directory, 'outcomes.json', record)
        current = make_report(directory)
        print(row['run_id'] + ': ' + current['runs'][row['run_id']]['status'], flush=True)
    return 0 if current['gates'][args.gate]['accepted'] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    freezing = sub.add_parser('freeze')
    freezing.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    for name in ('contract', 'forge', 'model', 'output'):
        freezing.add_argument('--' + name, type=Path, required=True)
    freezing.add_argument('--candidate-id', required=True)
    for action in ('run-g0', 'attach-g0', 'run-gate', 'report'):
        command = sub.add_parser(action)
        command.add_argument('--candidate-dir', type=Path, required=True)
        if action in ('run-g0', 'attach-g0'):
            command.add_argument('--check-id', required=action == 'attach-g0')
        if action == 'attach-g0':
            command.add_argument('--log', type=Path, required=True)
            command.add_argument('--junit', type=Path)
            command.add_argument('--exit-code', type=int, required=True)
            command.add_argument('--started-utc', required=True)
            command.add_argument('--finished-utc', required=True)
        if action == 'run-gate':
            command.add_argument('--gate', choices=['G1', 'G2', 'G3', 'G4', 'G5', 'G6'], required=True)
    args = parser.parse_args(argv)
    if args.action == 'freeze':
        freeze(args)
    elif args.action == 'run-g0':
        return run_g0(args)
    elif args.action == 'attach-g0':
        attach_g0(args)
    elif args.action == 'run-gate':
        return run_gate(args)
    else:
        print(json.dumps(make_report(args.candidate_dir.resolve())['gates'], indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
