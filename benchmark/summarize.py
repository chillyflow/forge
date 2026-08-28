"""Publish numeric benchmark records without copying private session content.

Server Prometheus counters include all OpenCode inference requests, including
metadata/title generation. Never substitute per-step usage for those counters.
"""
import argparse
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def counters(path):
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line and not line.startswith('#'):
            key, value = line.split()
            result[key] = float(value)
    return result


def aggregate(records):
    keys = ['prompt_tokens', 'prefill_tokens', 'cached_tokens', 'generated_tokens', 'prefill_ms', 'decode_ms']
    return {'tasks': len(records), 'passed': sum(r['passed'] for r in records),
            'wall_seconds': sum(r['wall_seconds'] for r in records),
            **{key: sum(r['metrics'][key] for r in records) for key in keys}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forge', type=Path, required=True)
    parser.add_argument('--opencode', type=Path, required=True)
    parser.add_argument('--forge-revision', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    forge_env, other_env = read(args.forge / 'environment.json'), read(args.opencode / 'environment.json')
    if forge_env['model_sha256'] != other_env['model_sha256']:
        parser.error('Model SHA-256 differs')
    forge = read(args.forge / 'results.json')
    baseline = read(args.opencode / 'results.json')
    for record in baseline:
        task = record['task']
        if Path(task).name != task:
            parser.error('Invalid task identifier')
        before = counters(args.opencode / task / 'server-before.prom')
        after = counters(args.opencode / task / 'server-after.prom')
        delta = {key: value - before.get(key, 0) for key, value in after.items()}
        new = int(delta['llamacpp:prompt_tokens_total'])
        cached = int(delta['llamacpp:prompt_tokens_cached_total'])
        record['metrics'] = {
            'prompt_tokens': new + cached, 'prefill_tokens': new, 'cached_tokens': cached,
            'generated_tokens': int(delta['llamacpp:tokens_predicted_total']),
            'prefill_ms': delta['llamacpp:prompt_seconds_total'] * 1000,
            'decode_ms': delta['llamacpp:tokens_predicted_seconds_total'] * 1000,
        }
        record.pop('step_usage', None)
    task_set = {r['task'] for r in baseline}
    groups = {variant: [r for r in forge if r['variant'] == variant] for variant in {r['variant'] for r in forge}}
    if any({r['task'] for r in group} != task_set for group in groups.values()):
        parser.error('Task sets differ')
    if any(r['metrics'].get('simulated') for r in forge):
        parser.error('Cannot publish simulated benchmark as inference')
    args.output.mkdir(parents=True, exist_ok=True)
    environment = {key: forge_env[key] for key in ['model_file', 'model_sha256', 'platform', 'gpu', 'context_tokens', 'go_version', 'forge_binary_sha256'] if key in forge_env}
    environment.update(forge_revision=args.forge_revision, opencode_version=other_env['opencode_version'],
                       server_command=other_env['server_command'], baseline_conditions=other_env['notes'])
    write(args.output / 'environment.json', environment)
    write(args.output / 'forge.json', forge)
    write(args.output / 'opencode.json', baseline)
    totals = {f'Forge {variant}': aggregate(group) for variant, group in sorted(groups.items())}
    totals['OpenCode'] = aggregate(baseline)
    write(args.output / 'summary.json', totals)
    lines = ['# Local Go repair measurements', '',
             'Ten tiny synthetic tasks, one run per configuration. All failures are retained.',
             'These results do not establish general repository performance or statistical significance.', '',
             '| Harness | Passed | Logical prompt tokens | Evaluated prompt tokens | Generated tokens |',
             '| --- | ---: | ---: | ---: | ---: |']
    for name, values in totals.items():
        lines.append(f'| {name} | {values["passed"]}/{values["tasks"]} | {values["prompt_tokens"]:,} | {values["prefill_tokens"]:,} | {values["generated_tokens"]:,} |')
    lines += ['', '## Conditions and limits', '',
              '- Same GGUF hash, laptop GPU, 16,384-token context, greedy decoding, seed 42.',
              '- OpenCode uses its normal tool protocol. Remote tools and subagents are disabled.',
              '- OpenCode model stays loaded, but its slot KV is erased before each task; RAM prompt caching is disabled.',
              '- OpenCode counters include all local inference requests during the task, including title generation.',
              '- Forge uses a fresh process for each task. Its driver wall time includes model load and independent verification.',
              '- OpenCode wall time excludes server/model startup and independent verification. Do not compare these wall times directly.',
              '- Native Forge generation time includes grammar sampling and token event I/O; server generation time measures a different boundary.',
              '- Test-file hashes must remain unchanged, and independent `go test -json ./...` must pass.',
              '- See `environment.json` for exact model, hardware and revisions; numeric per-task records are included.', '']
    (args.output / 'README.md').write_text('\n'.join(lines), encoding='utf-8')
    for name, values in totals.items():
        print(name, values)


if __name__ == '__main__':
    main()
