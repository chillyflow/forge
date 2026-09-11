"""Fail-closed accounting for the preregistered agent-loop acceptance campaign.

This reporter never starts inference, repairs a fixture, retries a run, or selects
the best outcome. Records are evidence envelopes, not an alternate success oracle.
See the frozen acceptance directory's README for the envelope contract.
"""
import argparse
import collections
import datetime
import hashlib
import json
import os
from pathlib import Path
import random
import re
import zipfile

VERIFICATION_POLICY = {
    'python_cache_policy': 'fresh-external-prefix-no-write',
    'other_cache_policy': 'not-applicable',
    'terminal_retention_policy': 'complete-except-git-forge',
    'input_inventory': 'all-files-except-root-git-forge-directories',
    'retain_before_verification': True,
}


def configuration_digest(profiles, policies, verification_policy=None):
    value = {'profiles': profiles, 'profile_policies': policies}
    if verification_policy is not None:
        value['verification_policy'] = verification_policy
    return json_digest(value)


class EvidenceError(ValueError):
    """The retained evidence cannot establish the claimed observation."""


def json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def file_digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def timestamp(value):
    require(isinstance(value, str), 'missing timestamp')
    result = datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    require(result.tzinfo is not None, 'timestamp must include a timezone')
    return result


def expected_schedule(contract):
    """Expand all populations, including failures and the second seed block."""
    result = []
    # JSON object order is deliberately irrelevant to a frozen schedule.
    groups = sorted(contract['suites'], key=lambda group:
                    (group.split('-')[0], 0 if group.endswith('regression') else 1))
    for phase in ('development', 'confirmation'):
        for population in groups:
            suite = contract['suites'][population]
            profile = contract['profiles'][suite['profile']]
            seeds = [42, 42, 42]
            if phase == 'confirmation' and profile['temperature'] > 0:
                seeds += [43, 44, 45]
            for repetition, seed in enumerate(seeds, 1):
                manifests = sorted(suite['manifests'])
                random.Random(profile['order_seed'] + repetition - 1).shuffle(manifests)
                for manifest in manifests:
                    task = contract['fixtures'][manifest]['task']
                    result.append({
                        'run_id': f'{phase}-{population}-{suite["profile"]}-{task}'
                                  f'-s{seed}-r{repetition:03}',
                        'phase': phase, 'gate': population.split('-')[0]
                        if phase == 'development' else 'G6',
                        'population': population, 'profile': suite['profile'],
                        'manifest': manifest, 'task': task, 'seed': seed,
                        'repetition': repetition, 'order_index': len(result) + 1})
    return result


def validate_contract(contract, root=None):
    unhashed = {k: v for k, v in contract.items() if k != 'contract_sha256'}
    require(json_digest(unhashed) == contract.get('contract_sha256'),
            'acceptance contract hash mismatch')
    require(contract.get('schema_version') == 1, 'unsupported contract schema')
    require(contract['schedule'] == expected_schedule(contract),
            'schedule does not contain exactly the frozen populations and seeds')
    counts = collections.Counter((row['phase'], row['population'])
                                 for row in contract['schedule'])
    require(len(contract['schedule']) == contract['denominators']['coding_total'],
            'coding denominator does not match schedule')
    for phase in ('development', 'confirmation'):
        require(dict((population, n) for (p, population), n in counts.items()
                     if p == phase) == contract['denominators'][phase],
                'population denominator does not match schedule')
    for population, suite in contract['suites'].items():
        require(len(suite['manifests']) == len(set(suite['manifests'])) == suite['count'],
                'missing or duplicated manifest in ' + population)
        require(sorted(contract['fixtures'][m]['task'] for m in suite['manifests'])
                == sorted(suite['tasks']), 'suite membership mismatch')
    if root is not None:
        for path, fixture in contract['fixtures'].items():
            require(file_digest(Path(root) / path) == fixture['manifest_sha256'],
                    'frozen manifest changed: ' + path)


