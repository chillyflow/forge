"""W3 screen: 14 runs on a frozen candidate, before any gate batch is spent.

Four Python retraction manifests at three repetitions, plus the two Go manifests
at one repetition. The Go runs are included because a loop change affects every
manifest and Go was 4/4 under candidate 2; screening Python alone would let a Go
regression surface only after the gate batch was already spent.

Screening records go through benchmark/run.py directly and are stored under
<candidate>/screening/. They are NEVER written to a candidate's outcomes.json:
agent_loop_acceptance.report() appends "unscheduled run: <id>" to errors for any
record outside the frozen schedule, and a non-empty errors list sets
accepted: false for every gate, permanently.

These are not gate outcomes, are never pooled with gate outcomes, and are never
re-run to find a better repetition.
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
TASK_DIR = ROOT / 'benchmark/results/2026-09-08-repair-control/tasks'
RUN_PY = ROOT / 'benchmark/run.py'
MODEL = Path('C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf')

PYTHON_TASKS = [
    'generalize_retractions_original',
    'generalize_retractions_renamed',
    'generalize_retractions_paraphrased',
    'generalize_retractions_distractor',
]
GO_TASKS = ['go_api_pagination', 'go_multifile_transfer']
PYTHON_REPETITIONS = 3
GO_REPETITIONS = 1

PROFILE_FLAGS = [
    '--gpu-layers=-1', '--context=16384', '--output-reserve=2048',
    '--temperature=0.6', '--max-turns=32', '--max-tokens=32768',
    '--max-input=262144', '--timeout=600', '--verification-timeout=120',
    '--order-seed=20260831', '--gpu-index=0', '--prompt-protocol=native',
]
SEED = 42


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def schedule() -> list[dict]:
    rows, index = [], 0
    for task, repetitions in ([(t, PYTHON_REPETITIONS) for t in PYTHON_TASKS] +
                              [(t, GO_REPETITIONS) for t in GO_TASKS]):
        for repetition in range(1, repetitions + 1):
            index += 1
            rows.append({
                'order_index': index,
                'run_id': f'screen-{task}-s{SEED}-r{repetition:03d}',
                'task': task,
                'language': 'python' if task.startswith('generalize') else 'go',
                'seed': SEED,
                'repetition': repetition,
            })
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate-dir', type=Path, required=True)
    parser.add_argument('--variant', required=True)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    forge = args.candidate_dir / 'runtime' / 'forge.exe'
    for path in (forge, MODEL, TASK_DIR, RUN_PY):
        if not path.exists():
            print(f'missing required path: {path}', file=sys.stderr)
            return 2

    base = args.candidate_dir / 'screening'
    base.mkdir(parents=True, exist_ok=True)
    rows = schedule()

    protocol = base / 'protocol.json'
    if not protocol.exists():
        candidate = json.loads((args.candidate_dir / 'candidate.json').read_text(encoding='utf-8'))
        protocol.write_text(json.dumps({
            'schema_version': 1,
            'purpose': 'W3 screen before spending a gate batch.',
            'frozen_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'counts_toward_model_acceptance': False,
            'is_scheduled_gate_evidence': False,
            'candidate_id': candidate.get('candidate_id'),
            'candidate_identity': candidate.get('identity'),
            'forge_sha256': digest(forge),
            'model_sha256': digest(MODEL),
            'variant': args.variant,
            'profile_flags': PROFILE_FLAGS,
            'seed': SEED,
            'manifest_sha256': {row['task']: digest(TASK_DIR / f"{row['task']}.json")
                                for row in rows},
            'promotion_rule': (
                'Preregistered in the plan before any run: promotion requires at least '
                '11 of 12 Python passes, AND at least one pass in each of the four '
                'Python manifests, AND no Go regression.'),
            'schedule': rows,
        }, indent=2) + '\n', encoding='utf-8')
        print(f'wrote {protocol}', flush=True)

    executed = 0
    for row in rows:
        run_dir = base / 'runs' / row['run_id']
        harness = run_dir / 'harness'
        if (harness / 'results.json').exists():
            print(f'[skip] {row["run_id"]} already complete', flush=True)
            continue
        if harness.exists():
            print(f'[halt] {row["run_id"]} started but incomplete; inspect {harness}', flush=True)
            return 3
        command = [
            sys.executable, str(RUN_PY),
            '--forge', str(forge), '--model', str(MODEL),
            '--task-dir', str(TASK_DIR), '--tasks', row['task'],
            '--suite', 'all', '--variants', args.variant,
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
            **row, 'variant': args.variant, 'started_utc': started,
            'finished_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
            'returncode': completed.returncode, 'passed': passed,
        }, indent=2) + '\n', encoding='utf-8')
        print(f'[done] {row["run_id"]} rc={completed.returncode} passed={passed}', flush=True)
        executed += 1

    print(f'executed {executed} run(s)', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
