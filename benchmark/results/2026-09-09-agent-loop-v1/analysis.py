"""Analyze the frozen 42-run loop pilot; never infer or repair model candidates.

Run only after the controller's final audit. Secondary manifest verification is
performed on copies of failed terminal workspaces and excluded from all primary
timing and pass/fail results. Existing primary evidence is never overwritten.
"""
import argparse
import hashlib
import json
import math
import shutil
import statistics
import sys
import tempfile
from collections import Counter
from pathlib import Path

ARMS = ("minimal", "candidate", "best-of-2", "semantic", "impact", "reflection", "combined")
SEARCH_ARMS = {"best-of-2", "combined"}
METRICS = (
    "turns", "tool_calls", "generated_tokens", "prompt_tokens", "prefill_tokens",
    "cached_tokens", "prefill_ms", "decode_ms", "load_ms", "validation_ms",
    "validation_commands", "validation_failures", "duration_ms", "tool_ms",
    "think_tokens", "loop_warnings", "index_ms", "end_to_end_seconds",
    "agent_process_seconds", "verification_seconds",
)


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def event_stream(path, issues):
    if not path.is_file():
        issues.append("Missing retained event stream: " + str(path))
        return []
    events = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
            if not isinstance(event, dict) or not isinstance(event.get("type"), str):
                raise ValueError("event lacks a type")
            events.append(event)
        except (ValueError, TypeError) as error:
            issues.append(f"Malformed event at {path}:{line_number}: {error}")
    return events


def flatten(events, issues):
    """Unwrap candidate_event once; never double-count the original child event."""
    flat = []
    current_candidate = None
    for event in events:
        if event["type"] == "candidate_start" and isinstance(event.get("data"), dict):
            current_candidate = event["data"].get("candidate")
        if event["type"] != "candidate_event":
            flat.append({"scope": "root", "candidate": None, "event": event})
            continue
        envelope = event.get("data")
        child = envelope.get("data") if isinstance(envelope, dict) else None
        if isinstance(child, dict) and isinstance(child.get("type"), str) and "data" in child:
            flat.append({"scope": "candidate", "candidate": current_candidate, "event": child})
        else:
            issues.append("Malformed candidate_event envelope at root sequence " + str(event.get("sequence")))
    return flat


def records(flat, kind, scope=None):
    return [dict(scope=item["scope"], candidate=item["candidate"],
                 sequence=item["event"].get("sequence"), data=item["event"].get("data"))
            for item in flat if item["event"]["type"] == kind and
            (scope is None or scope == item["scope"])]


