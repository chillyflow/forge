"""Candidate-09 confirmation screen for the beat-contemporaries campaign.

14 runs: same four Python retraction manifests at three repetitions plus two
Go manifests at one repetition, same frozen candidate-09 runtime, same
loop-budget policy and loop-pilot profile as c09. New run ids (c09b-*): this
is a second preregistered look at NEW data, not a re-run of c09.

Preregistered bar, written after c09 but before any run here: confirm at
>=3/12 Python WITH at least 2 manifests covered, and the flag escalates to
broader-suite measurement (populations where success varies across arms);
anything less closes both the success reading and the flag. No gate batch
follows from either screen under any outcome. Outcomes here are never pooled
with c09 for a p-value; the two screens are interpreted jointly and
qualitatively.

This script lives under benchmark/results/ on purpose: benchmark/*.py is
covered by the candidate identity observation and must not gain new files
between freeze and gate.

It writes nothing to any candidate directory and never touches outcomes.json.
Records produced here are screening evidence, not scheduled gate runs.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
CANDIDATE = HERE.parent / 'candidate-09-budget-guidance'

FORGE = CANDIDATE / 'runtime' / 'forge.exe'
MODEL = Path('C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf')
TASK_DIR = ROOT / 'benchmark/results/2026-09-08-repair-control/tasks'
RUN_PY = ROOT / 'benchmark/run.py'

PYTHON_TASKS = [
    'generalize_retractions_original',
    'generalize_retractions_renamed',
    'generalize_retractions_paraphrased',
    'generalize_retractions_distractor',
]
GO_TASKS = [
    'go_api_pagination',
    'go_multifile_transfer',
]

VARIANT = 'loop-budget'
VARIANT_FLAGS = ['--minimal-agent', '--thought-history',
                 '--candidate-checkpoint', '--bounded-repair',
                 '--budget-guidance']

# The loop-pilot profile, byte-for-byte as W0 used it.
PROFILE_FLAGS = [
    '--gpu-layers=-1',
    '--context=16384',
    '--output-reserve=2048',
    '--temperature=0.6',
    '--max-turns=32',
    '--max-tokens=32768',
    '--max-input=262144',
    '--timeout=600',
    '--verification-timeout=120',
    '--order-seed=20260831',
    '--gpu-index=0',
    '--prompt-protocol=native',
]
SEED = 42
PYTHON_REPS = 3


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def schedule() -> list[dict]:
    """Repetition-interleaved order, so wall-clock drift cannot be confounded."""
    rows = []
    index = 0
    for repetition in range(1, PYTHON_REPS + 1):
        for task in PYTHON_TASKS:
            index += 1
            rows.append({
                'order_index': index,
                'run_id': f'c09b-{task}-s{SEED}-r{repetition:03d}',
                'variant': VARIANT,
                'variant_flags': VARIANT_FLAGS,
                'task': task,
                'seed': SEED,
                'repetition': repetition,
            })
        if repetition == 1:
            for task in GO_TASKS:
                index += 1
                rows.append({
                    'order_index': index,
                    'run_id': f'c09b-{task}-s{SEED}-r001',
                    'variant': VARIANT,
                    'variant_flags': VARIANT_FLAGS,
                    'task': task,
                    'seed': SEED,
                    'repetition': 1,
                })
    return rows


def command_for(row: dict, output: Path) -> list[str]:
    return [
        sys.executable,
        str(RUN_PY),
        '--forge', str(FORGE),
        '--model', str(MODEL),
        '--task-dir', str(TASK_DIR),
        '--tasks', row['task'],
        '--suite', 'all',
        '--variants', row['variant'],
        '--retain-terminal',
        '--output', str(output),
        '--repetitions', '1',
        '--no-randomize',
        *PROFILE_FLAGS,
        '--seed', str(row['seed']),
    ]


def write_protocol(rows: list[dict]) -> None:
    target = HERE / 'protocol.json'
    if target.exists():
        return
    protocol = {
        'schema_version': 1,
        'purpose': 'Candidate-09 confirmation: frozen runtime, '
                   'loop-budget policy, new data. Bar: >=3/12 Python with '
                   '>=2 manifests or the success reading closes.',
        'candidate': 'candidate-06-lock-in',
        'frozen_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
        'counts_toward_model_acceptance': False,
        'is_scheduled_gate_evidence': False,
        'forge': {'path': str(FORGE), 'sha256': digest(FORGE)},
        'model': {'path': str(MODEL), 'sha256': digest(MODEL)},
        'task_dir': str(TASK_DIR),
        'manifest_sha256': {
            task: digest(TASK_DIR / f'{task}.json')
            for task in PYTHON_TASKS + GO_TASKS
        },
        'profile': {
            'flags': PROFILE_FLAGS,
            'seed': SEED,
            'python_repetitions': PYTHON_REPS,
        },
        'variant': {'variant': VARIANT, 'flags': VARIANT_FLAGS},
        'schedule': rows,
    }
    target.write_text(json.dumps(protocol, indent=2) + '\n', encoding='utf-8')
    print(f'wrote {target}', flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--limit', type=int, default=0,
                        help='execute at most N pending runs (0 = all)')
    args = parser.parse_args()

    for path in (FORGE, MODEL, TASK_DIR, RUN_PY):
        if not path.exists():
            print(f'missing required path: {path}', file=sys.stderr)
            return 2

    rows = schedule()
    write_protocol(rows)

    runs_dir = HERE / 'runs'
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
        command = command_for(row, harness)
        (run_dir / 'command.json').write_text(json.dumps(command, indent=2) + '\n',
                                              encoding='utf-8')
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        (run_dir / 'started.json').write_text(
            json.dumps({**row, 'started_utc': started}, indent=2) + '\n', encoding='utf-8')

        print(f'[run {row["order_index"]}/{len(rows)}] {row["run_id"]} '
              f'started {started}', flush=True)
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
