import argparse
import copy
import json
from pathlib import Path
import re
import statistics
import tempfile

from common import digest, load_tasks, materialize, protected_files, write_json


SOURCE = Path(__file__).parent / 'holdout' / '2026-09-02' / 'tasks'
VARIANTS = ('original', 'renamed', 'paraphrased', 'distractor', 'contrast')
MATCHED_FIELDS = ('model_sha256', 'gpu_layers', 'chat_template', 'prompt_protocol',
                  'context_tokens', 'max_turns', 'output_reserve', 'temperature', 'seed',
                  'repetitions', 'order_seed', 'randomized_order', 'lifecycle', 'gpu_index')


def substitute(task, names):
    pattern = re.compile(r'\b(' + '|'.join(map(re.escape, names)) + r')\b')
    replace = lambda text: pattern.sub(lambda match: names[match.group()], text)
    task['prompt'] = replace(task['prompt'])
    for field in ('files', 'oracle_files'):
        task[field] = {replace(path): replace(text) for path, text in task[field].items()}


def build_tasks():
    tasks = []
    for family, source_name in (('retractions', 'holdout_py_retractions'),
                                ('window', 'holdout_go_window')):
        source_path = SOURCE / f'{source_name}.json'
        original = json.loads(source_path.read_text(encoding='utf-8'))
        for variant in VARIANTS:
            task = copy.deepcopy(original)
            task.update(id=f'generalize_{family}_{variant}', suite='generalization-v1')
            task['generalization'] = {'family': family, 'variant': variant,
                                      'relation': 'contrast' if variant == 'contrast' else 'equivalent',
                                      'source_task': source_name, 'source_sha256': digest(source_path)}
            if variant == 'renamed':
                if family == 'retractions':
                    substitute(task, {'service': 'ledger', 'test_service': 'test_ledger',
                                      'balances': 'positions_for', 'events': 'messages',
                                      'event': 'message', 'seen': 'visited', 'cancelled': 'voided',
                                      'postings': 'entries', 'totals': 'positions', 'account': 'bucket',
                                      'amount': 'delta', 'target': 'reference', 'id': 'key',
                                      'Retractions': 'JournalCases'})
                else:
                    substitute(task, {'service': 'buffer', 'service_test': 'buffer_test',
                                      'Window': 'Accumulator', 'Add': 'Observe', 'Width': 'Horizon',
                                      'sample': 'reading', 'samples': 'readings', 'last': 'previous',
                                      'started': 'initialized', 'first': 'head', 'sum': 'aggregate',
                                      'TestWindow': 'TestAccumulator'})
            elif variant == 'paraphrased':
                if family == 'retractions':
                    task['prompt'] = (
                        'Repair balances: process each globally unique message once. A void notice '
                        'neutralizes its referenced credit, whether the notice comes before or '
                        'after that credit. A credit contributes at most once. Unknown references '
                        'create no accounts; known accounts remain present at zero. Inspect source '
                        'and run the tests. Do not modify tests.')
                    task['files']['test_service.py'] = task['files']['test_service.py'].replace(
                        'class Retractions', 'class BehaviorChecks')
                else:
                    task['prompt'] = (
                        'Repair Window.Add. At time t, retain precisely the readings newer than '
                        't-Width and no later than t. Equal timestamps are valid. Reject a call '
                        'older than the last accepted call without changing any stored state. '
                        'Width is positive. Inspect source and run the tests. Do not modify tests.')
                    task['files']['service_test.go'] = task['files']['service_test.go'].replace(
                        'at %d sum %d', 'tick %d aggregate mismatch: %d').replace(
                        'backdated sample accepted', 'monotonicity contract violated')
            elif variant == 'distractor':
                task['files']['legacy.py' if family == 'retractions' else 'legacy.go'] = (
                    'def replay(value):\n    return value\n' if family == 'retractions' else
                    'package holdout\nfunc Replay(value int) int { return value }\n')
            elif variant == 'contrast':
                if family == 'retractions':
                    task['prompt'] = (
                        'balances replays postings and retractions in arrival order. Event IDs are '
                        'globally idempotent. Unlike deferred cancellation, a retraction whose '
                        'posting has not arrived is ignored permanently and must not consume the '
                        'posting cancellation. A later distinct retraction may cancel that posting '
                        'once. Accounts retain zero entries. Inspect source, repair the implementation, '
                        'and run the tests. Do not modify tests.')
                    task['oracle_files']['service.py'] = task['files']['service.py'].replace(
                        'if target in cancelled:', 'if target in cancelled or target not in postings:')
                    task['files']['test_service.py'] = (
                        'import unittest\nfrom service import balances\n'
                        "def post(i, amount): return {'id': i, 'kind': 'post', 'account': 'cash', 'amount': amount}\n"
                        "def retract(i, target): return {'id': i, 'kind': 'retract', 'target': target}\n"
                        'class Retractions(unittest.TestCase):\n'
                        '    def test_arrival_order(self):\n'
                        "        p = post('p', 7)\n"
                        "        self.assertEqual(balances([retract('early', 'p'), p]), {'cash': 7})\n"
                        "        self.assertEqual(balances([retract('early', 'p'), p, retract('later', 'p')]), {'cash': 0})\n"
                        "        self.assertEqual(balances([p, p, retract('r', 'p'), retract('s', 'p')]), {'cash': 0})\n"
                        "        self.assertEqual(balances([retract('same', 'p'), p, retract('same', 'p')]), {'cash': 7})\n"
                        "        self.assertEqual(balances([retract('r', 'missing')]), {})\n"
                        '        self.assertEqual(balances([]), {})\n')
                else:
                    task['prompt'] = (
                        'Window.Add accepts nondecreasing integer timestamps and sums values in '
                        '[now-width, now], INCLUDING the lower boundary. Rejected out-of-order '
                        'timestamps must leave the window unchanged. Width is positive. Inspect '
                        'the code, repair the implementation, and run the tests. Do not modify tests.')
                    task['files']['service.go'], task['oracle_files']['service.go'] = (
                        task['oracle_files']['service.go'], task['files']['service.go'])
                    task['files']['service_test.go'] = task['files']['service_test.go'].replace(
                        '{5,7,9},{5,-2,7},{10,1,1}', '{5,7,12},{5,-2,10},{10,1,6}').replace(
                        'got!=3', 'got!=8')
            task['protected_files'] = protected_files(task)
            if variant == 'distractor':
                task['protected_files'].append('legacy.py' if family == 'retractions' else 'legacy.go')
            tasks.append(task)
    return tasks


