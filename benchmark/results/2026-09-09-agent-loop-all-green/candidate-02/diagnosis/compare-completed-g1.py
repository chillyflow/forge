"""Frozen two-Python-run comparison. Reads only completed runs; no GPU/model.

Stages execute directly compiled retained source and unchanged tests in memory.
The comparison describes observed transitions; it is not a candidate/oracle edit.
"""
import ast
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import types
import unittest

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "AGENTS.md").is_file())
READS = {}


def read(p):
    b = p.read_bytes()
    READS[p.relative_to(ROOT).as_posix()] = hashlib.sha256(b).hexdigest()
    return b


def load(p):
    return json.loads(read(p))


def stage(source, module_name, function_name, test_source):
    module = types.ModuleType(module_name)
    exec(compile(source, "retained-source", "exec", dont_inherit=True), module.__dict__)
    prior = sys.modules.get(module_name)
    sys.modules[module_name] = module
    try:
        test = types.ModuleType("retained_stage_tests")
        exec(compile(test_source, "retained-test", "exec", dont_inherit=True), test.__dict__)
        p = test.post("p", 7)
        cases = [[p, test.retract("r", "p")], [test.retract("r", "p"), p],
                 [p, p, test.retract("r", "p"), test.retract("s", "p")]]
        out = io.StringIO()
        result = unittest.TextTestRunner(stream=out, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(test))
        return {"cases": [getattr(module, function_name)(x) for x in cases],
                "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
                "passed": result.wasSuccessful(), "output": out.getvalue()}
    finally:
        if prior is None:
            del sys.modules[module_name]
        else:
            sys.modules[module_name] = prior


def analyze(task, module_name, function_name):
    run = HERE.parent / f"runs/development-G1-loop-pilot-{task}-s42-r001"
    outcome = load(run / "outcome.json")  # Required completion marker before any raw reads.
    harness = run / f"harness/{task}-loop-repair-r001"
    session = harness / "session"
    metrics = load(session / "metrics.json")
    events = [json.loads(x) for x in read(session / "events.jsonl").splitlines()]
    by_turn, tool_id_to_turn = {}, {}
    turn = 0
    for event in events:
        if event["type"] == "context_plan":
            turn = event["data"]["turn"]
        if turn:
            by_turn.setdefault(turn, []).append(event)
        if event["type"] == "tool_result":
            tool_id_to_turn[event["data"]["id"]] = turn
    bounded = [x["data"] for x in events if x["type"] == "bounded_context"]
    actions = []
    cumulative_input = cumulative_output = 0
    for turn in range(1, metrics["turns"] + 1):
        ctx = load(session / f"context/{turn:04}.json")
        memory = next(x["text"] for x in ctx["segments"] if x["kind"] == 4 and x["selected"])
        inf = next(x["data"] for x in by_turn[turn] if x["type"] == "inference")
        cumulative_input += inf["prompt_tokens"]
        cumulative_output += inf["generated_tokens"]
        source = re.search(r"CURRENT_SOURCE_OBSERVATION path=(.*?) content_hash=(\w+)", memory)
        identities = re.search(r"current_inputs=(\w+) latest_validation_inputs=(\w+) validation_id=(\d+)", memory)
        actions.append({"action": turn, "input": inf["prompt_tokens"], "output": inf["generated_tokens"],
                        "cumulative_input": cumulative_input, "cumulative_output": cumulative_output,
                        "omitted_segments": bounded[turn-1]["omitted_segments"],
                        "input_budget": bounded[turn-1]["input_budget"],
                        "source_path": source.group(1) if source else None,
                        "source_hash": source.group(2) if source else None,
                        "input_identities": identities.groups() if identities else None,
                        "memory": memory,
                        "events": [x for x in by_turn[turn] if x["type"] in ["model_output", "tool_call", "tool_result", "candidate_checkpoint", "final_rejected"]]})
    test_source = read(harness / f"terminal-workspace/test_{module_name}.py")
    edits = []
    for path in sorted((session / "tool").glob("*.edit.json")):
        edit = load(path)
        before = read(session / edit["before_artifact"])
        after = read(session / edit["after_artifact"])
        row = {"tool_id": edit["tool_call"], "action": tool_id_to_turn[edit["tool_call"]],
               "path": edit["path"], "before_sha256": hashlib.sha256(before).hexdigest(),
               "after_sha256": hashlib.sha256(after).hexdigest(),
               "ast_unchanged": ast.dump(ast.parse(before)) == ast.dump(ast.parse(after)),
               "artifact": path.relative_to(ROOT).as_posix(), "before": before.decode(), "after": after.decode()}
        if edit["path"] == f"{module_name}.py":
            row["before_tests"] = stage(before, module_name, function_name, test_source)
            row["after_tests"] = stage(after, module_name, function_name, test_source)
        edits.append(row)
    validations = [load(p) for p in sorted((session / "validation").glob("[0-9][0-9][0-9][0-9].json"))]
    result = {"run": outcome["run_id"], "outcome": outcome,
              "metrics": metrics, "envelope_metrics": load(run / "envelope/metrics.json"),
              "verification": load(harness / "verification.json"),
              "verification_stderr": read(harness / "verification.stderr").decode(),
              "terminal_source": read(harness / f"terminal-workspace/{module_name}.py").decode(),
              "event_counts": dict(Counter(x["type"] for x in events)),
              "actions": actions, "edits": edits, "validations": validations}
    assert cumulative_input == metrics["prompt_tokens"]
    assert cumulative_output == metrics["generated_tokens"]
    return result


def main():
    runs = [analyze("generalize_retractions_renamed", "ledger", "positions_for"),
            analyze("generalize_retractions_distractor", "service", "balances")]
    result = {"schema_version": 1, "scope": "Two completed G1 Python runs, only completed outcome markers accepted",
              "method": "Fresh direct compile of every retained implementation edit and unchanged test suite; no imports from retained workspace, no bytecode writes, no new model calls",
              "causality_limit": "Temporal trace association and model's stated reasoning; no controlled causal intervention",
              "runs": runs, "input_sha256": READS,
              "all_read_inputs_unchanged": all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == h for p, h in READS.items())}
    assert result["all_read_inputs_unchanged"]
    (HERE / "g1-two-python-comparison.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"inputs": len(READS), "unchanged": True,
                      "runs": [{"run": r["run"], "pass": r["verification"]["passed"],
                                "edit_stage_passes": [(e["action"], e["after_tests"]["passed"]) for e in r["edits"] if "after_tests" in e]}
                               for r in runs]}))


if __name__ == "__main__":
    main()
