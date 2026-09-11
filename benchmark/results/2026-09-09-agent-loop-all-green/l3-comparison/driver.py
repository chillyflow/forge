"""Standalone frozen two-arm diagnostic. No acceptance outcomes are produced.

Only `run` launches model work. Freeze requires all 18 reference G1 executions.
The driver lives outside Forge's source inventory and records its own hash.
"""
import argparse
import copy
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import uuid
import zipfile

sys.dont_write_bytecode = True
# -B prevents writes but still permits reading stale bytecode. Fresh import paths
# bind this driver and its run.py subprocess to the recorded Python source bytes.
IMPORT_CACHE = tempfile.TemporaryDirectory(prefix='forge-l3-import-cache-')
sys.pycache_prefix = IMPORT_CACHE.name
os.environ['PYTHONPYCACHEPREFIX'] = IMPORT_CACHE.name
os.environ['PYTHONDONTWRITEBYTECODE'] = '1'
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'benchmark'))
import agent_loop_acceptance as evidence
import agent_loop_campaign as campaign
from run import VARIANTS
from common import verify_task

ARM_A = 'loop-repair'
ARM_B = 'loop-repair-best-of-2'
IGNORED = {'.git', '.forge'}
require = evidence.require
read = evidence.read_json
write = campaign.write
digest = evidence.file_digest
jd = evidence.json_digest
ref = campaign.reference
AUDIT_ERRORS = (KeyError, ValueError, OSError, TypeError, zipfile.BadZipFile)


def events(path):
    result = [json.loads(line) for line in path.read_text(encoding='utf-8').splitlines() if line.strip()]
    require(all(isinstance(event, dict) for event in result), 'invalid event stream')
    sequence = [event['sequence'] for event in result]
    require(sequence == sorted(set(sequence)), 'duplicate or reordered session event')
    return result


def snapshot(root):
    require(root.is_dir(), 'missing retained workspace: ' + str(root))
    result = {}
    for path in root.rglob('*'):
        relative = path.relative_to(root)
        first = relative.parts[0].casefold() if os.name == 'nt' else relative.parts[0]
        if first in IGNORED and (len(relative.parts) > 1 or path.is_dir()):
            continue
        require(not path.is_symlink(), 'symlink is not a retained file snapshot')
        if path.is_file():
            result[path.relative_to(root).as_posix()] = digest(path)
    return result


def protected_match(files, fixture):
    require(all(files.get(name) == value for name, value in fixture['protected_files'].items()),
            'protected file mutation')


def retained_inputs(output, result, fixture):
    require(result.get('terminal_retention_policy') == 'complete-except-git-forge',
            'unsupported-retention: use a new baseline retaining all pre/post root cache bytes')
    expected_cache = 'fresh-external-prefix-no-write' if result.get('language') == 'python' else 'not-applicable'
    require(result.get('verification', {}).get('python_cache_policy') == expected_cache,
            'verifier lacks recorded fresh-source cache policy')
    terminal = snapshot(output / 'terminal-workspace')
    before = snapshot(output / 'pre-verification-workspace')
    inputs = read(output / 'verification-inputs.json')
    require(inputs['before'] == before and inputs['after'] == terminal,
            'full verifier input maps differ from retained pre/post bytes')
    protected_match(before, fixture)
    protected_match(terminal, fixture)
    return before, terminal


