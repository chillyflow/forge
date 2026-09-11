"""Summarize six completed G1 r001 runs; CPU-only fresh-copy original check."""
import ast
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "AGENTS.md").is_file())
READS = {}


def read(path):
    b = path.read_bytes()
    READS[path.relative_to(ROOT).as_posix()] = hashlib.sha256(b).hexdigest()
    return b


def load(path):
    return json.loads(read(path))


TASKS = ["generalize_retractions_renamed", "generalize_retractions_distractor",
         "go_api_pagination", "generalize_retractions_paraphrased",
         "go_multifile_transfer", "generalize_retractions_original"]
rows = []
for task in TASKS:
    run = HERE.parent / f"runs/development-G1-loop-pilot-{task}-s42-r001"
    outcome = load(run / "outcome.json")  # Completion marker checked first.
    assert outcome["schedule"]["repetition"] == 1 and outcome["schedule"]["gate"] == "G1"
    harness = run / f"harness/{task}-loop-repair-r001"
    session = harness / "session"
    metrics = load(run / "envelope/metrics.json")
    execution = load(run / "envelope/execution.json")
    validation = load(run / "envelope/validation.json")
    verification = load(harness / "verification.json")
    terminal = load(run / "envelope/terminal.json")
    events = [json.loads(x) for x in read(session / "events.jsonl").splitlines()]
    turn, by_turn, tool_turn = 0, {}, {}
    for event in events:
        if event["type"] == "context_plan":
            turn = event["data"]["turn"]
        if turn:
            by_turn.setdefault(turn, []).append(event)
        if event["type"] == "tool_result":
            tool_turn[event["data"]["id"]] = turn
    bounded = [e["data"] for e in events if e["type"] == "bounded_context"]
    contexts = []
    rejected_noops, rejected_finals, checkpoints = [], [], []
    for turn, turn_events in by_turn.items():
        context = load(session / f"context/{turn:04}.json")
        memory = next(s["text"] for s in context["segments"] if s["selected"] and s["kind"] == 4)
        inference = next(e["data"] for e in turn_events if e["type"] == "inference")
        contexts.append({"action": turn, "actual_prompt_tokens": inference["prompt_tokens"],
                         "omitted_segments": bounded[turn - 1]["omitted_segments"],
                         "input_budget": bounded[turn - 1]["input_budget"],
                         "validation_reserved": "Validation checkpoint: call validate_candidate now." in memory,
                         "completion_reserved": "Completion checkpoint: call final now." in memory,
                         "memory": memory})
        for event in turn_events:
            if event["type"] == "tool_result" and "old_text and new_text are identical" in event["data"].get("output", ""):
                rejected_noops.append(turn)
            if event["type"] == "final_rejected":
                rejected_finals.append(turn)
            if event["type"] == "candidate_checkpoint":
                checkpoints.append({"action": turn, **event["data"]})
    edits = []
    for path in sorted((session / "tool").glob("*.edit.json")):
        edit = load(path)
        before = read(session / edit["before_artifact"])
        after = read(session / edit["after_artifact"])
        row = {"action": tool_turn[edit["tool_call"]], "path": edit["path"],
               "before_sha256": hashlib.sha256(before).hexdigest(), "after_sha256": hashlib.sha256(after).hexdigest(),
               "before": before.decode(), "after": after.decode(), "ast_unchanged": None}
        if edit["path"].endswith(".py"):
            row["ast_unchanged"] = ast.dump(ast.parse(before)) == ast.dump(ast.parse(after))
        edits.append(row)
    primary_pass = bool(execution["agent_completed"] and metrics["status"] == "ok" and validation["complete"] and
                        validation["returncode"] == 0 and verification["passed"] and outcome["protected_files_unchanged"] and
                        outcome["identity_before"] == outcome["identity_after"])
    rows.append({"task": task, "run_id": outcome["run_id"], "outcome": outcome,
                 "primary_pass": primary_pass, "execution": execution, "validation_envelope": validation,
                 "independent_verification": verification, "terminal": terminal, "metrics": metrics,
                 "first_omission_action": next((x["action"] for x in contexts if x["omitted_segments"]), None),
                 "rejected_noop_actions": rejected_noops, "rejected_final_actions": rejected_finals,
                 "reserved_validation_actions": [x["action"] for x in contexts if x["validation_reserved"]],
                 "reserved_completion_actions": [x["action"] for x in contexts if x["completion_reserved"]],
                 "event_counts": dict(Counter(e["type"] for e in events)), "checkpoints": checkpoints,
                 "contexts": contexts, "edits": edits,
                 "action_events": [{"action": t, "events": [e for e in es if e["type"] in ["model_output", "tool_call", "tool_result"]]} for t, es in by_turn.items()]})

