"""Read-only diagnosis of one frozen failed run; writes only beside this script.

Run with Python 3.11 (matching retained pyc). No Forge/model invocation, fixture
mutation, imports from a retained workspace, or generated test/oracle changes.
"""
from __future__ import annotations

import ast
from collections import Counter
import hashlib
import importlib.util
import io
import json
import marshal
from pathlib import Path
import re
import sys
import types
import unittest

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "AGENTS.md").is_file())
RUN = HERE.parent / "runs/development-G1-loop-pilot-generalize_retractions_renamed-s42-r001"
HARNESS = RUN / "harness/generalize_retractions_renamed-loop-repair-r001"
SESSION = HARNESS / "session"
READS = {}


def read(path):
    data = path.read_bytes()
    READS[str(path.relative_to(ROOT)).replace("\\", "/")] = hashlib.sha256(data).hexdigest()
    return data


def load(path):
    return json.loads(read(path))


def ref(path):
    read(path)
    return str(path.relative_to(ROOT)).replace("\\", "/")


def ast_hash(data):
    return hashlib.sha256(ast.dump(ast.parse(data), include_attributes=False).encode()).hexdigest()


def code_identity(code):
    # Compare executable structure recursively, excluding source locations.
    names = ("co_code", "co_consts", "co_names", "co_varnames", "co_freevars",
             "co_cellvars", "co_flags", "co_argcount", "co_posonlyargcount",
             "co_kwonlyargcount", "co_stacksize", "co_exceptiontable")
    return tuple(tuple(code_identity(x) if isinstance(x, types.CodeType) else x
                       for x in getattr(code, name)) if name == "co_consts"
                 else getattr(code, name) for name in names)


