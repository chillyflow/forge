"""Archive a complete rejected evaluation without changing its validation rules.

This post-run evidence helper is not part of the frozen evaluator. It invokes
the frozen strict auditor, requires its rejection, and preserves the rejection
alongside every original record. It never computes replacement confidence
intervals, modifies an outcome, or substitutes a repetition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str((args.source_root / 'benchmark').resolve()))
    import export_evidence as evidence
    from common import schedule

    lock = evidence.read(args.source / 'protocol-lock.json')
    identity = {k: v for k, v in lock.items()
                if k not in ('protocol_sha256', 'created_utc')}
    encoded = json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()
    evidence.require(hashlib.sha256(encoded).hexdigest() == lock['protocol_sha256'],
                     'Lock hash differs')
    evidence.require(lock['clean_revision_required'] and not lock['git_status_at_freeze'],
                     'Freeze was not clean')
    for name, expected in lock['source_files'].items():
        evidence.require(evidence.digest(args.source_root / name) == expected['sha256'],
                         f'Frozen source changed: {name}')
    for name in ('export_evidence.py', 'report.py', 'common.py'):
        evidence.require(evidence.digest(Path(evidence.__file__).parent / name) ==
                         lock['source_files']['benchmark/' + name]['sha256'],
                         f'Local audit dependency differs from frozen source: {name}')
    try:
        evidence.audit(args.source, args.source_root)
    except ValueError as error:
        rejection = str(error)
    else:
        parser.error('Strict audit passed; use export_evidence.py for valid evidence')

    config = lock['configuration']
    expected_order = list(schedule(sorted(lock['tasks']), config['repetitions'],
                                   config['order_seed']))
    checked = {
        'status': 'rejected',
        'strict_audit_error': rejection,
        'reporter_measurement_checks_passed': False,
        'comparative_claim_valid': False,
        'git_revision': lock['git_revision'],
        'protocol_sha256': lock['protocol_sha256'],
        'audit_tool_sha256': evidence.digest(Path(evidence.__file__)),
        'archive_tool_sha256': evidence.digest(Path(__file__)),
        'source_directory': str(args.source.resolve()),
        'clean_freeze': True,
        'source_hashes_match': True,
        'source_file_count': len(lock['source_files']),
        'complete_schedules': True,
        'protected_file_violations': [],
        'legs': {},
    }
    for leg in ('forge', 'opencode', 'aider'):
        origin = args.source / leg
        rows = evidence.read(origin / 'results.json')
        observed = [(r['task'], r['repetition'], r['order_index']) for r in rows]
        evidence.require(observed == expected_order, f'Incomplete {leg} schedule')
        for row in rows:
            evidence.require(row == evidence.read(origin / row['run_id'] / 'result.json'),
                             f'Per-run record differs: {leg}/{row["run_id"]}')
            if not row['protected_files_unchanged']:
                checked['protected_file_violations'].append({
                    'harness': leg, 'run_id': row['run_id'], 'passed': row['passed']})
        checked['legs'][leg] = {
            'scheduled': len(expected_order), 'runs': len(rows),
            'passed': sum(r['passed'] for r in rows),
            'failed': [r['run_id'] for r in rows if not r['passed']],
        }
    evidence.export(args.source, args.output, checked)
    try:
        evidence.audit(args.output, args.source_root)
    except ValueError as error:
        evidence.require(str(error) == rejection, 'Export changed the strict audit rejection')
    else:
        raise ValueError('Export unexpectedly passed the strict audit')

    # Verify copied bytes, including failed fixture sources, against the originals.
    copied = {}
    for target in args.output.rglob('*'):
        if not target.is_file():
            continue
        relative = target.relative_to(args.output)
        original = args.source / relative
        if original.is_file():
            actual = evidence.digest(target)
            evidence.require(actual == evidence.digest(original),
                             f'Exported bytes differ: {relative}')
            copied[relative.as_posix()] = actual
    evidence.write_json(args.output / 'export-integrity.json', {
        'copied_files_verified': len(copied), 'sha256': copied,
        'note': 'Byte identity verifies retention, not evaluation validity.',
    })
    print(json.dumps(checked, indent=2))


if __name__ == '__main__':
    main()
