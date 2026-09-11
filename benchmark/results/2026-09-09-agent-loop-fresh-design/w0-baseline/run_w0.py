"""W0 baseline calibration batch for the agent-loop fresh design.

Runs three arms over the four Python retraction manifests at three repetitions
each (36 runs) using the UNCHANGED frozen candidate-2 runtime. No code change is
under test here; this establishes the comparator that the plan's later screens
are measured against.

This script lives under benchmark/results/ on purpose: benchmark/*.py is covered
by the candidate identity observation and must not gain new files between freeze
and gate.

It writes nothing to any candidate directory and never touches outcomes.json.
Records produced here are screening/baseline evidence, not scheduled gate runs.
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

FORGE = ROOT / 'benchmark/results/2026-09-09-agent-loop-all-green/candidate-02/runtime/forge.exe'
MODEL = Path('C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf')
TASK_DIR = ROOT / 'benchmark/results/2026-09-08-repair-control/tasks'
RUN_PY = ROOT / 'benchmark/run.py'

TASKS = [
    'generalize_retractions_original',
    'generalize_retractions_renamed',
    'generalize_retractions_paraphrased',
    'generalize_retractions_distractor',
]

# Arm name -> run.py --variants value. Flags are supplied by run.py's VARIANTS
# table; they are recorded here only so the protocol states them explicitly.
ARMS = [
    ('plain-control', 'minimal', ['--minimal-agent', '--thought-history']),
    ('checkpoint-only', 'candidate-checkpoint',
     ['--minimal-agent', '--thought-history', '--candidate-checkpoint']),
    ('candidate-2', 'loop-repair',
     ['--minimal-agent', '--thought-history', '--candidate-checkpoint', '--bounded-repair']),
]

REPETITIONS = 3

# The loop-pilot profile, byte-for-byte as candidate 2's G1 batch used it.
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


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def schedule() -> list[dict]:
    """Arm-interleaved order, so wall-clock drift cannot be confounded with arm."""
    rows = []
    index = 0
    for repetition in range(1, REPETITIONS + 1):
        for task in TASKS:
            for arm, variant, flags in ARMS:
                index += 1
                rows.append({
                    'order_index': index,
                    'run_id': f'w0-{arm}-{task}-s{SEED}-r{repetition:03d}',
                    'arm': arm,
                    'variant': variant,
                    'variant_flags': flags,
                    'task': task,
                    'seed': SEED,
                    'repetition': repetition,
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
        'purpose': 'W0 baseline calibration: three arms on the four Python '
                   'retraction manifests, three repetitions, unchanged candidate-2 runtime.',
        'frozen_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
        'counts_toward_model_acceptance': False,
        'is_scheduled_gate_evidence': False,
        'forge': {'path': str(FORGE), 'sha256': digest(FORGE)},
        'model': {'path': str(MODEL), 'sha256': digest(MODEL)},
        'task_dir': str(TASK_DIR),
        'manifest_sha256': {
            task: digest(TASK_DIR / f'{task}.json') for task in TASKS
        },
        'profile': {
            'flags': PROFILE_FLAGS,
            'seed': SEED,
            'repetitions': REPETITIONS,
            'note': 'run.py ignores --order-seed under --no-randomize; execution '
                    'order is this protocol\'s arm-interleaved schedule and does '
                    'NOT match G1\'s shuffled order. Recorded, not claimed identical.',
        },
        'arms': [{'arm': a, 'variant': v, 'flags': f} for a, v, f in ARMS],
        'prespecified_instrumentation': [
            'identical_replacement_count and apply_patch denominator',
            'rejected_final_count',
            'run_command_count and near_duplicate_reproduction_count',
            'forced_actions, turns, forced_actions/turns',
            'cumulative prompt_tokens',
            'cached_tokens series per turn',
            'action count and terminating reason (metrics.status)',
        ],
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
            # run.py refuses an existing per-run subdirectory; a half-written
            # attempt is retained, never silently replaced.
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
