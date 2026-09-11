"""Read-only extraction of all failed pilot trajectories; no inference or fixture execution.

Run from any directory with Python 3.11+. Outputs are confined to this directory.
The input files are hashed on first read and checked again before writing results.
"""
from __future__ import annotations

import ast
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SOURCE = ROOT / "benchmark/results/2026-09-09-agent-loop-v1"
INPUTS: dict[str, str] = {}
KINDS = ["system", "tools", "repo", "task", "memory", "source", "action", "result"]
LIMITS = {"turns": 32, "generated_tokens": 32768, "prompt_tokens": 262144, "duration_ms": 600000}


def rel(path):
    return Path(path).resolve().relative_to(ROOT).as_posix()


def read(path):
    path = Path(path)
    raw = path.read_bytes()
    key = rel(path)
    digest = hashlib.sha256(raw).hexdigest()
    if key in INPUTS:
        assert INPUTS[key] == digest, f"Input changed during extraction: {key}"
    INPUTS[key] = digest
    return raw.decode("utf-8-sig")


def data(path):
    return json.loads(read(path))


def ast_identity(text):
    try:
        return hashlib.sha256(ast.dump(ast.parse(text), include_attributes=False).encode()).hexdigest()
    except SyntaxError:
        return None


def evidence(path, text=False):
    content = read(path)
    item = {"path": rel(path), "sha256": INPUTS[rel(path)], "bytes": Path(path).stat().st_size}
    if text:
        item["text"] = content
    return item


def compact_context(path):
    c = data(path)
    groups = {}
    for kind in KINDS:
        ss = [s for s in c["segments"] if KINDS[s["kind"]] == kind]
        if ss:
            groups[kind] = {"segments": len(ss), "tokens": sum(s["tokens"] for s in ss),
                            "bytes": sum(len(s["text"].encode()) for s in ss),
                            "pinned_tokens": sum(s["tokens"] for s in ss if s["pinned"])}
    return {"path": rel(path), "capacity": c["capacity"], "reserve": c["reserve"],
            "planned": c["planned"], "planned_tokens": c["planned_tokens"],
            "evicted_segments": c["planned_evicted"], "segments": len(c["segments"]),
            "segment_tokens": sum(s["tokens"] for s in c["segments"]),
            "pinned_tokens": sum(s["tokens"] for s in c["segments"] if s["pinned"]),
            "by_kind": groups}


