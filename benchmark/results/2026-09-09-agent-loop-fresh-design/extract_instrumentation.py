"""Prespecified instrumentation extractor for the fresh-design campaign.

Computes, for any run directory produced by benchmark/run.py, the metric set the
plan names in W0. Written before W0's outcomes existed and validated against the
two tables the plan already publishes for candidate 2, so the same numbers are
reproducible across sessions.

Metric definitions, verbatim from the plan where it gives one:

  identical_replacement_count
      tool_call events with data.tool == "apply_patch" and non-empty
      data.args.old_text == data.args.new_text, read from session/events.jsonl.
  forced_opener_rate
      metrics.forced_actions / metrics.turns.
  Every count is reported with its denominator.

Locally defined, because the plan names the quantity but not the predicate:

  rejected_final_count
      events of type "final_rejected", which the host emits when it refuses a
      model completion claim. An earlier draft of this extractor counted
      non-ok `validate_candidate` results instead; that over-counts, because
      those are genuine failed validations (broad_tests exit 1), not refused
      completions. The `final_rejected` predicate reproduces the plan's
      published multiset; the validate_candidate one did not.
  run_command_repeat_count
      run_command calls whose whitespace-normalised command string equals one
      issued earlier in the same run. "Near-duplicate reproduction" is
      operationalised as an exact repeat after normalisation; anything looser
      would not be reproducible across sessions.

  transition counts (the H-ECHO statistic)
      Over consecutive pairs in a run's apply_patch outcome string, classified
      by the predecessor:
          applied_to_identical / applied_to_other      predecessor applied (O)
          identical_to_identical / identical_to_other  predecessor identical (N)
      P(identical | previous identical) is
      identical_to_identical / (identical_to_identical + identical_to_other),
      summed across runs.

      Convention, stated because it is otherwise ambiguous and load-bearing: a
      pair whose predecessor is a rejection for some *other* reason (X, e.g. an
      anchor mismatch) is SKIPPED - not counted, and not used to splice its
      neighbours together. The alternative reading deletes X and pairs across
      the gap, which gives different denominators.

      X is not hypothetical: 12 occurrences across this campaign. It is absent
      from the two populations the headline refutation compares - candidate 2's
      W0 arm and the W1 screen - so 60.0% versus 59.0% is invariant under both
      readings. It is present in W0's plain-control and checkpoint-only arms,
      where the choice does move the cell (9/16 here versus 9/15 under the
      alternative), so the published W0 table is only reproducible together with
      this rule. That is why the rule lives in code rather than in a session's
      ad-hoc script.

Usage:
    python extract_instrumentation.py <runs-dir> [--output out.json]

<runs-dir> is scanned for */harness/*/session/events.jsonl, which matches both
the campaign layout (candidate-NN/runs/<run_id>/harness/...) and the W0 layout.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

WS = re.compile(r'\s+')


def load_events(path: Path) -> list[dict]:
    events = []
    with path.open(encoding='utf-8') as handle:
        for line in handle:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return events


def normalise(command) -> str:
    if isinstance(command, list):
        command = ' '.join(str(part) for part in command)
    return WS.sub(' ', str(command)).strip()


def analyse_session(session: Path) -> dict:
    events = load_events(session / 'events.jsonl')
    metrics = {}
    metrics_path = session / 'metrics.json'
    if metrics_path.exists():
        metrics = json.loads(metrics_path.read_text(encoding='utf-8'))

    calls = [e for e in events if e.get('type') == 'tool_call']
    results = [e for e in events if e.get('type') == 'tool_result']
    inferences = [e for e in events if e.get('type') == 'inference']

    patches = [e for e in calls if e['data'].get('tool') == 'apply_patch']
    identical = []
    for event in patches:
        args = event['data'].get('args') or {}
        old, new = args.get('old_text'), args.get('new_text')
        if old and old == new:
            identical.append(event)

    # Ordered outcome string over apply_patch calls: O applied, N identical-rejected,
    # X rejected for some other reason. Pairing is positional within apply_patch,
    # which is sound because the loop is strictly serial: one result per call.
    patch_results = [e for e in results if e['data'].get('name') == 'apply_patch']
    outcome = []
    for index, event in enumerate(patches):
        args = event['data'].get('args') or {}
        is_identical = bool(args.get('old_text')) and args.get('old_text') == args.get('new_text')
        status = patch_results[index]['data'].get('status') if index < len(patch_results) else None
        if is_identical:
            outcome.append('N')
        elif status == 'ok':
            outcome.append('O')
        else:
            outcome.append('X')

    rejected_final = [e for e in events if e.get('type') == 'final_rejected']
    failed_validations = [e for e in results
                          if e['data'].get('name') == 'validate_candidate'
                          and e['data'].get('status') != 'ok']

    # run_command's argument is `argv`, a list. An earlier draft read a
    # non-existent `command` key, which normalised every call to the empty
    # string and counted all but the first as repeats.
    commands = [normalise((e['data'].get('args') or {}).get('argv', []))
                for e in calls if e['data'].get('tool') == 'run_command']
    seen: set[str] = set()
    repeats = 0
    for command in commands:
        if command in seen:
            repeats += 1
        else:
            seen.add(command)

    outcome_string = ''.join(outcome)
    transitions = {'applied_to_identical': 0, 'applied_to_other': 0,
                   'identical_to_identical': 0, 'identical_to_other': 0}
    for index in range(1, len(outcome_string)):
        previous, current = outcome_string[index - 1], outcome_string[index]
        if previous == 'O':
            key = 'applied_to_identical' if current == 'N' else 'applied_to_other'
        elif previous == 'N':
            key = 'identical_to_identical' if current == 'N' else 'identical_to_other'
        else:
            continue  # predecessor rejected for another reason; see module docstring
        transitions[key] += 1

    turns = metrics.get('turns') or 0
    forced = metrics.get('forced_actions') or 0

    return {
        'identical_replacement_count': len(identical),
        'apply_patch_total': len(patches),
        'apply_patch_outcome_string': outcome_string,
        'other_rejection_count': outcome_string.count('X'),
        **transitions,
        'rejected_final_count': len(rejected_final),
        'failed_candidate_validation_count': len(failed_validations),
        'run_command_count': len(commands),
        'run_command_repeat_count': repeats,
        'run_command_distinct': len(seen),
        'forced_actions': forced,
        'turns': turns,
        'forced_opener_rate': round(forced / turns, 4) if turns else None,
        'prompt_tokens_cumulative': metrics.get('prompt_tokens'),
        'generated_tokens': metrics.get('generated_tokens'),
        'cached_tokens_total': metrics.get('cached_tokens'),
        'cached_tokens_series': [e['data'].get('cached_tokens') for e in inferences],
        'prompt_tokens_series': [e['data'].get('prompt_tokens') for e in inferences],
        'tool_calls': metrics.get('tool_calls'),
        'terminating_reason': metrics.get('status'),
        'loop_warnings': metrics.get('loop_warnings'),
        'context_evictions': metrics.get('context_evictions'),
    }


def find_runs(root: Path) -> list[tuple[str, Path, Path, list[Path]]]:
    """Yield (run_id, run_dir, root_session, trial_sessions) for every run.

    Single-trial runs have only the root session; multi-candidate runs add
    live trial sessions at session/trial-*/.forge/sessions/*/. Copies under
    failed-workspace/ are byte duplicates of retained terminal workspaces and
    are always excluded, or every trial would count twice.
    """
    found = []
    for session in sorted(root.glob('*/harness/*/session')):
        if not (session / 'events.jsonl').exists():
            continue
        run_dir = session.parent.parent.parent
        trials = sorted(
            p for p in session.glob('trial-*/.forge/sessions/*')
            if 'failed-workspace' not in p.parts
            and (p / 'events.jsonl').exists()
        )
        found.append((run_dir.name, run_dir, session, trials))
    return found


def outcome_of(run_dir: Path) -> dict:
    results = run_dir / 'harness' / 'results.json'
    if not results.exists():
        return {'passed': None, 'returncode': None}
    records = json.loads(results.read_text(encoding='utf-8'))
    if not isinstance(records, list) or not records:
        return {'passed': None, 'returncode': None}
    record = records[0]
    return {
        'passed': record.get('passed'),
        'returncode': record.get('returncode'),
        'task': record.get('task'),
        'variant': record.get('variant'),
        'wall_seconds': record.get('wall_seconds'),
        'protected_files_unchanged': record.get('protected_files_unchanged'),
    }


def merge_trial_rows(rows: list[dict]) -> dict:
    """Merge per-session analyses of one run's trials into a per-run row.

    Scalar counts and transition pairs sum across trials; pairs never cross a
    trial boundary because each trial's outcome string is paired separately
    before joining with '+'. Metrics-derived fields (turns, tokens, series,
    terminating state) come from the root orchestration session, whose
    metrics.json already aggregates the whole run; event-derived fields come
    from the trial sessions, since the root session records orchestration,
    not tool calls, in candidate mode. run_command repeats are counted
    within each trial and, separately, across trials (a later trial repeating
    an earlier trial's command).
    """
    merged: dict = {}
    numeric_sum = ['identical_replacement_count', 'apply_patch_total',
                   'other_rejection_count', 'applied_to_identical',
                   'applied_to_other', 'identical_to_identical',
                   'identical_to_other', 'rejected_final_count',
                   'failed_candidate_validation_count', 'run_command_count',
                   'run_command_repeat_count']
    for key in numeric_sum:
        merged[key] = sum(row.get(key) or 0 for row in rows)
    merged['apply_patch_outcome_string'] = '+'.join(
        row.get('apply_patch_outcome_string', '') for row in rows)
    return merged


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('runs_dir', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()

    rows = []
    for run_id, run_dir, session, trials in find_runs(args.runs_dir):
        row = {'run_id': run_id}
        row.update(outcome_of(run_dir))
        if not trials:
            row.update(analyse_session(session))
        else:
            root_row = analyse_session(session)
            trial_rows = [analyse_session(trial) for trial in trials]
            row.update(root_row)
            row.update(merge_trial_rows(trial_rows))
            row['trial_count'] = len(trials)
            row['trial_sessions_analyzed'] = [
                str(trial.relative_to(run_dir)) for trial in trials]
            # Cross-trial command repeats, kept separate from the
            # within-trial predicate above.
            seen: set[str] = set()
            cross = 0
            for trial in trials:
                for event in load_events(trial / 'events.jsonl'):
                    if event.get('type') != 'tool_call':
                        continue
                    if (event.get('data') or {}).get('tool') != 'run_command':
                        continue
                    command = normalise(
                        ((event.get('data') or {}).get('args') or {}).get('argv', []))
                    if command in seen:
                        cross += 1
                    else:
                        seen.add(command)
            row['run_command_cross_trial_repeats'] = cross
            row['run_command_distinct'] = len(seen)
        rows.append(row)

    payload = {
        'schema_version': 1,
        'runs_dir': str(args.runs_dir),
        'run_count': len(rows),
        'runs': rows,
    }
    text = json.dumps(payload, indent=2) + '\n'
    if args.output:
        args.output.write_text(text, encoding='utf-8')
        print(f'wrote {args.output} ({len(rows)} runs)')
    else:
        print(text)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
