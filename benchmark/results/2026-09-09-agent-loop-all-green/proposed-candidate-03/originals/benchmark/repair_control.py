"""Preregister and run frozen, interleaved repair diagnostics.

Every task in --task-dir is used, including mixed suites. Each scheduled cell
gets one cold run.py invocation and a retained terminal outcome, even if that
harness crashes. Resume never retries an interrupted or failed cell. This is a
diagnostic comparison, not a correctness, latency, or promotion gate.
"""
import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import random
import signal
import statistics
import subprocess
import sys
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (CREATE_NO_WINDOW, FIXTURE_PREPARATION, check_tools, digest,
                    load_tasks, platform_metadata, runtime_bundle)
from freeze import source_identity as benchmark_source_identity
from run import VARIANTS as RUN_VARIANTS

ARMS = ('checkpoint', 'current', 'minimal')
LOOP_ARMS = {'minimal': 'minimal', 'candidate': 'candidate-checkpoint',
             'best-of-2': 'loop-best-of-2', 'semantic': 'loop-semantic',
             'impact': 'loop-impact', 'reflection': 'loop-reflection',
             'combined': 'loop-combined'}
DIRECTORY = Path(__file__).resolve().parent
HISTORICAL_NOTE = ('The historical checkpoint predates native decoding fixes and has different '
                   'thinking/tool-choice defaults despite identical CLI settings. Its comparison '
                   'is descriptive of the whole system; current/minimal share the current decoder.')


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(temporary, path)


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


@contextmanager
def exclusive_output(output, resume):
    """OS locks release on process exit; a stale file alone cannot block resume."""
    output.mkdir(parents=True, exist_ok=True)
    with (output / '.lock').open('a+b') as lock:
        if os.fstat(lock.fileno()).st_size == 0:
            lock.write(b'0')
            lock.flush()
        lock.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError('Another comparison owns this output directory') from error
        try:
            existing = [path for path in output.iterdir() if path.name != '.lock']
            if existing and not resume:
                raise ValueError('Output is not empty; use --resume with the identical protocol')
            if resume and not (output / 'protocol.json').is_file():
                raise ValueError('Cannot resume without an existing protocol.json')
            yield
        finally:
            lock.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_UN)


def arm_variants(args):
    if getattr(args, 'experiment', 'repair-control') == 'loop-completion':
        return dict(LOOP_ARMS)
    if getattr(args, 'experiment', 'repair-control') == 'candidate-checkpoint':
        return {'candidate': 'candidate-checkpoint', 'minimal': 'minimal'}
    return {arm: 'minimal' if arm == 'minimal' else 'optimized' for arm in ARMS}


def arm_runtimes(args):
    if getattr(args, 'experiment', 'repair-control') == 'loop-completion':
        return {arm: args.forge for arm in arm_variants(args)}
    return {arm: getattr(args, arm + '_forge') for arm in arm_variants(args)}


def validate_runtime_identity(args, identity):
    experiment = getattr(args, 'experiment', 'repair-control')
    if experiment not in ('candidate-checkpoint', 'loop-completion'):
        return
    bundles = [identity['runtimes'][arm]['bundle'] for arm in arm_variants(args)]
    if any(bundle != bundles[0] for bundle in bundles[1:]):
        if experiment == 'candidate-checkpoint':
            raise ValueError('Candidate checkpoints require identical runtime bundles in both arms')
        raise ValueError('Loop completion requires identical runtime bundles in all seven arms')


def make_schedule(task_ids, repetitions, seed, arm_names=ARMS):
    """All arms see the same shuffled task/repetition sequence, in adjacent blocks."""
    rng = random.Random(seed)
    result = []
    for repetition in range(1, repetitions + 1):
        order = sorted(task_ids)
        rng.shuffle(order)
        for task in order:
            arms = list(arm_names)
            rng.shuffle(arms)
            for arm in arms:
                result.append({'cell_id': f'{task}-{arm}-r{repetition:03d}',
                               'task': task, 'arm': arm, 'repetition': repetition,
                               'order_index': len(result) + 1})
    return result


def git_bytes(root, *arguments):
    return subprocess.check_output(['git', '-C', str(root), *arguments],
                                   stderr=subprocess.PIPE, timeout=60)


