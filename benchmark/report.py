"""Validate comparable campaign runs and report robust repeated-run statistics."""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path
import random
import statistics


IDENTITY = ('model_sha256', 'fixture_preparation', 'context_tokens', 'output_reserve',
            'max_turns', 'gpu_layers', 'chat_template', 'temperature', 'seed',
            'task_suite', 'repetitions', 'order_seed', 'randomized_order', 'platform',
            'gpu', 'gpu_index')
BOOTSTRAP_ITERATIONS = 20000
BOOTSTRAP_SEED = 20260901
ANALYSIS_PLAN = {
    'status': 'pre-registered before the 29-task primary matrix',
    'primary_endpoint': 'pass rate across all scheduled task/repetition runs',
    'all_run_timing': 'end-to-end time for every run, including failures',
    'matched_timing': 'paired end-to-end differences only where both harnesses pass',
    'subgroups': ['language', 'category'],
    'confidence_intervals': {
        'level': 0.95,
        'method': 'task-cluster percentile bootstrap',
        'iterations': BOOTSTRAP_ITERATIONS,
        'seed': BOOTSTRAP_SEED,
    },
    'scope': 'this fixture suite, locked model, locked hardware, and cold lifecycle only',
}


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def percentile(values, percent):
    values = sorted(values)
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    position = (len(values) - 1) * percent / 100
    lower, upper = math.floor(position), math.ceil(position)
    if lower == upper:
        return values[lower]
    return values[lower] + (values[upper] - values[lower]) * (position - lower)


def distribution(values):
    values = [float(value) for value in values if value is not None]
    if not values:
        return {'count': 0, 'min': None, 'p50': None, 'p90': None, 'p95': None,
                'max': None, 'mean': None, 'stdev': None}
    return {'count': len(values), 'min': min(values), 'p50': statistics.median(values),
            'p90': percentile(values, 90), 'p95': percentile(values, 95),
            'max': max(values), 'mean': statistics.fmean(values),
            'stdev': statistics.stdev(values) if len(values) > 1 else 0.0}


def _analysis_seed(label):
    digest = hashlib.sha256(label.encode('utf-8')).digest()
    return BOOTSTRAP_SEED ^ int.from_bytes(digest[:8], 'big')


def cluster_bootstrap_interval(records, value_fn, statistic, label):
    """Deterministic 95% task-cluster percentile bootstrap interval."""
    by_task = {}
    for record in records:
        value = value_fn(record)
        if value is not None:
            by_task.setdefault(record['task'], []).append((record, float(value)))
    tasks = sorted(by_task)
    values = [value for task in tasks for _, value in by_task[task]]
    seed = _analysis_seed(label)
    if not values:
        return {'count': 0, 'task_clusters': 0, 'level': 0.95,
                'method': 'task_cluster_percentile_bootstrap',
                'iterations': BOOTSTRAP_ITERATIONS, 'seed': seed,
                'estimate': None, 'lower': None, 'upper': None}
    rng = random.Random(seed)
    estimates = []
    for _ in range(BOOTSTRAP_ITERATIONS):
        sample = []
        for _ in tasks:
            selected = rng.choice(tasks)
            sample.extend(value for _, value in by_task[selected])
        estimates.append(float(statistic(sample)))
    return {'count': len(values), 'task_clusters': len(tasks), 'level': 0.95,
            'method': 'task_cluster_percentile_bootstrap',
            'iterations': BOOTSTRAP_ITERATIONS, 'seed': seed,
            'estimate': float(statistic(values)),
            'lower': percentile(estimates, 2.5), 'upper': percentile(estimates, 97.5)}


def resource_peak(record, key):
    values = []
    for field in ('startup_resource_usage', 'resource_usage',
                  'verification_resource_usage'):
        usage = record.get(field)
        if usage and usage.get(key) is not None:
            values.append(usage[key])
    return max(values) if values else None


def group_records(label, records):
    variants = sorted({record.get('variant') for record in records
                       if record.get('variant') is not None})
    if not variants:
        return {label: records}
    return {f'{label}/{variant}': [record for record in records
                                   if record.get('variant') == variant]
            for variant in variants}


def run_key(record):
    return record['task'], int(record.get('repetition', 1))