def meaningful_inputs(root):
    """Hash retained terminal inputs, excluding only top-level tool/git metadata."""
    files = {}
    for child in sorted(root.iterdir()):
        if child.name.lower() in {".git", ".forge"}:
            continue
        pending = [child]
        while pending:
            path = pending.pop()
            if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
                raise ValueError("Secondary verification refuses linked input: " + str(path))
            if path.is_dir():
                pending.extend(sorted(path.iterdir(), reverse=True))
            elif path.is_file():
                files[path.relative_to(root).as_posix()] = {"bytes": path.stat().st_size,
                                                          "sha256": digest(path)}
            else:
                raise ValueError("Secondary verification refuses special input: " + str(path))
    return dict(sorted(files.items()))


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def secondary_check(out, cell, run, task, task_info, record, settings, common_hash,
                    protected_unchanged, verify_task, *, workspace_override=None,
                    evidence_group="terminal-verification"):
    if record.get("passed") is True:
        return {"source": "original successful primary verification", "passed": True,
                "protected_files_unchanged": record.get("protected_files_unchanged") is True}
    workspace = workspace_override if workspace_override is not None else run / "failed-workspace"
    if not workspace.is_dir():
        return {"source": "unavailable", "passed": None,
                "error": "No retained failed terminal workspace", "workspace": str(workspace)}
    check = out / evidence_group / cell
    check.mkdir(parents=True, exist_ok=True)
    saved = check / "result.json"
    initial = meaningful_inputs(workspace)
    identity = {"task_sha256": task_info["sha256"], "common_sha256": common_hash,
                "terminal_inputs_sha256": fingerprint(initial),
                "verify": task["verify"], "timeout": settings["verification_timeout"]}
    if saved.is_file():
        evidence = read(saved)
        require(evidence.get("identity") == identity,
                "Saved secondary verification inputs differ for " + cell)
        return evidence
    before_protected = protected_unchanged(workspace, record.get("protected_files", {}))
    source = ("secondary copied failed child trial workspace; unchanged manifest verifier"
              if workspace_override is not None else
              "secondary copied terminal workspace; unchanged manifest verifier")
    evidence = {"source": source,
                "identity": identity, "workspace": str(workspace), "terminal_inputs": initial,
                "protected_files_before": before_protected}
    try:
        with tempfile.TemporaryDirectory(prefix="forge-loop-terminal-") as tmp:
            copy = Path(tmp) / "workspace"

            def ignore_root_metadata(directory, names):
                return [name for name in names if name.lower() in {".git", ".forge"}] \
                    if Path(directory) == workspace else []

            shutil.copytree(workspace, copy, ignore=ignore_root_metadata)
            require(meaningful_inputs(copy) == initial, "Secondary copy differs before verification")
            result = verify_task(copy, task, check, timeout=settings["verification_timeout"],
                                 gpu_index=settings.get("gpu_index", 0))
            after_protected = protected_unchanged(copy, record.get("protected_files", {}))
            evidence.update(result=result, passed=result["passed"],
                            protected_files_after=after_protected,
                            protected_files_unchanged=before_protected and after_protected)
    except (OSError, ValueError, KeyError) as error:
        evidence.update(passed=None, error=str(error))
    require(meaningful_inputs(workspace) == initial,
            "Retained terminal workspace changed during secondary verification: " + cell)
    evidence["retained_terminal_inputs_unchanged"] = True
    write(saved, evidence)
    return evidence


def impact_records(flat):
    result = []
    for record in records(flat, "validation_plan"):
        plan = record["data"]
        impact = plan.get("structural_impact") if isinstance(plan, dict) else None
        if not isinstance(impact, dict):
            continue
        narrow, compiler_only = [], []
        for stage in plan.get("stages", []):
            for command in stage.get("commands", []):
                argv = command.get("argv", [])
                if not isinstance(argv, list) or "-run" not in argv:
                    continue
                position = argv.index("-run")
                expression = argv[position + 1] if position + 1 < len(argv) else None
                entry = {"stage": stage.get("name"), "command": command}
                if expression == "^$":
                    compiler_only.append(entry)
                elif expression is not None and stage.get("name") in {"affected_tests", "dependent_tests"}:
                    narrow.append(entry)
        result.append({"scope": record["scope"], "candidate": record["candidate"],
                       "sequence": record["sequence"], "fallback": impact.get("fallback"),
                       "fallback_reasons": impact.get("fallback_reasons", []),
                       "narrowed_commands": narrow,
                       "narrowed_stages": sorted({entry["stage"] for entry in narrow}),
                       "compiler_only_commands": compiler_only, "impact": impact})
    return result


def reflection_offers(session, arm, issues):
    """Count numbered native prompts offering only the diagnostic action.

    Inspect one retained copy of each actual model session. Search-arm root
    contexts are not model turns, and failed-workspace contains duplicate logs.
    final.json and state snapshots are deliberately excluded.
    """
    if arm in SEARCH_ARMS:
        sessions = sorted({path.resolve() for path in session.glob("trial-*/.forge/sessions/*")
                           if path.is_dir()})
    else:
        sessions = [session]
    offers = []
    for model_session in sessions:
        for path in sorted((model_session / "context").glob("*.txt")):
            if not path.stem.isdecimal():
                continue
            try:
                prompt = read(path)
            except (OSError, ValueError) as error:
                issues.append(f"Cannot inspect native reflection offer {path}: {error}")
                continue
            if not isinstance(prompt, dict) or prompt.get("protocol") != "forge-native-v1":
                continue
            tools = prompt.get("tools")
            if not isinstance(tools, list) or len(tools) != 1:
                continue
            tool = tools[0]
            function = tool.get("function") if isinstance(tool, dict) else None
            if not isinstance(tool, dict) or tool.get("type") != "function" or not isinstance(function, dict) or function.get("name") != "reflect_failure":
                continue
            offers.append({"scope": "candidate" if arm in SEARCH_ARMS else "root",
                           "session": str(model_session), "turn": int(path.stem),
                           "context_path": str(path)})
    return offers


