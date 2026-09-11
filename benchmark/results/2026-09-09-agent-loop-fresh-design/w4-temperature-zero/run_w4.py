"""W4 arm 1: the Python retraction family at temperature 0 under the 32-action budget.

A separately declared arm, as the plan requires. It is NOT a substitute for a G1
outcome and G1's profile is unchanged.

Why this arm exists: the only prior temperature-0 evidence for this family
(2026-09-08-repair-control) used a 16-action budget, so sampler variance and
capability are entangled with the budget. This holds every other setting equal
to W0's candidate-2 arm and changes only the temperature, so the comparison is
single-variable: W0 candidate-2 at temperature 0.6 measured 4/12 on exactly this
population, profile and frozen runtime.

Uses the unchanged candidate-2 runtime, so no code change is under test.
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
VARIANT = 'loop-repair'
REPETITIONS = 3
SEED = 42

# Identical to W0 except --temperature.
PROFILE_FLAGS = [
    '--gpu-layers=-1', '--context=16384', '--output-reserve=2048',
    '--temperature=0', '--max-turns=32', '--max-tokens=32768',
    '--max-input=262144', '--timeout=600', '--verification-timeout=120',
    '--order-seed=20260831', '--gpu-index=0', '--prompt-protocol=native',
]


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def schedule() -> list[dict]:
    rows, index = [], 0
    for repetition in range(1, REPETITIONS + 1):
        for task in TASKS:
            index += 1
            rows.append({
                'order_index': index,
                'run_id': f'w4-temp0-{task}-s{SEED}-r{repetition:03d}',
                'task': task, 'variant': VARIANT,
                'seed': SEED, 'repetition': repetition,
            })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    for path in (FORGE, MODEL, TASK_DIR, RUN_PY):
        if not path.exists():
            print(f'missing required path: {path}', file=sys.stderr)
            return 2

    rows = schedule()
    protocol = HERE / 'protocol.json'
    if not protocol.exists():
        protocol.write_text(json.dumps({
            'schema_version': 1,
            'purpose': 'W4 arm 1: temperature 0 under the unchanged 32-action budget.',
            'frozen_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'counts_toward_model_acceptance': False,
            'is_scheduled_gate_evidence': False,
            'substitutes_for_a_gate_outcome': False,
            'comparator': 'W0 candidate-2 arm, temperature 0.6, same population/profile/runtime, 4/12',
            'single_variable_change': 'temperature 0.6 -> 0',
            'forge': {'path': str(FORGE), 'sha256': digest(FORGE)},
            'model': {'path': str(MODEL), 'sha256': digest(MODEL)},
            'variant': VARIANT,
            'profile_flags': PROFILE_FLAGS,
            'manifest_sha256': {t: digest(TASK_DIR / f'{t}.json') for t in TASKS},
            'schedule': rows,
        }, indent=2) + '\n', encoding='utf-8')
        print(f'wrote {protocol}', flush=True)

    executed = 0
    for row in rows:
        run_dir = HERE / 'runs' / row['run_id']
        harness = run_dir / 'harness'
        if (harness / 'results.json').exists():
            print(f'[skip] {row["run_id"]} already complete', flush=True)
            continue
        if harness.exists():
            print(f'[halt] {row["run_id"]} started but incomplete; inspect {harness}', flush=True)
            return 3
        command = [
            sys.executable, str(RUN_PY),
            '--forge', str(FORGE), '--model', str(MODEL),
            '--task-dir', str(TASK_DIR), '--tasks', row['task'],
            '--suite', 'all', '--variants', VARIANT,
            '--retain-terminal', '--output', str(harness),
            '--repetitions', '1', '--no-randomize',
            *PROFILE_FLAGS, '--seed', str(row['seed']),
        ]
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / 'command.json').write_text(json.dumps(command, indent=2) + '\n',
                                              encoding='utf-8')
        started = dt.datetime.now(dt.timezone.utc).isoformat()
        print(f'[run {row["order_index"]}/{len(rows)}] {row["run_id"]} started {started}',
              flush=True)
        if args.dry_run:
            executed += 1
            continue
        with (run_dir / 'harness.log').open('wb') as log:
            completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                       cwd=str(ROOT))
        passed = None
        results = harness / 'results.json'
        if results.exists():
            records = json.loads(results.read_text(encoding='utf-8'))
            if isinstance(records, list) and records:
                passed = bool(records[0].get('passed'))
        (run_dir / 'attempt.json').write_text(json.dumps({
            **row, 'started_utc': started,
            'finished_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'returncode': completed.returncode, 'passed': passed,
        }, indent=2) + '\n', encoding='utf-8')
        print(f'[done] {row["run_id"]} rc={completed.returncode} passed={passed}', flush=True)
        executed += 1

    print(f'executed {executed} run(s)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
