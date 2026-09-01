"""Consolidate per-arm ablation runs into one auditable result set.

Each arm is run separately, so comparability is a claim that must be checked
rather than assumed: this tool refuses to merge arms whose binary, model,
fixture preparation, schedule, or decode configuration differ, and records the
shared identity it verified.  Arm directory names describe the treatment; they
do not have to match the variant label recorded by ``run.py``.

It also emits a thought census. The raw ``model_output`` event is written before
routed normalization, so an inline thought appears as a "thought" key while a
routed thought appears as plain text preceding the action object; counting only
the key would report zero for every routed arm regardless of what the model did.

  python3 benchmark/consolidate_arms.py <run-root> <out-dir> [--control <root>]
"""
import argparse
import json
import pathlib
import re
import sys

IDENTITY = ('forge_binary_sha256', 'model_sha256', 'fixture_preparation',
            'context_tokens', 'max_turns', 'gpu_layers', 'chat_template',
            'task_suite', 'output_reserve', 'temperature', 'seed',
            'repetitions', 'order_seed', 'randomized_order', 'lifecycle',
            'platform', 'go_version', 'gpu')
PROMPT_PROTOCOLS = ('flattened', 'native')
RECORD_IDENTITY = ('task', 'variant', 'repetition')


def classify(data, cue='Thought: '):
    stripped = data.lstrip()
    if cue and stripped.startswith(cue.strip()):
        # Host-injected routed cue: scaffold, not model reasoning.  The cue is
        # recorded by run.py so custom-cue arms remain census-correct.
        stripped = stripped[len(cue.strip()):].lstrip()
    native_call = re.search(r'<tool_call\b[^>]*>', stripped)
    if native_call:
        return 'routed_prefix' if stripped[:native_call.start()].strip() else 'none'
    match = re.search(r'\{[ \t\r\n]*"(?:tool|memory|final)"[ \t\r\n]*:', stripped)
    if match and stripped[:match.start()].strip():
        return 'routed_prefix'
    if match:
        stripped = stripped[match.start():]
    elif not stripped.startswith('{'):
        return 'none'
    try:
        parsed = json.loads(stripped)
    except ValueError:
        return 'none'
    if isinstance(parsed, dict) and 'thought' in parsed:
        return 'inline_thought'
    return 'none'


def census_arm(arm_dir, cue):
    total = {'model_actions': 0, 'inline_thought': 0, 'routed_prefix': 0, 'none': 0}
    per_run = {}
    for log in sorted(arm_dir.glob('*/stdout.jsonl')):
        counts = {'model_actions': 0, 'inline_thought': 0, 'routed_prefix': 0, 'none': 0}
        for line in log.read_text(encoding='utf-8').splitlines():
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if event.get('type') == 'model_output':
                counts['model_actions'] += 1
                counts[classify(event.get('data', ''), cue)] += 1
        per_run[log.parent.name] = counts
        for key in total:
            total[key] += counts[key]
    total['actions_with_thought'] = total['inline_thought'] + total['routed_prefix']
    return total, per_run


def load_arms(root):
    arms = {}
    for entry in sorted(pathlib.Path(root).iterdir()):
        if entry.is_dir() and (entry / 'results.json').exists():
            arms[entry.name] = (
                json.loads((entry / 'results.json').read_text(encoding='utf-8')),
                json.loads((entry / 'environment.json').read_text(encoding='utf-8')))
    return arms


def check_identity(arms):
    """Refuse to publish arms that are not actually comparable."""
    names = list(arms)
    base = {key: arms[names[0]][1].get(key) for key in IDENTITY}
    mismatches = {}
    for name in names[1:]:
        differing = {key: arms[name][1].get(key) for key in IDENTITY
                     if arms[name][1].get(key) != base[key]}
        if differing:
            mismatches[name] = differing
    return base, mismatches


def prompt_protocol_provenance(arms):
    """Describe the treatment arm without mistaking it for shared run identity."""
    by_arm = {name: environment.get('prompt_protocol', 'unrecorded')
              for name, (_, environment) in arms.items()}
    protocols = set(by_arm.values())
    return (next(iter(protocols)) if len(protocols) == 1 else 'mixed'), by_arm