def fixture_identity(record):
    return (record.get('fixture_preparation'), record.get('fixture_sha256'),
            record.get('fixture_files'), record.get('protected_files'))


def validate_measurements(groups):
    for name, records in groups.items():
        if not records:
            raise ValueError(f'{name} has no records')
        seeds = {int(record.get('order_seed', -1)) for record in records}
        if len(seeds) != 1 or -1 in seeds:
            raise ValueError(f'{name} has missing or mixed order seeds')
        seed = next(iter(seeds))
        tasks = sorted({record['task'] for record in records})
        repetitions = sorted({int(record.get('repetition', 1)) for record in records})
        expected = [(task, repetition) for repetition in repetitions for task in tasks]
        random.Random(seed).shuffle(expected)
        expected = [(task, repetition, index)
                    for index, (task, repetition) in enumerate(expected, 1)]
        observed = [(record['task'], int(record.get('repetition', 1)),
                     int(record.get('order_index', 0))) for record in records]
        if observed != expected:
            raise ValueError(f'{name} order is not reproducible from seed {seed}')
        for record in records:
            run_id = record.get('run_id', run_key(record))
            metrics = record.get('metrics', {})
            if metrics.get('prompt_tokens', 0) <= 0 or metrics.get('generated_tokens', 0) <= 0:
                raise ValueError(f'{name} has zero or missing tokens for {run_id}')
            timing = record.get('timing', {})
            for key in ('agent_seconds', 'verification_seconds', 'end_to_end_seconds'):
                if timing.get(key) is None or timing[key] <= 0:
                    raise ValueError(f'{name} has incomplete {key} for {run_id}')
            startup = timing.get('startup_seconds')
            if startup is None or startup < 0 or (
                    timing.get('lifecycle') == 'cold' and startup <= 0):
                raise ValueError(f'{name} has incomplete startup_seconds for {run_id}')
            if record.get('protected_files_unchanged') is not True:
                raise ValueError(f'{name} changed a protected file for {run_id}')
            for key in ('peak_process_tree_rss_bytes', 'peak_gpu_used_bytes'):
                value = resource_peak(record, key)
                if value is None or value <= 0:
                    raise ValueError(f'{name} has incomplete {key} for {run_id}')


def validate_failure_artifacts(label, path, records):
    for record in records:
        if record.get('passed'):
            continue
        run_id = record.get('run_id')
        if not run_id or not (Path(path) / run_id / 'failed-workspace').is_dir():
            raise ValueError(f'{label} did not preserve failed workspace for {run_id or run_key(record)}')


def validate_groups(groups):
    baseline_name, baseline = next(iter(groups.items()))
    baseline_by_run = {run_key(record): record for record in baseline}
    if len(baseline_by_run) != len(baseline):
        raise ValueError(f'{baseline_name} repeats a task/repetition')
    for name, records in groups.items():
        by_run = {run_key(record): record for record in records}
        if len(by_run) != len(records):
            raise ValueError(f'{name} repeats a task/repetition')
        if set(by_run) != set(baseline_by_run):
            raise ValueError(f'{name} has a different task/repetition set')
        for key, record in by_run.items():
            if fixture_identity(record) != fixture_identity(baseline_by_run[key]):
                raise ValueError(f'{name} prepared different fixture bytes for {key}')


def _end_to_end(record):
    value = record.get('timing', {}).get('end_to_end_seconds')
    return record.get('wall_seconds') if value is None else value