def source_identity(root, revisions):
    names = git_bytes(root, 'ls-files', '-z', '--cached', '--others', '--exclude-standard')
    files = benchmark_source_identity(root)
    for name in sorted(set(names.decode('utf-8').split('\0')) - {''}):
        if not name.startswith(('src/', 'include/', 'cmake/', 'tests/', 'scripts/', '.github/')):
            continue
        path = root / name
        files[name] = {'sha256': digest(path), 'bytes': path.stat().st_size} if path.is_file() else None
    diff = git_bytes(root, 'diff', '--binary', 'HEAD', '--', 'src', 'include', 'cmake',
                     'tests', 'scripts', '.github', 'CMakeLists.txt', 'benchmark/*.py', 'benchmark/*.md')
    return {'path': str(root),
            'head': git_bytes(root, 'rev-parse', 'HEAD').decode().strip(),
            'files': files, 'files_sha256': json_digest(files),
            'diff_sha256': hashlib.sha256(diff).hexdigest(),
            'revisions': {arm: {
                'commit': git_bytes(root, 'rev-parse', revision + '^{commit}').decode().strip(),
                'tree': git_bytes(root, 'rev-parse', revision + '^{tree}').decode().strip(),
            } for arm, revision in revisions.items()}}, diff


def collect_identity(args, tasks):
    revisions = ({arm: 'HEAD' for arm in arm_variants(args)}
                 if getattr(args, 'experiment', 'repair-control') in ('candidate-checkpoint', 'loop-completion')
                 else {'checkpoint': args.checkpoint_revision, 'current': args.current_revision})
    source, diff = source_identity(args.source_dir, revisions)
    identity = {'source': source,
                'model': {'path': str(args.model), 'bytes': args.model.stat().st_size,
                          'sha256': digest(args.model)},
                'tasks': {task['id']: {'path': str(path), 'sha256': digest(path),
                                      'has_oracle': bool(task.get('oracle_files'))}
                          for path, task in tasks},
                'platform': platform_metadata(),
                'harness': {name: digest(DIRECTORY / name) for name in
                            ('repair_control.py', 'run.py', 'common.py', 'preflight.py')},
                'runtimes': {arm: {'path': str(path), 'bundle': runtime_bundle(path)}
                             for arm, path in arm_runtimes(args).items()}}
    return identity, diff


def make_protocol(args, tasks, identity):
    variants = arm_variants(args)
    candidate = 'candidate' in variants
    loop = getattr(args, 'experiment', 'repair-control') == 'loop-completion'
    settings = {key: getattr(args, key) for key in (
        'gpu_layers', 'context', 'output_reserve', 'temperature', 'seed',
        'max_turns', 'max_tokens', 'max_input', 'timeout', 'verification_timeout',
        'repetitions', 'order_seed', 'gpu_index')}
    settings.update(prompt_protocol='native', chat_template='embedded', lifecycle='cold',
                    fixture_preparation=FIXTURE_PREPARATION)
    experiment = ('loop-completion-diagnostic' if loop else
                  'candidate-checkpoint-diagnostic' if candidate else 'repair-control-diagnostic')
    protocol = {'schema_version': 1, 'experiment': experiment,
            'identity': identity, 'settings': settings,
            'arms': {arm: {'variant': variant} for arm, variant in variants.items()},
            'schedule': make_schedule([task['id'] for _, task in tasks],
                                      args.repetitions, args.order_seed, tuple(variants)),
            'source_association': ('Both arms use the same runtime bundle and the retained current '
                                  'working-tree source snapshot.' if candidate else
                                  'Historical revision-to-runtime associations are supplied by '
                                  'the operator; hashes freeze those declared identities. '
                                  'Minimal records the current working tree, including untracked files.'),
            'historical_confound': ('Same-binary minimal versus opt-in candidate checkpoints; '
                                    'no historical arm.' if candidate else HISTORICAL_NOTE),
            'policy': 'All outcomes count; no retry, candidate selection, or promotion claim. '
                      'Each cell has the same model settings and per-task limits. '
                      'Harness/model hashing is outside recorded agent end-to-end time.'}
    if loop:
        protocol['arms'] = {arm: {'variant': variant, 'flags': list(RUN_VARIANTS[variant]['flags'])}
                            for arm, variant in variants.items()}
        protocol['source_association'] = (
            'All seven arms use one executable and identical runtime bundles, associated with '
            'the retained current working-tree source snapshot.')
        protocol['historical_confound'] = (
            'No historical binary arm. Candidate checkpoints are the intervention baseline. '
            'These examined development fixtures and one repetition cannot establish causality '
            'or general superiority; do not pool earlier zero-temperature measurements.')
        protocol['policy'] = (
            'All outcomes count; no harness retries, result-based exclusion, tuning during the '
            'matrix, or promotion claim. Best-of-2 and combined perform the preregistered '
            'in-agent real-workspace selection; all other arms generate one trajectory. '
            'Every cell shares identical total action, generated-token, cumulative-input-token '
            'and wall-time limits across its candidates, not a fresh budget per candidate. '
            'The independent verifier judges the terminal workspace. Harness/model hashing is '
            'outside recorded agent end-to-end time.')
        protocol['settings']['candidate_budget_scope'] = 'shared_per_task_across_all_candidates'
        protocol['settings']['candidate_counts'] = {
            arm: 2 if arm in ('best-of-2', 'combined') else 1 for arm in variants}
        protocol['settings']['candidate_seed_rule'] = 'uint32(seed + zero_based_candidate_index * 0x9e3779b9)'
    return protocol


