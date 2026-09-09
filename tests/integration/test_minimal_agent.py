"""Scripted evidence for the experimental control, not evidence of model repair quality."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())


def call(name, arguments=None, reasoning=None):
    message = {"role": "assistant", "content": "", "tool_calls": [
        {"type": "function", "function": {"name": name,
         "arguments": json.dumps(arguments or {}, separators=(",", ":"))}}]}
    if reasoning is not None:
        message["reasoning_content"] = reasoning
    return message


def read(path, reasoning=None):
    return call("read_file", {"path": path, "start": 1, "end": 200}, reasoning)


def patch(path, old, new):
    return call("apply_patch", {"path": path, "old_text": old, "new_text": new})


class MinimalAgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forge-minimal-")
        self.root = pathlib.Path(self.temp.name)
        (self.root / "sample.txt").write_text("original\n", encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, actions, *options, success=True, minimal=True, allow_write=True):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        argv = [FORGE, "run", "Repair the workspace and run its tests.", "--script", str(script),
                "--workspace", str(self.root), "--no-config", "--json",
                "--allow-exec", "--prompt-protocol", "native", "--context", "65536",
                "--output-reserve", "4096"]
        if minimal:
            argv.append("--minimal-agent")
        if allow_write:
            argv.append("--allow-write")
        result = subprocess.run([*argv, *options], capture_output=True, encoding="utf-8", timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        sessions = list((self.root / ".forge" / "sessions").iterdir())
        self.assertEqual(len(sessions), 1)
        session = sessions[0]
        metrics = json.loads((session / "metrics.json").read_text(encoding="utf-8"))
        self.assertTrue(metrics["simulated"] or not metrics["generated_tokens"])
        return events, session, metrics, result

    @staticmethod
    def outputs(events):
        return [event["data"] for event in events if event["type"] == "tool_result"]

    def test_complete_history_and_repeated_failure_have_no_recovery(self):
        reasoning = "The entire successful reasoning must remain visible. " * 65
        bad = call("run_command", {"argv": [sys.executable, "-c", "print('failed'); raise SystemExit(1)"]})
        events, session, metrics, _ = self.run_script([
            read("sample.txt", reasoning), bad, bad, read("sample.txt"),
            call("final", {"answer": "Tests failed; no repair made."}),
        ])
        self.assertEqual(len(self.outputs(events)), 4)
        self.assertEqual(metrics["turns"], 5)
        self.assertEqual(metrics["loop_warnings"], 0)
        self.assertEqual(metrics["validation_commands"], 0)
        self.assertEqual(metrics["repo_full_scans"], 0)
        self.assertEqual(metrics["context_evictions"], 0)
        self.assertNotIn("recovery", [event["type"] for event in events])
        prompts = [json.loads(path.read_text(encoding="utf-8")) for path in
                   sorted((session / "context").glob("[0-9][0-9][0-9][0-9].txt"))]
        for earlier, later in zip(prompts, prompts[1:]):
            self.assertEqual(earlier["messages"], later["messages"][:len(earlier["messages"])])
            self.assertEqual(earlier["tools"], later["tools"])
        final = prompts[-1]
        assistant = [message for message in final["messages"] if message["role"] == "assistant"]
        self.assertEqual(assistant[0]["content"], reasoning)
        ids = [message["tool_calls"][0]["id"] for message in assistant]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(ids, [message["tool_call_id"] for message in final["messages"]
                               if message["role"] == "tool"])
        self.assertNotIn("next_action_guidance", json.dumps(final))
        self.assertEqual({tool["function"]["name"] for tool in final["tools"]},
                         {"read_file", "apply_patch", "run_command", "list_directory", "final"})

    def test_multiple_files_allow_intermediate_invalid_syntax_and_no_final_validation(self):
        (self.root / "a.go").write_text("package sample\nfunc Value() int { return 1 }\n")
        (self.root / "b.go").write_text("package sample\nfunc Other() int { return 1 }\n")
        events, session, metrics, _ = self.run_script([
            patch("a.go", "return 1 }", "return 2"),  # Deliberately incomplete until the third edit.
            patch("b.go", "return 1", "return 2"),
            patch("a.go", "return 2", "return 2 }"),
            call("final", {"answer": "Edited two files; tests were not run."}),
        ])
        self.assertEqual([output["status"] for output in self.outputs(events)], ["ok"] * 3)
        self.assertEqual(metrics["files_modified"], 2)
        self.assertEqual(metrics["validation_commands"], 0)
        self.assertEqual(metrics["tool_calls"], 3)
        self.assertIn("return 2 }", (self.root / "a.go").read_text())
        self.assertIn("return 2 }", (self.root / "b.go").read_text())
        self.assertEqual(len(list((session / "tool").glob("*.patch"))), 3)
        self.assertEqual(list((session / "validation").iterdir()), [])

    def test_noop_is_visible_and_does_not_prevent_next_edit(self):
        events, _, metrics, _ = self.run_script([
            patch("sample.txt", "original", "original"),
            patch("sample.txt", "original", "repaired"),
            call("final", {"answer": "Repaired."}),
        ])
        outputs = self.outputs(events)
        self.assertEqual(outputs[0]["status"], "conflict")
        self.assertIn("identical", outputs[0]["output"])
        self.assertEqual(outputs[1]["status"], "ok")
        self.assertEqual(metrics["files_modified"], 1)
        self.assertEqual(metrics["loop_warnings"], 0)

    def test_last_action_keeps_the_same_tools_and_hard_turn_limit(self):
        events, session, metrics, result = self.run_script(
            [read("sample.txt")] * 4, "--max-turns", "3", success=False)
        self.assertEqual(len(self.outputs(events)), 3)
        self.assertEqual(metrics["turns"], 3)
        self.assertIn("Maximum turns", result.stderr)
        first = json.loads((session / "context" / "0001.txt").read_text())
        last = json.loads((session / "context" / "0003.txt").read_text())
        self.assertEqual(first["tools"], last["tools"])

    def test_context_overflow_stops_without_dropping_history(self):
        (self.root / "sample.txt").write_text("x" * 6000)
        events, session, metrics, _ = self.run_script(
            [read("sample.txt"), call("final", {"answer": "Cannot fit."})],
            "--context", "2048", "--output-reserve", "256", success=False)
        self.assertEqual(len(self.outputs(events)), 1)
        self.assertEqual(metrics["context_evictions"], 0)
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 1)
        final = json.loads((session / "context" / "final.json").read_text())
        self.assertIn("x" * 6000, json.dumps(final))

    def test_input_budget_stops_before_generation(self):
        events, _, metrics, _ = self.run_script(
            [call("final", {"answer": "Unused."})], "--max-input", "1", success=False)
        self.assertFalse([event for event in events if event["type"] == "inference"])
        self.assertEqual(metrics["generated_tokens"], 0)

    def test_generated_budget_bounds_first_response(self):
        events, _, metrics, _ = self.run_script(
            [call("final", {"answer": "Unused."})], "--max-tokens", "1", success=False)
        self.assertFalse([event for event in events if event["type"] == "final"])
        self.assertLessEqual(metrics["generated_tokens"], 1)

    def test_write_denial_remains_enforced(self):
        events, _, metrics, _ = self.run_script([
            patch("sample.txt", "original", "repaired"),
            call("final", {"answer": "Write was denied."}),
        ], allow_write=False)
        self.assertEqual(self.outputs(events)[0]["status"], "policy")
        self.assertEqual(metrics["files_modified"], 0)
        self.assertEqual((self.root / "sample.txt").read_text(), "original\n")

    def test_invalid_native_action_does_not_trigger_hidden_retry(self):
        events, _, metrics, _ = self.run_script(
            ["not a native message", call("final", {"answer": "Never reached."})], success=False)
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 1)
        self.assertEqual(metrics["tool_calls"], 0)

    def test_minimal_listing_needs_no_repository_index(self):
        (self.root / "nested").mkdir()
        (self.root / "nested" / "other.txt").write_text("other")
        events, _, metrics, _ = self.run_script([
            call("list_directory"), call("final", {"answer": "Listed files."}),
        ])
        output = self.outputs(events)[0]["output"].replace("\\", "/")
        self.assertIn("nested/other.txt", output)
        self.assertNotIn(".forge", output)
        self.assertEqual(metrics["repo_full_scans"], 0)

    def test_default_agent_retains_full_schema_and_go_syntax_guard(self):
        (self.root / "a.go").write_text("package sample\nfunc Value() int { return 1 }\n")
        events, session, _, _ = self.run_script([
            patch("a.go", "return 1 }", "return 2"),
            call("final", {"answer": "No change applied."}),
        ], minimal=False)
        self.assertNotEqual(self.outputs(events)[0]["status"], "ok")
        self.assertIn("return 1 }", (self.root / "a.go").read_text())
        first = json.loads((session / "context" / "0001.txt").read_text())
        self.assertIn("memory", {tool["function"]["name"] for tool in first["tools"]})
        self.assertIn("apply_hunk", {tool["function"]["name"] for tool in first["tools"]})


if __name__ == "__main__":
    unittest.main()
