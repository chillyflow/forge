"""Build the frozen population manifest for the larger TypeSafe repair-feedback
campaign from an engagement-screen run.

A screen run executes every candidate task once under the judge-armed
bounded-repair variant. A task is *engaged* when its run emitted at least one
``judge_feedback`` session event: the advisory TypeSafe call fires exactly when a
candidate validation runs, fails, and carries a summary (see
``append_judge_feedback`` in ``src/core/agent.c``), so the event is the direct
evidence that the mechanism can act on the task. Screen evidence selects the
population; it is never part of a gate record.

Usage::

    python benchmark/engagement_screen.py --protocol PROTOCOL.json \
        --screen SCREEN_DIR --output population-manifest.json
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import digest  # noqa: E402

JUDGE_EVENT = 'judge_feedback'
VALIDATION_EVENT = 'validation_result'
CONTROL_LIMIT = 2
CONTROL_SUITE_PREFERENCE = 'smoke'


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode('utf-8')


def count_events(path):
    """Count judge feedback and candidate validations in one event stream."""
    counts = {'judge_feedback': 0, 'validations': 0, 'failed_validations': 0}
    with Path(path).open(encoding='utf-8', errors='replace') as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = event.get('type')
            if kind == JUDGE_EVENT:
                counts['judge_feedback'] += 1
            elif kind == VALIDATION_EVENT:
                counts['validations'] += 1
                data = event.get('data') or {}
                if data.get('checks_passed') is False or data.get('status') == 'conflict':
                    counts['failed_validations'] += 1
    return counts


def load_cell(screen_dir, record):
    """Collect one screen cell: harness verdict plus mechanism counts."""
    run_dir = Path(screen_dir) / record['run_id']
    events = run_dir / 'session' / 'events.jsonl'
    if not events.is_file():
        events = run_dir / 'stdout.jsonl'
    if not events.is_file():
        raise SystemExit(f'No event stream retained for {record["run_id"]}')
    counts = count_events(events)
    metrics = record.get('metrics') or {}
    return {
        'task': record['task'],
        'variant': record['variant'],
        'run_id': record['run_id'],
        'order_index': record['order_index'],
        'passed': bool(record['passed']),
        'returncode': record['returncode'],
        'turns': metrics.get('turns'),
        'e2e_seconds': round(float(record['wall_seconds']), 3),
        'protected_files_unchanged': record.get('protected_files_unchanged'),
        'verification_inputs_unchanged': record.get('verification_inputs_unchanged'),
        **counts,
    }


def select_population(cells, control_limit=CONTROL_LIMIT,
                      control_suite_preference=CONTROL_SUITE_PREFERENCE):
    """Apply the frozen selection rule: engaged tasks plus inert controls."""
    engaged = sorted((cell for cell in cells if cell['judge_feedback'] >= 1),
                     key=lambda cell: cell['task'])
    inert = [cell for cell in cells
             if cell['judge_feedback'] == 0 and cell['failed_validations'] == 0]
    controls = sorted(inert, key=lambda cell: (cell['suite'] != control_suite_preference,
                                               cell['e2e_seconds'], cell['task']))[:control_limit]
    engaged_ids = {cell['task'] for cell in engaged}
    control_ids = {cell['task'] for cell in controls}
    excluded = sorted((cell for cell in cells
                       if cell['task'] not in engaged_ids and cell['task'] not in control_ids),
                      key=lambda cell: cell['task'])
    if not engaged:
        branch = 'zero_engaged'
    elif len(engaged) < 4:
        branch = 'one_to_three_engaged'
    else:
        branch = 'four_plus_engaged'
    return {'engaged': engaged, 'controls': controls, 'excluded': excluded, 'branch': branch}


def population_entry(cell, candidates, role):
    candidate = candidates[cell['task']]
    return {
        'task': cell['task'],
        'role': role,
        'suite': candidate['suite'],
        'task_file': candidate['task_file'],
        'task_sha256': candidate['sha256'],
        'engaged': cell['judge_feedback'] >= 1,
        'judge_calls': cell['judge_feedback'],
        'failed_candidate_validations': cell['failed_validations'],
        'candidate_validations': cell['validations'],
        'screen_passed': cell['passed'],
        'screen_turns': cell['turns'],
        'screen_e2e_seconds': cell['e2e_seconds'],
    }


def build_manifest(protocol, cells, protocol_path, screen_dir):
    candidates = protocol['candidates']
    for cell in cells:
        cell['suite'] = candidates[cell['task']]['suite']
    selection = select_population(cells)
    interpretation = protocol['selection_rule']['interpretation']
    manifest = {
        'schema_version': 1,
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
        'experiment': 'typesafe-repair-feedback-population-manifest',
        'purpose': 'Frozen population for the larger TypeSafe repair-feedback campaign, '
                   'selected by the engagement screen under the preregistered rule.',
        'selection_rule': protocol['selection_rule'],
        'interpretation_branch': selection['branch'],
        'interpretation_note': interpretation[selection['branch']],
        'screen': {
            'protocol': Path(protocol_path).as_posix(),
            'protocol_sha256_without_self': protocol['protocol_sha256_without_self'],
            'screen_dir': Path(screen_dir).as_posix(),
            'cells': len(cells),
            'order_seed': protocol['screen_settings']['order_seed'],
            'variant': protocol['screen_settings']['variant'],
            'forge_sha256': protocol['identity']['forge_sha256'],
            'model_sha256': protocol['identity']['model_sha256'],
            'judge_config_sha256': protocol['identity']['judge_config_sha256'],
        },
        'population': ([population_entry(cell, candidates, 'engaged') for cell in selection['engaged']]
                       + [population_entry(cell, candidates, 'inert-control') for cell in selection['controls']]),
        'excluded': [population_entry(cell, candidates, 'excluded') for cell in selection['excluded']],
        'summary': {
            'engaged': len(selection['engaged']),
            'inert_controls': len(selection['controls']),
            'excluded': len(selection['excluded']),
        },
    }
    manifest['manifest_sha256_without_self'] = hashlib.sha256(canonical(manifest)).hexdigest()
    return manifest


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--protocol', type=Path, required=True)
    parser.add_argument('--screen', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    protocol = json.loads(args.protocol.read_text(encoding='utf-8'))
    claimed = protocol.get('protocol_sha256_without_self')
    preimage = {key: value for key, value in protocol.items()
                if key != 'protocol_sha256_without_self'}
    actual = hashlib.sha256(canonical(preimage)).hexdigest()
    if claimed != actual:
        raise SystemExit(f'Protocol self-hash mismatch: claimed {claimed}, recomputed {actual}')
    records = json.loads((args.screen / 'results.json').read_text(encoding='utf-8'))
    cells = [load_cell(args.screen, record) for record in records]
    expected = set(protocol['candidates'])
    seen = {cell['task'] for cell in cells}
    if seen != expected or len(cells) != len(seen):
        raise SystemExit('Screen does not cover the frozen candidates exactly: '
                         f'missing={sorted(expected - seen)} extra={sorted(seen - expected)} '
                         f'cells={len(cells)}')
    variant = protocol['screen_settings']['variant']
    for cell in cells:
        if cell['variant'] != variant:
            raise SystemExit(f'Unexpected variant in {cell["run_id"]}: {cell["variant"]}')
        if cell['protected_files_unchanged'] is not True or cell['verification_inputs_unchanged'] is not True:
            raise SystemExit(f'Integrity failure in {cell["run_id"]}')
    manifest = build_manifest(protocol, cells, args.protocol, args.screen)
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('summary:', json.dumps(manifest['summary'], sort_keys=True))
    for entry in manifest['population']:
        print(f"{entry['role']:<14} {entry['task']:<32} judge_calls={entry['judge_calls']} "
              f"failed_validations={entry['failed_candidate_validations']} "
              f"screen={'PASS' if entry['screen_passed'] else 'FAIL'} turns={entry['screen_turns']}")
    print(f"branch={manifest['interpretation_branch']}: {manifest['interpretation_note']}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