def extract_session(session):
    ep = session / "events.jsonl"
    events = [json.loads(line) for line in read(ep).splitlines() if line.strip()]
    direct = [e for e in events if e["type"] != "candidate_event"]
    calls, edits, validations, contexts, incomplete, reflections, repeats, finals = [], [], [], [], [], [], [], []
    turn, generated, cumulative_input = 0, 0, 0
    pending_inference, pending_output, latest_call = None, None, None
    pending_tokens = []
    for e in direct:
        typ, d = e["type"], e["data"]
        base = {"sequence": e["sequence"], "elapsed_ms": e["elapsed_ms"], "event_path": rel(ep)}
        if typ == "context_plan":
            pending_tokens = []
            turn = d["turn"]
            cp = session / d["artifact"]
            item = compact_context(cp)
            item.update(base, action=turn, cumulative_input_before=cumulative_input, generated_before=generated)
            rendered = cp.with_suffix(".txt")
            if rendered.exists():
                item["rendered_prompt"] = evidence(rendered)
            contexts.append(item)
        elif typ == "inference":
            generated += d["generated_tokens"]
            cumulative_input += d["prompt_tokens"]
            pending_inference = dict(base, **d)
            if contexts:
                contexts[-1]["inference"] = d
        elif typ == "model_output":
            pending_output = dict(base, output=d)
        elif typ == "token":
            pending_tokens.append(d)
        elif typ == "tool_call":
            latest_call = dict(base, action=turn, **d)
            calls.append(latest_call)
            pending_inference, pending_output = None, None
            pending_tokens = []
        elif typ in ["final", "final_rejected"]:
            finals.append(dict(base, action=turn, accepted=typ == "final", result=d,
                               model_output=pending_output, inference=pending_inference))
            pending_inference, pending_output = None, None
            pending_tokens = []
        elif typ == "tool_result":
            if latest_call:
                latest_call["result"] = dict(base, **d)
        elif typ == "edit_prepared":
            item = dict(base, action=turn, **d)
            for key in ["before_artifact", "after_artifact", "diff_artifact", "outcome_artifact"]:
                p = session / d[key]
                if p.exists():
                    item[key.replace("artifact", "evidence")] = evidence(p, text=key == "diff_artifact")
            before = read(session / d["before_artifact"]) if d["before_exists"] else ""
            after = read(session / d["after_artifact"])
            item["before_ast_sha256"] = ast_identity(before)
            item["after_ast_sha256"] = ast_identity(after)
            item["ast_unchanged"] = item["before_ast_sha256"] is not None and item["before_ast_sha256"] == item["after_ast_sha256"]
            edits.append(item)
        elif typ == "edit_result":
            for item in reversed(edits):
                if item["tool_call"] == d["tool_call"]:
                    item["result"] = dict(base, **d)
                    break
        elif typ == "validation_result":
            item = dict(base, action=turn, **d)
            for cmd in item.get("commands", []):
                for key in ["stdout_artifact", "stderr_artifact"]:
                    p = session / cmd[key]
                    if p.exists():
                        cmd[key.replace("artifact", "evidence")] = evidence(p, text=True)
            validations.append(item)
        elif typ == "failure_reflection":
            reflections.append(dict(base, **d))
        elif typ == "semantic_loop":
            repeats.append(dict(base, **d))
    if pending_output or pending_inference or pending_tokens:
        incomplete.append({"action": turn, "inference": pending_inference,
                           "model_output": pending_output, "raw_token_count": len(pending_tokens),
                           "raw_text": "".join(pending_tokens)})
    final_context = session / "context/final.json"
    final = compact_context(final_context) if final_context.exists() else None
    applied = [e for e in edits if e.get("result", {}).get("state") == "applied"]
    complete = [v for v in validations if v.get("evidence_complete")]
    diagnostic = complete[-1] if complete else None
    last_edit = applied[-1] if applied else None
    return {"session": rel(session), "events": evidence(ep), "calls": calls,
            "tool_counts": dict(Counter(c["tool"] for c in calls)), "edits": edits,
            "applied_edits": len(applied), "ast_unchanged_applied_edits": sum(e["ast_unchanged"] for e in applied),
            "last_changed_candidate": last_edit, "validations": validations,
            "latest_complete_agent_validation": diagnostic,
            "edits_after_latest_complete_validation": [e["tool_call"] for e in applied if not diagnostic or e["sequence"] > diagnostic["sequence"]],
            "prompt_growth": contexts, "terminal_context": final,
            "incomplete_native_generations": incomplete, "reflections": reflections,
            "semantic_events": repeats,
            "final_calls": finals, "final_call_count": len(finals),
            "accepted_final_count": sum(f["accepted"] for f in finals),
            "native_call_count": len(calls) + len(finals)}


def budget_report(metrics, limits):
    return {k: {"limit": limit, "used": metrics[k], "remaining": limit - metrics[k]} for k, limit in limits.items()}


def terminal_sources(workspace, protected):
    return [dict(evidence(p, text=True), ast_sha256=ast_identity(read(p)))
            for p in sorted(workspace.glob("*.py")) if p.name not in protected]