def generate(output):
    tasks = build_tasks()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    for task in tasks:
        write_json(output / f'{task["id"]}.json', task)
    return tasks


def read_run(directory, tasks):
    directory = Path(directory)
    environment = json.loads((directory / 'environment.json').read_text(encoding='utf-8'))
    repetitions = environment.get('repetitions')
    if type(repetitions) is not int or repetitions < 1:
        raise ValueError('Missing positive repetition count')
    if environment.get('harness') != 'forge' or not environment.get('forge_binary_sha256'):
        raise ValueError('Missing Forge binary identity')
    records = json.loads((directory / 'results.json').read_text(encoding='utf-8'))
    expected = {(task_id, repetition) for task_id in tasks
                for repetition in range(1, repetitions + 1)}
    indexed = {}
    fixtures = {}
    for task_id, task in tasks.items():
        with tempfile.TemporaryDirectory(prefix='forge-generalization-check-') as temporary:
            root = Path(temporary)
            fixture = materialize(root, task)
            fixtures[task_id] = (fixture['fixture_sha256'],
                                 {name: digest(root / name) for name in protected_files(task)})
    for record in records:
        key = (record.get('task'), record.get('repetition'))
        if key not in expected or key in indexed or record.get('variant') != 'optimized':
            raise ValueError(f'Unexpected or duplicate run: {key}')
        if type(record.get('passed')) is not bool:
            raise ValueError(f'Missing outcome: {key}')
        if record.get('protected_files_unchanged') is not True:
            raise ValueError(f'Protected fixture mutation: {key}')
        fixture_hash, protected = fixtures[key[0]]
        if record.get('fixture_sha256') != fixture_hash or record.get('protected_files') != protected:
            raise ValueError(f'Fixture identity mismatch: {key}')
        if record.get('metrics', {}).get('simulated') is not False:
            raise ValueError(f'Missing real inference evidence: {key}')
        indexed[key] = record
    if set(indexed) != expected:
        raise ValueError(f'Incomplete run matrix: {len(indexed)}/{len(expected)}')
    return environment, indexed


