"""Classify why benchmark runs failed, from their raw model_output events.

`metrics.status` records that a run hit a limit but not what the model was
doing when it did, and the useful distinction is between a mechanism failure
(the routed cue, bans and action grammar misfiring) and ordinary model
weakness. So each failing action stream is classified directly from the raw
events, and the per-arm tables the results READMEs quote are derived rather
than transcribed.

  python3 benchmark/analyze_failures.py <consolidated-dir> --raw <run-root>

`<consolidated-dir>` is a `consolidate_arms.py` output (results.json and
thought-census.json); `<run-root>` is the arm run root it was built from, which
still holds the per-run `stdout.jsonl` needed for classification.
"""
import argparse
import collections
import json
import pathlib
import re

CUE = 'Thought: '
# Mirrors FG_ACTION_TRIGGER_PATTERN in src/internal.h: the action is the object
# opening a "tool"/"memory"/"final" key. Scanning for the first '{' instead
# misreads a brace inside the reasoning prose as a malformed action.
TRIGGER = re.compile(r'\{[ \t\r\n]*"(?:tool|memory|final)"[ \t\r\n]*:')

KINDS = """
  no_model_output  a generation produced no action event at all
  no_action        turns produced prose but never opened an action object
  malformed_json   an action object was opened but does not parse
  repeated_action  the same tool call is emitted three or more times
  turn_cap         ran out of turns with well formed, varied actions
  within_turn      a generation ended before completing an action: the routed
                   token-budget death, where reasoning outran the turn budget
  wrong_fix        actions are well formed and varied; the repair just failed
"""


def actions(run_dir):
    """Yield (parsed_or_None, opened_object) per model_output event."""
    log = pathlib.Path(run_dir) / 'stdout.jsonl'
    if not log.exists():
        return
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get('type') != 'model_output':
            continue
        body = event.get('data', '').lstrip()
        if body.startswith(CUE):
            body = body[len(CUE):].lstrip()
        match = TRIGGER.search(body)
        if not match:
            yield None, False
            continue
        # Trailing prose after the object is normal, so decode just the object.
        try:
            parsed, _ = json.JSONDecoder().raw_decode(body[match.start():])
            yield parsed, True
        except ValueError:
            yield None, True


def signature(parsed):
    return json.dumps(parsed, sort_keys=True)[:2000] if isinstance(parsed, dict) else None


def diagnose(run_dir, status, turns=None, max_turns=None):
    items = list(actions(run_dir))
    if not items:
        return 'no_model_output', {'actions': 0}
    parsed = [p for p, _ in items if p is not None]
    malformed = sum(1 for p, opened in items if opened and p is None)
    prose_only = sum(1 for _, opened in items if not opened)
    repeats = collections.Counter(s for s in (signature(p) for p in parsed) if s)
    top, top_n = repeats.most_common(1)[0] if repeats else ('', 0)
    detail = {'actions': len(items), 'opened_object': sum(1 for _, o in items if o),
              'parsed': len(parsed), 'malformed_json': malformed,
              'prose_only': prose_only, 'max_identical_actions': top_n}
    if malformed and malformed >= max(1, len(items) // 4):
        return 'malformed_json', detail
    if top_n >= 3:
        detail['repeated_tool'] = json.loads(top).get('tool')
        return 'repeated_action', detail
    if prose_only and prose_only >= max(1, len(items) // 4):
        return 'no_action', detail
    if status == 'limit':
        # A generation dying mid-action emits no model_output, so a limit run
        # whose turn count is below the cap ended inside a turn.
        if max_turns and turns is not None and turns >= max_turns:
            return 'turn_cap', detail
        return 'within_turn', detail
    return 'wrong_fix', detail


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='diagnoses:' + KINDS)
    parser.add_argument('directory')
    parser.add_argument('--raw', required=True, help='arm run root holding stdout.jsonl')
    parser.add_argument('--label', default='')
    args = parser.parse_args()

    directory, raw = pathlib.Path(args.directory), pathlib.Path(args.raw)
    records = json.loads((directory / 'results.json').read_text(encoding='utf-8'))
    census = json.loads((directory / 'thought-census.json').read_text(encoding='utf-8'))
    environment = json.loads((directory / 'environment.json').read_text(encoding='utf-8'))
    max_turns = environment.get('max_turns')

    grouped = collections.defaultdict(list)
    for record in records:
        grouped[record['variant']].append(record)
    arms = environment.get('arms') or sorted(grouped)

    print('== %s ==' % (args.label or directory))
    for key in ('model_file', 'model_sha256', 'forge_binary_sha256', 'chat_template'):
        print('%-20s %s' % (key, environment.get(key)))
    print('%-20s %s / %s' % ('context / turn cap', environment.get('context_tokens'), max_turns))
    print()
    print('| arm | passed | turns | prompt tokens | generated | actions with thought |')
    print('| --- | --- | --- | --- | --- | --- |')
    for arm in arms:
        runs = grouped.get(arm)
        if not runs:
            continue
        totals = census['per_arm'].get(arm, {})
        routed, inline = totals.get('routed_prefix', 0), totals.get('inline_thought', 0)
        kind = ' routed' if routed else (' inline' if inline else '')
        print('| %s | %d/%d | %d | %s | %s | %d/%d%s |'
              % (arm, sum(1 for r in runs if r['passed']), len(runs),
                 sum(r['metrics'].get('turns', 0) for r in runs),
                 format(sum(r['metrics'].get('prompt_tokens', 0) for r in runs), ','),
                 format(sum(r['metrics'].get('generated_tokens', 0) for r in runs), ','),
                 totals.get('actions_with_thought', 0), totals.get('model_actions', 0), kind))

    rows = []
    for arm in arms:
        for run in sorted(grouped.get(arm, []), key=lambda r: r['task']):
            if run['passed']:
                continue
            metrics = run['metrics']
            status = metrics.get('status', '(none)')
            kind, detail = diagnose(raw / arm / ('%s-%s' % (run['task'], arm)), status,
                                    metrics.get('turns'), max_turns)
            rows.append((arm, run['task'], status, metrics.get('turns'),
                         metrics.get('generated_tokens'), kind, detail))
    print()
    if not rows:
        print('no failures')
        return 0
    print('failures (%d):' % len(rows))
    print('| arm | task | status | turns | generated | diagnosis | detail |')
    print('| --- | --- | --- | --- | --- | --- | --- |')
    for arm, task, status, turns, generated, kind, detail in rows:
        print('| %s | %s | %s | %s | %s | %s | acts=%s malformed=%s prose=%s maxrep=%s |'
              % (arm, task, status, turns, generated, kind, detail.get('actions'),
                 detail.get('malformed_json'), detail.get('prose_only'),
                 detail.get('max_identical_actions')))
    print()
    print('failure taxonomy by arm and diagnosis:')
    tally = collections.Counter((r[0], r[5]) for r in rows)
    for (arm, kind), count in sorted(tally.items()):
        print('  %-40s %-16s %d' % (arm, kind, count))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