def frozen_stamps(identity):
    """Cheap between-cell guard; independent full hashes still run before and after."""
    paths = {Path(identity['model']['path'])}
    paths.update(Path(value['path']) for value in identity['tasks'].values())
    paths.update(Path(identity['source']['path']) / name for name in identity['source']['files'])
    paths.update(DIRECTORY / name for name in identity['harness'])
    for runtime in identity['runtimes'].values():
        executable = Path(runtime['path'])
        paths.add(executable.parent)
        paths.update(executable.parent / name for name in runtime['bundle']['files'])
    return {str(path): (path.stat().st_size, path.stat().st_mtime_ns, path.stat().st_ctime_ns)
            if path.exists() else None for path in paths}


def validate_resume(saved, proposed):
    if saved != proposed:
        raise ValueError('Resume protocol mismatch: settings, source, model, tasks, harness, '
                         'runtimes and schedule must all remain frozen')


def validate_source_evidence(output, identity):
    expected = {name: value['sha256'] for name, value in identity['source']['files'].items()
                if value is not None}
    if digest(output / 'minimal-source.diff') != identity['source']['diff_sha256']:
        raise ValueError('Retained source diff is missing or changed')
    with zipfile.ZipFile(output / 'minimal-source.zip') as archive:
        if Counter(archive.namelist()) != Counter(expected.keys()):
            raise ValueError('Retained source snapshot has missing, extra or duplicate members')
        if any(hashlib.sha256(archive.read(name)).hexdigest() != value for name, value in expected.items()):
            raise ValueError('Retained source snapshot differs from the frozen source')


def preflight(args, protocol):
    path = args.output / 'preflight.json'
    if not path.exists():
        if any((args.output / 'cells').glob('*/outcome.json')):
            raise ValueError('Missing preflight evidence after runs have started')
        command = [sys.executable, str(DIRECTORY / 'preflight.py'), '--suite', 'all',
                   '--task-dir', str(args.task_dir), '--timeout', str(args.verification_timeout),
                   '--output', str(path)]
        atomic_json(args.output / 'preflight-command.json', command)
        with (args.output / 'preflight.stdout.txt').open('w', encoding='utf-8') as out, \
                (args.output / 'preflight.stderr.txt').open('w', encoding='utf-8') as err:
            result = subprocess.run(command, stdout=out, stderr=err, check=False,
                                    creationflags=CREATE_NO_WINDOW)
        if result.returncode != 0:
            raise ValueError('Fixture preflight failed; all evidence retained and no inference started')
    report = read_json(path)
    expected = Counter(protocol['identity']['tasks'].keys())
    actual = Counter(row['task'] for row in report.get('records', []))
    if report.get('passed') is not True or actual != expected:
        raise ValueError('Preflight must pass for exactly the preregistered task population')
    for row in report['records']:
        if row.get('error') or row.get('broken_returncode') in (None, 0, 124):
            raise ValueError('Preflight does not prove a failing initial fixture')
        if row.get('oracle_returncode') not in (None, 0):
            raise ValueError('Preflight oracle failed')
        if protocol['identity']['tasks'][row['task']].get('has_oracle') and row.get('oracle_returncode') != 0:
            raise ValueError('Preflight did not verify the supplied oracle')
    return {row['task']: row for row in report['records']}


