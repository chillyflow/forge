"""Consolidate per-arm ablation runs into one auditable result set.

Each arm is run separately (one ``run.py --variants <arm>`` invocation per arm),
so comparability is a claim that must be checked rather than assumed: this tool
refuses to merge arms whose binary, model, fixture preparation or turn cap
differ, and records the shared identity it verified.

It also emits a thought census. The raw ``model_output`` event is written before
routed normalization, so an inline thought appears as a "thought" key while a
routed thought appears as plain text preceding the action object; counting only
the key would report zero for every routed arm regardless of what the model did.

  python3 benchmark/consolidate_arms.py <run-root> <out-dir> [--control <root>]
"""
import argparse
import json
import pathlib
import sys

IDENTITY = ('forge_binary_sha256', 'model_sha256', 'fixture_preparation',
            'context_tokens', 'max_turns')


def classify(data):
    stripped = data.lstrip()
    if stripped.startswith('Thought:'):
        # Host-injected routed cue (FG_THOUGHT_CUE): scaffold, not model
        # reasoning. Only text beyond it counts as a routed prefix.
        stripped = stripped[len('Thought:'):].lstrip()
    if not stripped.startswith('{'):
        return 'routed_prefix'
    try:
        parsed = json.loads(stripped)
    except ValueError:
        return 'none'
    if isinstance(parsed, dict) and 'thought' in parsed:
        return 'inline_thought'
    return 'none'


def census_arm(arm_dir):
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
                counts[classify(event.get('data', ''))] += 1
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
    args = parser.parse_args()

    arms = load_arms(args.root)
    if not arms:
        parser.error('no arm directories with results.json under ' + args.root)
    base, mismatches = check_identity(arms)
    if mismatches:
        print('REFUSING to consolidate: arms differ in run identity', file=sys.stderr)
        print(json.dumps(mismatches, indent=2), file=sys.stderr)
        return 1

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    records, census = [], {}
    for name, (arm_records, _) in arms.items():
        records += arm_records
        total, per_run = census_arm(pathlib.Path(args.root) / name)
        census[name] = {'totals': total, 'per_run': per_run}

    environment = dict(arms[list(arms)[0]][1])
    environment['arms'] = list(arms)
    environment['identity_verified'] = list(IDENTITY)
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
