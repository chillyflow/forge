"""Summarize the full three-leg campaign."""
import json

legs = {}
for leg in ['forge', 'opencode', 'aider']:
    with open(rf'.scratch\2026-09-01-tranche2-native-full-campaign2\{leg}\results.json') as f:
        legs[leg] = json.load(f)

print('=' * 72)
print('FULL CAMPAIGN SUMMARY (29 tasks x 3 reps = 87 runs per leg)')
print('=' * 72)
for leg, data in legs.items():
    passed = sum(1 for r in data if r.get('passed') is True)
    failed = len(data) - passed
    print('{:10s}: {}/{} passed ({} failed)'.format(leg.upper(), passed, len(data), failed))

print()
print('Per-task breakdown (passes/runs):')
tasks = sorted(set(r['task'] for r in legs['forge']))
header = '{:35s} {:>8s} {:>9s} {:>8s}'.format('task', 'forge', 'opencode', 'aider')
print(header)
print('-' * len(header))
for task in tasks:
    row = []
    for leg in ['forge', 'opencode', 'aider']:
        runs = [r for r in legs[leg] if r['task'] == task]
        p = sum(1 for r in runs if r.get('passed') is True)
        row.append('{}/{}'.format(p, len(runs)))
    print('{:35s} {:>8s} {:>9s} {:>8s}'.format(task, row[0], row[1], row[2]))

print()
print('Failure details:')
for leg in ['forge', 'opencode', 'aider']:
    fails = [r for r in legs[leg] if r.get('passed') is not True]
    if fails:
        print('  {}:'.format(leg.upper()))
        for r in fails:
            print('    {} r{} rc={} wall={:.1f}s'.format(
                r['task'], r['repetition'], r['returncode'], r['wall_seconds']))