def hypotheses(record):
    s, reasons, result = record["trace"], record["terminal_reason"], []
    fail = record["latest_complete_diagnostics"]["stderr"]["text"]
    if "Pinned context" in reasons:
        result.append({"mechanism": "context_exhaustion", "observation": "Every historical segment is pinned; the terminal segment sum exceeds capacity minus reserve while other task budgets remain.",
                       "test": "Replay this final context with bounded complete exchanges and the same task/current diagnostics. The next prompt and complete native repair/final call must fit 16384 context and 2048 reserve, without dropping task or current evidence."})
    if "complete native call" in reasons or s["incomplete_native_generations"]:
        result.append({"mechanism": "incomplete_native_call", "observation": "The bounded reflection response exhausted generation before a complete parsed call; the root stopped with substantial shared budget remaining.",
                       "test": "Replay the malformed reflection at its original bounded output allowance; consume one reflection attempt, return to ordinary repair within remaining task limits, and require a complete final after a passing candidate."})
    if record["scope"] == "child":
        result.append({"mechanism": "candidate_allocation", "observation": "The independent child reaches its action allocation without a passing completed candidate; secondary verification also fails.",
                       "test": "Compare repaired single-candidate and equal-split search at identical shared limits, including every losing child; do not promote this incomplete child or assign a fresh task budget."})
    if record["scope"] == "root" and record.get("child_failures"):
        result.append({"mechanism": "no_eligible_candidate", "observation": "Neither child qualified, and the root terminal workspace is the restored broken baseline. Root failure is distinct from each child's repair mechanism.",
                       "test": "Inspect linked child traces and retain exact restore/protected-file evidence. Only select a completed independently passing candidate; measure a repaired single trajectory before changing allocation."})
    if s["ast_unchanged_applied_edits"]:
        result.append({"mechanism": "ineffective_comment_or_format_edits", "observation": f"{s['ast_unchanged_applied_edits']} applied edits preserve Python AST exactly; these change bytes without changing executable behavior.",
                       "test": "Replay these edits and repeated diagnostic operands. Recovery must remain active until a behavior-changing candidate passes complete validation; comment/format edits must not reset recovery."})
    if record["semantic_repeats"]:
        result.append({"mechanism": "repetition", "observation": f"Host semantic repetition fired {record['semantic_repeats']} times without a passing terminal candidate.",
                       "test": "Replay canonical diagnostics and current source, retaining failing operands; the next bounded step should inspect/edit an implicated operation rather than repeat a known failed candidate or restate its hypothesis."})
    assertion = re.findall(r"(?:AssertionError:.*|(?:Syntax|Indentation|Name|Type|Key)Error:.*)", fail)
    source = next(s for s in record["terminal_source"] if Path(s["path"]).name in ["service.py", "ledger.py"])
    if "AssertionError: {} != {'cash': 0}" in fail:
        first_wrong = "A cancelled posting is skipped before creating its account's zero entry. Cancellation affects the amount, but the terminal implementation also removes the required account key."
        test = "Trace cancellation followed by posting and check account initialization independently from amount addition; preserve the zero account entry and rerun unchanged full tests, including duplicates and unmatched retractions."
    elif "AssertionError: {'cash': -7} != {'cash': 0}" in fail:
        first_wrong = "The source tests target IDs against cancelled but adds retraction event IDs, allowing two distinct retractions of one posting to subtract its amount twice. Queued retractions also subtract once per queued event."
        test = "Trace one posting followed by two distinct retractions of that posting. The cancellation identity must be the referenced posting; observe exactly one subtraction in either arrival order and rerun unchanged full tests."
    else:
        assert "AssertionError: {'cash': 7} != {'cash': 0}" in fail, assertion
        first_wrong = "A posting adds its amount after an earlier retraction targeted it. The posting branch never consults prior cancellation, or the retraction branch fails to retain unmatched target IDs."
        test = "Trace retraction then its posting against this exact source, identify the missing state check/update, preserve zero-account initialization and global event idempotence, then rerun unchanged full tests."
    result.append({"mechanism": "ineffective_logic_repair", "observation": "Independent terminal verification fails: " + "; ".join(assertion),
                   "source_evidence": source["path"], "first_incorrect_operation_hypothesis": first_wrong,
                   "test": test + " Context relief or completion relabelling alone cannot satisfy this test."})
    return result


def link(path, label):
    return "[" + label + "](" + Path(os.path.relpath(ROOT / path, HERE)).as_posix() + ")"