def read_artifact(root, reference):
    require(isinstance(reference, dict), 'missing artifact reference')
    name = reference.get('path')
    require(isinstance(name, str) and name, 'missing artifact path')
    path = (Path(root) / name).resolve()
    require(path.is_file(), 'missing evidence: ' + name)
    require(file_digest(path) == reference.get('sha256'),
            'evidence hash mismatch: ' + name)
    return path


def check_archive(root, archive, inventory):
    """Check every archived byte, rejecting omitted and duplicated entries."""
    archive_path = read_artifact(root, archive)
    manifest = read_json(read_artifact(root, inventory))
    files = manifest.get('files')
    require(isinstance(files, dict) and files, 'empty archive inventory')
    with zipfile.ZipFile(archive_path) as bundle:
        names = bundle.namelist()
        require(len(names) == len(set(names)), 'duplicate archive member')
        require(set(names) == set(files), 'archive inventory membership mismatch')
        for name in names:
            expected = files[name]
            require(isinstance(expected, dict), 'invalid archive inventory entry')
            content = bundle.read(name)
            require(len(content) == expected.get('bytes') and
                    hashlib.sha256(content).hexdigest() == expected.get('sha256'),
                    'archive inventory hash mismatch: ' + name)
    return files


def validate_candidate(contract, candidate, root):
    require(isinstance(candidate, dict), 'candidate is not frozen')
    unhashed = {k: v for k, v in candidate.items()
               if k not in ('candidate_sha256', 'confirmation_frozen_utc')}
    require(json_digest(unhashed) == candidate.get('candidate_sha256'),
            'candidate freeze hash mismatch')
    require(candidate.get('contract_sha256') == contract['contract_sha256'],
            'candidate belongs to another acceptance contract')
    require(isinstance(candidate.get('candidate_id'), str) and candidate['candidate_id'],
            'missing candidate identity')
    timestamp(candidate.get('frozen_utc'))
    verification_policy = candidate.get('verification_policy')
    if candidate.get('schema_version') == 2 or verification_policy is not None:
        require(verification_policy == VERIFICATION_POLICY, 'missing or unsupported frozen verification policy')
    identity = candidate['identity']
    for name in ('source_sha256', 'runtime_sha256', 'model_sha256', 'configuration_sha256'):
        require(re.fullmatch('[0-9a-f]{64}', identity.get(name, '')) is not None,
                'missing frozen ' + name)
    require(identity['model_sha256'] == contract['model']['sha256'], 'swapped model')
    policies = candidate['profile_policies']
    require(set(policies) == set(contract['profiles']), 'missing profile policy')
    for name, profile in contract['profiles'].items():
        policy = policies[name]
        require(type(policy.get('candidate_count')) is int and policy['candidate_count'] >= 1,
                'invalid candidate count')
        require(isinstance(policy.get('flags'), list), 'missing explicit policy flags')
        if profile['temperature'] == 0:
            require(policy['candidate_count'] == 1,
                    'zero-temperature preservation requires the single-candidate path')
    require(identity['configuration_sha256'] == configuration_digest(
        contract['profiles'], policies, verification_policy),
        'configuration identity mismatch')
    source = read_json(read_artifact(root, candidate['source_manifest']))
    source_files = check_archive(root, candidate['source_archive'], candidate['source_inventory'])
    require(source.get('files_sha256') == identity['source_sha256'] ==
            json_digest(source.get('files')), 'source snapshot identity mismatch')
    require(source_files == {k: v for k, v in source['files'].items() if v is not None},
            'source archive differs from source snapshot')
    runtime = read_json(read_artifact(root, candidate['runtime_manifest']))
    require(json_digest(runtime.get('files')) == runtime.get('sha256') ==
            identity['runtime_sha256'], 'runtime bundle identity mismatch')
    require('forge.exe' in runtime['files'], 'runtime has no Forge executable')
    for name, entry in runtime['files'].items():
        path = Path(candidate['runtime_directory']) / name
        require(path.is_file() and file_digest(path) == entry['sha256'] and
                path.stat().st_size == entry['bytes'], 'swapped runtime file: ' + name)