def child_evidence(session, root_events, settings, issues):
    children = []
    starts = {event.get("data", {}).get("candidate"): event.get("data", {})
              for event in root_events if event["type"] == "candidate_start" and
              isinstance(event.get("data"), dict)}
    for event in root_events:
        if event["type"] != "candidate_generated" or not isinstance(event.get("data"), dict):
            continue
        data = event["data"]
        index = data.get("candidate")
        child = {"candidate": index, "generation": data, "allocation": starts.get(index)}
        stored_session = data.get("session")
        # Original temporary absolute paths no longer exist; the copied session
        # tree retains the unique final session-directory component.
        name = str(stored_session).replace("\\", "/").rstrip("/").split("/")[-1]
        matches = [path for path in session.glob("**/metrics.json") if path.parent.name == name]
        if len(matches) != 1:
            child["retained_metrics_error"] = "Expected one copied child metrics file"
            issues.append(f"Candidate {index}: retained child metrics missing or ambiguous")
        else:
            path = matches[0]
            metrics = read(path)
            child["metrics"] = metrics
            child["metrics_path"] = str(path)
            child_events = event_stream(path.parent / "events.jsonl", issues)
            child["final_events"] = sum(event["type"] == "final" for event in child_events)
            child["error_events"] = [event.get("data") for event in child_events if event["type"] == "error"]
            child["real_inference"] = metrics.get("simulated") is False and metrics.get("generated_tokens", 0) > 0
            allocation = starts.get(index, {})
            if number(metrics.get("turns")) and number(allocation.get("max_turns")) and metrics["turns"] > allocation["max_turns"]:
                issues.append(f"Candidate {index} exceeded its allocated action budget")
        children.append(child)
    return children


def child_terminal_checks(out, cell, run, session, children, task, task_info, record, settings,
                          common_hash, protected_unchanged, verify_task, issues):
    """Audit discarded child states separately from the real root workspace.

    Search only the copied parent session's immediate trial directories. Never
    enter failed-workspace, whose .forge tree duplicates the retained sessions.
    One trial workspace is verified once even if unexpected extra logs exist.
    """
    model_sessions = sorted({path.resolve() for path in session.glob("trial-*/.forge/sessions/*")
                             if path.is_dir()})
    checked_workspaces = set()
    results = []
    for model_session in model_sessions:
        workspace = model_session.parents[2]
        if workspace in checked_workspaces:
            issues.append("Multiple retained model sessions share trial workspace: " + str(workspace))
            continue
        checked_workspaces.add(workspace)
        matched = [child for child in children if child.get("metrics_path") and
                   Path(child["metrics_path"]).parent.resolve() == model_session]
        child = matched[0] if len(matched) == 1 else None
        if child is None:
            # Retain incomplete trials even when their parent never emitted a
            # candidate_generated outcome; do not turn them into exclusions.
            metrics_path = model_session / "metrics.json"
            metrics = read(metrics_path) if metrics_path.is_file() else {}
            child_events = event_stream(model_session / "events.jsonl", issues)
            child = {"candidate": None, "generation": None, "metrics": metrics,
                     "final_events": sum(event["type"] == "final" for event in child_events)}
            issues.append("Retained child session lacks a unique generated-candidate record: " + str(model_session))
        generation = child.get("generation") or {}
        metrics = child.get("metrics") or {}
        final_count = child.get("final_events")
        completed = generation.get("status") == "ok" and metrics.get("status") == "ok" and final_count == 1
        item = {"trial": workspace.name, "candidate": child.get("candidate"),
                "workspace": str(workspace), "session": str(model_session),
                "generation_status": generation.get("status"),
                "agent_status": metrics.get("status"), "final_events": final_count,
                "completed_candidate": completed, "failed_or_incomplete_child": not completed}
        if completed:
            item["verification"] = {"source": "completed child candidate; not part of failed-child secondary verification",
                                    "passed": None}
        else:
            child_record = {"passed": False, "protected_files": record.get("protected_files", {})}
            try:
                verification = secondary_check(
                    out, cell + "/" + workspace.name, run, task, task_info, child_record,
                    settings, common_hash, protected_unchanged, verify_task,
                    workspace_override=workspace, evidence_group="child-terminal-verification")
            except (OSError, ValueError) as error:
                verification = {"source": "unavailable", "passed": None, "error": str(error)}
                issues.append("Failed-child secondary verification unavailable: " + str(error))
            item["verification"] = verification
            item["tests_pass"] = verification.get("passed")
            item["protected_valid_pass"] = (verification.get("passed") is True and
                                             verification.get("protected_files_unchanged") is True)
            item["test_passing_without_final"] = verification.get("passed") is True and final_count == 0
        results.append(item)
    return results