def main():
    outcome = load(RUN / "outcome.json")
    metrics = load(SESSION / "metrics.json")
    envelope_metrics = load(RUN / "envelope/metrics.json")
    events = [json.loads(x) for x in read(SESSION / "events.jsonl").splitlines()]
    by_turn = {}
    turn = 0
    for event in events:
        if event["type"] == "context_plan":
            turn = event["data"]["turn"]
        if turn:
            by_turn.setdefault(turn, []).append(event)
    bounded = [x["data"] for x in events if x["type"] == "bounded_context"]
    contexts = []
    actions = []
    cumulative_input = cumulative_output = 0
    for turn in range(1, 33):
        context = load(SESSION / f"context/{turn:04}.json")
        selected = [x for x in context["segments"] if x["selected"]]
        memory = next(x["text"] for x in selected if x["kind"] == 4)
        inference = next(x["data"] for x in by_turn[turn] if x["type"] == "inference")
        cumulative_input += inference["prompt_tokens"]
        cumulative_output += inference["generated_tokens"]
        source = re.search(r"CURRENT_SOURCE_OBSERVATION path=(.*?) content_hash=(\w+)", memory)
        validation_id = re.search(r"validation_id=(\d+)", memory)
        contexts.append({
            "action": turn, "artifact": ref(SESSION / f"context/{turn:04}.json"),
            "actual_prompt_tokens": inference["prompt_tokens"],
            "generated_tokens": inference["generated_tokens"],
            "cumulative_input": cumulative_input, "cumulative_output": cumulative_output,
            "remaining_input_after": 262144 - cumulative_input,
            "remaining_output_after": 32768 - cumulative_output,
            "omitted_segments": bounded[turn-1]["omitted_segments"],
            "bounded_input_budget": bounded[turn-1]["input_budget"],
            "source_observation_path": source.group(1) if source else None,
            "source_observation_hash": source.group(2) if source else None,
            "validation_id": int(validation_id.group(1)) if validation_id else None,
            "has_complete_failure": "LATEST_COMPLETE_CHECKPOINT_OBSERVATION" in memory and "FAILED (failures=1)" in memory,
            "contains_explicit_early_retract_result_7": "Result for [retract('r', 'p'), p]: {'cash': 7}" in memory,
            "previous_delta_includes_assistant_content": "PREVIOUS_APPLIED_DELTA" in memory and '"assistant_content"' in memory,
            "selected_ledger_reads": [x["id"] for x in selected if x["kind"] == 7 and "def positions_for" in x["text"]],
            "memory_text": memory,
        })
        row = {"action": turn}
        for event in by_turn[turn]:
            if event["type"] in ("model_output", "tool_call", "tool_result", "candidate_checkpoint", "final_rejected"):
                row.setdefault(event["type"], []).append({"sequence": event["sequence"], "elapsed_ms": event["elapsed_ms"], "data": event["data"]})
        actions.append(row)
    edits = []
    for path in sorted((SESSION / "tool").glob("*.edit.json")):
        edit = load(path)
        before = read(SESSION / edit["before_artifact"])
        after = read(SESSION / edit["after_artifact"])
        edits.append({"action": edit["tool_call"], "path": edit["path"],
                      "before_sha256": hashlib.sha256(before).hexdigest(),
                      "after_sha256": hashlib.sha256(after).hexdigest(),
                      "ast_unchanged": ast_hash(before) == ast_hash(after),
                      "before": before.decode(), "after": after.decode(),
                      "artifact": ref(path), "patch": ref(SESSION / edit["diff_artifact"]),
                      "result": load(SESSION / edit["outcome_artifact"])})
    validations = []
    for path in sorted((SESSION / "validation").glob("[0-9][0-9][0-9][0-9].json")):
        v = load(path)
        for command in v["commands"]:
            for stream in ("stdout", "stderr"):
                command[stream] = read(SESSION / command[f"{stream}_artifact"]).decode()
        validations.append({"artifact": ref(path), "record": v})

    # Compile retained bytes directly; never import from the failed workspace.
    terminal = HARNESS / "terminal-workspace"
    ledger_bytes = read(terminal / "ledger.py")
    test_bytes = read(terminal / "test_ledger.py")
    ledger = types.ModuleType("ledger")
    exec(compile(ledger_bytes, str(terminal / "ledger.py"), "exec", dont_inherit=True), ledger.__dict__)
    prior_ledger = sys.modules.get("ledger")
    sys.modules["ledger"] = ledger
    try:
        test_module = types.ModuleType("retained_ledger_tests_no_cache")
        exec(compile(test_bytes, str(terminal / "test_ledger.py"), "exec", dont_inherit=True), test_module.__dict__)
        post, retract = test_module.post, test_module.retract
        p = post("p", 7)
        cases = [[p, retract("r", "p")], [retract("r", "p"), p],
                 [p, p, retract("r", "p"), retract("s", "p")]]
        case_results = [{"iteration": i, "input": messages,
                         "actual": ledger.positions_for(messages), "expected": {"cash": 0}}
                        for i, messages in enumerate(cases, 1)]
        failing_locals = []
        class CaptureResult(unittest.TextTestResult):
            def addFailure(self, test, err):
                tb = err[2]
                while tb:
                    if tb.tb_frame.f_code.co_name == "test_order_and_duplicates":
                        failing_locals.append({"test": test.id(), "line": tb.tb_lineno,
                                               "messages": tb.tb_frame.f_locals.get("messages")})
                    tb = tb.tb_next
                super().addFailure(test, err)
        stream = io.StringIO()
        result = unittest.TextTestRunner(stream=stream, verbosity=2, resultclass=CaptureResult).run(
            unittest.defaultTestLoader.loadTestsFromModule(test_module))
    finally:
        if prior_ledger is None:
            del sys.modules["ledger"]
        else:
            sys.modules["ledger"] = prior_ledger

    pycs = []
    for name in ("ledger", "test_ledger"):
        source = read(terminal / f"{name}.py")
        path = HARNESS / f"failed-workspace/__pycache__/{name}.cpython-311.pyc"
        data = read(path)
        assert data[:4] == importlib.util.MAGIC_NUMBER, "Use Python 3.11 for this retained cache"
        retained_code = marshal.loads(data[16:])
        pycs.append({"artifact": ref(path), "header_hex": data[:16].hex(),
                     "executable_structure_equals_current_source": code_identity(retained_code) == code_identity(compile(source, str(terminal / f"{name}.py"), "exec", dont_inherit=True)),
                     "sha256": hashlib.sha256(data).hexdigest()})
    stage_results = []
    ledger_edits = [x for x in edits if x["path"] == "ledger.py"]
    for label, source in [("initial", ledger_edits[0]["before"])] + [(f"after_action_{x['action']}", x["after"]) for x in ledger_edits]:
        stage_ns = {}
        exec(compile(source, label, "exec", dont_inherit=True), stage_ns)
        stage_results.append({"stage": label, "case_results": [stage_ns["positions_for"](case) for case in cases]})
    old = next(x for x in load(HERE.parent.parent / "failure-analysis/failures.json")["failures"]
               if x["id"] == "generalize_retractions_renamed-candidate-r001")
    old_source_path = ROOT / old["terminal_source"][0]["path"]
    old_ns = {}
    exec(compile(read(old_source_path), str(old_source_path), "exec", dont_inherit=True), old_ns)
    comparison = {"purpose": "Diagnostic failed-checkpoint comparison only; no successful result or patch borrowed",
                  "id": old["id"], "terminal_reason": old["terminal_reason"],
                  "budget": old["budget"], "source": ref(old_source_path),
                  "case_results": [old_ns["positions_for"](case) for case in cases],
                  "verification_stderr": ref(ROOT / old["latest_complete_diagnostics"]["stderr"]["path"])}
    diagnostics = {"schema_version": 1, "run": ref(RUN / "outcome.json"),
                   "scope": "Single frozen candidate02 G1 first failure; no model run or runtime edits",
                   "identity": {k: outcome[k] for k in ("candidate_id", "candidate_sha256", "execution_id", "identity_before", "identity_after", "manifest_sha256", "fixture_sha256", "protected_files_unchanged")},
                   "metrics": metrics, "envelope_metrics": envelope_metrics,
                   "remaining": {"actions": 0, "input_tokens": 262144 - metrics["prompt_tokens"], "output_tokens": 32768 - metrics["generated_tokens"]},
                   "event_counts": dict(Counter(x["type"] for x in events)),
                   "terminal_reason": read(HARNESS / "stderr.txt").decode(errors="replace").splitlines()[-1],
                   "independent_verification": load(HARNESS / "verification.json"),
                   "independent_stderr": read(HARNESS / "verification.stderr").decode(),
                   "source_sha256": hashlib.sha256(ledger_bytes).hexdigest(),
                   "cache_probe": {"python": sys.version, "method": "exec(compile(retained source bytes)) plus recursive pyc executable structure comparison; no workspace imports or bytecode writes",
                                   "pycs": pycs, "cases": case_results, "failing_test_locals": failing_locals,
                                   "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
                                   "test_output": stream.getvalue(),
                                   "limitation": "Falsifies stale cache as explanation of terminal failure. Retained final pyc cannot establish each prior import's cache identity."},
                   "edits": edits, "edit_stage_case_results": stage_results, "validation_records": validations,
                   "contexts": contexts, "actions": actions, "archived_comparison": comparison}
    assert cumulative_input == metrics["prompt_tokens"] and cumulative_output == metrics["generated_tokens"]
    assert [x["actual"] for x in case_results] == [{"cash": 0}, {"cash": 7}, {"cash": 0}]
    assert all(x["executable_structure_equals_current_source"] for x in pycs)
    assert all(x["has_complete_failure"] for x in contexts[6:])
    assert len(validations) == 3 and result.testsRun == 2 and len(result.failures) == 1
    diagnostics["input_sha256"] = READS
    diagnostics["all_read_inputs_unchanged"] = all(hashlib.sha256((ROOT / path).read_bytes()).hexdigest() == sha for path, sha in READS.items())
    assert diagnostics["all_read_inputs_unchanged"]
    target = HERE / "renamed-r001-analysis.json"
    target.write_text(json.dumps(diagnostics, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(target), "read_inputs": len(READS), "actions": len(actions),
                      "cache_equal": True, "case_results": [x["actual"] for x in case_results],
                      "tests_run": result.testsRun, "failures": len(result.failures),
                      "input_unchanged": diagnostics["all_read_inputs_unchanged"]}))


if __name__ == "__main__":
    main()