def write_markdown(report):
    failures = report["failures"]
    contexts = [r for r in failures if "Pinned context" in r["terminal_reason"]]
    lines = ["# L0 failure evidence", "", "All **23 failed roots and 15 failed child trials** are classified. All 38 independent secondary verifications failed with unchanged protected files. The original campaign remains unchanged; this extraction launched no model or verifier process.", "",
             "[failures.json](failures.json) contains exact terminal source and hashes, the last applied edit and patch, all native calls, every complete host validation and diagnostic artifact, later edits that invalidate that validation, per-action prompt growth, remaining budgets, and a falsifiable mechanism hypothesis for each record. [analyze_failures.py](analyze_failures.py) regenerates it from the immutable pilot with Python 3.11+.", "",
             "## Mechanisms and next tests", "",
             "- Eleven single-candidate failures hit pinned history before exhausting the original actions/tokens/time. Terminal segment totals are 14,348–15,214 against 14,336 available input tokens. Assistant action history consumes 7,610–9,378, tool result history 3,034–4,561, and accumulated host source/state segments 1,334–1,664. Every recorded eviction count is zero. Bounded complete exchanges and replaceable current state are the first capacity experiment; rerun the same long-history prefixes at unchanged limits.",
             "- The renamed reflection run stops on action 12 when a 256-token bounded reflection never yields a complete native call. It retains 20 actions, 30,270 generated tokens, 227,616 cumulative input tokens, and 537,500 ms. Its raw partial response is retained. Consume the failed reflection attempt and resume bounded ordinary repair rather than making this a terminal limit.",
             "- Fourteen failed children reach 16/16 actions; renamed-combined trial 2 reaches 21/21 after the completed first child leaves capacity unused. Existing allocation already reclaims unused actions. Every failed child's final native call is complete but rejected by host validation; this is separate from the one incomplete reflection. Seven failed multi-candidate roots restore the broken baseline because neither child qualifies. The failed child in the successful renamed-combined root is also retained. Repaired single-candidate measurement must precede allocation changes.",
             "- Terminal logic failures reduce to three observed operands: `7` instead of `0` (prior cancellation ignored), missing `cash` instead of zero (cancelled posting skipped before account initialization), and `-7` instead of `0` (duplicate cancellation tracked with the wrong identity). Each record links its exact source and assertion; these are diagnostic findings, not task-specific runtime guidance.",
             "- AST-preserving applied edits and semantic repetition are reported independently. A new byte hash, rewritten comment, changed diagnostic, or complete final call cannot by itself establish repair progress. Several last validations predate further edits; terminal secondary verification supplies the current verdict.", "",
             "## Pressure replay inventory", "",
             "Segment counts below are the host's archived segment-token accounting. Actual native inference prompt counts, serialized prompt bytes and host planned totals are retained separately per action; they must not be substituted for one another.", "",
             "| Failure | Last native input | Terminal pinned sum | Actions remaining | Generated remaining | Cumulative input remaining |", "| --- | ---: | ---: | ---: | ---: | ---: |"]
    for r in contexts:
        t = r["trace"]
        rem = r["budget"]["remaining"]
        last = t["prompt_growth"][-1]
        name = r["task"].replace("generalize_retractions_", "") + "/" + r["arm"]
        lines.append(f"| {link(t['terminal_context']['path'], name)} | {last.get('inference', {}).get('prompt_tokens', 'not emitted')} | {t['terminal_context']['pinned_tokens']} | {rem['turns']['remaining']} | {rem['generated_tokens']['remaining']} | {rem['prompt_tokens']['remaining']} |")
    for scope in ["root", "child"]:
        lines += ["", "## " + ("Failed roots" if scope == "root" else "Failed children"), "",
                  "`last edit` identifies the last applied artifact, not proof of a successful candidate. `AST same` counts applied edits whose before/after Python ASTs are equal. The JSON retains complete command arguments, diagnostics, contexts and budgets.", "",
                  "| Failure | Boundary | Terminal actual → expected | Last edit / action | AST same | Evidence |", "| --- | --- | --- | --- | ---: | --- |"]
        for r in failures:
            if r["scope"] != scope:
                continue
            t = r["trace"]
            label = r["task"].replace("generalize_retractions_", "") + "/" + r["arm"]
            if scope == "child":
                label += "/" + str(r["candidate"])
            reason = r["terminal_reason"]
            boundary = "context" if "Pinned context" in reason else "incomplete reflection" if "complete native call" in reason else "no eligible child" if r.get("child_failures") else "independent tests" if t["accepted_final_count"] else "action allocation"
            err = r["latest_complete_diagnostics"]["stderr"]["text"]
            operand = "missing key → 0" if "AssertionError: {}" in err else "−7 → 0" if "'cash': -7" in err else "7 → 0"
            edit = t["last_changed_candidate"]
            edit_label = link(edit["diff_evidence"]["path"], str(edit["tool_call"])) + " / " + str(edit["action"]) if edit else "restored baseline"
            src = next(s for s in r["terminal_source"] if Path(s["path"]).name in ["service.py", "ledger.py"])
            refs = link(src["path"], "source") + ", " + link(r["latest_complete_diagnostics"]["stderr"]["path"], "diagnostics") + ", " + link(t["events"]["path"], "events")
            lines.append(f"| {label} | {boundary} | {operand} | {edit_label} | {t['ast_unchanged_applied_edits']} | {refs} |")
    lines += ["", "## Reproduction and limits", "", "Run `python benchmark/results/2026-09-09-agent-loop-all-green/failure-analysis/analyze_failures.py` from the repository root. The script asserts the complete 23+15 population, unique IDs, failed terminal tests, unchanged protected files, and stable SHA-256 for every evidence file read. Exact evidence links are repository-relative in JSON and relative to this report in Markdown.", "", "Child action allocations and usage are archived. Per-child generated/input/time caps were not emitted in `candidate_start`; the report does not invent them. Shared original task usage/remaining and child usage remain explicit. Root and child populations overlap and are not independent success denominators. No claim is made that capacity relief alone repairs any program.", ""]
    (HERE / "README.md").write_text("\n".join(lines), encoding="utf-8")