def compare(task_dir, baseline, candidate):
    tasks = {task['id']: task for _, task in load_tasks(Path(task_dir), 'generalization-v1')}
    expected_ids = {task['id'] for task in build_tasks()}
    if set(tasks) != expected_ids or any(task != next(item for item in build_tasks()
                                                      if item['id'] == task_id)
                                       for task_id, task in tasks.items()):
        raise ValueError('Generalization manifests differ from the versioned generator')
    before_env, before = read_run(baseline, tasks)
    after_env, after = read_run(candidate, tasks)
    for field in MATCHED_FIELDS:
        if field not in before_env or before_env[field] != after_env.get(field):
            raise ValueError(f'Unmatched run condition: {field}')
    families = {}
    for family in ('retractions', 'window'):
        variants = {}
        for variant in VARIANTS:
            task_id = next(task_id for task_id, task in tasks.items()
                           if task['generalization']['family'] == family and
                           task['generalization']['variant'] == variant)
            keys = [key for key in before if key[0] == task_id]
            variants[variant] = {
                'runs': len(keys),
                'baseline_passed': sum(before[key]['passed'] for key in keys),
                'candidate_passed': sum(after[key]['passed'] for key in keys),
                'candidate_failed_runs': [after[key]['run_id'] for key in keys if not after[key]['passed']],
                'baseline_e2e_median': statistics.median(before[key]['timing']['end_to_end_seconds'] for key in keys),
                'candidate_e2e_median': statistics.median(after[key]['timing']['end_to_end_seconds'] for key in keys),
                'baseline_generated_median': statistics.median(before[key]['metrics']['generated_tokens'] for key in keys),
                'candidate_generated_median': statistics.median(after[key]['metrics']['generated_tokens'] for key in keys)}
        families[family] = variants
    return {'schema_version': 1, 'scope': 'development generalization regressions; not a promotion holdout',
            'independent_families': len(families), 'confidence_intervals': None,
            'baseline_binary': before_env['forge_binary_sha256'],
            'candidate_binary': after_env['forge_binary_sha256'],
            'runs_per_candidate': len(after), 'baseline_passed': sum(r['passed'] for r in before.values()),
            'candidate_passed': sum(r['passed'] for r in after.values()),
            'all_candidate_checks_passed': all(r['passed'] for r in after.values()),
            'families': families}


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest='command', required=True)
    generate_parser = commands.add_parser('generate')
    generate_parser.add_argument('--output', type=Path, required=True)
    report_parser = commands.add_parser('compare')
    report_parser.add_argument('--task-dir', type=Path, required=True)
    report_parser.add_argument('--baseline', type=Path, required=True)
    report_parser.add_argument('--candidate', type=Path, required=True)
    report_parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == 'generate':
            print(json.dumps({'tasks': len(generate(args.output)), 'output': str(args.output)}))
            return 0
        if args.output.exists():
            raise FileExistsError(args.output)
        result = compare(args.task_dir, args.baseline, args.candidate)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_json(args.output, result)
        print(json.dumps(result, indent=2))
        return 0 if result['all_candidate_checks_passed'] else 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    raise SystemExit(main())