def check_binding(candidate, row, record):
    require(record.get('candidate_id') == candidate['candidate_id'] and
            record.get('candidate_sha256') == candidate['candidate_sha256'],
            'outcome borrowed from another candidate')
    require(record.get('identity_before') == candidate['identity'] and
            record.get('identity_after') == candidate['identity'],
            'source, runtime, model or configuration changed during execution')
    require(record.get('run_id') == row['run_id'], 'run identity mismatch')
    require(isinstance(record.get('execution_id'), str) and record['execution_id'],
            'missing execution identity')
    started, finished = timestamp(record.get('started_utc')), timestamp(record.get('finished_utc'))
    require(timestamp(candidate['frozen_utc']) <= started <= finished,
            'execution precedes candidate freeze or has invalid timing')


def archived_snapshot(archive_files, prefix):
    return {name[len(prefix):]: entry['sha256'] for name, entry in archive_files.items()
            if name.startswith(prefix)}


def validate_verification_policy(candidate, row, fixture, record, paths, archive_files, terminal_files):
    """Bind new-candidate metadata to full cache-inclusive archived input bytes."""
    policy = candidate['verification_policy']
    require(policy == VERIFICATION_POLICY and record.get('verification_policy') == policy,
            'run differs from frozen verification policy')
    validation = read_json(paths['validation'])
    harness = read_json(paths['harness_result'])
    inputs = read_json(paths['verification_inputs'])
    before = read_json(paths['pre_verification']).get('files')
    preflight = read_json(paths['preflight'])
    expected_cache = policy['python_cache_policy'] if fixture['verify'][0] == 'python' else policy['other_cache_policy']
    require(validation.get('python_cache_policy') == expected_cache ==
            harness.get('verification', {}).get('python_cache_policy'),
            'missing or mismatched fresh-source verifier policy')
    require(all(preflight.get('verification', {}).get(label, {}).get('python_cache_policy') == expected_cache
                for label in ('baseline', 'oracle')), 'preflight lacks fresh-source verifier policy')
    require(validation.get('terminal_retention_policy') == policy['terminal_retention_policy'] ==
            harness.get('terminal_retention_policy') and
            harness.get('pre_verification_workspace') == 'pre-verification-workspace' and
            harness.get('terminal_workspace') == 'terminal-workspace',
            'missing full before/after workspace retention policy')
    require(isinstance(before, dict) and inputs.get('before') == before and
            inputs.get('after') == terminal_files, 'full verifier input maps differ from retained snapshots')
    require(archived_snapshot(archive_files, 'pre-verification/') == before and
            archived_snapshot(archive_files, 'terminal/') == terminal_files,
            'full before/after snapshot membership differs from archive')
    variant = candidate['profile_policies'][row['profile']]['variant']
    raw_prefix = 'raw/harness/' + row['task'] + '-' + variant + '-r001/'
    require(archived_snapshot(archive_files, raw_prefix + 'pre-verification-workspace/') == before and
            archived_snapshot(archive_files, raw_prefix + 'terminal-workspace/') == terminal_files,
            'raw retained before/after bytes differ from verifier inventory')
    for files in (before, terminal_files):
        require(set(fixture['fixture_files']) <= set(files), 'verification snapshot omits fixture inputs')
        require(all(files.get(name) == value for name, value in fixture['protected_files'].items()),
                'protected pre/post-verification input changed')
        for name in files:
            parts = name.split('/')
            first = parts[0].casefold() if os.name == 'nt' else parts[0]
            require(name and '\\' not in name and not name.startswith('/') and ':' not in name and
                    '..' not in parts and not (len(parts) > 1 and first in ('.git', '.forge')),
                    'invalid retained input path')
    unchanged = before == terminal_files
    require(harness.get('verification_inputs_unchanged') is unchanged and
            validation.get('verification_inputs_unchanged') is unchanged,
            'verifier mutation flag differs from full input maps')
    return unchanged