def journal_records(session, flat, issues):
    prepared = records(flat, "candidate_edit_prepared", "root")
    outcomes = records(flat, "candidate_edit_outcome", "root")
    identifiers = []
    for record in prepared:
        data = record["data"]
        identifiers.append(data.get("id"))
        artifacts = {}
        for direction in ("before", "after"):
            name = data.get(direction)
            if not isinstance(name, str) or Path(name).name != name or name in {".", ".."}:
                issues.append("Invalid candidate journal artifact name")
                continue
            artifact = session / name
            if artifact.is_file():
                artifacts[direction] = {"bytes": artifact.stat().st_size, "sha256": digest(artifact)}
            else:
                issues.append("Missing prepared candidate journal artifact: " + name)
        record["artifacts"] = artifacts
    if len(set(identifiers)) != len(identifiers):
        issues.append("Duplicate root candidate journal prepared ids")
    counts = Counter(record["data"].get("id") for record in outcomes)
    if any(value != 1 or identifier not in identifiers for identifier, value in counts.items()):
        issues.append("Candidate journal outcomes lack unique matching intent")
    return {"prepared": prepared, "outcomes": outcomes,
            "unfinished_ids": [identifier for identifier in identifiers if identifier not in counts],
            "applied": sum(record["data"].get("applied") is True for record in outcomes),
            "failed": sum(record["data"].get("applied") is False for record in outcomes)}


