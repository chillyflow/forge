"""Validate comparable campaign runs and report robust repeated-run statistics."""
import argparse
import json
import math
from pathlib import Path
import statistics


IDENTITY = ('model_sha256', 'fixture_preparation', 'context_tokens', 'output_reserve',
            'max_turns', 'gpu_layers', 'chat_template', 'temperature', 'seed',
            'task_suite', 'repetitions', 'order_seed', 'randomized_order', 'platform',
            'gpu', 'gpu_index')


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
            record.get('fixture_files'))


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


def summarize(records):
    timing = lambda record, key: record.get('timing', {}).get(key)
    metric = lambda record, key: record.get('metrics', {}).get(key)
    tasks = sorted({record['task'] for record in records})
    per_task = {}
    for task in tasks:
        selected = [record for record in records if record['task'] == task]
        per_task[task] = {
            'runs': len(selected), 'passed': sum(bool(record['passed']) for record in selected),
            'end_to_end_seconds': distribution(
                timing(record, 'end_to_end_seconds') or record.get('wall_seconds')
                for record in selected),
            'generated_tokens': distribution(metric(record, 'generated_tokens')
                                             for record in selected)}
    return {
        'tasks': len(tasks), 'runs': len(records),
        'passed': sum(bool(record['passed']) for record in records),
        'pass_rate': sum(bool(record['passed']) for record in records) / len(records),
        'failures': [record.get('run_id', f'{record["task"]}-r{record.get("repetition", 1)}')
                     for record in records if not record['passed']],
        'end_to_end_seconds': distribution(
            timing(record, 'end_to_end_seconds') or record.get('wall_seconds')
            for record in records),
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
        'per_task': per_task,
    }


def parse_run(value):
    if '=' not in value:
        raise argparse.ArgumentTypeError('--run must be NAME=PATH')
    name, path = value.split('=', 1)
    if not name or not path:
        raise argparse.ArgumentTypeError('--run must be NAME=PATH')
    return name, Path(path)


def format_number(value, digits=2):
    return 'n/a' if value is None else f'{value:.{digits}f}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', action='append', type=parse_run, required=True,
                        help='comparison input as NAME=RESULT_DIRECTORY; repeat for each harness')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-mixed-lifecycle', action='store_true')
    args = parser.parse_args()
    environments = {}
    groups = {}
    for label, path in args.run:
        if label in environments:
            parser.error(f'duplicate run label: {label}')
        environments[label] = read(path / 'environment.json')
        groups.update(group_records(label, read(path / 'results.json')))
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
    except ValueError as error:
        parser.error(str(error))
    summary = {name: summarize(records) for name, records in groups.items()}
    report = {'schema_version': 1, 'comparable_lifecycle': len(lifecycles) == 1,
              'lifecycles': sorted(lifecycles),
              'identity': {key: first_environment.get(key) for key in IDENTITY},
              'environments': environments, 'groups': summary}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n',
                                              encoding='utf-8')
    lines = ['# Local coding campaign', '',
             f'Lifecycle: {", ".join(sorted(lifecycles))}. '
             f'{"Comparable" if len(lifecycles) == 1 else "Mixed; do not compare timing"}.', '',
             '| Harness | Passed | E2E p50 (s) | E2E p90 (s) | E2E stdev (s) | '
             'Generated p50 | Peak RSS p50 (GiB) | Peak VRAM p50 (GiB) |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    for name, values in summary.items():
        e2e = values['end_to_end_seconds']
        rss = values['peak_process_tree_rss_bytes']['p50']
        vram = values['peak_gpu_used_bytes']['p50']
        lines.append(f'| {name} | {values["passed"]}/{values["runs"]} | '
                     f'{format_number(e2e["p50"])} | {format_number(e2e["p90"])} | '
                     f'{format_number(e2e["stdev"])} | '
                     f'{format_number(values["generated_tokens"]["p50"], 0)} | '
                     f'{format_number(rss / (1024 ** 3) if rss is not None else None)} | '
                     f'{format_number(vram / (1024 ** 3) if vram is not None else None)} |')
    lines += ['', 'Every run and failure remains in its source result directory. Aggregate '
              'distributions cover all run records; per-task repetition distributions are in '
              '`summary.json`.', '']
    (args.output / 'README.md').write_text('\n'.join(lines), encoding='utf-8')
    print(f'Wrote {args.output / "summary.json"} from {len(groups)} groups')


if __name__ == '__main__':
    main()