def test_count(language, output):
    if language == 'python':
        counts = re.findall(r'^Ran (\d+) tests? in ', output, re.MULTILINE)
        require(len(counts) == 1 and re.search(r'^OK\s*$', output, re.MULTILINE),
                'independent verifier did not report complete passing unittest execution')
        return int(counts[0])
    events = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    require(events and not any(e.get('Action') in ('fail', 'skip') for e in events),
            'independent Go test execution failed, skipped, or missing')
    tests = {(e.get('Package'), e['Test']) for e in events
             if e.get('Action') == 'pass' and e.get('Test')}
    require(any(e.get('Action') == 'pass' and not e.get('Test') for e in events),
            'missing final Go package verdict')
    return len(tests)


def validate_coding_record(contract, candidate, row, record, root):
    check_binding(candidate, row, record)
    require(record.get('schedule') == row, 'record differs from scheduled run')
    fixture = contract['fixtures'][row['manifest']]
    profile = contract['profiles'][row['profile']]
    require(record.get('fixture_sha256') == fixture['fixture_sha256'] and
            record.get('manifest_sha256') == fixture['manifest_sha256'], 'fixture identity mismatch')
    require(record.get('settings') == {**profile, 'seed': row['seed']},
            'run profile, sampling seed or budget changed')
    archive_files = check_archive(root, record.get('archive'), record.get('inventory'))
    evidence = record.get('evidence', {})
    required = ('execution', 'command', 'preflight', 'prompts', 'outputs', 'journal',
                'validation', 'terminal', 'metrics', 'verification_stdout', 'verification_stderr')
    if candidate.get('verification_policy') is not None:
        required += ('verification_inputs', 'pre_verification', 'harness_result')
    paths = {}
    for role in required:
        require(role in evidence, 'missing ' + role + ' evidence')
        reference = evidence[role]
        paths[role] = read_artifact(root, reference)
        require(archive_files.get(reference.get('member'), {}).get('sha256') == reference['sha256'],
                'evidence absent from verified archive: ' + role)
    execution = read_json(paths['execution'])
    for field in ('run_id', 'execution_id', 'candidate_id', 'candidate_sha256',
                  'identity_before', 'identity_after', 'started_utc', 'finished_utc'):
        require(execution.get(field) == record[field], 'archived execution binding mismatch')
    if candidate.get('verification_policy') is not None:
        require(execution.get('verification_policy') == candidate['verification_policy'],
                'archived execution verification policy mismatch')
    require(read_json(paths['command']).get('settings') == record['settings'],
            'record settings differ from executed command')
    preflight = read_json(paths['preflight'])
    require(preflight.get('fixture_sha256') == fixture['fixture_sha256'] and
            preflight.get('baseline_failed') is True and preflight.get('oracle_passed') is True,
            'missing complete preflight')
    terminal = read_json(paths['terminal'])
    files = terminal.get('files', {})
    require(set(fixture['fixture_files']) <= set(files), 'terminal inputs are missing')
    require(all(files.get(name) == digest for name, digest in fixture['protected_files'].items()),
            'protected terminal file changed')
    require(record.get('protected_files_unchanged') is True,
            'protected file mutation occurred during execution')
    for name, digest in files.items():
        require(archive_files.get('terminal/' + name, {}).get('sha256') == digest,
                'terminal file absent from archive: ' + name)
    changed = any(files.get(name) != digest for name, digest in fixture['fixture_files'].items()
                  if name not in fixture['protected_files'])
    require(changed, 'no-op terminal workspace')
    metrics = read_json(paths['metrics'])
    require(metrics.get('simulated') is False, 'simulated or unspecified inference')
    for name, limit in (('turns', 'max_turns'), ('generated_tokens', 'max_tokens'),
                        ('prompt_tokens', 'max_input')):
        value = metrics.get(name)
        require(type(value) is int and 0 < value <= profile[limit],
                'missing measurement or exceeded budget: ' + name)
    for name, limit in (('agent_seconds', 'timeout'), ('verification_seconds', 'verification_timeout')):
        value = metrics.get(name)
        require(type(value) in (int, float) and 0 <= value <= profile[limit],
                'missing timing or exceeded limit: ' + name)
    validation = read_json(paths['validation'])
    require(validation.get('command') == fixture['verify'] and
            validation.get('terminal_sha256') == json_digest(files),
            'verification command or terminal input identity mismatch')
    terminal_passed = validation.get('returncode') == 0 and validation.get('complete') is True
    if candidate.get('verification_policy') is not None:
        terminal_passed = validate_verification_policy(candidate, row, fixture, record, paths,
                            archive_files, files) and terminal_passed
    if terminal_passed:
        output = paths['verification_stdout'].read_text(encoding='utf-8') + '\n' + \
                 paths['verification_stderr'].read_text(encoding='utf-8')
        language = 'python' if fixture['verify'][0] == 'python' else 'go'
        count = test_count(language, output)
        require(count > 0 and count == preflight.get('oracle_test_count') ==
                validation.get('test_count'), 'incomplete independent test execution')
    completion = execution.get('agent_completed') is True and execution.get('returncode') == 0
    if row['population'].startswith('G3'):
        require(execution.get('terminal_loop') is False and terminal.get('syntax_valid') is True,
                'terminal loop or syntax-broken preservation workspace')
    return {'passed': terminal_passed and completion,
            'terminal_passing_without_completion': terminal_passed and not completion}


