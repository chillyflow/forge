"""Resume missing OpenCode-leg runs with the genuine OpenCode harness.

This mirrors benchmark/opencode.py's cold-lifecycle run loop exactly, but skips
runs whose output directory already exists and appends to the existing
results.json. Used to repair the leg after run_missing.py (Forge harness) was
mistakenly used to fill missing OpenCode runs.
"""
import json
import sys
import subprocess
import tempfile
import shutil
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (FIXTURE_PREPARATION, digest, initialize_git, load_tasks, materialize,
                    protected_unchanged, run_monitored, schedule, snapshot_protected,
                    verify_task, write_json)
from opencode import (isolated_environment, metric_record, request, server_command,
                      start_server, stop_server)

OPENCODE = Path(r'C:\Users\flowc\dev\forge\.tools\opencode\node_modules\opencode-ai\bin\opencode.exe').resolve()
SERVER = Path(r'C:\Users\flowc\dev\forge\.tools\llama-cuda\llama-server.exe').resolve()
MODEL = Path(r'C:\Users\flowc\models\forge\Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf').resolve()
OUTPUT_DIR = Path(r'C:\Users\flowc\dev\forge\.scratch\2026-09-01-tranche2-native-full-campaign2\opencode').resolve()
TASK_DIR = Path(r'C:\Users\flowc\dev\forge\benchmark\tasks').resolve()
CONTEXT = 16384
OUTPUT_RESERVE = 2048
MAX_TURNS = 16
GPU_LAYERS = '-1'
TEMPERATURE = 0.0
SEED = 42
REPETITIONS = 3
ORDER_SEED = 20260831
TIMEOUT = 600
VERIFICATION_TIMEOUT = 120
SERVER_TIMEOUT = 180
GPU_INDEX = 0


class Args:
    """Minimal stand-in for the argparse namespace opencode.py helpers expect."""
    context = CONTEXT
    output_reserve = OUTPUT_RESERVE
    max_turns = MAX_TURNS
    gpu_layers = GPU_LAYERS
    chat_template = None
    temperature = TEMPERATURE
    seed = SEED
    server_timeout = SERVER_TIMEOUT
    gpu_index = GPU_INDEX
    timeout = TIMEOUT
    verification_timeout = VERIFICATION_TIMEOUT


args = Args()

# Load existing genuine records only (harness == 'opencode')
with open(OUTPUT_DIR / 'results.json') as f:
    all_records = json.load(f)
records = [r for r in all_records if r.get('harness') == 'opencode']
genuine_ids = {r['run_id'] for r in records}
print(f'Genuine opencode records: {len(records)}; contaminated: {len(all_records) - len(records)}')

tasks = load_tasks(TASK_DIR, 'all', [])
full_sched = schedule(tasks, REPETITIONS, ORDER_SEED, True)

missing = []
for (_, task), repetition, order_index in full_sched:
    run_id = f'{task["id"]}-opencode-r{repetition:03d}'
    if run_id not in genuine_ids:
        missing.append((task, repetition, order_index, run_id))

print(f'Missing genuine runs: {len(missing)}')
for task, rep, idx, run_id in missing:
    print(f'  {run_id}')

base_env = isolated_environment(OUTPUT_DIR)

for task, repetition, order_index, run_id in missing:
    output = OUTPUT_DIR / run_id
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix='forge-opencode-') as temporary:
        root = Path(temporary).resolve()
        fixture = materialize(root, task)
        initialize_git(root)
        before_protected = snapshot_protected(root, task)
        state = start_server(SERVER, MODEL, output, base_env, args)
        startup_seconds = state['startup_seconds']
        startup_usage = state['startup_resource_usage']
        try:
            erased = json.loads(request(state['port'], '/slots/0?action=erase', 'POST',
                                        state['key']))
            if erased.get('id_slot') != 0:
                raise RuntimeError('Cannot clear benchmark slot')
            before_metrics = request(state['port'], '/metrics', key=state['key'])
            (output / 'server-before.prom').write_text(before_metrics, encoding='utf-8')
            command = [str(OPENCODE), 'run', '--pure', '--dir', str(root), '--model',
                       'llama.cpp/forge-local', '--format', 'json', task['prompt']]
            with (output / 'events.jsonl').open('w', encoding='utf-8') as out, \
                    (output / 'stderr.txt').open('w', encoding='utf-8') as err:
                agent = run_monitored(command, stdout=out, stderr=err, env=state['env'],
                                      timeout=TIMEOUT, extra_pids=[state['process'].pid],
                                      gpu_index=GPU_INDEX)
            after_metrics = request(state['port'], '/metrics', key=state['key'])
            (output / 'server-after.prom').write_text(after_metrics, encoding='utf-8')
            events = []
            for line in (output / 'events.jsonl').read_text(encoding='utf-8').splitlines():
                try:
                    events.append(json.loads(line))
                except ValueError:
                    pass
            steps = [event['part'] for event in events
                     if event.get('type') == 'step_finish' and 'part' in event]
            metrics = metric_record(before_metrics, after_metrics, events)
        finally:
            teardown_seconds = stop_server(state)
        verification = verify_task(root, task, output, env=state['env'],
                                   timeout=VERIFICATION_TIMEOUT, gpu_index=GPU_INDEX)
        unchanged = protected_unchanged(root, before_protected)
        end_to_end = (startup_seconds + agent['wall_seconds'] + teardown_seconds +
                      verification['wall_seconds'])
        passed = agent['returncode'] == 0 and verification['passed'] and unchanged
        with (output / 'workspace-status.txt').open('w', encoding='utf-8') as status:
            subprocess.run(['git', '-C', str(root), 'status', '--short'], text=True,
                           stdout=status, check=False)
        with (output / 'workspace.diff').open('w', encoding='utf-8') as diff:
            subprocess.run(['git', '-C', str(root), 'diff', '--binary'], text=True,
                           stdout=diff, check=False)
        if not passed:
            shutil.copytree(root, output / 'failed-workspace',
                            ignore=shutil.ignore_patterns('.git'))
        timing = {'lifecycle': 'cold', 'startup_seconds': startup_seconds,
                  'agent_seconds': agent['wall_seconds'],
                  'teardown_seconds': teardown_seconds,
                  'verification_seconds': verification['wall_seconds'],
                  'end_to_end_seconds': end_to_end}
        record = {'schema_version': 2, 'run_id': run_id, 'task': task['id'],
                  'harness': 'opencode', 'repetition': repetition,
                  'order_index': order_index, 'order_seed': ORDER_SEED,
                  'returncode': agent['returncode'], 'passed': passed,
                  'wall_seconds': end_to_end, 'timing': timing,
                  'protected_files_unchanged': unchanged,
                  'protected_files': before_protected,
                  'suite': task.get('suite', 'smoke'),
                  'language': task.get('language', 'go'),
                  'category': task.get('category', 'repair'),
                  'metrics': metrics,
                  'step_usage': [step.get('tokens', {}) for step in steps],
                  'startup_resource_usage': startup_usage,
                  'resource_usage': agent['resource_usage'],
                  'verification_resource_usage': verification['resource_usage'], **fixture}
        records.append(record)
        write_json(output / 'result.json', record)
        write_json(OUTPUT_DIR / 'results.json', records)
        print(f'{run_id}: {"PASS" if passed else "FAIL"} ({end_to_end:.1f}s e2e, cold)',
              flush=True)

print(f'Done. Total genuine runs: {len(records)}')
passed = sum(1 for r in records if r['passed'])
print(f'Passed: {passed}, Failed: {len(records) - passed}')