def summarize_subset(records, label):
    timing = lambda record, key: record.get('timing', {}).get(key)
    metric = lambda record, key: record.get('metrics', {}).get(key)
    passed = sum(bool(record['passed']) for record in records)
    return {
        'tasks': len({record['task'] for record in records}), 'runs': len(records),
        'passed': passed, 'pass_rate': passed / len(records),
        'pass_rate_ci': cluster_bootstrap_interval(
            records, lambda record: float(bool(record['passed'])), statistics.fmean,
            f'{label}:pass-rate'),
        'failures': [record.get('run_id', f'{record["task"]}-r{record.get("repetition", 1)}')
                     for record in records if not record['passed']],
        'end_to_end_seconds': distribution(_end_to_end(record) for record in records),
        'end_to_end_seconds_ci': cluster_bootstrap_interval(
            records, _end_to_end, statistics.median, f'{label}:end-to-end-median'),
        'startup_seconds': distribution(timing(record, 'startup_seconds') for record in records),
        'agent_seconds': distribution(timing(record, 'agent_seconds') for record in records),
        'verification_seconds': distribution(
            timing(record, 'verification_seconds') for record in records),
        'prompt_tokens': distribution(metric(record, 'prompt_tokens') for record in records),
        'prefill_tokens': distribution(metric(record, 'prefill_tokens') for record in records),
        'cached_tokens': distribution(metric(record, 'cached_tokens') for record in records),
        'generated_tokens': distribution(metric(record, 'generated_tokens') for record in records),
        'peak_process_tree_rss_bytes': distribution(
            resource_peak(record, 'peak_process_tree_rss_bytes') for record in records),
        'peak_gpu_used_bytes': distribution(
            resource_peak(record, 'peak_gpu_used_bytes') for record in records),
    }


def summarize(records, label='group'):
    result = summarize_subset(records, label)
    result['per_task'] = {
        task: summarize_subset(
            [record for record in records if record['task'] == task],
            f'{label}:task:{task}')
        for task in sorted({record['task'] for record in records})
    }
    result['per_language'] = {
        language: summarize_subset(
            [record for record in records if record.get('language', 'unknown') == language],
            f'{label}:language:{language}')
        for language in sorted({record.get('language', 'unknown') for record in records})
    }
    result['per_category'] = {
        category: summarize_subset(
            [record for record in records if record.get('category', 'unknown') == category],
            f'{label}:category:{category}')
        for category in sorted({record.get('category', 'unknown') for record in records})
    }
    return result


def pairwise_comparisons(groups):
    comparisons = {}
    for (left_name, left), (right_name, right) in itertools.combinations(groups.items(), 2):
        left_by_run = {run_key(record): record for record in left}
        right_by_run = {run_key(record): record for record in right}
        pairs = []
        matched = []
        for key in sorted(left_by_run):
            left_record, right_record = left_by_run[key], right_by_run[key]
            pair = {
                'task': key[0], 'repetition': key[1],
                'pass_difference': (float(bool(right_record['passed'])) -
                                    float(bool(left_record['passed']))),
            }
            pairs.append(pair)
            if left_record['passed'] and right_record['passed']:
                matched.append({
                    'task': key[0], 'repetition': key[1],
                    'difference_seconds': _end_to_end(right_record) - _end_to_end(left_record),
                })
        pass_interval = cluster_bootstrap_interval(
            pairs, lambda pair: pair['pass_difference'], statistics.fmean,
            f'{right_name}-minus-{left_name}:pass-rate')
        timing_interval = cluster_bootstrap_interval(
            matched, lambda pair: pair['difference_seconds'], statistics.median,
            f'{right_name}-minus-{left_name}:matched-end-to-end')
        comparisons[f'{right_name} minus {left_name}'] = {
            'direction': 'right_minus_left',
            'all_pairs': len(pairs),
            'both_passed_pairs': len(matched),
            'pass_rate_difference': pass_interval['estimate'],
            'pass_rate_difference_ci': pass_interval,
            'matched_end_to_end_difference_seconds': timing_interval['estimate'],
            'matched_end_to_end_difference_seconds_ci': timing_interval,
        }
    return comparisons


def parse_run(value):
    if '=' not in value:
        raise argparse.ArgumentTypeError('--run must be NAME=PATH')
    name, path = value.split('=', 1)
    if not name or not path:
        raise argparse.ArgumentTypeError('--run must be NAME=PATH')
    return name, Path(path)


def format_number(value, digits=2):
    return 'n/a' if value is None else f'{value:.{digits}f}'


