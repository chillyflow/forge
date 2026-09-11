"""Read-only cost accounting for completed frozen candidate runs.

These are descriptive timings, not acceptance outcomes or evidence-reuse grants.
"""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze(candidate):
    outcomes_path = candidate / "outcomes.json"
    outcomes = json.loads(outcomes_path.read_text(encoding="utf-8"))
    rows = []
    for outcome in outcomes:
        if outcome["schedule"]["gate"] == "G0":
            continue
        metrics_path = Path(outcome["evidence"]["metrics"]["path"])
        journal_path = Path(outcome["evidence"]["journal"]["path"])
        metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
        reports = sorted(path for path in (journal_path.parent / "validation").glob("*.json")
                         if path.stem.isdigit())
        inputs = {str(metrics_path): digest(metrics_path)}
        stages, exact_commands = {}, {}
        command_ms = snapshot_ms = report_ms = 0.0
        for path in reports:
            report = json.loads(path.read_text(encoding="utf-8"))
            inputs[str(path)] = digest(path)
            snapshot_ms += report.get("snapshot_ms", 0.0)
            report_ms += report.get("duration_ms", 0.0)
            for command in report.get("commands", []):
                if not command.get("started"):
                    continue
                elapsed = command.get("duration_ms", 0.0)
                command_ms += elapsed
                stage = stages.setdefault(command["stage"], {"commands": 0, "milliseconds": 0.0})
                stage["commands"] += 1
                stage["milliseconds"] += elapsed
                # Toolchain/environment identity is not available here: even an
                # exact repeated key is only a reuse opportunity to investigate.
                key = json.dumps([report.get("input_hash_before"), command.get("cwd"),
                                  command.get("argv"), command.get("require_empty_stdout")],
                                 sort_keys=True)
                exact_commands[key] = exact_commands.get(key, 0) + 1
        command_count = sum(stage["commands"] for stage in stages.values())
        if command_count != metrics.get("validation_commands", 0):
            raise ValueError(f"Incomplete command accounting for {outcome['run_id']}: "
                             f"reports={command_count}, metrics={metrics.get('validation_commands')}")
        session_ms = metrics.get("duration_ms", 0.0)
        rows.append({
            "run_id": outcome["run_id"], "gate": outcome["schedule"]["gate"],
            "session_ms": session_ms, "agent_seconds": metrics.get("agent_seconds"),
            "load_ms": metrics.get("load_ms"), "prefill_ms": metrics.get("prefill_ms"),
            "decode_ms": metrics.get("decode_ms"), "tool_ms": metrics.get("tool_ms"),
            "validation_ms": metrics.get("validation_ms"),
            "validation_command_ms": command_ms, "validation_snapshot_ms": snapshot_ms,
            "validation_report_ms": report_ms,
            "validation_report_other_ms": report_ms - command_ms - snapshot_ms,
            "validation_share_of_session": metrics.get("validation_ms", 0.0) / session_ms if session_ms else None,
            "broad_final_verification_seconds": metrics.get("verification_seconds"),
            "repo_full_scans": metrics.get("repo_full_scans"),
            "index_ms_metric": metrics.get("index_ms"),
            "same_inputs_and_command_repeats": sum(count - 1 for count in exact_commands.values()),
            "stages": stages, "input_sha256": inputs,
        })
    return {
        "candidate": str(candidate.resolve()), "analysis_sha256": digest(Path(__file__)),
        "outcomes_sha256": digest(outcomes_path), "completed_coding_runs": len(rows),
        "limitations": [
            "Completed runs only; failed and passing trajectories remain separate rows.",
            "Session generation includes sampling; sampling_ms must not be added again.",
            "The minimal checkpoint branch does not independently time every index or input snapshot; index_ms=0 does not establish zero indexing cost.",
            "Snapshot/report millisecond resolution can round short work to zero.",
            "Repeated commands require additional complete toolchain/environment/plan identity before any evidence reuse.",
            "No narrowed arm was measured by this script; it cannot establish validation savings.",
        ],
        "runs": rows,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    report = analyze(args.candidate)
    target = args.candidate / "validation-cost-analysis.json"
    target.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(target), "completed_runs": report["completed_coding_runs"],
                      "runs": [{key: row[key] for key in ("run_id", "validation_ms", "session_ms",
                                                          "same_inputs_and_command_repeats")}
                               for row in report["runs"]]}, indent=2))