def validate_g0(candidate, check, record, root):
    check_binding(candidate, {'run_id': 'G0-' + check['check_id']}, record)
    artifact = read_json(read_artifact(root, record['evidence']))
    for field in ('run_id', 'execution_id', 'candidate_id', 'candidate_sha256'):
        require(artifact.get(field) == record[field], 'G0 evidence binding mismatch')
    require(artifact.get('returncode') == 0 and artifact.get('status') == 'passed',
            'G0 check did not pass (unsupported is not a pass)')
    read_artifact(root, artifact.get('log'))
    if 'junit' in artifact:
        read_artifact(root, artifact['junit'])
    g0_runtime = candidate.get('g0_runtime', {}).get('files', {})
    require(bool(g0_runtime), 'missing frozen G0 executable/runtime inventory')
    expected_runtime = {name: value['sha256'] for name, value in g0_runtime.items()}
    require(artifact.get('g0_runtime_before') == expected_runtime and
            artifact.get('g0_runtime_after') == expected_runtime, 'G0 executable/runtime changed')
    for entry in g0_runtime.values():
        read_artifact(root, entry)
    if 'required_tests' in check:
        tests = artifact.get('tests', [])
        names = [test.get('name') for test in tests]
        require(len(names) == len(set(names)), 'duplicate G0 test')
        require(set(check['required_tests']) <= set(names), 'missing maintained G0 test')
        for test in tests:
            require(test.get('status') == 'passed' or
                    (test.get('status') == 'skipped' and
                     test['name'] in check.get('allowed_ctest_skip', [])),
                    'non-passing G0 test: ' + str(test.get('name')))
    else:
        require(artifact.get('command') == check['command'], 'wrong direct model probe')
        expected = candidate.get('g0_binaries', {}).get(check['command'][0])
        require(expected is not None and artifact.get('binary_sha256') == expected['sha256'],
                'direct model probe binary identity mismatch')
        read_artifact(root, expected)