def classify(row, outcome, record):
    labels = []
    if row["passed"]:
        return ["primary_pass"]
    if outcome.get("status") != "completed":
        labels.append("harness_" + str(outcome.get("status", "unknown")))
    if record:
        status = record.get("metrics", {}).get("status")
        if record.get("returncode") != 0:
            labels.append("agent_" + str(status or "nonzero_exit"))
        if record.get("returncode") == 124:
            labels.append("harness_process_timeout")
        if record.get("protected_files_unchanged") is False:
            labels.append("protected_input_changed")
        if record.get("returncode") == 0 and record.get("protected_files_unchanged") is True:
            labels.append("independent_verification_failed")
    notes = outcome.get("trace_notes", {})
    for key in ("context_exhaustion_observed", "turn_cap_reached", "generated_token_cap_reached", "input_token_cap_reached"):
        if notes.get(key):
            labels.append(key)
    if row["terminal_valid"] is True:
        labels.append("passing_terminal_without_primary_completion")
        if row["root_final_events"] == 0:
            labels.append("passing_terminal_without_final")
    elif row["terminal_tests_pass"] is False:
        labels.append("terminal_tests_failed")
    if row["evidence_issues"]:
        labels.append("evidence_check_failed")
    return labels or ["unclassified_failure"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repository", type=Path, default=Path.cwd())
    args = parser.parse_args()
    out, repository = args.output.resolve(), args.repository.resolve()
    protocol_path, audit_path, outcomes_path = (out / name for name in ("protocol.json", "audit.json", "outcomes.json"))
    protocol, audit, outcomes = read(protocol_path), read(audit_path), read(outcomes_path)
    require(audit.get("frozen_unchanged") is True and audit.get("population", {}).get("complete") is True,
            "Refusing analysis before a complete unchanged frozen audit")
    require(audit["population"].get("expected") == 42 and audit["population"].get("observed") == 42,
            "This script requires the complete preregistered 42-run population")
    require(protocol.get("experiment") == "loop-completion-diagnostic" and set(protocol["arms"]) == set(ARMS),
            "Not the seven-arm loop-completion protocol")
    require(len(outcomes) == 42 and Counter(row["cell_id"] for row in outcomes) ==
            Counter(row["cell_id"] for row in protocol["schedule"]), "Population mismatch or duplicate outcome")
    require(Counter(row["arm"] for row in outcomes) == Counter({arm: 6 for arm in ARMS}),
            "Each arm must retain all six scheduled outcomes")
    # Controller audit hashes canonical JSON, not the indented file bytes.
    require(fingerprint(protocol) == audit["protocol_sha256"], "Protocol hash disagrees with final audit")
    require(not (out / "analysis.json").exists(), "Refusing to overwrite existing analysis.json")
    primary_hashes = {str(path): digest(path) for path in (protocol_path, audit_path, outcomes_path)}
    for cell in protocol["schedule"]:
        path = out / "cells" / cell["cell_id"] / "outcome.json"
        primary_hashes[str(path)] = digest(path)
    common_path = repository / "benchmark" / "common.py"
    common_hash = digest(common_path)
    require(common_hash == protocol["identity"]["harness"]["common.py"], "Manifest verifier implementation changed")
    sys.path.insert(0, str(repository / "benchmark"))
    from common import protected_unchanged, verify_task

    task_data = {}
    for name, info in protocol["identity"]["tasks"].items():
        require(digest(info["path"]) == info["sha256"], "Manifest changed: " + name)
        task_data[name] = read(info["path"])
    settings = protocol["settings"]
    require(settings.get("candidate_budget_scope") == "shared_per_task_across_all_candidates",
            "Candidate budgets are not declared shared")
    rows = []
    for outcome in sorted(outcomes, key=lambda item: item["order_index"]):
        cell, arm = outcome["cell_id"], outcome["arm"]
        record = outcome.get("record", {})
        metrics = record.get("metrics", {})
        issues = []
        run = out / "cells" / cell / "run" / str(record.get("run_id", "missing-run"))
        session = run / "session"
        root_events = event_stream(session / "events.jsonl", issues) if record else []
        flat = flatten(root_events, issues)
        real = metrics.get("simulated") is False and metrics.get("generated_tokens", 0) > 0
        protected = record.get("protected_files_unchanged") is True
        if not real:
            issues.append("Record does not establish real inference")
        if not protected:
            issues.append("Protected-input integrity is false or unavailable")
        budgets = {}
        for field, setting in (("turns", "max_turns"), ("generated_tokens", "max_tokens"), ("prompt_tokens", "max_input")):
            value = metrics.get(field)
            budgets[field] = {"measured": value, "limit": settings[setting],
                              "within": number(value) and 0 <= value <= settings[setting]}
            if not budgets[field]["within"]:
                issues.append("Shared budget evidence missing or exceeded: " + field)
        duration = metrics.get("duration_ms")
        budgets["duration_ms"] = {"measured": duration, "limit": settings["timeout"] * 1000,
                                  "within": number(duration) and 0 <= duration <= settings["timeout"] * 1000}
        if not budgets["duration_ms"]["within"]:
            issues.append("Agent wall budget evidence missing or exceeded")
        # Cold startup and external manifest verification remain separate from
        # the in-agent deadline; end-to-end latency is never mislabeled as it.
        terminal = {"source": "unavailable", "passed": None, "error": "No primary run record"}
        if record:
            try:
                terminal = secondary_check(out, cell, run, task_data[outcome["task"]],
                                           protocol["identity"]["tasks"][outcome["task"]], record,
                                           settings, common_hash, protected_unchanged, verify_task)
            except (OSError, ValueError) as error:
                terminal = {"source": "unavailable", "passed": None, "error": str(error)}
                issues.append("Secondary verification unavailable: " + str(error))
        children = child_evidence(session, root_events, settings, issues) if arm in SEARCH_ARMS else []
        child_terminals = child_terminal_checks(
            out, cell, run, session, children, task_data[outcome["task"]],
            protocol["identity"]["tasks"][outcome["task"]], record, settings, common_hash,
            protected_unchanged, verify_task, issues) if arm in SEARCH_ARMS else []
        starts = records(flat, "candidate_start", "root")
        generated = records(flat, "candidate_generated", "root")
        selections = records(flat, "candidate_selection", "root")
        selected = records(flat, "candidate_selected", "root")
        if len(starts) > settings["candidate_counts"][arm]:
            issues.append("Candidate count exceeds preregistered count")
        if arm in SEARCH_ARMS and children and all("metrics" in child for child in children):
            for field in ("turns", "generated_tokens", "prompt_tokens"):
                child_sum = sum(child["metrics"].get(field, 0) for child in children)
                if child_sum != metrics.get(field):
                    issues.append(f"Parent {field} does not equal total child {field}: {child_sum}")
        checks = records(flat, "candidate_checkpoint")
        finals = records(flat, "final", "root")
        if outcome.get("passed"):
            if len(finals) != 1:
                issues.append("Passing primary lacks exactly one root final")
            if arm in SEARCH_ARMS:
                if len(selected) != 1 or not any(item["data"].get("passed") is True for item in selections):
                    issues.append("Passing search lacks a selected real-workspace-validated candidate")
            elif arm != "minimal":
                passing = [item["data"] for item in checks if isinstance(item["data"], dict) and item["data"].get("passed")]
                if not passing or not any(item.get("validated") and item.get("commands", 0) > 0 and
                                          item.get("input_hash") != item.get("initial_hash") for item in passing):
                    issues.append("Passing candidate lacks changed validated input evidence")
        if arm == "minimal" and (checks or metrics.get("validation_commands", 0) != 0):
            issues.append("Minimal control unexpectedly used candidate validation")
        semantic = records(flat, "semantic_loop")
        reflection = records(flat, "failure_reflection")
        offers = reflection_offers(session, arm, issues)
        if len(reflection) > len(offers):
            issues.append("Completed reflections exceed retained diagnostic-only prompt offers")
        impacts = impact_records(flat)
        journal = journal_records(session, flat, issues)
        stderr = (run / "stderr.txt").read_text(encoding="utf-8", errors="replace") if (run / "stderr.txt").is_file() else ""
        errors = [line for line in stderr.splitlines() if line.startswith("forge: ")]
        terminal_pass = terminal.get("passed")
        terminal_valid = (terminal_pass and terminal.get("protected_files_unchanged") is True) if terminal_pass is not None else None
        row = {"cell_id": cell, "task": outcome["task"], "arm": arm,
               "repetition": outcome["repetition"], "order_index": outcome["order_index"],
               "passed": outcome.get("passed") is True, "outcome_status": outcome.get("status"),
               "harness_error": outcome.get("error"), "harness_returncode": outcome.get("harness_returncode"),
               "agent_returncode": record.get("returncode"), "agent_status": metrics.get("status"),
               "terminal_tests_pass": terminal_pass, "terminal_valid": terminal_valid,
               "root_final_events": len(finals), "final_rejections": len(records(flat, "final_rejected")),
               "terminal_reason": errors[-1] if errors else outcome.get("error") or metrics.get("status") or "unknown",
               "stderr_errors": errors, "trace_notes": outcome.get("trace_notes"),
               "error_events": records(flat, "error"), "real_inference": real,
               "protected_files_unchanged": protected, "budgets": budgets,
               "terminal_verification": terminal, "candidate_checkpoints": checks,
               "semantic_events": semantic, "semantic_repeats": sum(item["data"].get("repeated_failed_state") is True for item in semantic),
               "semantic_incomplete": sum(item["data"].get("complete") is not True for item in semantic),
               "reflections": reflection, "reflection_count": len(reflection),
               "reflection_offers": offers, "reflection_offer_count": len(offers),
               "reflection_uncompleted_offer_count": max(0, len(offers) - len(reflection)),
               "impact_plans": impacts, "impact_plan_count": len(impacts),
               "impact_narrowed_plan_count": sum(bool(item["narrowed_commands"]) for item in impacts),
               "impact_narrowed_stage_count": sum(len(item["narrowed_stages"]) for item in impacts),
               "impact_targeted_command_count": sum(len(item["narrowed_commands"]) for item in impacts),
               "impact_compiler_only_command_count": sum(len(item["compiler_only_commands"]) for item in impacts),
               "impact_fallback_count": sum(item["fallback"] is True for item in impacts),
               "candidate_starts": starts, "candidate_generated": generated,
               "candidate_selections": selections, "candidate_selected": selected,
               "candidate_children": children, "candidate_journal": journal,
               "child_terminal_verifications": child_terminals,
               "completed_child_candidates": sum(item["completed_candidate"] for item in child_terminals),
               "failed_child_trials": sum(item["failed_or_incomplete_child"] for item in child_terminals),
               "failed_child_trials_tests_pass": sum(item.get("tests_pass") is True for item in child_terminals),
               "failed_child_trials_tests_unknown": sum(item["failed_or_incomplete_child"] and item.get("tests_pass") is None for item in child_terminals),
               "failed_child_trials_protected_valid_pass": sum(item.get("protected_valid_pass") is True for item in child_terminals),
               "failed_child_trials_tests_pass_without_final": sum(item.get("test_passing_without_final") is True for item in child_terminals),
               "root_event_counts": dict(Counter(event["type"] for event in root_events)),
               "unwrapped_event_counts": dict(Counter(item["event"]["type"] for item in flat)),
               "metrics": metrics, "timing": record.get("timing"), "evidence_issues": issues}
        for field in METRICS:
            row[field] = record.get("timing", {}).get(field) if field.endswith("_seconds") else metrics.get(field)
        row["failure_classes"] = classify(row, outcome, record)
        rows.append(row)
        print(f"{cell}: primary={row['passed']} terminal_tests={terminal_pass} final={len(finals)} issues={len(issues)}", flush=True)

    summary = {}
    for arm in ARMS:
        group = [row for row in rows if row["arm"] == arm]
        item = {"observations": len(group), "completed": sum(row["passed"] for row in group),
                "terminal_tests_pass": sum(row["terminal_tests_pass"] is True for row in group),
                "terminal_tests_unknown": sum(row["terminal_tests_pass"] is None for row in group),
                "passing_without_completion": sum(row["terminal_valid"] is True and not row["passed"] for row in group),
                "passing_without_final": sum(row["terminal_valid"] is True and row["root_final_events"] == 0 for row in group),
                "failure_classes": dict(Counter(label for row in group for label in row["failure_classes"])),
                "terminal_reasons": dict(Counter(row["terminal_reason"] for row in group)),
                "evidence_issues": {row["cell_id"]: row["evidence_issues"] for row in group if row["evidence_issues"]}}
        for field in METRICS:
            values = [row[field] for row in group if number(row[field])]
            item["observed_" + field] = len(values)
            item["total_" + field] = sum(values) if values else None
            item["median_" + field] = statistics.median(values) if values else None
        for field in ("root_final_events", "final_rejections", "semantic_repeats", "semantic_incomplete",
                      "reflection_count", "reflection_offer_count", "reflection_uncompleted_offer_count",
                      "impact_plan_count", "impact_narrowed_plan_count",
                      "impact_narrowed_stage_count", "impact_targeted_command_count",
                      "impact_compiler_only_command_count", "impact_fallback_count",
                      "completed_child_candidates", "failed_child_trials", "failed_child_trials_tests_pass",
                      "failed_child_trials_tests_unknown", "failed_child_trials_protected_valid_pass",
                      "failed_child_trials_tests_pass_without_final"):
            item["total_" + field] = sum(row[field] for row in group)
        for key in ("candidate_starts", "candidate_generated", "candidate_selections", "candidate_selected"):
            item["total_" + key] = sum(len(row[key]) for row in group)
        item["total_candidate_selection_passes"] = sum(record["data"].get("passed") is True for row in group for record in row["candidate_selections"])
        for key in ("prepared", "outcomes", "unfinished_ids"):
            item["total_journal_" + key] = sum(len(row["candidate_journal"][key]) for row in group)
        for key in ("applied", "failed"):
            item["total_journal_" + key] = sum(row["candidate_journal"][key] for row in group)
        summary[arm] = item
    per_task = []
    for task in sorted(task_data):
        entry = {"task": task}
        for arm in ARMS:
            group = [row for row in rows if row["task"] == task and row["arm"] == arm]
            entry[arm] = {"runs": len(group), "completed": sum(row["passed"] for row in group),
                          "terminal_tests_pass": sum(row["terminal_tests_pass"] is True for row in group),
                          "failure_classes": [row["failure_classes"] for row in group]}
        per_task.append(entry)
    require(all(digest(path) == expected for path, expected in primary_hashes.items()),
            "Primary evidence changed during analysis")
    result = {"schema_version": 1, "diagnostic_only": True, "frozen_unchanged": True,
              "population_complete": True, "observations": len(rows),
              "all_real_inference": all(row["real_inference"] for row in rows),
              "all_protected_files_unchanged": all(row["protected_files_unchanged"] for row in rows),
              "all_shared_budgets_within": all(check["within"] for row in rows for check in row["budgets"].values()),
              "all_evidence_checks_passed": all(not row["evidence_issues"] for row in rows),
              "primary_evidence_unchanged": True, "primary_sha256": primary_hashes,
              "analysis_script_sha256": digest(__file__), "summary": summary, "per_task": per_task, "runs": rows,
              "notes": ["All 42 scheduled outcomes count, including harness and process failures.",
                        "Secondary verification uses terminal input copies and unchanged manifest commands; primary outcomes and timing are unchanged.",
                        "Failed or incomplete retained child trials are independently verified on separate copies under child-terminal-verification. Their passing-test/missing-final counts are separate from root completion loss and from completed child candidates; no discarded child is promoted or substituted into a primary result.",
                        "Completion loss means independently passing protected terminal inputs without primary completion; missing final is reported separately.",
                        "Root metrics are audited against one shared task budget; candidate budgets are never multiplied.",
                        "Nested candidate_event envelopes are unwrapped once for mechanism counts. Child final events are audited in retained child sessions.",
                        "Reflection offers count numbered native context/*.txt prompts with exactly reflect_failure in the tool registry, once per root or unique child model session. Completed reflections count failure_reflection events; the difference includes token-exhausted or malformed diagnostic generations and is not itself a completed reflection.",
                        "Impact narrowing is preliminary; broad final validation remains required.",
                        "Impact command/stage counts describe planned selections, not executed tests. Only affected_tests/dependent_tests with a non-^$ filter count as targeting; compiler-only -run ^$ checks are reported separately.",
                        "Validation_ms is executor time; snapshots, indexing, cold startup and secondary verification are distinct costs.",
                        "One repetition on examined fixtures is not a superiority, preservation, fresh-holdout or release claim."]}
    write(out / "analysis.json", result)
    print(json.dumps(summary, indent=2, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