def verify_fresh(workspace, task, target, fixture, oracle_count, timeout):
    """Verify an isolated source copy; -B alone does not prevent reading old pyc."""
    target.mkdir(parents=True)
    source = snapshot(workspace)
    source = {name: value for name, value in source.items()
              if Path(name).suffix.lower() not in ('.pyc', '.pyo') and
              '__pycache__' not in Path(name).parts}
    copied = target / 'workspace'
    copied.mkdir()
    for name in source:
        destination = copied / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(workspace / name, destination)
    before = snapshot(copied)
    env = campaign.environment(ROOT)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    result = verify_task(copied, task, target, env=env, timeout=timeout)
    after = snapshot(copied)
    protected_match(after, fixture)
    count = None
    if result['returncode'] == 0:
        text = (target / 'verification.stdout').read_text(encoding='utf-8') + '\n' + \
               (target / 'verification.stderr').read_text(encoding='utf-8')
        try:
            count = evidence.test_count(task.get('language', 'go'), text)
        except ValueError:
            count = 0
    result.update(command=task['verify'], source_before=before, source_after=after,
                  test_count=count, oracle_test_count=oracle_count,
                  complete=before == after and result['returncode'] != 124,
                  passed=result['returncode'] == 0 and before == after and
                  count is not None and count > 0 and count == oracle_count,
                  copied_without_bytecode=True)
    write(target / 'verification.json', result)
    return result


def native_final(stream):
    finals = [event for event in stream if event['type'] == 'final']
    if not finals:
        return None
    require(len(finals) == 1, 'multiple child finals')
    outputs = [event['data'] for event in stream if event['type'] == 'model_output'
               and event['sequence'] < finals[0]['sequence']]
    require(outputs and isinstance(outputs[-1], str) and '<function=final>' in outputs[-1]
            and '</tool_call>' in outputs[-1], 'child completion lacks a complete native final')
    return finals[0]['data']


def validation_pass(session, result):
    require(result.get('passed') is True and result.get('evidence_complete') is True and
            result.get('inputs_checked') is True and result.get('inputs_changed') is False,
            'incomplete or changed real-workspace validation')
    commands = result.get('commands', [])
    require(commands and result.get('commands_run') == len(commands), 'partial selection validation')
    require(any(command.get('stage') == 'broad_tests' for command in commands),
            'selection lacks authoritative broad tests')
    for command in commands:
        require(command.get('started') is True and command.get('exit_code') == 0 and
                not any(command.get(key) for key in ('timeout', 'cancelled', 'truncated')),
                'failed or incomplete selection command')
        for field in ('stdout_artifact', 'stderr_artifact'):
            require((session / command[field]).is_file(), 'missing selection command output')