def report(contract, candidate, outcomes, g0, root):
    validate_contract(contract, root)
    errors = []
    if candidate is not None:
        try:
            validate_candidate(contract, candidate, root)
        except (EvidenceError, KeyError, OSError, ValueError, zipfile.BadZipFile) as exc:
            errors.append('candidate: ' + str(exc))
    else:
        errors.append('candidate is not frozen')
    scheduled = {row['run_id']: row for row in contract['schedule']}
    checks = {'G0-' + check['check_id']: check for check in contract['g0_checks']}
    rows = collections.defaultdict(list)
    executions = collections.Counter()
    for record in [*outcomes, *g0]:
        require(isinstance(record, dict), 'outcome must be an object')
        run_id = record.get('run_id')
        if run_id not in scheduled and run_id not in checks:
            errors.append('unscheduled run: ' + str(run_id))
        rows[run_id].append(record)
        if record.get('execution_id'):
            executions[record['execution_id']] += 1
    summaries = {}
    terminal_only = []
    for run_id in [*checks, *scheduled]:
        records = rows[run_id]
        status, reason = 'missing', 'scheduled run not executed'
        if records:
            status = 'invalid'
            try:
                require(candidate is not None and not errors, 'candidate or campaign evidence invalid')
                require(len(records) == 1, 'duplicated scheduled run')
                record = records[0]
                require(executions[record.get('execution_id')] == 1,
                        'execution identity reused by multiple scheduled runs')
                if run_id in checks:
                    validate_g0(candidate, checks[run_id], record, root)
                    status = 'passed'
                else:
                    result = validate_coding_record(contract, candidate, scheduled[run_id], record, root)
                    status = 'passed' if result['passed'] else 'failed'
                    if result['terminal_passing_without_completion']:
                        terminal_only.append(run_id)
                reason = None
            except (EvidenceError, KeyError, OSError, ValueError, zipfile.BadZipFile) as exc:
                reason = str(exc)
        summaries[run_id] = {'status': status, 'reason': reason}
    gate_reports = {}
    prior_passed = True
    for gate in ('G0', 'G1', 'G2', 'G3', 'G4', 'G5', 'G6'):
        ids = list(checks) if gate == 'G0' else [r['run_id'] for r in contract['schedule'] if r['gate'] == gate]
        counts = collections.Counter(summaries[i]['status'] for i in ids)
        complete = counts['passed'] == len(ids)
        gate_reports[gate] = {'required': len(ids), **{state: counts[state] for state in
                             ('passed', 'failed', 'invalid', 'missing')},
                             'accepted': prior_passed and complete and not errors}
        if any(rows[i] for i in ids) and not prior_passed:
            errors.append(gate + ' executed before preceding gates passed')
        prior_passed = prior_passed and complete
    if any(r.get('run_id') in scheduled and scheduled[r['run_id']]['gate'] == 'G6' for r in outcomes):
        try:
            frozen = timestamp(candidate.get('confirmation_frozen_utc'))
            development = [r for r in outcomes if r.get('run_id') in scheduled and
                           scheduled[r['run_id']]['phase'] == 'development']
            confirmation = [r for r in outcomes if r.get('run_id') in scheduled and
                            scheduled[r['run_id']]['phase'] == 'confirmation']
            require(all(timestamp(r['finished_utc']) <= frozen for r in development) and
                    all(timestamp(r['started_utc']) >= frozen for r in confirmation),
                    'confirmation was not frozen after development and before execution')
        except (EvidenceError, KeyError, TypeError, ValueError) as exc:
            errors.append('confirmation freeze: ' + str(exc))
    return {'schema_version': 1, 'contract_sha256': contract['contract_sha256'],
            'candidate_id': candidate.get('candidate_id') if candidate else None,
            'verification_policy': candidate.get('verification_policy', 'legacy-unspecified') if candidate else None,
            'accepted': not errors and all(g['accepted'] for g in gate_reports.values()),
            'gates': gate_reports, 'populations': contract['denominators'],
            'terminal_passing_without_completion': terminal_only, 'errors': errors,
            'runs': summaries, 'interpretation': 'Overlapping suite outcomes are not independent observations.'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--outcomes', type=Path)
    parser.add_argument('--g0', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    result = report(read_json(args.contract), read_json(args.candidate) if args.candidate else None,
                    read_json(args.outcomes) if args.outcomes else [],
                    read_json(args.g0) if args.g0 else [], args.root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(json.dumps({'accepted': result['accepted'], 'gates': result['gates'],
                      'errors': result['errors']}))
    return 0 if result['accepted'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
