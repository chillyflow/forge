"""Replay archived native ACTION prefixes on fresh fixtures without model inference.

This is a deterministic mechanism probe, never a coding-success or real-tokenizer gate.
The script backend counts ceil(serialized UTF-8 bytes / 4), not Qwen tokens, and does
not render the embedded Qwen chat template. All archived arguments/prose remain exact.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

import common

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT / "benchmark/results/2026-09-09-agent-loop-v1"
TASKS = ROOT / "benchmark/results/2026-09-08-repair-control/tasks"
DEFAULT_OUTPUT = ROOT / "benchmark/results/2026-09-09-agent-loop-all-green/pressure-replay"
LIMITS = {"max_turns": 32, "context": 16384, "output_reserve": 2048,
          "max_tokens": 32768, "max_input": 262144, "wall_ms": 600000,
          "verification_timeout_seconds": 120}
INPUT_HASHES = {}


def json_read(path):
    path = Path(path)
    raw = path.read_bytes()
    INPUT_HASHES[str(path.resolve())] = hashlib.sha256(raw).hexdigest()
    return json.loads(raw.decode("utf-8-sig"))


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def native(action):
    """Use reasoning_content so minimal_action restores the exact assistant_content."""
    assert set(action) <= {"assistant_content", "tool", "args", "final"}, set(action)
    if "final" in action:
        name, arguments = "final", {"answer": action["final"]}
    else:
        name, arguments = action["tool"], action["args"]
    message = {"role": "assistant", "content": "", "tool_calls": [
        {"type": "function", "function": {"name": name, "arguments": arguments}}]}
    if action.get("assistant_content"):
        message["reasoning_content"] = action["assistant_content"]
    return message


def events(path):
    return [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines()
            if line.strip()]


def command(forge, script, workspace, task, arm, policy):
    result = [str(forge), "run", task["prompt"], "--script", str(script),
              "--workspace", str(workspace), "--no-config", "--json", "--minimal-agent",
              "--candidate-checkpoint", "--prompt-protocol", "native", "--context", "16384",
              "--output-reserve", "2048", "--max-turns", "32", "--max-tokens", "32768",
              "--max-input", "262144", "--wall-ms", "600000", "--timeout-ms", "120000",
              "--temperature", "0.6", "--seed", "42", "--allow-write", "--allow-exec"]
    if arm == "semantic":
        result.append("--semantic-loops")
    elif arm == "impact":
        result.append("--symbol-impact")
    elif arm == "reflection":
        result.append("--failure-reflection")
    else:
        assert arm == "candidate", arm
    if policy == "bounded":
        result.append("--bounded-repair")
    return result


def probe(root, outcome, cell, script, forge, policy, source_actions):
    task = json_read(TASKS / (outcome["task"] + ".json"))
    workspace = root / "workspace"
    workspace.mkdir()
    fixture = common.materialize(workspace, task)
    assert fixture["fixture_sha256"] == outcome["record"]["fixture_sha256"]
    protected = common.snapshot_protected(workspace, task)
    common.initialize_git(workspace)
    common.protect_protected(workspace, task)
    args = command(forge, script, workspace, task, outcome["arm"], policy)
    write(cell / "command.json", {"argv": args, "cwd": str(workspace),
                                  "script_outside_workspace": not script.is_relative_to(workspace)})
    started = time.monotonic()
    with (cell / "stdout.jsonl").open("w", encoding="utf-8") as out, (cell / "stderr.txt").open("w", encoding="utf-8") as err:
        try:
            process = subprocess.run(args, cwd=workspace, stdout=out, stderr=err, timeout=610,
                                     creationflags=common.CREATE_NO_WINDOW if os.name == "nt" else 0)
            rc, timed_out = process.returncode, False
        except subprocess.TimeoutExpired:
            rc, timed_out = 124, True
    elapsed = time.monotonic() - started
    sessions = list((workspace / ".forge/sessions").glob("*"))
    assert len(sessions) == 1, sessions
    session = sessions[0]
    es = events(session / "events.jsonl")
    metrics = json_read(session / "metrics.json")
    final_context = json_read(session / "context/final.json")
    actual_actions = [json.loads(s["text"]) for s in final_context["segments"] if s["kind"] == 6]
    match = 0
    for expected, actual in zip(source_actions, actual_actions):
        if expected != actual:
            break
        match += 1
    current_turn = 0
    after = []
    for e in es:
        if e["type"] == "context_plan":
            current_turn = e["data"]["turn"]
        if current_turn > len(source_actions) and e["type"] in ["tool_call", "final", "final_rejected", "failure_reflection"]:
            after.append({"turn": current_turn, "sequence": e["sequence"], "type": e["type"], "data": e["data"]})
    verification_before = common.snapshot_protected(workspace, task)
    with (cell / "verification.stdout").open("w", encoding="utf-8") as out, (cell / "verification.stderr").open("w", encoding="utf-8") as err:
        try:
            verified = subprocess.run(task["verify"], cwd=workspace, stdout=out, stderr=err,
                                      timeout=120, creationflags=common.CREATE_NO_WINDOW if os.name == "nt" else 0)
            vrc, vtimeout = verified.returncode, False
        except subprocess.TimeoutExpired:
            vrc, vtimeout = 124, True
    verification_after = common.snapshot_protected(workspace, task)
    terminal = cell / "terminal-workspace"
    shutil.copytree(workspace, terminal, ignore=shutil.ignore_patterns(".git"))
    source_identity = {p.relative_to(terminal).as_posix(): common.digest(p) for p in terminal.rglob("*")
                       if p.is_file() and ".forge" not in p.relative_to(terminal).parts}
    same_protected = protected == verification_before == verification_after
    validations = [e for e in es if e["type"] == "validation_result"]
    result = {"source_cell_id": outcome["cell_id"], "policy": policy, "arm": outcome["arm"],
              "original_stop_action": len(source_actions) + 1, "archived_action_count": len(source_actions),
              "exact_archived_prefix_actions_replayed": match,
              "all_archived_actions_replayed_exactly": match == len(source_actions),
              "executed_native_actions": len(actual_actions), "after_original_stop": after,
              "reached_next_action_after_original_prefix": match == len(source_actions) and bool(after),
              "returncode": rc, "timed_out": timed_out, "wall_seconds": elapsed,
              "metrics": metrics, "fixture": fixture, "protected_files_unchanged": same_protected,
              "terminal_source_sha256": source_identity,
              "terminal_verification": {"argv": task["verify"], "returncode": vrc,
                                        "timed_out": vtimeout, "passed": vrc == 0 and same_protected},
              "latest_host_validation": validations[-1]["data"] if validations else None,
              "context_omissions_observed": any(e["type"] == "bounded_context" and e["data"]["omitted_segments"] for e in es),
              "stderr": (cell / "stderr.txt").read_text(encoding="utf-8"),
              "session": str(terminal / ".forge/sessions" / session.name),
              "simulated": True, "counts_toward_model_acceptance": False}
    write(cell / "result.json", result)
    print(outcome["cell_id"], policy, "prefix", match, "/", len(source_actions),
          "next", result["reached_next_action_after_original_prefix"], "status", metrics.get("status"), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--forge", required=True, type=Path)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    forge, output = args.forge.resolve(), args.output.resolve()
    if output.exists():
        raise SystemExit("Use a new output directory; earlier replay evidence is immutable.")
    output.mkdir(parents=True)
    analysis = json_read(OLD / "analysis.json")
    outcomes = {r["cell_id"]: r for r in json_read(OLD / "outcomes.json")}
    roots = [r for r in analysis["runs"] if not r["passed"] and "Pinned context" in r["terminal_reason"]]
    assert len(roots) == 11
    roots.sort(key=lambda r: r["cell_id"])
    runtime = common.runtime_bundle(forge)
    schedules = []
    for r in roots:
        # The retained run/session copy is preferred over the duplicate inside failed-workspace.
        final = min((OLD / "cells" / r["cell_id"]).rglob("context/final.json"), key=lambda p: len(str(p)))
        context = json_read(final)
        actions = [json.loads(s["text"]) for s in context["segments"] if s["kind"] == 6]
        assert len(actions) + 1 == r["turns"]
        suffix = [{"tool": "validate_candidate", "args": {}},
                  {"final": "Scripted pressure replay reached completion dispatch; unchanged host verification determines the outcome."}]
        assert len(actions) + len(suffix) <= LIMITS["max_turns"]
        native_actions = [native(a) for a in actions + suffix]
        unsupported = [{"action": n, "argv": a["args"]["argv"]} for n, a in enumerate(actions, 1)
                       if a.get("tool") == "run_command" and any(re.search(r"[A-Za-z]:[\\/]", str(x)) for x in a["args"]["argv"])]
        task_path = TASKS / (r["task"] + ".json")
        json_read(task_path)
        for policy in ["append-only", "bounded"]:
            cell = output / "cells" / r["cell_id"] / policy
            cell.mkdir(parents=True)
            script = cell / "actions.json"
            write(script, native_actions)
            write(cell / "source-actions.json", actions)
            schedules.append({"id": r["cell_id"] + "/" + policy, "source_cell_id": r["cell_id"],
                              "policy": policy, "source_context": str(final), "manifest": str(task_path),
                              "script": str(script), "script_sha256": common.digest(script),
                              "preserved_absolute_argv": unsupported,
                              "source_actions": len(actions), "appended_steps": len(suffix)})
    write(output / "protocol.json", {"schema_version": 1, "script": str(Path(__file__).resolve()),
          "script_sha256": common.digest(Path(__file__)), "runtime": runtime, "limits": LIMITS,
          "schedule": schedules, "source_inputs_sha256": INPUT_HASHES.copy(),
          "method": "Exact archived ACTION arguments/prose replayed using script native assistant messages; two host-gated validation/final steps appended within 32 actions.",
          "limitations": ["No Qwen inference or native embedded-template tokenization. Script backend counts ceil(serialized UTF-8 bytes/4).",
                          "Actions are frozen, not adaptive to new diagnostics, source excerpts, or validation scheduling.",
                          "No new repair or expected patch is supplied. Passing host dispatch is not real-model success.",
                          "After a rejected appended final, the finite script may exhaust before action32; that is retained as a replay-tail limitation.",
                          "All original absolute argv strings are preserved; no shell/path interpretation or adaptation is introduced."]})
    results = []
    for scheduled in schedules:
        script = Path(scheduled["script"])
        source_actions = json.loads((script.parent / "source-actions.json").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory(prefix="forge-pressure-replay-") as temp:
            results.append(probe(Path(temp), outcomes[scheduled["source_cell_id"]], script.parent,
                                 script, forge, scheduled["policy"], source_actions))
    assert runtime == common.runtime_bundle(forge), "Runtime changed during replay"
    assert len(results) == 22 and len({(r["source_cell_id"], r["policy"]) for r in results}) == 22
    source_stable = all(common.digest(Path(p)) == digest for p, digest in INPUT_HASHES.items() if Path(p).exists())
    assert source_stable
    summary = {"schema_version": 1, "scheduled": 22, "recorded": len(results),
               "all_source_inputs_unchanged": source_stable, "runtime_unchanged": True,
               "all_protected_files_unchanged": all(r["protected_files_unchanged"] for r in results),
               "reached_next_after_archived_prefix": {policy: sum(r["policy"] == policy and r["reached_next_action_after_original_prefix"] for r in results) for policy in ["append-only", "bounded"]},
               "real_model_runs": 0, "counts_toward_model_acceptance": False, "results": results}
    write(output / "summary.json", summary)
    lines = ["# Archived history pressure replay", "", "This is a **scripted mechanism replay, not real-model coding success**. Eleven unchanged failed prefixes were each replayed under the append-only control and bounded repair, for 22 scheduled executions. Native tool arguments and assistant prose are preserved; only validation and final are appended, within the original 32-action limit.", "",
             "The script backend counts `ceil(serialized UTF-8 bytes / 4)` and does not use Qwen's tokenizer or embedded chat template. Different token counts, fixed non-adaptive actions and finite-script exhaustion after rejected finals limit comparison with the original GPU runs. No budgets were increased, fixture repairs supplied, old paths rewritten, or failed outcomes removed.", "",
             "| Archived failure | Append-only exact prefix | Bounded exact prefix | Bounded reaches next action | Bounded terminal tests |", "| --- | ---: | ---: | --- | --- |"]
    for r in roots:
        old, new = [next(x for x in results if x["source_cell_id"] == r["cell_id"] and x["policy"] == policy) for policy in ["append-only", "bounded"]]
        label = r["cell_id"].replace("generalize_retractions_", "").replace("-r001", "")
        lines.append(f"| [{label}](cells/{r['cell_id']}/bounded/result.json) | {old['exact_archived_prefix_actions_replayed']}/{old['archived_action_count']} | {new['exact_archived_prefix_actions_replayed']}/{new['archived_action_count']} | {new['reached_next_action_after_original_prefix']} | {new['terminal_verification']['passed']} |")
    lines += ["", "[protocol.json](protocol.json) freezes the schedule, original limits, source hashes, scripts and runtime identities before execution. [summary.json](summary.json) retains every outcome. Each cell contains the command, source actions, script, stdout/stderr, metrics/events/context snapshots in the copied terminal workspace, and unchanged-manifest independent verification output.", "",
              "Scripts and outputs lived outside fresh temporary workspaces, so their files did not alter candidate input snapshots. Fresh fixture hashes matched the original campaign. The two invalid no-shell `>` argv calls in distractor/reflection remained unchanged. Source/runtime hashes and all protected-file hashes were checked after the complete schedule.", "",
              "Reproduce using a new output directory: `python benchmark/replay_loop_pressure.py --forge build-gpu/Release/forge.exe --output <new-directory>`. The output is immutable: the runner refuses to overwrite an existing replay directory.", ""]
    (output / "README.md").write_text("\n".join(lines), encoding="utf-8")
    print(json.dumps({k: v for k, v in summary.items() if k != "results"}, indent=2), flush=True)


if __name__ == "__main__":
    main()