def cell_command(args, cell, output):
    command = [sys.executable, str(DIRECTORY / 'run.py'),
               '--forge', str(arm_runtimes(args)[cell['arm']]),
               '--model', str(args.model), '--task-dir', str(args.task_dir), '--suite', 'all',
               '--tasks', cell['task'], '--variants',
               arm_variants(args)[cell['arm']],
               '--output', str(output), '--prompt-protocol', 'native',
               '--repetitions', '1', '--no-randomize']
    for key in ('gpu_layers', 'context', 'output_reserve', 'temperature', 'seed',
                'max_turns', 'max_tokens', 'max_input', 'timeout', 'verification_timeout', 'order_seed', 'gpu_index'):
        command.extend(['--' + key.replace('_', '-'), str(getattr(args, key))])
    return command


def validate_record(cell, record, environment, protocol, prepared):
    settings, identity = protocol['settings'], protocol['identity']
    variant = protocol['arms'][cell['arm']]['variant']
    if record.get('task') != cell['task'] or record.get('variant') != variant or record.get('repetition') != 1:
        raise ValueError('Harness returned an unexpected task, variant or repetition')
    checks = {'model_sha256': identity['model']['sha256'],
              'forge_runtime_bundle': identity['runtimes'][cell['arm']]['bundle'],
              'gpu_layers': str(settings['gpu_layers']), 'context_tokens': settings['context'],
              'output_reserve': settings['output_reserve'], 'max_turns': settings['max_turns'],
              'max_tokens': settings['max_tokens'], 'max_input': settings['max_input'],
              'temperature': settings['temperature'], 'seed': settings['seed'],
              'prompt_protocol': 'native', 'chat_template': 'embedded', 'lifecycle': 'cold',
              'fixture_preparation': FIXTURE_PREPARATION}
    checks.update(identity.get('platform', {}))
    if any(environment.get(key) != value for key, value in checks.items()):
        raise ValueError('Harness model, runtime or settings differ from the frozen protocol')
    if record.get('fixture_sha256') != prepared[cell['task']]['fixture_sha256']:
        raise ValueError('Materialized fixture differs from the preflight fixture')
    if not isinstance(record.get('passed'), bool):
        raise ValueError('Harness result is missing its pass/fail outcome')
    if record['passed'] and (record.get('returncode') != 0 or record.get('protected_files_unchanged') is not True):
        raise ValueError('Harness pass contradicts process status or protected-file integrity')
    metrics = record.get('metrics', {})
    if metrics.get('simulated') is True or (record['passed'] and
            (metrics.get('simulated') is not False or metrics.get('generated_tokens', 0) < 1)):
        raise ValueError('Result does not demonstrate real inference')
    for metric, limit in (('turns', 'max_turns'), ('generated_tokens', 'max_tokens'), ('prompt_tokens', 'max_input')):
        if metrics.get(metric, 0) > settings[limit]:
            raise ValueError('Measured ' + metric + ' exceeds the preregistered budget')


