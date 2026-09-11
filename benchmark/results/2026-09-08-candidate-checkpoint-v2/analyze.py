"""Post-run audit and secondary verification; never calls a model or repairs a workspace."""
import argparse, json, shutil, statistics, sys, tempfile
from collections import Counter
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('output', type=Path)
parser.add_argument('--repository', type=Path, default=Path.cwd())
args=parser.parse_args()
out=args.output.resolve()
sys.path.insert(0, str(args.repository/'benchmark'))
from common import digest, protected_unchanged, verify_task

def read(path):
    return json.loads(path.read_text(encoding='utf-8'))

def write(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True)+'\n', encoding='utf-8')

p=read(out/'protocol.json')
audit=read(out/'audit.json')
assert audit['frozen_unchanged'] and audit['population']['complete']
outcomes=read(out/'outcomes.json')
assert Counter(row['cell_id'] for row in outcomes)==Counter(row['cell_id'] for row in p['schedule'])
rows=[]
secondary=out/'terminal-verification'
secondary.mkdir(exist_ok=True)
for outcome in outcomes:
    record=outcome['record']
    cell=outcome['cell_id']
    run=out/'cells'/cell/'run'/record['run_id']
    events=[json.loads(line) for line in (run/'session/events.jsonl').read_text(encoding='utf-8').splitlines()]
    assert record['metrics']['simulated'] is False and record['metrics']['generated_tokens']>0
    assert record['protected_files_unchanged']
    taskinfo=p['identity']['tasks'][outcome['task']]
    assert digest(taskinfo['path'])==taskinfo['sha256']
    task=read(Path(taskinfo['path']))
    terminal_pass=record['passed']
    terminal_evidence={'source':'original successful primary verification'}
    if not record['passed']:
        check=secondary/cell
        check.mkdir(exist_ok=True)
        saved=check/'result.json'
        if saved.exists():
            terminal_evidence=read(saved)
        else:
            workspace=run/'failed-workspace'
            assert workspace.is_dir()
            assert protected_unchanged(workspace,record['protected_files'])
            with tempfile.TemporaryDirectory(prefix='forge-checkpoint-terminal-') as tmp:
                root=Path(tmp)/'workspace'
                shutil.copytree(workspace, root, ignore=shutil.ignore_patterns('.forge','.git'))
                result=verify_task(root,task,check,timeout=p['settings']['verification_timeout'])
                terminal_evidence={'source':'secondary copied terminal workspace; unchanged manifest verifier',
                                   'workspace':str(workspace), 'result':result,
                                   'protected_files_unchanged':protected_unchanged(root,record['protected_files'])}
            write(saved,terminal_evidence)
        terminal_pass=terminal_evidence['result']['passed'] and terminal_evidence['protected_files_unchanged']
    checks=[e['data'] for e in events if e['type']=='candidate_checkpoint']
    finals=[e for e in events if e['type']=='final']
    errors=[line for line in (run/'stderr.txt').read_text(encoding='utf-8').splitlines() if line.startswith('forge: ')]
    m=record['metrics']
    if outcome['arm']=='candidate' and record['passed']:
        assert len(finals)==1 and checks and checks[-1]['passed']
        assert checks[-1]['input_hash'] != checks[-1]['initial_hash']
        assert any(check['validated'] and check['passed'] and check['commands']>0 for check in checks)
    if outcome['arm']=='minimal':
        assert not checks and m['validation_commands']==0
    rows.append({'cell_id':cell,'task':outcome['task'],'arm':outcome['arm'],'repetition':outcome['repetition'],
                 'passed':record['passed'],'terminal_tests_pass':terminal_pass,'agent_returncode':record['returncode'],
                 'final_events':len(finals),'final_rejections':sum(e['type']=='final_rejected' for e in events),
                 'terminal_reason':errors[-1] if errors else ('successful task completion' if record['passed'] else 'agent completed; independent verification failed'),
                 'turns':m['turns'],'tool_calls':m['tool_calls'],'generated_tokens':m['generated_tokens'],
                 'prompt_tokens':m['prompt_tokens'],'validation_ms':m['validation_ms'],
                 'prefill_tokens':m['prefill_tokens'],'cached_tokens':m['cached_tokens'],
                 'prefill_ms':m['prefill_ms'],'decode_ms':m['decode_ms'],'load_ms':m['load_ms'],
                 'validation_commands':m['validation_commands'],'candidate_checkpoints':checks,
                 'end_to_end_seconds':record['timing']['end_to_end_seconds'],
                 'terminal_verification':terminal_evidence})
    print(f"{cell}: completion={record['passed']} terminal_tests={terminal_pass}",flush=True)
summary={}
for arm in p['arms']:
    group=[row for row in rows if row['arm']==arm]
    summary[arm]={'observations':len(group),'completed':sum(row['passed'] for row in group),
                  'terminal_tests_pass':sum(row['terminal_tests_pass'] for row in group),
                  'passing_without_completion':sum(row['terminal_tests_pass'] and not row['passed'] for row in group),
                  'final_events':sum(row['final_events'] for row in group),
                  'terminal_reasons':dict(Counter(row['terminal_reason'] for row in group))}
    for field in ('turns','generated_tokens','prompt_tokens','prefill_tokens','cached_tokens',
                  'prefill_ms','decode_ms','load_ms','validation_ms','validation_commands','end_to_end_seconds'):
        summary[arm]['median_'+field]=statistics.median(row[field] for row in group)
        summary[arm]['total_'+field]=sum(row[field] for row in group)
per_task=[]
for task in sorted(p['identity']['tasks']):
    entry={'task':task}
    for arm in p['arms']:
        group=[row for row in rows if row['arm']==arm and row['task']==task]
        entry[arm]={'completed':sum(row['passed'] for row in group),
                    'terminal_tests_pass':sum(row['terminal_tests_pass'] for row in group),'runs':len(group)}
    per_task.append(entry)
write(out/'analysis.json',{'diagnostic_only':True,'all_real_inference':True,'all_protected_files_unchanged':True,
                         'summary':summary,'per_task':per_task,'runs':rows,
                         'note':'Primary outcomes unchanged. Secondary verification runs after the frozen matrix on copies and is excluded from original timing. No inference or repair during this analysis.'})
print(json.dumps(summary,indent=2),flush=True)
