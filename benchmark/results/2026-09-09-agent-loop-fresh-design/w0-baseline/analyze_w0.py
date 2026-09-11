"""W0 analysis: three per-arm rates with intervals, and the explicit
better/equal/worse verdict the plan's acceptance criterion demands.

Reads the W0 runs directory plus the prespecified instrumentation extractor's
output, and emits analysis.json + a printed summary.

Statistics, chosen before any outcome was seen:

  * Per-arm rate over 12 runs with a Clopper-Pearson (exact binomial) 95%
    interval. Exact rather than normal-approximate because n is 12 and the
    rates may sit at 0 or 1.
  * Each policy arm against the plain control by Fisher exact, two-sided.
  * A per-manifest breakdown, because the plan warns explicitly that candidate
    2's outcomes clustered by manifest and "the effective sample size is nearer
    4 than 12". The pooled interval is therefore reported as optimistic.

The verdict wording is fixed in advance so it cannot be tuned to the result:
"better" requires the arm's point estimate to exceed the control AND the Fisher
two-sided p to be below 0.05; "worse" is the mirror; anything else is "equal
(not separated at this sample size)".
"""

from __future__ import annotations

import argparse
import json
from math import comb
from pathlib import Path

ARMS = ['plain-control', 'checkpoint-only', 'candidate-2']
TASKS = [
    'generalize_retractions_original',
    'generalize_retractions_renamed',
    'generalize_retractions_paraphrased',
    'generalize_retractions_distractor',
]
ALPHA = 0.05