def format_interval(interval, digits=2):
    if not interval or interval.get('estimate') is None:
        return 'n/a'
    return (f'{interval["estimate"]:.{digits}f} '
            f'[{interval["lower"]:.{digits}f}, {interval["upper"]:.{digits}f}]')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='append', type=parse_run, required=True,
                        help='comparison input as NAME=RESULT_DIRECTORY; repeat for each harness')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-mixed-lifecycle', action='store_true')
    args = parser.parse_args()
    environments = {}
    groups = {}
    try:
        for label, path in args.run:
            if label in environments:
                parser.error(f'duplicate run label: {label}')
            environments[label] = read(path / 'environment.json')
            records = read(path / 'results.json')
            validate_failure_artifacts(label, path, records)
            groups.update(group_records(label, records))
    except ValueError as error:
        parser.error(str(error))
    _, first_environment = next(iter(environments.items()))
    mismatches = []
    for name, environment in environments.items():
        for key in IDENTITY:
            if environment.get(key) != first_environment.get(key):
                mismatches.append(f'{name}: {key}')
    if mismatches:
        parser.error('run identity differs: ' + ', '.join(mismatches))
    lifecycles = {environment.get('lifecycle', 'legacy') for environment in environments.values()}
    if len(lifecycles) > 1 and not args.allow_mixed_lifecycle:
        parser.error('warm/cold lifecycle differs; report separately or pass --allow-mixed-lifecycle')
    try:
        validate_groups(groups)
        validate_measurements(groups)
    except ValueError as error:
        parser.error(str(error))
    summary = {name: summarize(records, name) for name, records in groups.items()}
    comparisons = pairwise_comparisons(groups)
    report = {'schema_version': 2, 'analysis_plan': ANALYSIS_PLAN,
              'comparable_lifecycle': len(lifecycles) == 1,
              'lifecycles': sorted(lifecycles),
              'identity': {key: first_environment.get(key) for key in IDENTITY},
              'environments': environments, 'groups': summary,
              'pairwise_comparisons': comparisons}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n',
                                              encoding='utf-8')
    lines = ['# Local coding campaign', '',
             f'Lifecycle: {", ".join(sorted(lifecycles))}. '
             f'{"Comparable" if len(lifecycles) == 1 else "Mixed; do not compare timing"}.', '',
             '| Harness | Passed | Pass rate, 95% CI | E2E p50, 95% CI (s) | '
             'E2E p90 (s) | E2E stdev (s) | Generated p50 | '
             'Peak RSS p50 (GiB) | Peak VRAM p50 (GiB) |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for name, values in summary.items():
        e2e = values['end_to_end_seconds']
        pass_ci = values['pass_rate_ci']
        pass_text = (f'{100 * pass_ci["estimate"]:.1f}% '
                     f'[{100 * pass_ci["lower"]:.1f}, {100 * pass_ci["upper"]:.1f}]')
        rss = values['peak_process_tree_rss_bytes']['p50']
        vram = values['peak_gpu_used_bytes']['p50']
        lines.append(f'| {name} | {values["passed"]}/{values["runs"]} | {pass_text} | '
                     f'{format_interval(values["end_to_end_seconds_ci"])} | '
                     f'{format_number(e2e["p90"])} | {format_number(e2e["stdev"])} | '
                     f'{format_number(values["generated_tokens"]["p50"], 0)} | '
                     f'{format_number(rss / (1024 ** 3) if rss is not None else None)} | '
                     f'{format_number(vram / (1024 ** 3) if vram is not None else None)} |')
    lines += ['', '## Pairwise matched analysis', '',
              '| Contrast | All pairs | Both pass | Pass-rate difference, 95% CI | '
              'Matched E2E difference, 95% CI (s) |',
              '| --- | ---: | ---: | ---: | ---: |']
    for name, values in comparisons.items():
        pass_ci = values['pass_rate_difference_ci']
        pass_text = (f'{100 * pass_ci["estimate"]:.1f}% '
                     f'[{100 * pass_ci["lower"]:.1f}, {100 * pass_ci["upper"]:.1f}]')
        lines.append(
            f'| {name} | {values["all_pairs"]} | {values["both_passed_pairs"]} | '
            f'{pass_text} | '
            f'{format_interval(values["matched_end_to_end_difference_seconds_ci"])} |')
    lines += ['', 'Every run and failure remains in its source result directory. Aggregate '
              'timing covers all records; matched timing includes only pairs where both harnesses '
              'pass. Per-task, per-language, and per-category results are in summary.json.',
              '', f'Scope: {ANALYSIS_PLAN["scope"]}.', '']
    (args.output / 'README.md').write_text('\n'.join(lines), encoding='utf-8')
    print(f'Wrote {args.output / "summary.json"} from {len(groups)} groups')


if __name__ == '__main__':
    main()