def audit_children(session, fixture, profile, root_metrics, terminal=None):
    """Audit real retained trial layout, aggregate limits, selection and restoration."""
    stream = events(session / 'events.jsonl')
    starts = [event for event in stream if event['type'] == 'candidate_start']
    generated = [event for event in stream if event['type'] == 'candidate_generated']
    require([event['data'].get('candidate') for event in starts] == [1, 2] and
            [event['data'].get('candidate') for event in generated] == [1, 2],
            'missing or duplicated planned child; root remains non-passing')
    require({path.name for path in session.glob('trial-*')} == {'trial-01', 'trial-02'},
            'missing or extra retained trial workspace')
    spent = {'turns': 0, 'generated_tokens': 0, 'prompt_tokens': 0}
    limits = {'turns': 'max_turns', 'generated_tokens': 'max_tokens', 'prompt_tokens': 'max_input'}
    children = {}
    deadline = profile['timeout'] * 1000
    search_deadline = deadline - min(5000, deadline // 10)
    for index in (1, 2):
        start, generated_event = starts[index - 1], generated[index - 1]
        info = start['data']
        require(info.get('count') == 2 and info.get('independent_baseline') is True and
                info.get('seed') == (42 + (index - 1) * 0x9e3779b9) & 0xffffffff,
                'wrong independent-child seed or count')
        trial = session / f'trial-{index:02}'
        sessions = list((trial / '.forge/sessions').glob('*'))
        require(len(sessions) == 1 and sessions[0].is_dir(), 'missing or ambiguous nested child session')
        child_session = sessions[0]
        named_session = generated_event['data'].get('session', '').replace('\\', '/')
        require(named_session.endswith(f'trial-{index:02}/.forge/sessions/{child_session.name}'),
                'candidate_generated points at another child session')
        metrics = read(child_session / 'metrics.json')
        child_events = events(child_session / 'events.jsonl')
        child_done = [event['data'] for event in child_events if event['type'] == 'done']
        require(metrics.get('status') == generated_event['data'].get('status'),
                'child metrics and generation status are inconsistent')
        # Forge emits done only after successful completion; failed pilot children
        # end with limit metrics and candidate_generated, without a done event.
        require(len(child_done) <= 1 and (metrics.get('status') != 'ok' or child_done) and
                all(done.get('status') == metrics.get('status') and
                    all(done.get(metric) == metrics.get(metric) for metric in limits)
                    for done in child_done), 'child end-stage status or accounting is inconsistent')
        prompts = sorted(child_session.glob('context/*.txt'))
        require(prompts, 'missing nested child prompts')
        require(metrics.get('simulated') is False, 'missing real child inference')
        allocation = {}
        for metric, key in limits.items():
            allocation[metric] = (profile[key] - spent[metric]) // (3 - index)
            value = metrics.get(metric)
            require(type(value) is int and 0 <= value <= allocation[metric],
                    'child exceeded shared ' + metric + ' allocation')
            spent[metric] += value
        require(info.get('max_turns') == allocation['turns'], 'announced action allocation changed')
        wall_cap = (search_deadline - start['elapsed_ms']) // (4 - index)
        require(0 <= metrics.get('duration_ms', -1) <= wall_cap, 'child exceeded shared time allocation')
        files = snapshot(trial)
        protected_match(files, fixture)
        final = native_final(child_events)
        completed = generated_event['data'].get('status') == 'ok' and metrics.get('status') == 'ok'
        if completed:
            require(final is not None, 'completed child has no complete native final')
        children[index] = {'session': child_session.relative_to(session).as_posix(),
                           'prompts': [path.relative_to(session).as_posix() for path in prompts],
                           'files': files, 'completed': completed, 'final': final,
                           'allocated': allocation, 'time_cap_upper_ms': wall_cap,
                           'metrics': metrics}
    for metric, key in limits.items():
        require(root_metrics.get(metric) == spent[metric] <= profile[key],
                'root aggregate differs from shared child accounting: ' + metric)
    done = [event['data'] for event in stream if event['type'] == 'done']
    require(len(done) <= 1 and (root_metrics.get('status') != 'ok' or done) and
            all(item.get('status') == root_metrics.get('status') and
                all(item.get(metric) == root_metrics.get(metric) for metric in limits) for item in done),
            'root end-stage status or budget accounting is missing')
    require(0 <= root_metrics.get('duration_ms', -1) <= deadline, 'root exceeded total task deadline')
    virtual = dict(fixture['fixture_files'])
    pending = {}
    latest_validation = None
    passing = []
    winner = None
    root_finals = []
    for event in stream:
        kind, data = event['type'], event.get('data')
        if kind == 'candidate_start':
            require(virtual == fixture['fixture_files'], 'next child started before guarded baseline restoration')
        elif kind == 'candidate_edit_prepared':
            require(data['id'] not in pending, 'duplicate candidate file operation')
            require(data['path'] not in fixture['protected_files'], 'selection journal modifies protected file')
            before, after = session / data['before'], session / data['after']
            require(before.is_file() and after.is_file(), 'missing candidate before/after artifact')
            require(virtual.get(data['path']) == (digest(before) if data['before_exists'] else None),
                    'candidate journal before state does not match actual state')
            pending[data['id']] = data
            latest_validation = None
        elif kind == 'candidate_edit_outcome':
            require(data['id'] in pending and data.get('applied') is True,
                    'failed, missing or duplicated candidate apply/restore operation')
            operation = pending.pop(data['id'])
            if operation['after_exists']:
                virtual[operation['path']] = digest(session / operation['after'])
            else:
                virtual.pop(operation['path'], None)
        elif kind == 'validation_result':
            latest_validation = data
        elif kind == 'candidate_selection':
            require(data['candidate'] in children and data.get('real_workspace') is True,
                    'selection did not validate a known child in the real workspace')
            child = children[data['candidate']]
            require(child['completed'] and child['final'] is not None and virtual == child['files'],
                    'selection borrowed completion or validated another workspace')
            if data.get('passed'):
                require(event['elapsed_ms'] <= search_deadline, 'selection exceeded reserved deadline')
                validation_pass(session, latest_validation or {})
                passing.append((data['changed_content_cost'], data['candidate']))
        elif kind == 'candidate_selected':
            require(winner is None and passing and data['candidate'] == min(passing)[1],
                    'winner violates passing-first deterministic ranking')
            winner = data['candidate']
            require(virtual == children[winner]['files'], 'final apply differs from selected child')
            validation_pass(session, latest_validation or {})
            require(event['elapsed_ms'] <= search_deadline, 'final selection exceeded reserved deadline')
        elif kind == 'final':
            root_finals.append(data)
    require(not pending, 'unfinished candidate file operation')
    completed = root_metrics.get('status') == 'ok'
    if completed:
        require(winner is not None and root_finals == [children[winner]['final']],
                'root completion is absent or borrowed from another child')
    else:
        require(not root_finals, 'failed search reported a root final')
    if terminal is not None:
        require(virtual == terminal, 'terminal workspace differs from journaled selection/restoration')
    return {'planned': 2, 'started': 2, 'completed': sum(c['completed'] for c in children.values()),
            'winner': winner, 'root_completed': completed, 'children': children,
            'shared_consumption': spent, 'selection_count': len(passing)}


def validate_protocol(protocol):
    require(jd({k: v for k, v in protocol.items() if k != 'protocol_sha256'}) ==
            protocol['protocol_sha256'], 'diagnostic protocol changed')
    require(digest(__file__) == protocol['driver']['sha256'], 'diagnostic driver changed after freeze')
    contract = protocol['contract']
    evidence.validate_contract(contract, ROOT)
    evidence.validate_candidate(contract, protocol['candidate'], ROOT)
    expected = [row for row in contract['schedule'] if row['gate'] == 'G1']
    require(protocol['schedule'] == make_schedule(expected), 'diagnostic paired schedule changed')
    require(len(protocol['baseline_records']) == len(expected) == 18, 'incomplete baseline denominator')
    require([record['run_id'] for record in protocol['baseline_records']] ==
            [row['run_id'] for row in expected], 'baseline membership changed')
    require(len({record['execution_id'] for record in protocol['baseline_records']}) == 18,
            'baseline execution duplicated')
    for row, record in zip(expected, protocol['baseline_records']):
        evidence.check_binding(protocol['candidate'], row, record)
        evidence.check_archive(ROOT, record['archive'], record['inventory'])
        require(protocol['baseline_status'][row['run_id']]['status'] in ('passed', 'failed'),
                'baseline contains missing or invalid evidence')
    a, b = protocol['arms']['A'], protocol['arms']['B']
    require(a['variant'] == ARM_A and b['variant'] == ARM_B and
            b['flags'] == a['flags'] + ['--candidates', '2'], 'unexpected diagnostic intervention')
    require(protocol['profile'] == contract['profiles']['loop-pilot'], 'shared budget profile changed')


def make_schedule(rows, name='l3-search'):
    return [{'run_id': name + '-loop-repair-best-of-2-' + row['task'] +
             f'-s42-r{row["repetition"]:03}', 'baseline_run_id': row['run_id'],
             'order_index': index, 'task': row['task'], 'manifest': row['manifest'],
             'seed': 42, 'repetition': row['repetition']} for index, row in enumerate(rows, 1)]


def freeze(args):
    output = args.output.resolve()
    require(not output.exists(), 'diagnostic output already exists')
    baseline = args.baseline.resolve()
    contract, candidate = campaign.load(baseline)
    evidence.validate_candidate(contract, candidate, ROOT)
    require(campaign.observe(candidate) == candidate['identity'], 'source/runtime/model differ from baseline')
    rows = [row for row in contract['schedule'] if row['gate'] == 'G1']
    all_records = read(baseline / 'outcomes.json')
    by_id = {}
    for record in all_records:
        require(record['run_id'] not in by_id, 'duplicate reference outcome')
        by_id[record['run_id']] = record
    require(all(row['run_id'] in by_id for row in rows), 'finish all 18 single-candidate G1 runs first')
    records = [by_id[row['run_id']] for row in rows]
    baseline_summary = evidence.report(contract, candidate, all_records, read(baseline / 'g0.json'), ROOT)
    require(baseline_summary['gates']['G0']['accepted'], 'baseline G0 has not passed')
    for row, record in zip(rows, records):
        evidence.check_binding(candidate, row, record)
        inventory = evidence.check_archive(ROOT, record['archive'], record['inventory'])
        for reference in record['evidence'].values():
            evidence.read_artifact(ROOT, reference)
        run_root = baseline / 'runs' / row['run_id']
        output_root = run_root / 'harness' / (row['task'] + '-' + ARM_A + '-r001')
        harness = read(output_root / 'result.json')
        retained_inputs(output_root, harness, contract['fixtures'][row['manifest']])
        for path in output_root.rglob('*'):
            if path.is_file():
                member = 'raw/' + path.relative_to(run_root).as_posix()
                require(member in inventory and digest(path) == inventory[member]['sha256'],
                        'baseline raw retention differs from frozen archive: ' + member)
    arms = {name: {'variant': variant, 'flags': VARIANTS[variant]['flags']}
            for name, variant in [('A', ARM_A), ('B', ARM_B)]}
    require(candidate['profile_policies']['loop-pilot']['variant'] == ARM_A,
            'reference is not the repaired single-candidate arm')
    protocol = {'schema_version': 1, 'diagnostic_only': True, 'frozen_utc': campaign.now(),
                'candidate': candidate, 'contract': contract, 'baseline_records': records,
                'baseline_status': {row['run_id']: baseline_summary['runs'][row['run_id']]
                                    for row in rows},
                'baseline_directory': str(baseline), 'driver': ref(__file__), 'arms': arms,
                'profile': contract['profiles']['loop-pilot'], 'schedule': make_schedule(rows),
                'denominators': {'A_referenced_roots': 18, 'B_new_roots': 18, 'B_planned_children': 36},
                'timing_limit': 'Sequential A-then-B blocks; no causal latency claim.'}
    protocol['arm_configuration_sha256'] = {arm: jd({'profile': protocol['profile'], 'arm': value})
                                           for arm, value in arms.items()}
    protocol['protocol_sha256'] = jd(protocol)
    validate_protocol(protocol)
    output.mkdir(parents=True)
    shutil.copy2(__file__, output / 'frozen-driver.py')
    write(output / 'protocol.json', protocol)
    write(output / 'outcomes.json', [])
    report(output)
    print(json.dumps({'protocol_sha256': protocol['protocol_sha256'], 'new_roots': 18}))


def observed(protocol):
    result = campaign.observe(protocol['candidate'])
    require(result == protocol['candidate']['identity'], 'source/runtime/model changed since freeze')
    return {**result, 'configuration_sha256': protocol['arm_configuration_sha256']['B']}


def audit_root(protocol, row, target):
    fixture = protocol['contract']['fixtures'][row['manifest']]
    output = target / 'harness' / (row['task'] + '-' + ARM_B + '-r001')
    result, env = read(output / 'result.json'), read(target / 'harness/environment.json')
    profile = protocol['profile']
    require(result['task'] == row['task'] and result['variant'] == ARM_B and
            result['fixture_sha256'] == fixture['fixture_sha256'], 'wrong B task/variant/fixture')
    expected = {key: profile[key] for key in ('max_turns', 'max_tokens', 'max_input', 'temperature',
                'output_reserve', 'order_seed', 'lifecycle', 'prompt_protocol', 'gpu_index')}
    expected.update(seed=42, context_tokens=profile['context'], gpu_layers=str(profile['gpu_layers']),
                    chat_template='embedded', model_sha256=protocol['candidate']['identity']['model_sha256'])
    require(all(env.get(key) == value for key, value in expected.items()) and
            env['forge_runtime_bundle']['sha256'] == protocol['candidate']['identity']['runtime_sha256'],
            'harness changed model/runtime/profile')
    command = read(output / 'command.json')
    require(command[-len(protocol['arms']['B']['flags']):] == protocol['arms']['B']['flags'],
            'executed command differs from frozen B policy')
    pre_verification, terminal = retained_inputs(output, result, fixture)
    require(result.get('protected_files_unchanged') is True and
            result.get('protected_before_verification') is True and
            result.get('verification_inputs_unchanged') is True, 'protected or verifier-input mutation')
    require(pre_verification == terminal, 'verifier observed another terminal workspace')
    audit = audit_children(output / 'session', fixture, profile, result['metrics'], terminal)
    require(0 <= result['timing']['agent_seconds'] <= profile['timeout'] and
            0 <= result['timing']['verification_seconds'] <= profile['verification_timeout'],
            'root or independent verifier exceeded fixed deadline')
    baseline = next(record for record in protocol['baseline_records'] if record['run_id'] == row['baseline_run_id'])
    preflight = read(evidence.read_artifact(ROOT, baseline['evidence']['preflight']))
    verified = result['verification']['returncode'] == 0
    if verified:
        text = (output / 'verification.stdout').read_text(encoding='utf-8') + '\n' + \
               (output / 'verification.stderr').read_text(encoding='utf-8')
        count = evidence.test_count(result.get('language', 'go'), text)
        require(count > 0 and count == preflight['oracle_test_count'], 'missing/partial independent tests')
    complete = result['returncode'] == 0 and audit['root_completed']
    fresh = read(target / 'secondary.json')
    require(set(fresh) == {'root', 'trial-01', 'trial-02'}, 'missing independent child verification')
    for name, checked in fresh.items():
        require(checked.get('command') == fixture['verify'] and
                checked.get('copied_without_bytecode') is True and checked.get('complete') is True,
                'incomplete fresh-source verification: ' + name)
    winner_fresh = audit['winner'] is not None and fresh[f'trial-{audit["winner"]:02}']['passed']
    source_pass = verified and fresh['root']['passed']
    return {'passed': source_pass and complete and winner_fresh and terminal != fixture['fixture_files'],
            'terminal_passing_without_completion': source_pass and not complete,
            'children': audit, 'timing': result['timing'], 'metrics': result['metrics']}


def report(directory):
    protocol = read(directory / 'protocol.json')
    validate_protocol(protocol)
    records = read(directory / 'outcomes.json')
    ids = [record['run_id'] for record in records]
    require(len(ids) == len(set(ids)), 'duplicate B scheduled outcome')
    execution_ids = [record['execution_id'] for record in records]
    require(len(execution_ids) == len(set(execution_ids)), 'duplicate B execution identity')
    require(not set(execution_ids) & {r['execution_id'] for r in protocol['baseline_records']},
            'B borrowed an A execution')
    schedule = {row['run_id']: row for row in protocol['schedule']}
    require(set(ids) <= set(schedule), 'unscheduled B outcome')
    expected_identity = {**protocol['candidate']['identity'],
                         'configuration_sha256': protocol['arm_configuration_sha256']['B']}
    pairs = []
    for row in protocol['schedule']:
        a = protocol['baseline_status'][row['baseline_run_id']]['status'] == 'passed'
        found = next((record for record in records if record['run_id'] == row['run_id']), None)
        status, error, b = 'missing', None, False
        if found:
            try:
                require(found['protocol_sha256'] == protocol['protocol_sha256'] and
                        found['identity_before'] == found['identity_after'] == expected_identity,
                        'B source/runtime/arm identity changed')
                require(evidence.timestamp(found['started_utc']) >= evidence.timestamp(protocol['frozen_utc']),
                        'B executed before preregistration')
                target = directory / 'runs' / row['run_id']
                inventory = evidence.check_archive(ROOT, found['archive'], found['inventory'])
                excluded = {'complete-evidence.zip', 'complete-evidence-inventory.json', 'outcome.json'}
                require(set(inventory) == {path.relative_to(target).as_posix()
                        for path in target.rglob('*') if path.is_file() and
                        path.relative_to(target).as_posix() not in excluded},
                        'raw evidence membership differs from complete archive')
                for name, entry in inventory.items():
                    require((target / name).is_file() and digest(target / name) == entry['sha256'],
                            'retained raw evidence differs from archive: ' + name)
                archived_execution = read(target / 'execution.json')
                require(archived_execution == {key: value for key, value in found.items()
                        if key not in ('archive', 'inventory')}, 'outcome differs from archived execution')
                audit = audit_root(protocol, row, target)
                require(audit == found['audit'], 'post-run audit differs from recorded evidence')
                b, status = audit['passed'], 'complete'
            except AUDIT_ERRORS as exc:
                status, error = 'invalid', str(exc)
        pairs.append({'task': row['task'], 'repetition': row['repetition'], 'A_passed': a,
                      'B_passed': b, 'B_status': status, 'error': error})
    counts = {key: sum(pair['A_passed'] == a and pair['B_passed'] == b for pair in pairs)
              for key, a, b in [('both_pass', True, True), ('lost_A_pass', True, False),
                                ('gained_B_pass', False, True), ('both_nonpassing', False, False)]}
    complete = all(pair['B_status'] == 'complete' for pair in pairs)
    benefit = complete and counts['lost_A_pass'] == 0 and counts['gained_B_pass'] > 0
    summary = {'diagnostic_only': True, 'protocol_sha256': protocol['protocol_sha256'],
               'scheduled_roots': 36, 'A_referenced': 18, 'B_scheduled': 18,
               'A_passed': sum(pair['A_passed'] for pair in pairs),
               'B_passed': sum(pair['B_passed'] for pair in pairs), 'complete': complete,
               'paired_counts': counts, 'measured_development_benefit': benefit,
               'keep_best_of_N_experimental': True,
               'next_step': 'Further preregistered evaluation; no acceptance credit.' if benefit else
                            'No qualifying preservation-and-correctness benefit established.', 'pairs': pairs}
    write(directory / 'summary.json', summary)
    return summary


def close(directory):
    """Seal reports and raw evidence, retaining external A/source archive identities."""
    require(not (directory / '.running').exists(), 'cannot close an active diagnostic')
    require(not (directory / 'closure.json').exists(), 'diagnostic closure already exists')
    protocol = read(directory / 'protocol.json')
    errors = []
    try:
        summary = report(directory)
    except AUDIT_ERRORS as exc:
        errors.append(str(exc))
        summary = {'diagnostic_only': True, 'complete': False, 'evidence_error': str(exc)}
        write(directory / 'summary.json', summary)
    identity = None
    try:
        identity = observed(protocol)
    except AUDIT_ERRORS as exc:
        errors.append(str(exc))
    complete = summary['complete'] and not errors
    write(directory / 'identity-check.json', {'checked_utc': campaign.now(), 'identity': identity,
          'errors': errors,
          'protocol_sha256': protocol['protocol_sha256'], 'complete': complete,
          'external_A_archives': [{key: record[key] for key in ('run_id', 'execution_id', 'archive', 'inventory')}
                                 for record in protocol['baseline_records']],
          'external_source': {key: protocol['candidate'][key] for key in
                              ('source_manifest', 'source_archive', 'source_inventory', 'runtime_manifest')}})
    paths = {path.relative_to(directory).as_posix(): path for path in directory.rglob('*')
             if path.is_file() and path.name not in ('diagnostic.zip', 'diagnostic-inventory.json')}
    archive, inventory = campaign.archive_files(directory, paths, 'diagnostic')
    write(directory / 'closure.json', {'closed_utc': campaign.now(), 'diagnostic_only': True,
          'protocol_sha256': protocol['protocol_sha256'], 'complete': complete,
          'archive': archive, 'inventory': inventory})
    return 0 if complete else 1


def run(directory):
    require(not (directory / 'closure.json').exists(), 'closed diagnostic cannot be extended')
    protocol = read(directory / 'protocol.json')
    validate_protocol(protocol)
    candidate = copy.deepcopy(protocol['candidate'])
    candidate['profile_policies']['loop-pilot'] = {'variant': ARM_B, 'candidate_count': 2,
                                                 'flags': protocol['arms']['B']['flags']}
    lock = directory / '.running'
    with lock.open('x') as marker:
        marker.write(str(os.getpid()))
    try:
        for row in protocol['schedule']:
            if any(record['run_id'] == row['run_id'] for record in read(directory / 'outcomes.json')):
                continue
            target = directory / 'runs' / row['run_id']
            require(not target.exists(), 'started outcome cannot be overwritten or retried')
            before = observed(protocol)
            target.mkdir(parents=True)
            command = campaign.command_for(candidate, {**row, 'profile': 'loop-pilot'},
                                           protocol['profile'], ROOT, target / 'harness')
            started = campaign.now()
            execution = {'run_id': row['run_id'], 'execution_id': str(uuid.uuid4()),
                         'protocol_sha256': protocol['protocol_sha256'], 'identity_before': before,
                         'started_utc': started, 'command': command}
            write(target / 'started.json', execution)
            with (target / 'harness.log').open('w', encoding='utf-8') as log:
                code = campaign.run_process(command, ROOT, log, 900)
            execution.update(finished_utc=campaign.now(), harness_exit=code)
            try:
                execution['identity_after'] = observed(protocol)
                fixture = protocol['contract']['fixtures'][row['manifest']]
                task = read(ROOT / row['manifest'])
                baseline = next(record for record in protocol['baseline_records']
                                if record['run_id'] == row['baseline_run_id'])
                oracle_count = read(evidence.read_artifact(ROOT, baseline['evidence']['preflight']))['oracle_test_count']
                output = target / 'harness' / (row['task'] + '-' + ARM_B + '-r001')
                secondary = {}
                roots = {'root': output / 'terminal-workspace',
                         'trial-01': output / 'session/trial-01', 'trial-02': output / 'session/trial-02'}
                for name, path in roots.items():
                    secondary[name] = verify_fresh(path, task, target / 'secondary' / name,
                        fixture, oracle_count, protocol['profile']['verification_timeout'])
                write(target / 'secondary.json', secondary)
                execution['audit'] = audit_root(protocol, row, target)
            except AUDIT_ERRORS as exc:
                execution['audit_error'] = str(exc)
            write(target / 'execution.json', execution)
            members = {path.relative_to(target).as_posix(): path for path in target.rglob('*') if path.is_file()}
            execution['archive'], execution['inventory'] = campaign.archive_files(target, members, 'complete-evidence')
            write(target / 'outcome.json', execution)
            records = read(directory / 'outcomes.json')
            records.append(execution)
            write(directory / 'outcomes.json', records)
            summary = report(directory)
            print(row['run_id'] + ': ' + summary['pairs'][row['order_index'] - 1]['B_status'], flush=True)
        return 0 if report(directory)['complete'] else 1
    finally:
        lock.unlink(missing_ok=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    freezing = commands.add_parser('freeze')
    freezing.add_argument('--baseline', type=Path, required=True)
    freezing.add_argument('--output', type=Path, required=True)
    for name in ('run', 'report', 'close'):
        command = commands.add_parser(name)
        command.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.action == 'freeze':
        freeze(args)
    elif args.action == 'run':
        return run(args.directory.resolve())
    elif args.action == 'close':
        return close(args.directory.resolve())
    else:
        summary = report(args.directory.resolve())
        print(json.dumps(summary, indent=2))
        return 0 if summary['complete'] else 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
