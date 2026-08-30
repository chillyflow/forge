"""Paired retention analysis across replicated runs of the two required arms.

The retention question is a paired comparison: on the same fixture, does
retaining the elicited thought in ACTION history (``thought-required``) change
the outcome relative to stripping it (``thought-required-decode-only``)? A
single sweep gave three discordant pairs, all favoring stripping, which is a
direction (two-sided sign test p = 0.25), not a magnitude. Replication turns
each fixture x replicate into one pair; this tool collects every replicate,
refuses mixed identities exactly like consolidate_arms.py, and reports the
discordant pairs with an exact two-sided sign test.

  python3 benchmark/analyze_retention.py <out.json> <run-root> [<run-root> ...]

Each run root must contain ``thought-required/results.json`` and
``thought-required-decode-only/results.json`` as produced by one
``run.py --variants <arm>`` invocation per arm.
"""
import argparse
import json
import math
import pathlib
import sys

RETAINED = 'thought-required'
STRIPPED = 'thought-required-decode-only'
IDENTITY = ('forge_binary_sha256', 'model_sha256', 'fixture_preparation',
            'context_tokens', 'max_turns')


def load_arm(root, arm):
    base = pathlib.Path(root) / arm
    results = json.loads((base / 'results.json').read_text(encoding='utf-8'))
    environment = json.loads((base / 'environment.json').read_text(encoding='utf-8'))
    return {record['task']: bool(record['passed']) for record in results}, environment


def sign_test_two_sided(wins, losses):
    """Exact two-sided sign test: total probability of outcomes no more
    likely than the observed split under a fair coin."""
    trials = wins + losses
    if not trials:
        return None
    observed = math.comb(trials, wins)
    total = sum(math.comb(trials, k) for k in range(trials + 1)
                if math.comb(trials, k) <= observed)
    return min(1.0, total / 2.0 ** trials)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out')
    parser.add_argument('roots', nargs='+')
    args = parser.parse_args()
    identity = None
    replicates = []
    for root in args.roots:
        retained, retained_env = load_arm(root, RETAINED)
        stripped, stripped_env = load_arm(root, STRIPPED)
        for name, environment in ((RETAINED, retained_env), (STRIPPED, stripped_env)):
            current = {key: environment.get(key) for key in IDENTITY}
            if identity is None:
                identity = current
            elif current != identity:
                print(f'REFUSING mixed identity in {root}/{name}: {current} != {identity}',
                      file=sys.stderr)
                return 1
        if sorted(retained) != sorted(stripped):
            print(f'REFUSING mismatched task sets under {root}', file=sys.stderr)
            return 1
        replicates.append((pathlib.Path(root).name, retained, stripped))
    pairs = []
    stripped_only = retained_only = both = neither = 0
    for label, retained, stripped in replicates:
        for task in sorted(retained):
            outcome = (retained[task], stripped[task])
            if outcome == (True, True):
                both += 1
            elif outcome == (False, False):
                neither += 1
            else:
                kind = 'stripped_only' if outcome == (False, True) else 'retained_only'
                stripped_only += kind == 'stripped_only'
                retained_only += kind == 'retained_only'
                pairs.append({'replicate': label, 'task': task, 'kind': kind})
    report = {
        'schema_version': 1,
        'description': 'Paired fixture x replicate outcomes for retained vs stripped '
                       'required thought, with an exact two-sided sign test over the '
                       'discordant pairs.',
        'produced_by': 'benchmark/analyze_retention.py',
        'identity': identity,
        'replicates': [label for label, _, _ in replicates],
        'pairs_total': both + neither + stripped_only + retained_only,
        'both_passed': both,
        'both_failed': neither,
        'stripped_only_passed': stripped_only,
        'retained_only_passed': retained_only,
        'discordant_pairs': pairs,
        'sign_test_two_sided_p': sign_test_two_sided(stripped_only, retained_only),
    }
    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in
                      ('pairs_total', 'both_passed', 'both_failed', 'stripped_only_passed',
                       'retained_only_passed', 'sign_test_two_sided_p')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