def process_alive(pid):
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return ctypes.get_last_error() != 87  # Access denied is conservatively alive.
        code = wintypes.DWORD()
        try:
            return not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def stop_process(process):
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                       capture_output=True, check=False, creationflags=CREATE_NO_WINDOW)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def execute_cell(args, cell, protocol, prepared):
    output = args.output / 'cells' / cell['cell_id']
    outcome_path = output / 'outcome.json'
    if outcome_path.exists():
        outcome = read_json(outcome_path)
        if any(outcome.get(key) != value for key, value in cell.items()):
            raise ValueError('Saved outcome does not match its scheduled cell')
        if (outcome.get('status') not in ('completed', 'harness_error', 'interrupted') or
                not isinstance(outcome.get('passed'), bool)):
            raise ValueError('Saved outcome has an invalid status or pass/fail value')
        if outcome.get('status') == 'completed':
            validate_record(cell, outcome['record'], read_json(output / 'run' / 'environment.json'), protocol, prepared)
            if read_json(output / 'run' / 'results.json') != [outcome['record']]:
                raise ValueError('Saved outcome differs from retained harness evidence')
            if (outcome['passed'] != outcome['record']['passed'] or
                    outcome.get('harness_returncode') != (0 if outcome['passed'] else 1)):
                raise ValueError('Saved outcome contradicts its harness result')
            if outcome.get('trace_notes') != trace_notes(output / 'run', outcome['record'], protocol['settings']):
                raise ValueError('Saved trace notes differ from retained traces')
        elif outcome['passed']:
            raise ValueError('An incomplete or crashed harness cannot report a passing outcome')
        return outcome
    if output.exists():
        started_path = output / 'started.json'
        started = read_json(started_path) if started_path.exists() else {}
        if started.get('pid') and process_alive(started['pid']):
            raise ValueError('An interrupted cell still has a live harness process; stop it before resuming')
        outcome = {**cell, 'status': 'interrupted', 'passed': False,
                   'error': 'Controller ended before recording a terminal outcome; not rerun',
                   'harness_returncode': None, 'controller_seconds': None}
        atomic_json(outcome_path, outcome)
        return outcome
    output.mkdir(parents=True)
    command = cell_command(args, cell, output / 'run')
    atomic_json(output / 'command.json', command)
    atomic_json(output / 'started.json', {**cell, 'started_unix': time.time()})
    outcome = {**cell, 'status': 'harness_error', 'passed': False, 'harness_returncode': None}
    start = time.monotonic()
    process = None
    interrupted = False
    try:
        with (output / 'harness.stdout.txt').open('w', encoding='utf-8') as out, \
                (output / 'harness.stderr.txt').open('w', encoding='utf-8') as err:
            process = subprocess.Popen(command, stdout=out, stderr=err,
                                       creationflags=CREATE_NO_WINDOW,
                                       start_new_session=os.name != 'nt')
            atomic_json(output / 'started.json', {**cell, 'pid': process.pid,
                                                 'started_unix': time.time()})
            outcome['harness_returncode'] = process.wait()
        records = read_json(output / 'run' / 'results.json')
        if not isinstance(records, list) or len(records) != 1:
            raise ValueError('One harness result is required per scheduled cell')
        record = records[0]
        validate_record(cell, record, read_json(output / 'run' / 'environment.json'), protocol, prepared)
        outcome['record'] = record
        outcome['trace_notes'] = trace_notes(output / 'run', record, protocol['settings'])
        expected_code = 0 if record['passed'] else 1
        if outcome['harness_returncode'] != expected_code:
            raise ValueError('Harness exit status contradicts its result')
        outcome.update(status='completed', passed=record['passed'])
    except KeyboardInterrupt:
        interrupted = True
        if process:
            stop_process(process)
        outcome.update(status='interrupted', error='Interrupted by operator; not rerun')
    except (OSError, ValueError, KeyError, TypeError) as error:
        outcome['error'] = str(error)
    outcome['controller_seconds'] = time.monotonic() - start
    atomic_json(outcome_path, outcome)
    if interrupted:
        raise KeyboardInterrupt
    return outcome


def trace_notes(run_dir, record, settings):
    """Only inspect error events for context exhaustion, never incidental prompt text."""
    errors = []
    for path in run_dir.glob('*/session/events.jsonl'):
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if event.get('type') == 'error':
                errors.append(event.get('data'))
    metrics = record.get('metrics', {})
    messages = [data for data in errors if isinstance(data, str)]
    messages.extend(data[key] for data in errors if isinstance(data, dict)
                    for key in ('message', 'reason', 'error') if isinstance(data.get(key), str))
    for path in run_dir.glob('*/stderr.txt'):
        messages.extend(line for line in path.read_text(encoding='utf-8', errors='replace').splitlines()
                        if line.startswith('forge: limit:'))
    error_text = '\n'.join(messages).lower()
    return {'loop_warnings': metrics.get('loop_warnings'), 'error_events': errors,
            'context_exhaustion_observed': 'context' in error_text and any(
                word in error_text for word in ('limit', 'exceed', 'budget', 'full', 'exhaust')),
            'turn_cap_reached': metrics.get('turns', 0) >= settings['max_turns'],
            'generated_token_cap_reached': metrics.get('generated_tokens', 0) >= settings['max_tokens'],
            'input_token_cap_reached': metrics.get('prompt_tokens', 0) >= settings['max_input']}


