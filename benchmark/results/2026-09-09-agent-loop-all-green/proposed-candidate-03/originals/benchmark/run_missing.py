"""Run missing benchmark cases for OpenCode leg."""
import json
import sys
import subprocess
import tempfile
import shutil
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (load_tasks, schedule, materialize, initialize_git, snapshot_protected,
                    protected_unchanged, verify_task, write_json, runtime_bundle, platform_metadata,
                    FIXTURE_PREPARATION, digest, run_monitored)

# Import VARIANTS from run module
import run
VARIANTS = run.VARIANTS
PROMPT_PROTOCOLS = run.PROMPT_PROTOCOLS

# Configuration matching the original run
FORGE = Path(r'C:\Users\flowc\dev\forge\build-gpu\Release\forge.exe').resolve()
MODEL = Path(r'C:\Users\flowc\models\forge\Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf').resolve()
OUTPUT_DIR = Path(r'C:\Users\flowc\dev\forge\.scratch\2026-09-01-tranche2-native-full-campaign2\opencode').resolve()
TASK_DIR = Path(r'C:\Users\flowc\dev\forge\benchmark\tasks').resolve()
VARIANT = 'optimized'
PROMPT_PROTOCOL = 'native'
CONTEXT = 16384
OUTPUT_RESERVE = 2048
MAX_TURNS = 16
GPU_LAYERS = -1
TEMPERATURE = 0.0
SEED = 42
REPETITIONS = 3
ORDER_SEED = 20260831
TIMEOUT = 600
VERIFICATION_TIMEOUT = 120
GPU_INDEX = 0

# Load existing results
with open(OUTPUT_DIR / 'results.json') as f:
    records = json.load(f)

completed = set()
for r in records:
    key = (r['task'], r.get('variant', VARIANT), r['repetition'])
    completed.add(key)

# Generate full schedule
tasks = load_tasks(TASK_DIR, 'all', [])
cases = [(task, VARIANT) for _, task in tasks]
full_sched = schedule(cases, REPETITIONS, ORDER_SEED, True)

# Filter to missing
missing = []
for item in full_sched:
    case = item[0]
    rep = item[1]
    task = case[0]
    variant = case[1]
    key = (task['id'], variant, rep)
    if key not in completed:
        missing.append((task, variant, rep))

print(f'Found {len(missing)} missing runs out of {len(full_sched)} total')
for task, variant, rep in missing:
    print(f'  {task["id"]} {variant} r{rep}')

policy = VARIANTS[VARIANT]
model_sha256 = digest(MODEL)
forge_sha256 = digest(FORGE)
forge_runtime = runtime_bundle(FORGE)

for task, variant, repetition in missing:
    run_id = f'{task["id"]}-{variant}-r{repetition:03d}'
    output = OUTPUT_DIR / run_id

    if output.exists():
        print(f'Skipping {run_id} - already exists')
        continue

    output.mkdir(parents=True)

    with tempfile.TemporaryDirectory(prefix='forge-bench-') as temporary:
        root = Path(temporary).resolve()
        fixture = materialize(root, task)
        initialize_git(root)
        before_protected = snapshot_protected(root, task)

        command = [str(FORGE), 'run', task['prompt'], '--workspace', str(root),
                   '--model', str(MODEL), '--gpu-layers', str(GPU_LAYERS),
                   '--prompt-protocol', PROMPT_PROTOCOL,
                   '--context', str(CONTEXT), '--output-reserve', str(OUTPUT_RESERVE),
                   '--temperature', str(TEMPERATURE), '--seed', str(SEED),
                   '--allow-write', '--allow-exec', '--json', '--max-turns',
                   str(MAX_TURNS), '--wall-ms', str(TIMEOUT * 1000),
                   *policy['flags']]

        with (output / 'stdout.jsonl').open('w', encoding='utf-8') as out, \
                (output / 'stderr.txt').open('w', encoding='utf-8') as err:
            process_result = run_monitored(command, stdout=out, stderr=err,
                                           timeout=TIMEOUT + 30,
                                           gpu_index=GPU_INDEX)

        sessions = list((root / '.forge' / 'sessions').glob('*'))
        metrics = {}
        if sessions:
            session = max(sessions, key=lambda path: path.stat().st_mtime_ns)
            shutil.copytree(session, output / 'session', dirs_exist_ok=True)
            if (session / 'metrics.json').exists():
                metrics = json.loads((session / 'metrics.json').read_text(encoding='utf-8'))

        verification = verify_task(root, task, output, timeout=VERIFICATION_TIMEOUT,
                                   gpu_index=GPU_INDEX)
        unchanged = protected_unchanged(root, before_protected)
        startup_seconds = (metrics['load_ms'] / 1000.0) if 'load_ms' in metrics else None
        agent_seconds = (max(0.0, process_result['wall_seconds'] - startup_seconds)
                         if startup_seconds is not None else None)
        end_to_end = process_result['wall_seconds'] + verification['wall_seconds']
        passed = process_result['returncode'] == 0 and verification['passed'] and unchanged

        with (output / 'workspace-status.txt').open('w', encoding='utf-8') as status:
            subprocess.run(['git', '-C', str(root), 'status', '--short'], text=True,
                           stdout=status, check=False)
        with (output / 'workspace.diff').open('w', encoding='utf-8') as diff:
            subprocess.run(['git', '-C', str(root), 'diff', '--binary'], text=True,
                           stdout=diff, check=False)
        if not passed:
            shutil.copytree(root, output / 'failed-workspace', ignore=shutil.ignore_patterns('.git'))

        timing = {'lifecycle': 'cold', 'startup_seconds': startup_seconds,
                  'agent_seconds': agent_seconds,
                  'agent_process_seconds': process_result['wall_seconds'],
                  'verification_seconds': verification['wall_seconds'],
                  'end_to_end_seconds': end_to_end}

        record = {'schema_version': 2, 'run_id': run_id, 'task': task['id'],
                  'variant': variant, 'repetition': repetition, 'order_index': 0,  # Will be filled
                  'prompt_protocol': PROMPT_PROTOCOL,
                  'order_seed': ORDER_SEED, 'returncode': process_result['returncode'],
                  'wall_seconds': end_to_end, 'timing': timing, 'passed': passed,
                  'protected_files_unchanged': unchanged,
                  'protected_files': before_protected, 'suite': task.get('suite', 'smoke'),
                  'language': task.get('language', 'go'),
                  'category': task.get('category', 'repair'),
                  'thought_cue': policy.get('thought_cue'),
                  'thought_budget': policy.get('thought_budget'), 'metrics': metrics,
                  'resource_usage': process_result['resource_usage'],
                  'verification_resource_usage': verification['resource_usage'], **fixture}

        # Find the correct order_index from the full schedule
        for idx, item in enumerate(full_sched, 1):
            case = item[0]
            rep = item[1]
            if case[0]['id'] == task['id'] and case[1] == variant and rep == repetition:
                record['order_index'] = idx
                break

        records.append(record)
        write_json(output / 'result.json', record)
        write_json(OUTPUT_DIR / 'results.json', records)
        print(f'{run_id}: {"PASS" if passed else "FAIL"} ({end_to_end:.1f}s e2e)', flush=True)

print(f'Done. Total runs: {len(records)}')
passed = sum(1 for r in records if r['passed'])
failed = len(records) - passed
print(f'Passed: {passed}, Failed: {failed}')