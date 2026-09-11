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

    def run_script(self, actions, *options, success=True, minimal=True, allow_write=True,
                   allow_exec=True):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        argv = [FORGE, "run", "Repair the workspace and run its tests.", "--script", str(script),
                "--workspace", str(self.root), "--no-config", "--json",
                "--prompt-protocol", "native", "--context", "65536",
                "--output-reserve", "4096"]
        if minimal:
            argv.append("--minimal-agent")
        if allow_write:
            argv.append("--allow-write")
        if allow_exec:
            argv.append("--allow-exec")
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

    def candidate_fixture(self, two_files=False):
        (self.root / "value.py").write_text("def value():\n    return 1\n")
        (self.root / "test_value.py").write_text(
            "import unittest\nfrom value import value\n"
            + ("from other import other\n" if two_files else "")
            + "class Tests(unittest.TestCase):\n"
              "    def test_value(self):\n        self.assertEqual(value(), 2)\n"
            + ("    def test_other(self):\n        self.assertEqual(other(), 3)\n"
               if two_files else ""))
        if two_files:
            (self.root / "other.py").write_text("def other():\n    return 1\n")

    @staticmethod
    def checkpoints(events):
        return [event["data"] for event in events if event["type"] == "candidate_checkpoint"]

    def test_candidate_multifile_and_completion_reuse(self):
        self.candidate_fixture(two_files=True)
        events, session, metrics, _ = self.run_script([
            patch("value.py", "return 1", "return 2"), read("other.py"),
            patch("other.py", "return 1", "return 3"), call("validate_candidate"),
            call("final", {"answer": "Both repairs passed host validation."}),
        ], "--candidate-checkpoint")
        checks = self.checkpoints(events)
        self.assertTrue(checks[0]["validated"])
        self.assertTrue(checks[0]["passed"])
        self.assertEqual(checks[0]["candidate_attempts"], 1)
        self.assertEqual(checks[1]["commands"], 0)
        self.assertTrue(checks[1]["passed"])
        last = json.loads((session / "context" / "0005.txt").read_text())
        self.assertEqual([tool["function"]["name"] for tool in last["tools"]], ["final"])
        self.assertGreater(metrics["validation_commands"], 0)
        prompts = [json.loads((session / "context" / f"{turn:04d}.txt").read_text())
                   for turn in range(1, 6)]
        for earlier, later in zip(prompts, prompts[1:]):
            self.assertEqual(earlier["messages"], later["messages"][:len(earlier["messages"])])
        self.assertNotIn("CANDIDATE_STATE:", prompts[0]["messages"][0]["content"])

    def test_candidate_episode_survives_reads_noops_and_failed_candidates(self):
        self.candidate_fixture()
        events, session, _, _ = self.run_script([
            patch("value.py", "return 1", "return 3"), call("validate_candidate"),
            read("value.py"), patch("value.py", "return 3", "return 3"),
            call("validate_candidate"), patch("value.py", "return 3", "return 1"),
            call("validate_candidate"), patch("value.py", "return 1", "return 2"),
            call("validate_candidate"), call("final", {"answer": "Repaired and checked."}),
        ], "--candidate-checkpoint")
        checks = self.checkpoints(events)
        self.assertEqual([x["candidate_attempts"] for x in checks], [1, 1, 1, 2, 2])
        self.assertEqual([x["episode_active"] for x in checks], [True, True, True, False, False])
        self.assertFalse(checks[1]["novel"])
        self.assertFalse(checks[2]["changed"])
        self.assertEqual(checks[1]["commands"], 0)
        prompt = (session / "context" / "0005.txt").read_text()
        self.assertIn("episode_active=true", prompt)

    def test_candidate_reserves_validation_then_final(self):
        self.candidate_fixture()
        events, session, metrics, _ = self.run_script([
            patch("value.py", "return 1", "return 2"), call("validate_candidate"),
            call("final", {"answer": "Repaired."}),
        ], "--candidate-checkpoint", "--max-turns", "3")
        second = json.loads((session / "context" / "0002.txt").read_text())
        self.assertEqual([tool["function"]["name"] for tool in second["tools"]], ["validate_candidate"])
        self.assertEqual(metrics["turns"], 3)
        self.assertTrue(self.checkpoints(events)[-1]["passed"])

    def test_candidate_dispatch_enforces_reserved_action(self):
        self.candidate_fixture()
        events, _, metrics, result = self.run_script([
            patch("value.py", "return 1", "return 2"),
            call("run_command", {"argv": [sys.executable, "-c", "raise Exception('must not run')"]}),
        ], "--candidate-checkpoint", "--max-turns", "3", success=False)
        self.assertIn("Reserved checkpoint", result.stderr)
        self.assertEqual(len(self.outputs(events)), 1)
        self.assertEqual(metrics["turns"], 2)

    def test_candidate_passing_tests_without_change_cannot_finish(self):
        self.candidate_fixture()
        (self.root / "value.py").write_text("def value():\n    return 2\n")
        events, _, metrics, _ = self.run_script([
            call("final", {"answer": "Done."}), call("validate_candidate"),
            call("final", {"answer": "Done."}),
        ], "--candidate-checkpoint", "--max-turns", "3", success=False)
        self.assertEqual(metrics["validation_commands"], 0)
        self.assertTrue(all(not x["changed"] for x in self.checkpoints(events)))
        self.assertNotIn("final", [x["type"] for x in events])

    def test_candidate_early_final_runs_host_validation(self):
        self.candidate_fixture()
        events, _, _, _ = self.run_script([
            patch("value.py", "return 1", "return 2"),
            call("final", {"answer": "Repair complete."}),
        ], "--candidate-checkpoint")
        self.assertTrue(self.checkpoints(events)[0]["validated"])
        self.assertTrue(self.checkpoints(events)[0]["passed"])

    def test_candidate_exit_zero_command_cannot_establish_pass(self):
        self.candidate_fixture()
        events, _, _, _ = self.run_script([
            patch("value.py", "return 1", "return 3"),
            call("run_command", {"argv": [sys.executable, "-c", "print('OK')"]}),
            call("validate_candidate"), call("final", {"answer": "Tests passed."}),
        ], "--candidate-checkpoint", "--max-turns", "4", success=False)
        self.assertEqual(self.outputs(events)[1]["exit_code"], 0)
        self.assertFalse(any(x["passed"] for x in self.checkpoints(events)))
        self.assertNotIn("final", [x["type"] for x in events])

    def test_candidate_without_tests_is_blocked(self):
        (self.root / "value.py").write_text("def value():\n    return 1\n")
        events, _, metrics, _ = self.run_script([
            patch("value.py", "return 1", "return 2"), call("validate_candidate"),
            call("final", {"answer": "Done."}),
        ], "--candidate-checkpoint", "--max-turns", "3", success=False)
        self.assertEqual(metrics["validation_commands"], 0)
        self.assertTrue(all(x["candidate_attempts"] == 0 for x in self.checkpoints(events)))

    def test_candidate_validation_obeys_process_denial(self):
        self.candidate_fixture()
        events, _, metrics, _ = self.run_script([
            patch("value.py", "return 1", "return 2"), call("validate_candidate"),
            call("final", {"answer": "Done."}),
        ], "--candidate-checkpoint", "--max-turns", "3", allow_exec=False, success=False)
        self.assertEqual(metrics["validation_commands"], 0)
        self.assertFalse(any(x["passed"] for x in self.checkpoints(events)))

    def test_candidate_validation_rejects_mutating_test(self):
        self.candidate_fixture()
        with (self.root / "test_value.py").open("a") as target:
            target.write("        from pathlib import Path\n"
                         "        Path('value.py').write_text('def value(): return 3\\n')\n")
        events, _, _, _ = self.run_script([
            patch("value.py", "return 1", "return 2"), call("validate_candidate"),
            call("final", {"answer": "Done."}),
        ], "--candidate-checkpoint", "--max-turns", "3", success=False)
        self.assertFalse(self.checkpoints(events)[0]["passed"])
        self.assertFalse(self.checkpoints(events)[0]["validated"])

    def test_candidate_tool_arguments_are_strict(self):
        events, _, _, _ = self.run_script([
            call("validate_candidate", {"passed": True}),
        ], "--candidate-checkpoint", success=False)
        self.assertFalse(self.checkpoints(events))


if __name__ == "__main__":
    unittest.main()