def check_prompt_protocol_provenance(arms):
    """Require each arm's records to agree with its declared prompt protocol."""
    protocol, by_arm = prompt_protocol_provenance(arms)
    mismatches = {}
    for name, (records, environment) in arms.items():
        expected = environment.get('prompt_protocol')
        differences = {}
        if expected not in PROMPT_PROTOCOLS:
            differences['environment_prompt_protocol'] = expected or 'unrecorded'
        recorded = sorted({record.get('prompt_protocol') or 'unrecorded'
                           for record in records})
        if records and recorded != [expected]:
            differences['record_prompt_protocols'] = recorded
        if name in PROMPT_PROTOCOLS and expected != name:
            differences['arm_label_prompt_protocol'] = {
                'arm': name, 'environment': expected or 'unrecorded'}
        if differences:
            mismatches[name] = differences
    return protocol, by_arm, mismatches


def _record_identity(record):
    return tuple(record.get(key) for key in RECORD_IDENTITY)


def _display_record_identities(identities):
    ordered = sorted(identities, key=lambda identity: tuple(
        '' if value is None else str(value) for value in identity))
    return [dict(zip(RECORD_IDENTITY, identity)) for identity in ordered]


def check_records(arms):
    """Require an identical fixture/task/variant/repetition set in every arm."""
    names = list(arms)
    expected = None
    mismatches = {}
    for name in names:
        records, environment = arms[name]
        seen = {}
        duplicates = []
        invalid = []
        repetitions = {}
        for record in records:
            identity = _record_identity(record)
            valid_identity = (isinstance(identity[0], str) and bool(identity[0])
                              and isinstance(identity[1], str) and bool(identity[1])
                              and isinstance(identity[2], int)
                              and not isinstance(identity[2], bool)
                              and identity[2] > 0)
            if not valid_identity:
                invalid.append(identity)
            if identity in seen:
                duplicates.append(identity)
            else:
                seen[identity] = (
                    record.get('fixture_preparation'), record.get('fixture_sha256'),
                    record.get('fixture_files'), record.get('suite'),
                    record.get('order_index'))
            if valid_identity:
                repetitions.setdefault(identity[:2], set()).add(identity[2])
        if expected is None:
            expected = seen
        differences = {}
        if seen != expected:
            expected_ids, seen_ids = set(expected), set(seen)
            differences['missing_records'] = _display_record_identities(expected_ids - seen_ids)
            differences['unexpected_records'] = _display_record_identities(
                seen_ids - expected_ids)
            changed = {identity for identity in expected_ids & seen_ids
                       if expected[identity] != seen[identity]}
            differences['changed_fixtures_or_order'] = _display_record_identities(changed)
        if duplicates:
            differences['duplicate_records'] = _display_record_identities(set(duplicates))
        if invalid:
            differences['invalid_record_identities'] = _display_record_identities(set(invalid))
        declared_repetitions = environment.get('repetitions')
        if isinstance(declared_repetitions, int) and declared_repetitions > 0:
            complete = set(range(1, declared_repetitions + 1))
            incomplete = {identity: sorted(observed)
                          for identity, observed in repetitions.items()
                          if observed != complete}
            if incomplete:
                differences['incomplete_repetitions'] = [
                    {'task': identity[0], 'variant': identity[1],
                     'observed': observed, 'expected': sorted(complete)}
                    for identity, observed in sorted(
                        incomplete.items(), key=lambda item: tuple(str(value)
                                                                  for value in item[0]))]
        if differences:
            mismatches[name] = differences
    tasks = {identity[0] for identity in (expected or {})
             if isinstance(identity[0], str)}
    return sorted(tasks), mismatches


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def control_summary(control_arms, arms, base):
    records, summary = [], {}
    for name, (arm_records, arm_env) in control_arms.items():
        records += arm_records
        passing = sorted(r['task'] for r in arm_records if r['passed'])
        entry = {'max_turns_control': arm_env.get('max_turns'),
                 'forge_binary_sha256': arm_env.get('forge_binary_sha256'),
                 'passed_control': len(passing),
                 'passing_tasks_control': passing,
                 'turns_control': sum(r['metrics']['turns'] for r in arm_records)}
        baseline = arms.get(name)
        if baseline:
            base_pass = sorted(r['task'] for r in baseline[0] if r['passed'])
            entry.update({
                'max_turns_baseline': baseline[1].get('max_turns'),
                'passed_baseline': len(base_pass),
                'passing_tasks_baseline': base_pass,
                'turns_baseline': sum(r['metrics']['turns'] for r in baseline[0]),
                'identical_task_set': base_pass == passing,
                'same_binary_as_baseline':
                    arm_env.get('forge_binary_sha256') == base['forge_binary_sha256']})
        summary[name] = entry
    return records, summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root')
    parser.add_argument('out')
    parser.add_argument('--control',
                        help='root of a same-binary control sweep, e.g. a turn-cap control')
    parser.add_argument('--expect-arms', nargs='+', help='refuse missing or unexpected arms')
    parser.add_argument('--expect-tasks', nargs='+', help='refuse missing or unexpected tasks')
    args = parser.parse_args()

    arms = load_arms(args.root)
    if not arms:
        parser.error('no arm directories with results.json under ' + args.root)
    base, mismatches = check_identity(arms)
    if mismatches:
        print('REFUSING to consolidate: arms differ in run identity', file=sys.stderr)
        print(json.dumps(mismatches, indent=2), file=sys.stderr)
        return 1
    protocol, protocol_by_arm, protocol_mismatches = check_prompt_protocol_provenance(arms)
    if protocol_mismatches:
        print('REFUSING to consolidate: prompt protocol provenance is invalid',
              file=sys.stderr)
        print(json.dumps(protocol_mismatches, indent=2), file=sys.stderr)
        return 1
    task_set, record_mismatches = check_records(arms)
    if record_mismatches:
        print('REFUSING to consolidate: arms differ in task/fixture identity', file=sys.stderr)
        print(json.dumps(record_mismatches, indent=2), file=sys.stderr)
        return 1
    if args.expect_arms and sorted(args.expect_arms) != sorted(arms):
        print('REFUSING to consolidate: arm set differs from --expect-arms', file=sys.stderr)
        return 1
    if args.expect_tasks and sorted(args.expect_tasks) != task_set:
        print('REFUSING to consolidate: task set differs from --expect-tasks', file=sys.stderr)
        return 1

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    records, census = [], {}
    for name, (arm_records, _) in arms.items():
        records += arm_records
        sample = arms[name][0][0] if arms[name][0] else {}
        total, per_run = census_arm(pathlib.Path(args.root) / name,
                                    sample.get('thought_cue'))
        census[name] = {'totals': total, 'per_run': per_run}

    environment = dict(arms[list(arms)[0]][1])
    environment['prompt_protocol'] = protocol
    environment['prompt_protocol_by_arm'] = protocol_by_arm
    environment['arms'] = list(arms)
    environment['identity_verified'] = list(IDENTITY)
    environment['record_identity_verified'] = list(RECORD_IDENTITY)
    environment['task_set_verified'] = task_set
    environment['consolidated_by'] = 'benchmark/consolidate_arms.py'
    write(out / 'environment.json', environment)
    write(out / 'results.json', records)
    write(out / 'thought-census.json', {
        'schema_version': 2,
        'description': ('Per-arm and per-run count of model actions carrying reasoning, '
                        'counted from raw model_output events. An inline thought is a '
                        '"thought" key; a routed thought is non-whitespace text before '
                        'the action object.'),
        'produced_by': 'benchmark/consolidate_arms.py',
        'per_arm': {name: census[name]['totals'] for name in arms},
        'per_run': {name: census[name]['per_run'] for name in arms}})

    if args.control:
        control_records, summary = control_summary(load_arms(args.control), arms, base)
        write(out / 'turncap-control-results.json', control_records)
        write(out / 'turncap-control.json', {
            'schema_version': 3,
            'description': ('Confound control: the same arms re-run at a higher turn cap. '
                            'Per-run records are in turncap-control-results.json.'),
            'produced_by': 'benchmark/consolidate_arms.py',
            'control': summary})

    print('consolidated %d records from %d arms' % (len(records), len(arms)))
    print('verified shared identity: '
          + ', '.join('%s=%s' % (key, base[key]) for key in IDENTITY))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
