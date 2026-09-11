"""Capability stage for the post-Phase-1 campaign (beat-contemporaries).

Two declared arm groups on one frozen binary, same loop-pilot profile.

Group A (deployable minimal-native loop, variant loop-repair /
loop-repair-thinking, output reserve 4096 for both arms so the single
variable is the model and its thinking path):
  a0-qwen-coder     Qwen3-Coder-30B-A3B-Instruct Q4_K_M
  a1-qwen-thinking  Qwen3-30B-A3B-Thinking-2507 Q4_K_M (--enable-thinking)

Group B (ordinary flattened agent, variant optimized, --prompt-protocol
flattened, output reserve 2048). This is the only configuration in which
Devstral runs at this revision: its Mistral template enforces strict
user/assistant alternation and rejects the minimal loop's trailing control
user-message after tool results (render raises "conversation roles must
alternate"; count falls back to a sentinel, which surfaces as a mandatory
prompt limit). The flattened ordinary arm is a declared configuration, not
a capability result.
  b0-qwen-coder-flat  Qwen3-Coder Q4_K_M
  b1-devstral-flat    Devstral-Small-2-24B-Instruct-2512 Q4_K_M

Stage 1 population: six reasoning-gated Go fixtures, 3 reps per arm.
Stage 2 population: four Python retraction fixtures for cleared arms.

Preregistered bars (frozen in protocol.json before the first run):
  G-Model: within a group, an arm clears at >=4/6 manifests each with >=1
           pass AND >=6/18 total passes, no protected-file violations.
           If no arm clears, that group's model-swap axis closes.
  G-Retract: a cleared arm reaches >=4/12 with >=2 manifests.

Screening evidence only: never written to outcomes.json, never pooled with
gate outcomes, never re-run for a better repetition.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

FROZEN = ROOT / 'benchmark/results/2026-09-10-beat-contemporaries/candidate-09-budget-guidance/runtime/forge.exe'
RUN_PY = ROOT / 'benchmark/run.py'

ARMS = {
    'a0-qwen-coder': {
        'model': Path('C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf'),
        'variant': 'loop-repair',
        'protocol': 'native',
        'reserve': 4096,
        'group': 'A-deployable',
    },
    'a1-qwen-thinking': {
        'model': Path('C:/Users/flowc/models/forge/Qwen3-30B-A3B-Thinking-2507-Q4_K_M.gguf'),
        'variant': 'loop-repair-thinking',
        'protocol': 'native',
        'reserve': 4096,
        'group': 'A-deployable',
    },
    'b0-qwen-coder-flat': {
        'model': Path('C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf'),
        'variant': 'optimized',
        'protocol': 'flattened',
        'reserve': 2048,
        'group': 'B-flattened',
    },
    'b1-devstral-flat': {
        'model': Path('C:/Users/flowc/models/forge/Devstral-Small-2-24B-Instruct-2512-Q4_K_M.gguf'),
        'variant': 'optimized',
        'protocol': 'flattened',
        'reserve': 2048,
        'group': 'B-flattened',
    },
}

STAGE1 = {
    'tasks': [
        'reasoning_atomic_transfers',
        'reasoning_dependency_order',
        'reasoning_event_replay',
        'reasoning_interval_union',
        'reasoning_quota_allocation',
        'reasoning_route_specificity',
    ],
    'task_dir': ROOT / 'benchmark/tasks',
    'suite': 'reasoning-gated',
    'reps': 3,
}
STAGE2 = {
    'tasks': [
        'generalize_retractions_original',
        'generalize_retractions_renamed',
        'generalize_retractions_paraphrased',
        'generalize_retractions_distractor',
    ],
    'task_dir': ROOT / 'benchmark/results/2026-09-08-repair-control/tasks',
    'suite': 'all',
    'reps': 3,
}

PROFILE_FLAGS = [
    '--gpu-layers=-1',
    '--context=16384',
    '--temperature=0.6',
    '--max-turns=32',
    '--max-tokens=32768',
    '--max-input=262144',
    '--timeout=600',
    '--verification-timeout=120',
    '--order-seed=20260831',
    '--gpu-index=0',
]
SEED = 42
GROUPS = {
    'A-deployable': ['a0-qwen-coder', 'a1-qwen-thinking'],
    'B-flattened': ['b0-qwen-coder-flat', 'b1-devstral-flat'],
}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def schedule(stage: int, group: str) -> list[dict]:
    spec = STAGE1 if stage == 1 else STAGE2
    rows = []
    index = 0
    for repetition in range(1, spec['reps'] + 1):
        for task in spec['tasks']:
            for arm in GROUPS[group]:
                index += 1
                rows.append({
                    'order_index': index,
                    'run_id': f'cap{stage}-{arm}-{task}-s{SEED}-r{repetition:03d}',
                    'stage': stage,
                    'group': group,
                    'arm': arm,
                    'task': task,
                    'repetition': repetition,
                    'seed': SEED,
                })
    return rows


def command_for(row: dict, spec: dict, output: Path) -> list[str]:
    arm = ARMS[row['arm']]
    return [
        sys.executable,
        str(RUN_PY),
        '--forge', str(FROZEN),
        '--model', str(arm['model']),
        '--task-dir', str(spec['task_dir']),
        '--tasks', row['task'],
        '--suite', spec['suite'],
        '--variants', arm['variant'],
        '--prompt-protocol', arm['protocol'],
        '--retain-terminal',
        '--output', str(output),
        '--repetitions', '1',
        '--no-randomize',
        *PROFILE_FLAGS,
        f"--output-reserve={arm['reserve']}",
        '--seed', str(row['seed']),
    ]


def write_protocol() -> None:
    target = HERE / 'protocol.json'
    if target.exists():
        return
    protocol = {
        'schema_version': 1,
        'purpose': 'Capability stage: two declared arm groups on one frozen '
                   'binary. Group A is the deployable minimal-native loop; '
                   'Group B is the ordinary flattened agent, the only '
                   'configuration in which Devstral runs at this revision.',
        'frozen_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
        'counts_toward_model_acceptance': False,
        'is_scheduled_gate_evidence': False,
        'devstral_native_incompatibility': 'Devstral mistral3 template enforces '
            'strict user/assistant alternation; the minimal loop appends a '
            'control user-message after tool results, which raises in the '
            'template and surfaces as an input-limit error. Recorded finding, '
            'not a capability result.',
        'bars': {
            'G-Model': 'within a group: clear at >=4/6 manifests each with >=1 '
                       'pass AND >=6/18 total; no protected-file violations; '
                       'else that group axis closes',
            'G-Retract': 'cleared arm reaches >=4/12 with >=2 manifests',
        },
        'forge': {'path': str(FROZEN), 'sha256': digest(FROZEN)},
        'models': {name: {'path': str(arm['model']), 'sha256': digest(arm['model'])}
                   for name, arm in ARMS.items()},
        'arms': {name: {'variant': arm['variant'], 'protocol': arm['protocol'],
                        'output_reserve': arm['reserve'], 'group': arm['group']}
                 for name, arm in ARMS.items()},
        'profile': {'flags': PROFILE_FLAGS, 'seed': SEED},
        'stage1': {
            'task_dir': str(STAGE1['task_dir']), 'suite': STAGE1['suite'],
            'tasks': STAGE1['tasks'], 'repetitions': STAGE1['reps'],
            'manifest_sha256': {t: digest(STAGE1['task_dir'] / f'{t}.json')
                                for t in STAGE1['tasks']},
        },
        'stage2': {
            'task_dir': str(STAGE2['task_dir']), 'suite': STAGE2['suite'],
            'tasks': STAGE2['tasks'], 'repetitions': STAGE2['reps'],
            'manifest_sha256': {t: digest(STAGE2['task_dir'] / f'{t}.json')
                                for t in STAGE2['tasks']},
        },
    }
    target.write_text(json.dumps(protocol, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {target}', flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--stage', type=int, choices=(1, 2), required=True)
    parser.add_argument('--group', choices=sorted(GROUPS), required=True)
    parser.add_argument('--arms', nargs='+', choices=sorted(ARMS), default=None,
                        help='optional subset of the group arms (default: the whole group)')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--limit', type=int, default=0)
    args = parser.parse_args()

    for path in (FROZEN, RUN_PY):
        if not path.exists():
            print(f'missing required path: {path}', file=sys.stderr)
            return 2
    for arm in (args.arms or GROUPS[args.group]):
        if not ARMS[arm]['model'].exists():
            print(f'missing model: {ARMS[arm]["model"]}', file=sys.stderr)
            return 2

    write_protocol()
    spec = STAGE1 if args.stage == 1 else STAGE2
    rows = schedule(args.stage, args.group)
    if args.arms:
        rows = [row for row in rows if row['arm'] in args.arms]
        for index, row in enumerate(rows, 1):
            row['order_index'] = index

    runs_dir = HERE / f'stage{args.stage}' / args.group / 'runs'
    runs_dir.mkdir(parents=True, exist_ok=True)

    executed = 0
    for row in rows:
        run_dir = runs_dir / row['run_id']
        harness = run_dir / 'harness'
        results = harness / 'results.json'
        if results.exists():
            print(f'[skip] {row["run_id"]} already complete', flush=True)
            continue
        if harness.exists():
            print(f'[halt] {row["run_id"]} has a started-but-incomplete harness dir; '
                  f'inspect {harness} and move it aside deliberately', flush=True)
            return 3
        if args.limit and executed >= args.limit:
            print(f'[stop] limit {args.limit} reached', flush=True)
            break

        run_dir.mkdir(parents=True, exist_ok=True)
        command = command_for(row, spec, harness)
        (run_dir / 'command.json').write_text(json.dumps(command, indent=2) + '\n',
                                              encoding='utf-8')
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        (run_dir / 'started.json').write_text(
            json.dumps({**row, 'started_utc': started}, indent=2) + '\n', encoding='utf-8')

        print(f'[run {row["order_index"]}/{len(rows)}] {row["run_id"]} started {started}',
              flush=True)
        if args.dry_run:
            executed += 1
            continue

        with (run_dir / 'harness.log').open('wb') as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=str(ROOT))
        finished = dt.datetime.now(dt.timezone.utc).isoformat()

        passed = None
        if results.exists():
            records = json.loads(results.read_text(encoding='utf-8'))
            if isinstance(records, list) and records:
                passed = bool(records[0].get('passed'))
        (run_dir / 'attempt.json').write_text(json.dumps({
            **row,
            'started_utc': started,
            'finished_utc': finished,
            'returncode': completed.returncode,
            'passed': passed,
        }, indent=2) + '\n', encoding='utf-8')
        print(f'[done] {row["run_id"]} rc={completed.returncode} passed={passed} '
              f'finished {finished}', flush=True)
        executed += 1

    print(f'executed {executed} run(s)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