def beta_quantile(a: float, b: float, p: float) -> float:
    """Inverse regularised incomplete beta by bisection; adequate at this size."""
    if a <= 0:
        return 0.0
    lo, hi = 0.0, 1.0
    for _ in range(200):
        mid = (lo + hi) / 2
        if betainc(a, b, mid) < p:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def betainc(a: float, b: float, x: float) -> float:
    """Regularised incomplete beta via the binomial identity, integer a/b only."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    n = int(a + b - 1)
    k = int(a)
    # I_x(k, n-k+1) = sum_{j=k}^{n} C(n,j) x^j (1-x)^(n-j)
    return sum(comb(n, j) * x ** j * (1 - x) ** (n - j) for j in range(k, n + 1))


def clopper_pearson(k: int, n: int) -> tuple[float, float]:
    if n == 0:
        return (0.0, 1.0)
    lower = 0.0 if k == 0 else beta_quantile(k, n - k + 1, ALPHA / 2)
    upper = 1.0 if k == n else beta_quantile(k + 1, n - k, 1 - ALPHA / 2)
    return (round(lower, 4), round(upper, 4))


def fisher_two_sided(a: int, b: int, c: int, d: int) -> float:
    """Two-sided Fisher exact on [[a,b],[c,d]] by summing tables no more likely."""
    n = a + b + c + d
    row1, col1 = a + b, a + c
    def prob(x: int) -> float:
        return comb(row1, x) * comb(c + d, col1 - x) / comb(n, col1)
    lo = max(0, col1 - (c + d))
    hi = min(row1, col1)
    observed = prob(a)
    total = sum(prob(x) for x in range(lo, hi + 1) if prob(x) <= observed + 1e-12)
    return min(1.0, total)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--runs', type=Path, required=True)
    parser.add_argument('--instrumentation', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()

    instrumentation = json.loads(args.instrumentation.read_text(encoding='utf-8'))
    by_run = {row['run_id']: row for row in instrumentation['runs']}

    rows = []
    for run_dir in sorted(args.runs.iterdir()):
        attempt = run_dir / 'attempt.json'
        if not attempt.exists():
            continue
        record = json.loads(attempt.read_text(encoding='utf-8'))
        record['instrumentation'] = by_run.get(run_dir.name)
        rows.append(record)

    arms = {}
    for arm in ARMS:
        arm_rows = [r for r in rows if r['arm'] == arm]
        passed = sum(1 for r in arm_rows if r.get('passed'))
        total = len(arm_rows)
        per_manifest = {}
        for task in TASKS:
            task_rows = [r for r in arm_rows if r['task'] == task]
            per_manifest[task] = {
                'passed': sum(1 for r in task_rows if r.get('passed')),
                'total': len(task_rows),
            }
        arms[arm] = {
            'passed': passed,
            'total': total,
            'rate': round(passed / total, 4) if total else None,
            'clopper_pearson_95': clopper_pearson(passed, total) if total else None,
            'manifests_with_at_least_one_pass':
                sum(1 for v in per_manifest.values() if v['passed'] > 0),
            'per_manifest': per_manifest,
        }

    control = arms['plain-control']
    comparisons = {}
    for arm in ARMS[1:]:
        this = arms[arm]
        if not this['total'] or not control['total']:
            continue
        p = fisher_two_sided(this['passed'], this['total'] - this['passed'],
                             control['passed'], control['total'] - control['passed'])
        if this['rate'] > control['rate'] and p < ALPHA:
            verdict = 'better than the plain control'
        elif this['rate'] < control['rate'] and p < ALPHA:
            verdict = 'worse than the plain control'
        else:
            verdict = 'equal to the plain control (not separated at this sample size)'
        comparisons[arm] = {
            'arm_rate': this['rate'],
            'control_rate': control['rate'],
            'fisher_two_sided_p': round(p, 6),
            'verdict': verdict,
        }

    instrumentation_by_arm = {}
    for arm in ARMS:
        arm_rows = [r for r in rows if r['arm'] == arm and r.get('instrumentation')]
        if not arm_rows:
            continue
        ident = sum(r['instrumentation']['identical_replacement_count'] for r in arm_rows)
        denom = sum(r['instrumentation']['apply_patch_total'] for r in arm_rows)
        instrumentation_by_arm[arm] = {
            'identical_replacement_count': ident,
            'apply_patch_total': denom,
            'identical_replacement_rate': round(ident / denom, 4) if denom else None,
            'rejected_final_count': sum(r['instrumentation']['rejected_final_count'] for r in arm_rows),
            'run_command_count': sum(r['instrumentation']['run_command_count'] for r in arm_rows),
            'run_command_repeat_count': sum(r['instrumentation']['run_command_repeat_count'] for r in arm_rows),
            'terminating_reasons': {
                reason: sum(1 for r in arm_rows if r['instrumentation']['terminating_reason'] == reason)
                for reason in sorted({r['instrumentation']['terminating_reason'] for r in arm_rows})
            },
            'mean_forced_opener_rate': round(
                sum(r['instrumentation']['forced_opener_rate'] or 0 for r in arm_rows) / len(arm_rows), 4),
            'runs_hitting_action_wall': sum(
                1 for r in arm_rows if r['instrumentation']['turns'] == 32),
        }

    payload = {
        'schema_version': 1,
        'run_count': len(rows),
        'expected_run_count': 36,
        'complete': len(rows) == 36,
        'arms': arms,
        'comparisons_against_plain_control': comparisons,
        'instrumentation_by_arm': instrumentation_by_arm,
        'caveats': [
            'Outcomes cluster by manifest; the pooled 12-run interval is optimistic '
            'and the effective sample size is nearer 4 than 12.',
            'Execution order is this batch\'s arm-interleaved schedule, not G1\'s '
            'shuffled order; run.py ignores --order-seed under --no-randomize.',
            'This is a baseline/screening measurement. It is not scheduled gate '
            'evidence and must never be pooled with gate outcomes.',
        ],
        'runs': [{k: v for k, v in r.items() if k != 'instrumentation'} for r in rows],
    }
    args.output.write_text(json.dumps(payload, indent=2) + '\n', encoding='utf-8')

    print(f'W0 runs analysed: {len(rows)}/36')
    print()
    print(f'{"arm":<18}{"pass":<8}{"rate":<8}{"95% CI":<20}{"manifests>=1 pass"}')
    for arm in ARMS:
        a = arms[arm]
        if not a['total']:
            continue
        ci = a['clopper_pearson_95']
        print(f'{arm:<18}{str(a["passed"])+"/"+str(a["total"]):<8}'
              f'{a["rate"]:<8}{f"[{ci[0]:.3f}, {ci[1]:.3f}]":<20}'
              f'{a["manifests_with_at_least_one_pass"]}/4')
    print()
    for arm, comparison in comparisons.items():
        print(f'{arm}: {comparison["verdict"]} '
              f'(p={comparison["fisher_two_sided_p"]})')
    print()
    print(f'wrote {args.output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
