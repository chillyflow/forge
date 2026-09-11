"""Read-only mechanism accounting for one retained candidate; no inference."""
import collections
import hashlib
import json
from pathlib import Path
import sys


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


directory = Path(sys.argv[1]).resolve()
candidate = read(directory / "candidate.json")
report = read(directory / "acceptance-report.json")
rows = []
for outcome in read(directory / "outcomes.json"):
    run_id = outcome["run_id"]
    target = directory / "runs" / run_id
    results = list((target / "harness").glob("*/result.json"))
    item = {"run_id": run_id, "acceptance": report["runs"].get(run_id),
            "schedule": outcome.get("schedule"), "evidence_error": outcome.get("evidence_error")}
    if results:
        result = read(results[0])
        session = results[0].parent / "session"
        events = [json.loads(line) for line in (session / "events.jsonl").read_text(
            encoding="utf-8").splitlines()] if (session / "events.jsonl").exists() else []
        kinds = collections.Counter(event["type"] for event in events)
        contexts = [event["data"] for event in events if event["type"] == "bounded_context"]
        checkpoints = [event["data"] for event in events if event["type"] == "candidate_checkpoint"]
        calls = [event["data"] for event in events if event["type"] == "tool_call"]
        item.update(primary_passed=result["passed"], metrics=result.get("metrics"),
                    timing=result.get("timing"), verification=result.get("verification"),
                    protected_files_unchanged=result.get("protected_files_unchanged"),
                    verification_inputs_unchanged=result.get("verification_inputs_unchanged"),
                    events=dict(kinds), contexts=contexts, checkpoints=checkpoints, calls=calls,
                    compaction_activated=any(x.get("omitted_segments", 0) for x in contexts),
                    terminal_stderr=(results[0].parent / "verification.stderr").read_text(
                        encoding="utf-8", errors="replace"),
                    agent_stderr=(results[0].parent / "stderr.txt").read_text(
                        encoding="utf-8", errors="replace"),
                    evidence=str(results[0].relative_to(directory)))
    rows.append(item)
summary = {"candidate_id": candidate["candidate_id"], "candidate_sha256": candidate["candidate_sha256"],
           "analysis_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
           "scheduled_total": len(report["runs"]), "recorded_coding_runs": len(rows),
           "gates": report["gates"], "runs": rows}
(directory / "mechanism-analysis.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"recorded_coding_runs": len(rows), "primary_passes": sum(
    row.get("primary_passed", False) for row in rows), "compaction_runs": sum(
    row.get("compaction_activated", False) for row in rows), "gates": report["gates"]}, indent=2))
