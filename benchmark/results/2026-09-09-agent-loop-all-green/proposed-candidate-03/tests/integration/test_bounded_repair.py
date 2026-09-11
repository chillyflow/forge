"""Deterministic bounded-repair contracts; scripted runs do not measure model quality."""

import json
import pathlib
import re
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


def patch(old, new, reasoning=None):
    return call("apply_patch", {"path": "value.py", "old_text": old, "new_text": new}, reasoning)


def read(reasoning=None):
    return call("read_file", {"path": "value.py", "start": 1, "end": 20}, reasoning)


def validate():
    return call("validate_candidate")


def final():
    return call("final", {"answer": "Repaired value and validated the unchanged tests."})


class BoundedRepairTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forge-bounded-repair-")
        self.root = pathlib.Path(self.temp.name)
        self.source = "def value():\n    return 1\n"
        (self.root / "value.py").write_text(self.source, encoding="utf-8")
        self.tests = ("import unittest\nfrom value import value\n\n"
                      "class ValueTests(unittest.TestCase):\n"
                      "    def test_value(self):\n"
                      "        self.assertEqual(value(), 2)\n")
        (self.root / "test_value.py").write_text(self.tests, encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def command(self, actions, *options, task=None, checkpoint=True, bounded=True, command="run"):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        args = [FORGE, command]
        if command == "run":
            args.append(task or "Repair value() without changing tests, validate, and finish.")
        args += ["--script", str(script), "--workspace", str(self.root), "--no-config", "--json",
                "--minimal-agent", "--prompt-protocol", "native", "--context", "16384",
                "--output-reserve", "2048", "--allow-write", "--allow-exec", "--max-turns", "10",
                "--max-input", "80000", "--max-tokens", "32768", "--wall-ms", "60000"]
        if checkpoint:
            args.append("--candidate-checkpoint")
        if bounded:
            args.append("--bounded-repair")
        return [*args, *options]

    def run_script(self, actions, *options, success=True, stdin=None, **kwargs):
        result = subprocess.run(self.command(actions, *options, **kwargs), capture_output=True,
                                input=stdin, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        self.assertEqual((self.root / "test_value.py").read_text(encoding="utf-8"), self.tests)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        sessions = sorted((self.root / ".forge" / "sessions").iterdir())
        self.assertEqual(len(sessions), 1)
        session = sessions[0]
        metrics = json.loads((session / "metrics.json").read_text(encoding="utf-8"))
        return events, session, metrics, result

    @staticmethod
    def kind(events, name):
        return [event["data"] for event in events if event["type"] == name]

    @staticmethod
    def prompt(session, turn):
        return json.loads((session / "context" / f"{turn:04d}.txt").read_text(encoding="utf-8"))

    @staticmethod
    def tools(prompt):
        return [tool["function"]["name"] for tool in prompt["tools"]]

    @staticmethod
    def host_text(prompt):
        return "\n".join(message.get("content", "") for message in prompt["messages"]
                         if message["role"] == "user")

    def assert_complete_pairs(self, prompt):
        messages = iter(prompt["messages"])
        ids = set()
        for message in messages:
            if message["role"] != "assistant":
                self.assertNotEqual(message["role"], "tool", "Orphaned tool response")
                continue
            calls = message["tool_calls"]
            self.assertEqual(len(calls), 1)
            call_id = calls[0]["id"]
            self.assertNotIn(call_id, ids)
            ids.add(call_id)
            reply = next(messages, None)
            if reply is None:
                self.fail("Orphaned assistant call")
            self.assertEqual(reply["role"], "tool")
            self.assertEqual(reply["tool_call_id"], call_id)

    def assert_verified_repair(self, events):
        self.assertIn("return 2", (self.root / "value.py").read_text(encoding="utf-8"))
        self.assertEqual(len(self.kind(events, "final")), 1)
        self.assertTrue(self.kind(events, "candidate_checkpoint")[-1]["passed"])
        result = subprocess.run([sys.executable, "-B", "-m", "unittest", "discover", "-s", ".",
                                 "-p", "test_*.py"], cwd=self.root, capture_output=True,
                                text=True, encoding="utf-8", timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_long_history_omits_complete_pairs_preserves_raw_evidence_and_repairs(self):
        reasoning = [f"history-observation-{index}: " + "Consider observations before editing. " * 165
                     for index in range(4)]
        events, session, metrics, _ = self.run_script(
            [*(read(text) for text in reasoning), patch("return 1", "return 2"), validate(), final()],
            "--max-turns", "16")
        self.assert_verified_repair(events)
        contexts = self.kind(events, "bounded_context")
        self.assertTrue(any(item["omitted_segments"] > 0 for item in contexts))
        self.assertGreater(metrics["context_evictions"], 0)
        self.assertLessEqual(metrics["prompt_tokens"], 80000)
        self.assertTrue(all(item["rendered_tokens"] <= item["input_budget"] for item in contexts))
        prompts = [self.prompt(session, turn) for turn in range(1, 8)]
        for prompt in prompts:
            self.assert_complete_pairs(prompt)
        self.assertTrue(any("history-observation-0" not in json.dumps(prompt)
                            for prompt in prompts[2:5]))
        raw = json.loads((session / "context" / "final.json").read_text(encoding="utf-8"))
        raw_text = json.dumps(raw)
        for text in reasoning:
            self.assertIn(text, raw_text)
        after_edit = self.host_text(prompts[5])
        source = after_edit.split("CURRENT_SOURCE_OBSERVATION", 1)[1].split("END_SOURCE_OBSERVATION", 1)[0]
        self.assertIn("return 2", source)
        self.assertNotIn("return 1", source)
        self.assertEqual(self.tools(prompts[-1]), ["final"])

    def test_current_failure_and_input_identities_survive_a_changed_candidate(self):
        events, session, _, _ = self.run_script([
            patch("return 1", "return 0"), validate(), read(),
            patch("return 0", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        checkpoints = self.kind(events, "candidate_checkpoint")
        self.assertFalse(checkpoints[0]["passed"])
        self.assertTrue(checkpoints[-1]["passed"])
        unchanged_failure = self.host_text(self.prompt(session, 3))
        self.assertIn("CURRENT_CANDIDATE_VALIDATION: FAILED", unchanged_failure)
        self.assertIn("outcome=FAILED input_relation=matching", unchanged_failure)
        after_edit = self.host_text(self.prompt(session, 5))
        identities = re.search(r"current_inputs=([0-9a-f]+) latest_validation_inputs=([0-9a-f]+)",
                               after_edit)
        if identities is None:
            self.fail("Current and validated input identities are absent from host evidence")
        self.assertNotEqual(identities[1], identities[2])
        self.assertIn("CURRENT_CANDIDATE_VALIDATION: UNVERIFIED", after_edit)
        self.assertIn("outcome=FAILED input_relation=historical", after_edit)
        self.assertIn("did not test the current candidate", after_edit)
        self.assertNotIn("CANDIDATE_CHECKPOINT: NOT PASSED", after_edit)
        self.assertIn("AssertionError", after_edit)
        source = after_edit.split("CURRENT_SOURCE_OBSERVATION", 1)[1].split("END_SOURCE_OBSERVATION", 1)[0]
        self.assertIn("return 2", source)
        self.assertNotIn("return 0", source)
        after_pass = self.host_text(self.prompt(session, 6))
        self.assertIn("CURRENT_CANDIDATE_VALIDATION: PASSED", after_pass)
        self.assertIn("outcome=PASSED input_relation=matching", after_pass)
        self.assertNotIn("CANDIDATE_CHECKPOINT: PASS. Call final now", after_pass)

    def test_host_applied_delta_excludes_assistant_hypotheses_but_raw_history_keeps_them(self):
        hypothesis = "UNSUPPORTED_HYPOTHESIS: the first test must be broken. " * 60
        events, session, _, _ = self.run_script([
            patch("return 1", "return 0", hypothesis), validate(),
            patch("return 0", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        host = self.host_text(self.prompt(session, 2))
        delta = host.split("PREVIOUS_APPLIED_DELTA", 1)[1].split("CURRENT_SOURCE_OBSERVATION", 1)[0]
        self.assertIn("tool=apply_patch call_id=1", delta)
        self.assertIn('"old_text":"return 1"', delta)
        self.assertIn('"new_text":"return 0"', delta)
        self.assertNotIn("assistant_content", delta)
        self.assertNotIn("UNSUPPORTED_HYPOTHESIS", host)
        raw = json.loads((session / "context" / "final.json").read_text(encoding="utf-8"))
        self.assertIn(hypothesis, json.dumps(raw))
        self.assertTrue(any(hypothesis in item.get("assistant_content", "")
                            for item in self.kind(events, "tool_call")))

    def test_incomplete_check_keeps_prior_failure_separate_until_stable_repair(self):
        failing = "def value():\n    return 0\n"
        mutating = ("def value():\n    from pathlib import Path\n"
                    "    Path('marker.txt').write_text('created during validation', encoding='utf-8')\n"
                    "    return 2\n")
        repaired = "def value():\n    return 2\n"
        events, session, _, _ = self.run_script([
            patch("return 1", "return 0"), validate(), patch(failing, mutating), validate(),
            read(), patch(mutating, repaired), validate(), final()])
        self.assert_verified_repair(events)
        checkpoints = self.kind(events, "candidate_checkpoint")
        self.assertTrue(checkpoints[0]["validated"])
        self.assertFalse(checkpoints[0]["passed"])
        self.assertFalse(checkpoints[1]["validated"])
        self.assertFalse(checkpoints[1]["passed"])
        host = self.host_text(self.prompt(session, 5))
        self.assertIn("CURRENT_CANDIDATE_VALIDATION: UNVERIFIED", host)
        self.assertIn("outcome=FAILED input_relation=historical", host)
        self.assertIn("AssertionError: 0 != 2", host)
        self.assertRegex(host, r"latest_validation_inputs=[0-9a-f]+ validation_id=1 ")
        self.assertRegex(host, r"INCOMPLETE_VALIDATION_ATTEMPT input_hash=[0-9a-f]+ validation_id=2 ")
        self.assertNotIn("CURRENT_CANDIDATE_VALIDATION: PASSED", host)
        self.assertNotIn("CANDIDATE_CHECKPOINT: NOT PASSED", host)
        self.assertTrue(checkpoints[2]["validated"])
        self.assertTrue(checkpoints[2]["passed"])

    def test_related_multi_file_edits_finish_after_older_history_is_omitted(self):
        helper = self.root / "helper.py"
        helper.write_text("def read_value():\n    return 0\n", encoding="utf-8")
        integrated = "from helper import read_value\ndef value():\n    return read_value()\n"
        history = [read(f"Inspection {index}. " + "Keep current evidence separate from hypotheses. " * 140)
                   for index in range(4)]
        events, session, metrics, _ = self.run_script([
            patch(self.source, integrated), *history,
            call("apply_patch", {"path": "helper.py", "old_text": "return 0", "new_text": "return 2"}),
            validate(), final()], "--max-turns", "17")
        self.assertEqual((self.root / "value.py").read_text(encoding="utf-8"), integrated)
        self.assertIn("return 2", helper.read_text(encoding="utf-8"))
        self.assertEqual(metrics["files_modified"], 2)
        self.assertTrue(self.kind(events, "candidate_checkpoint")[-1]["passed"])
        self.assertEqual(len(self.kind(events, "final")), 1)
        self.assertTrue(any(item["omitted_segments"] > 0 for item in self.kind(events, "bounded_context")))
        for turn in range(1, 9):
            self.assert_complete_pairs(self.prompt(session, turn))
        raw = json.loads((session / "context" / "final.json").read_text(encoding="utf-8"))
        retained_actions = []
        for segment in raw["segments"]:
            try:
                item = json.loads(segment["text"])
            except json.JSONDecodeError:
                continue
            if isinstance(item, dict) and item.get("tool") == "apply_patch":
                retained_actions.append(item)
        self.assertTrue(any(item.get("args", {}).get("new_text") == integrated
                            for item in retained_actions))

    def test_user_clarification_remains_paired_and_mandatory_when_history_is_omitted(self):
        task = "Repair value and preserve the existing tests."
        answer = "Return exactly two; retain the original function name and tests."
        history = [read(f"Old inspection {index}. " + "Consider observations before editing. " * 165)
                   for index in range(4)]
        events, session, _, _ = self.run_script([
            call("ask_user", {"question": "Which return value is required?"}), *history,
            patch("return 1", "return 2"), validate(), final()],
            "--max-turns", "17", command="chat", stdin=f"{task}\n{answer}\n/quit\n")
        self.assert_verified_repair(events)
        self.assertTrue(any(item["omitted_segments"] > 0
                            for item in self.kind(events, "bounded_context")))
        for turn in range(2, 9):
            prompt = self.prompt(session, turn)
            self.assert_complete_pairs(prompt)
            self.assertIn(task, self.host_text(prompt))
            answers = [json.loads(message["content"])["answer"] for message in prompt["messages"]
                       if message["role"] == "tool" and message["name"] == "ask_user"]
            self.assertEqual(answers, [answer])

    def test_impossible_mandatory_task_fails_before_generation_without_task_loss(self):
        task = "Mandatory task detail must not disappear. " * 500
        events, session, metrics, result = self.run_script(
            [final()], "--context", "4096", "--output-reserve", "512", task=task, success=False)
        self.assertFalse(self.kind(events, "inference"))
        self.assertFalse(self.kind(events, "final"))
        self.assertEqual(metrics["generated_tokens"], 0)
        self.assertRegex(result.stderr.lower(), r"(mandatory|pinned).*(budget|context)")
        raw = json.loads((session / "context" / "final.json").read_text(encoding="utf-8"))
        self.assertTrue(any(segment["text"] == task for segment in raw["segments"]))

    def test_generated_capacity_reserves_validation_and_complete_final(self):
        events, session, metrics, _ = self.run_script([
            patch("return 1", "return 2", "The source literal needs correction. " * 10),
            validate(), final()], "--context", "4096", "--output-reserve", "512",
            "--max-tokens", "1100", "--max-input", "12000")
        self.assert_verified_repair(events)
        self.assertEqual(self.tools(self.prompt(session, 2)), ["validate_candidate"])
        self.assertEqual(self.tools(self.prompt(session, 3)), ["final"])
        self.assertLessEqual(metrics["generated_tokens"], 1100)
        self.assertLessEqual(metrics["prompt_tokens"], 12000)

    def test_total_input_capacity_reserves_validation_and_complete_final(self):
        events, session, metrics, _ = self.run_script([
            read("Inspect the exact current source before editing. " * 12),
            patch("return 1", "return 2"), validate(), final()],
            "--context", "4096", "--output-reserve", "512", "--max-input", "9000")
        self.assert_verified_repair(events)
        self.assertEqual(self.tools(self.prompt(session, 3)), ["validate_candidate"])
        self.assertEqual(self.tools(self.prompt(session, 4)), ["final"])
        self.assertLessEqual(metrics["prompt_tokens"], 9000)
        self.assertTrue(all(item["rendered_tokens"] <= item["input_budget"]
                            for item in self.kind(events, "bounded_context")))

    def test_failed_candidate_at_budget_closure_stops_after_one_rejected_final(self):
        events, _, metrics, result = self.run_script([
            patch("return 1", "return 0", "The source literal needs correction. " * 10),
            validate(), *[final() for _ in range(8)]],
            "--context", "4096", "--output-reserve", "512", "--max-tokens", "1100",
            "--max-input", "12000", success=False)
        self.assertFalse(self.kind(events, "final"))
        self.assertEqual(metrics["turns"], 3)
        self.assertEqual(len(self.kind(events, "inference")), 3)
        checkpoints = self.kind(events, "candidate_checkpoint")
        self.assertEqual(len(checkpoints), 2)
        self.assertFalse(any(item["passed"] for item in checkpoints))
        self.assertRegex(result.stderr.lower(), r"(completion|budget|limit)")

    def reflection_recovery(self, response):
        events, session, metrics, _ = self.run_script([
            patch("return 1", "return 0"), validate(), response,
            patch("return 0", "return 2"), validate(), final()],
            "--failure-reflection", "--reflection-tokens", "256")
        self.assert_verified_repair(events)
        self.assertEqual(len(self.kind(events, "failure_reflection_failed")), 1)
        self.assertFalse(self.kind(events, "failure_reflection"))
        self.assertEqual(metrics["turns"], 6)
        self.assertEqual(len(self.kind(events, "inference")), 6)
        self.assertEqual(self.tools(self.prompt(session, 3)), ["reflect_failure"])
        self.assertIn("apply_patch", self.tools(self.prompt(session, 4)))
        self.assertNotIn("reflect_failure", self.tools(self.prompt(session, 4)))
        self.assertEqual(metrics["files_modified"], 1)
        self.assertEqual(len(list((session / "tool").glob("*.patch"))), 2)
        for turn in range(1, 7):
            self.assert_complete_pairs(self.prompt(session, turn))

    def test_incomplete_native_reflection_returns_to_ordinary_repair(self):
        self.reflection_recovery('{"role":"assistant","tool_calls":[')

    def test_reflection_output_exhaustion_returns_to_ordinary_repair(self):
        self.reflection_recovery(call("reflect_failure", {"diagnosis": "Unfinished reasoning. " * 100}))

    def test_reflection_cannot_execute_an_edit_then_recover_again(self):
        self.reflection_recovery(patch("return 0", "return 99"))

    def test_validation_mutation_cannot_leave_a_stale_passing_verdict(self):
        self.tests += ("        from pathlib import Path\n"
                       "        Path('value.py').write_text('def value(): return 3\\n')\n")
        (self.root / "test_value.py").write_text(self.tests, encoding="utf-8")
        events, _, _, _ = self.run_script([
            patch("return 1", "return 2"), validate(), final()], "--max-turns", "3", success=False)
        self.assertFalse(self.kind(events, "final"))
        self.assertTrue(self.kind(events, "candidate_checkpoint"))
        self.assertFalse(any(item["passed"] for item in self.kind(events, "candidate_checkpoint")))

    def test_bounded_repair_requires_checkpoint(self):
        result = subprocess.run(self.command([final()], checkpoint=False), capture_output=True,
                                text=True, encoding="utf-8", timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