def summarize(protocol, outcomes):
    expected = Counter(cell['cell_id'] for cell in protocol['schedule'])
    actual = Counter(row['cell_id'] for row in outcomes)
    rows = []
    for task in sorted(protocol['identity']['tasks']):
        for arm in protocol['arms']:
            group = [row for row in outcomes if row['task'] == task and row['arm'] == arm]
            times = [row['record']['timing']['end_to_end_seconds'] for row in group
                     if row.get('record', {}).get('timing', {}).get('end_to_end_seconds') is not None]
            rows.append({'task': task, 'arm': arm, 'passed': sum(row['passed'] for row in group),
                         'observed': len(group), 'scheduled': protocol['settings']['repetitions'],
                         'incomplete_or_harness_errors': sum(row['status'] != 'completed' for row in group),
                         'median_end_to_end_seconds': statistics.median(times) if times else None,
                         'timed_observations': len(times),
                         'loop_warnings': sum(row.get('trace_notes', {}).get('loop_warnings') or 0 for row in group),
                         'context_exhaustions_observed': sum(bool(row.get('trace_notes', {}).get('context_exhaustion_observed')) for row in group),
                         'turn_caps_reached': sum(bool(row.get('trace_notes', {}).get('turn_cap_reached')) for row in group),
                         'all_end_to_end_seconds': times})
    return {'schema_version': 1, 'diagnostic_only': True,
            'population': {'complete': expected == actual, 'expected': sum(expected.values()),
                           'observed': sum(actual.values()),
                           'missing': sorted((expected - actual).elements()),
                           'unexpected_or_duplicate': sorted((actual - expected).elements())},
            'per_task': rows,
            'historical_confound': protocol['historical_confound'],
            'notes': 'Failures, crashes and interruptions remain in the scheduled denominator. '
                     'Time summaries contain only available end-to-end measurements, across passes and failures. '
                     'Warnings and context exhaustion are trace observations, not a repair-success measure. '
                     'This diagnostic comparison makes no promotion or statistical advantage claim.'}