# Use existing direct-compile stage probe for the newly completed original run.
helper_path = HERE / "compare-completed-g1.py"
read(helper_path)
helper = runpy.run_path(str(helper_path))
original = helper["analyze"]("generalize_retractions_original", "service", "balances")
READS.update(helper["READS"])
(HERE / "original-r001-analysis.json").write_text(json.dumps(original, indent=2) + "\n", encoding="utf-8")
behavior = {
    "generalize_retractions_renamed": {"observed_behavior_edit_actions": [4, 7], "note": "Action4 regresses duplicate-post case; action7 restores it. Early-retraction failure persists; action19 comment-only."},
    "generalize_retractions_distractor": {"observed_behavior_edit_actions": [8, 21, 23, 29], "note": "Action8 changes wrong balance to missing zero key;21 passes,23 reverts to failure,29 restores passing behavior. Action17 comment-only;5 changes AST but not observed outcomes."},
    "go_api_pagination": {"observed_behavior_edit_actions": [4], "note": "Validates positive numeric parameters and computes page offset from page-1. Tests pass at5; remaining repair is gofmt, host validation passes16."},
    "generalize_retractions_paraphrased": {"observed_behavior_edit_actions": [], "note": "Action4 removes unconditional wrapper/adds comment;12 removes comment. No observed outcome change."},
    "go_multifile_transfer": {"observed_behavior_edit_actions": [5, 7], "note": "Credits destination/rejects negative transfer at5; allows full-balance debit at7. Tests pass8; remaining repair is formatting, host validation passes27."},
    "generalize_retractions_original": {"observed_behavior_edit_actions": [20, 22], "note": "Action20 changes wrong balance to missing zero key;22 first passes unchanged suite. Action5 changes internal posting state without changing tested outcomes;9,10,19 comment-only."},
}
for row in rows:
    row["behavior_analysis"] = behavior[row["task"]]

run = HERE.parent / "runs/development-G1-loop-pilot-generalize_retractions_original-s42-r001"
terminal_path = run / "harness/generalize_retractions_original-loop-repair-r001/terminal-workspace"
source_files = {p.relative_to(terminal_path).as_posix(): read(p) for p in terminal_path.rglob("*") if p.is_file()}
assert not any("__pycache__" in p or p.endswith(".pyc") for p in source_files)
before = {p: hashlib.sha256(b).hexdigest() for p, b in source_files.items()}
command = load(run / "envelope/validation.json")["command"]
out_dir = HERE / "original-fresh-verify"
out_dir.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="forge-original-r001-analysis-") as tmp:
    workspace = Path(tmp).resolve()
    # Verify the absolute cleanup target before TemporaryDirectory's recursive cleanup.
    assert workspace.parent == Path(tempfile.gettempdir()).resolve()
    assert workspace.name.startswith("forge-original-r001-analysis-")
    for relative, data in source_files.items():
        target = workspace / relative
        assert target.resolve().is_relative_to(workspace)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(command, cwd=workspace, env=env, capture_output=True, timeout=120)
    after = {p.relative_to(workspace).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in workspace.rglob("*") if p.is_file()}
    fresh = {"method": "Source-only fresh disposable copy; original verifier command; PYTHONDONTWRITEBYTECODE=1",
             "command": command, "cwd": str(workspace), "returncode": result.returncode,
             "before_sha256": before, "after_sha256": after, "sources_unchanged": before == after,
             "fresh_copy_had_no_bytecode": True, "temporary_copy_removed": True,
             "stdout_artifact": "original-fresh-verify/stdout.txt", "stderr_artifact": "original-fresh-verify/stderr.txt"}
    (out_dir / "stdout.txt").write_bytes(result.stdout)
    (out_dir / "stderr.txt").write_bytes(result.stderr)
    assert result.returncode == 0 and before == after and b"Ran 2 tests" in result.stderr
(out_dir / "result.json").write_text(json.dumps(fresh, indent=2) + "\n", encoding="utf-8")
history_path = ROOT / "benchmark/results/2026-09-08-repair-recovery/README.md"
history = read(history_path).decode()
summary = {"schema_version": 1, "scope": "Exactly six completed G1 first repetitions; no ongoing repetition reads",
           "primary_pass_count": sum(r["primary_pass"] for r in rows), "primary_total": len(rows),
           "runs": rows, "original_fresh_copy_verification": fresh,
           "original_edit_stage_results": [{"action": e["action"], "path": e["path"], "ast_unchanged": e["ast_unchanged"], "after_tests": e.get("after_tests")} for e in original["edits"]],
           "prior_rejected_experiment": {"readme": history_path.relative_to(ROOT).as_posix(),
                 "readme_sha256": hashlib.sha256(history.encode()).hexdigest(),
                 "finding": "September 8 v4 traceback locals exposed the right failing operand but produced ineffective code; full matrix regressed from 7/10 to 5/10 and lost preserved retraction passes; reverted.",
                 "interpretation": "Correct-operand capture is not a new or qualified solution. Prior suggestions in diagnosis reports are hypotheses and are constrained by this negative evidence."},
           "input_sha256": READS,
           "all_read_inputs_unchanged": all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == h for p, h in READS.items())}
assert summary["primary_pass_count"] == 4 and len(rows) == 6 and summary["all_read_inputs_unchanged"]
(HERE / "first-repetition-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"primary_pass": "4/6", "original_fresh_pass": fresh["returncode"] == 0,
                  "input_hashes": len(READS), "unchanged": True,
                  "rows": [{"task": r["task"], "first_omission": r["first_omission_action"], "noops": r["rejected_noop_actions"], "rejected_finals": r["rejected_final_actions"], "reserved_validation": r["reserved_validation_actions"], "reserved_completion": r["reserved_completion_actions"]} for r in rows]}))
