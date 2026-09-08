"""Report gate-5 statistics as Forge-minus-other (the promotion direction)."""
import json
from pathlib import Path

data = json.loads(Path(r'.scratch\2026-09-01-tranche2-native-full-campaign2\report\summary.json')
                  .read_text(encoding='utf-8'))

print('Analysis plan:', data['analysis_plan']['status'])
print('Identity check: all legs share locked identity ->',
      data['comparable_lifecycle'], data['lifecycles'])
print()

print('Per-harness pass rate (cluster bootstrap):')
for name, g in data['groups'].items():
    ci = g['pass_rate_ci']
    print('  {:20s} {:3d}/{:3d}  {:5.1f}%  [{:.1f}, {:.1f}]  clusters={}'.format(
        name, g['passed'], g['runs'], 100 * ci['estimate'],
        100 * ci['lower'], 100 * ci['upper'], ci['task_clusters']))
print()

FORGE = 'forge/optimized'
print('Forge-minus-X contrasts (report stores "A minus B" = A - B):')
for name, c in data['pairwise_comparisons'].items():
    a, b = name.split(' minus ')
    ci = c['pass_rate_difference_ci']
    # a minus b = +ci; forge minus X requires flipping when forge is the right term.
    if b == FORGE:
        est, lo, hi, other = -ci['estimate'], -ci['upper'], -ci['lower'], a
    elif a == FORGE:
        est, lo, hi, other = ci['estimate'], ci['lower'], ci['upper'], b
    else:
        continue
    verdict = 'LOWER BOUND > 0' if lo > 0 else 'straddles zero'
    print('  forge minus {:14s} pass_diff={:+.1f}pp  [{:+.1f}, {:+.1f}]  -> {}'.format(
        other, 100 * est, 100 * lo, 100 * hi, verdict))
    tci = c['matched_end_to_end_difference_seconds_ci']
    if tci.get('estimate') is not None:
        if b == FORGE:
            e, l, u = -tci['estimate'], -tci['upper'], -tci['lower']
        else:
            e, l, u = tci['estimate'], tci['lower'], tci['upper']
        print('  {:26s} matched E2E diff = {:+.2f}s [{:+.2f}, {:+.2f}] '
              '(negative = forge faster)'.format('', e, l, u))
print()
print('Non-forge contrast (context only):')
for name, c in data['pairwise_comparisons'].items():
    a, b = name.split(' minus ')
    if FORGE in (a, b):
        continue
    ci = c['pass_rate_difference_ci']
    lo = ci['lower']
    print('  {:28s} pass_diff={:+.1f}pp  [{:+.1f}, {:+.1f}]  -> {}'.format(
        name, 100 * ci['estimate'], 100 * lo, 100 * ci['upper'],
        'straddles zero' if lo <= 0 <= ci['upper'] else 'significant'))
print()

forge = data['groups']['forge/optimized']
print('Tasks where Forge is not 3/3:')
for task, sub in sorted(forge['per_task'].items()):
    if sub['passed'] != sub['runs']:
        print('  {:32s} {}/{}'.format(task, sub['passed'], sub['runs']))
print()

print('Per-category (forge):')
for cat, sub in sorted(forge['per_category'].items()):
    print('  {:20s} {}/{}'.format(cat, sub['passed'], sub['runs']))