def write_summary(args, protocol):
    outcomes = [read_json(path) for path in sorted((args.output / 'cells').glob('*/outcome.json'))]
    outcomes.sort(key=lambda row: row['order_index'])
    atomic_json(args.output / 'outcomes.json', outcomes)
    report = summarize(protocol, outcomes)
    atomic_json(args.output / 'summary.json', report)
    lines = ['# Repair control diagnostic', '', report['notes'], '', protocol['historical_confound'], '',
             '| Task | Arm | Passes / scheduled | Observed | Median end-to-end seconds |',
             '| --- | --- | ---: | ---: | ---: |']
    for row in report['per_task']:
        elapsed = row['median_end_to_end_seconds']
        lines.append(f"| {row['task']} | {row['arm']} | {row['passed']}/{row['scheduled']} | "
                     f"{row['observed']} | {elapsed:.3f} |" if elapsed is not None else
                     f"| {row['task']} | {row['arm']} | {row['passed']}/{row['scheduled']} | {row['observed']} | n/a |")
    lines.extend(['', f"Population complete: {report['population']['complete']}.", ''])
    (args.output / 'SUMMARY.md').write_text('\n'.join(lines), encoding='utf-8')
    return report


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--experiment', choices=['repair-control', 'candidate-checkpoint', 'loop-completion'],
                        default='repair-control')
    parser.add_argument('--forge', type=Path,
                        help='single shared executable required for loop-completion')
    for arm in (*ARMS, 'candidate'):
        parser.add_argument('--' + arm + '-forge', type=Path)
    parser.add_argument('--checkpoint-revision')
    parser.add_argument('--current-revision')
    parser.add_argument('--source-dir', type=Path, default=DIRECTORY.parent)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--task-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resume', action='store_true')
    for name, default in [('gpu-layers', -1), ('context', 16384), ('output-reserve', 2048),
                          ('seed', 42), ('max-turns', None), ('timeout', 600),
                          ('max-tokens', 32768), ('max-input', 262144),
                          ('verification-timeout', 120), ('repetitions', None),
                          ('order-seed', 20260831), ('gpu-index', 0)]:
        parser.add_argument('--' + name, type=int, default=default)
    parser.add_argument('--temperature', type=float)
    args = parser.parse_args(argv)
    loop = args.experiment == 'loop-completion'
    for key, original, pilot in (('max_turns', 16, 32), ('repetitions', 3, 1),
                                 ('temperature', 0.0, 0.6)):
        if getattr(args, key) is None:
            setattr(args, key, pilot if loop else original)
    if args.experiment == 'repair-control' and (not args.checkpoint_revision or not args.current_revision):
        parser.error('repair-control requires checkpoint and current revisions')
    if loop and any(getattr(args, arm + '_forge') is not None for arm in (*ARMS, 'candidate')):
        parser.error('loop-completion takes one --forge; do not supply per-arm executables')
    if not loop and args.forge is not None:
        parser.error('--forge is only used with --experiment loop-completion')
    runtime_keys = ('forge',) if loop else tuple(arm + '_forge' for arm in arm_variants(args))
    for key in runtime_keys:
        if getattr(args, key) is None:
            parser.error('--' + key.replace('_', '-') + ' is required for this experiment')
    for key in ('model', 'task_dir', 'source_dir', 'output', *runtime_keys):
        setattr(args, key, getattr(args, key).resolve())
    for key in ('model', *runtime_keys):
        if not getattr(args, key).is_file():
            parser.error(key + ' must be an existing file')
    if args.output.is_relative_to(args.task_dir) or args.task_dir.is_relative_to(args.output):
        parser.error('output and task-dir must be separate directories')
    for key in ('context', 'output_reserve', 'max_turns', 'max_tokens', 'max_input', 'timeout', 'verification_timeout', 'repetitions'):
        if getattr(args, key) < 1:
            parser.error(key + ' must be positive')
    if not 0 <= args.temperature <= 2 or not 0 <= args.seed <= 4294967295:
        parser.error('temperature or seed is out of range')
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        tasks = load_tasks(args.task_dir, 'all')
        check_tools(tasks)
        with exclusive_output(args.output, args.resume):
            identity, diff = collect_identity(args, tasks)
            validate_runtime_identity(args, identity)
            stamps = frozen_stamps(identity)
            protocol = make_protocol(args, tasks, identity)
            if args.resume:
                validate_resume(read_json(args.output / 'protocol.json'), protocol)
                if (args.output / 'audit.json').exists() and not read_json(args.output / 'audit.json')['frozen_unchanged']:
                    raise ValueError('A prior frozen-identity audit failed; this experiment cannot resume')
            else:
                atomic_json(args.output / 'protocol.json', protocol)
                (args.output / 'minimal-source.diff').write_bytes(diff)
                with zipfile.ZipFile(args.output / 'minimal-source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
                    for name, value in identity['source']['files'].items():
                        if value is not None:
                            content = (args.source_dir / name).read_bytes()
                            if hashlib.sha256(content).hexdigest() != value['sha256']:
                                raise ValueError('Source changed while snapshot was being retained')
                            archive.writestr(name, content)
                atomic_json(args.output / 'environment.json', identity['platform'])
            validate_source_evidence(args.output, identity)
            prepared = preflight(args, protocol)
            preflight_identity = {'sha256': digest(args.output / 'preflight.json')}
            if args.resume:
                if read_json(args.output / 'preflight-identity.json') != preflight_identity:
                    raise ValueError('Preflight evidence changed after preregistration')
            else:
                atomic_json(args.output / 'preflight-identity.json', preflight_identity)
            try:
                for cell in protocol['schedule']:
                    if frozen_stamps(identity) != stamps:
                        raise ValueError('Frozen input metadata changed before a scheduled cell')
                    outcome = execute_cell(args, cell, protocol, prepared)
                    write_summary(args, protocol)
                    print(f"{cell['cell_id']}: {outcome['status']} "
                          f"{'PASS' if outcome['passed'] else 'FAIL'}", flush=True)
            finally:
                report = write_summary(args, protocol)
                try:
                    validate_source_evidence(args.output, identity)
                    after, _ = collect_identity(args, load_tasks(args.task_dir, 'all'))
                except (OSError, ValueError, subprocess.SubprocessError) as error:
                    atomic_json(args.output / 'audit.json', {
                        'frozen_unchanged': False, 'error': str(error), 'population': report['population']})
                    raise
                atomic_json(args.output / 'audit.json', {
                    'protocol_sha256': json_digest(protocol), 'after_identity': after,
                    'frozen_unchanged': identity == after,
                    'population': report['population'], 'preflight_sha256': digest(args.output / 'preflight.json')})
            if identity != after:
                raise ValueError('Frozen identity changed during execution; comparison is invalid')
            return 0 if report['population']['complete'] else 1
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'repair-control: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