def build():
    analysis = data(SOURCE / "analysis.json")
    outcomes = {r["cell_id"]: r for r in data(SOURCE / "outcomes.json")}
    protocol = evidence(SOURCE / "protocol.json")
    records = []
    for run in analysis["runs"]:
        cell = SOURCE / "cells" / run["cell_id"]
        outcome = outcomes[run["cell_id"]]
        protected = outcome["record"]["protected_files"]
        for child in run["child_terminal_verifications"]:
            if not child["failed_or_incomplete_child"]:
                continue
            info = next(c for c in run["candidate_children"] if c["candidate"] == child["candidate"])
            session, workspace = Path(child["session"]), Path(child["workspace"])
            verification = SOURCE / "child-terminal-verification" / run["cell_id"] / child["trial"]
            record = {"id": run["cell_id"] + "/" + child["trial"], "cell_id": run["cell_id"],
                      "scope": "child", "arm": run["arm"], "task": run["task"],
                      "candidate": child["candidate"], "allocation": info["allocation"],
                      "terminal_reason": "Child action allocation exhausted without a passing completed candidate (generation status: " + child["generation_status"] + ").",
                      "budget": {"scope": "child action limit plus original shared root budget",
                                 "allocated_actions": budget_report(info["metrics"], {"turns": info["allocation"]["max_turns"]}),
                                 "child_usage": info["metrics"], "shared_root_terminal": budget_report(run["metrics"], LIMITS),
                                 "token_time_allocation_note": "Child generated/input/time caps are not emitted in candidate_start; original shared total remaining is reported without fabricating per-child caps."},
                      "trace": extract_session(session), "terminal_source": terminal_sources(workspace, protected),
                      "latest_complete_diagnostics": {"scope": "independent secondary terminal verification", "result": evidence(verification / "result.json", text=True),
                                                      "stdout": evidence(verification / "verification.stdout", text=True), "stderr": evidence(verification / "verification.stderr", text=True)},
                      "protected_files_unchanged": child["verification"]["protected_files_unchanged"],
                      "terminal_tests_pass": child["tests_pass"],
                      "semantic_repeats": sum(e["candidate"] == child["candidate"] and e["data"].get("repeated_failed_state", False) for e in run["semantic_events"])}
            records.append(record)
        if run["passed"]:
            continue
        candidates = [p.parent for p in cell.rglob("events.jsonl") if "trial-" not in str(p)]
        session = min(candidates, key=lambda p: len(str(p)))
        workspace = Path(run["terminal_verification"]["workspace"])
        verification = SOURCE / "terminal-verification" / run["cell_id"]
        record = {"id": run["cell_id"], "cell_id": run["cell_id"], "scope": "root", "arm": run["arm"], "task": run["task"],
                  "terminal_reason": run["terminal_reason"], "primary_failure_classes": run["failure_classes"],
                  "budget": {"scope": "original shared task", "remaining": budget_report(run["metrics"], LIMITS)},
                  "trace": extract_session(session), "terminal_source": terminal_sources(workspace, protected),
                  "latest_complete_diagnostics": {"scope": "independent secondary terminal verification", "result": evidence(verification / "result.json", text=True),
                                                  "stdout": evidence(verification / "verification.stdout", text=True), "stderr": evidence(verification / "verification.stderr", text=True)},
                  "protected_files_unchanged": run["protected_files_unchanged"], "terminal_tests_pass": run["terminal_tests_pass"],
                  "semantic_repeats": run["semantic_repeats"],
                  "child_failures": [run["cell_id"] + "/" + c["trial"] for c in run["child_terminal_verifications"] if c["failed_or_incomplete_child"]]}
        records.append(record)
    records.sort(key=lambda r: (r["cell_id"], r["scope"] != "root", r["id"]))
    for r in records:
        r["hypotheses"] = hypotheses(r)
    counts = Counter(r["scope"] for r in records)
    assert counts == {"root": 23, "child": 15}, counts
    assert len({r["id"] for r in records}) == 38
    assert all(not r["terminal_tests_pass"] and r["protected_files_unchanged"] for r in records)
    for name, digest in INPUTS.items():
        assert hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest, f"Input mutated: {name}"
    report = {"schema_version": 1, "source_campaign": rel(SOURCE), "protocol": protocol,
              "extractor_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "population": dict(counts), "all_38_secondary_verifications_failed": True,
              "all_read_evidence_unchanged": True, "input_sha256": INPUTS,
              "notes": ["All paths are repository-relative. Sequences refer to the linked session events JSONL.",
                        "Numbered context tokens are host segment/planner counts; inference.prompt_tokens is the actual rendered native inference count. Do not conflate them.",
                        "Independent secondary diagnostics correspond to terminal inputs; latest complete agent validation may be stale after a later edit and is reported separately.",
                        "AST equality proves unchanged syntax structure for these Python edits, not general semantic equivalence. Distinct ASTs need not change behavior.",
                        "The multi-candidate root terminal is a restored baseline; child failures remain separate, and no child success is borrowed.",
                        "This extractor reads existing evidence only; it performs no model run, test execution, fixture mutation, or runtime edit."],
              "failures": records}
    (HERE / "failures.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    pressure = []
    for r in records:
        if not any(h["mechanism"] in ["context_exhaustion", "incomplete_native_call"] for h in r["hypotheses"]):
            continue
        t = r["trace"]
        pressure.append({"id": r["id"], "terminal_reason": r["terminal_reason"], "budget": r["budget"],
                         "session": t["session"], "terminal_context": t["terminal_context"],
                         "prompt_growth": t["prompt_growth"],
                         "last_edit": t["last_changed_candidate"],
                         "latest_host_validation_sequence": t["latest_complete_agent_validation"]["sequence"] if t["latest_complete_agent_validation"] else None,
                         "edits_after_validation": t["edits_after_latest_complete_validation"],
                         "incomplete_native_generations": t["incomplete_native_generations"]})
    (HERE / "pressure-traces.json").write_text(json.dumps({"schema_version": 1, "traces": pressure}, indent=2) + "\n", encoding="utf-8")
    write_markdown(report)
    print(json.dumps({"population": dict(counts), "read_files_unchanged": len(INPUTS),
                      "mechanisms": dict(Counter(h["mechanism"] for r in records for h in r["hypotheses"]))}, indent=2))


if __name__ == "__main__":
    build()
