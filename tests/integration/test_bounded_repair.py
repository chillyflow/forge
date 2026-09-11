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
        after_edit = self.host_text(self.prompt(session, 5))
        identities = re.search(r"current_inputs=([0-9a-f]+) latest_validation_inputs=([0-9a-f]+)",
                               after_edit)
        if identities is None:
            self.fail("Current and validated input identities are absent from host evidence")
        self.assertNotEqual(identities[1], identities[2])
        self.assertIn("AssertionError", after_edit)
        source = after_edit.split("CURRENT_SOURCE_OBSERVATION", 1)[1].split("END_SOURCE_OBSERVATION", 1)[0]
        self.assertIn("return 2", source)
        self.assertNotIn("return 0", source)

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


ELIDE = "--elide-noop-edits"
OBSERVATION = "HOST_RECORD: an apply_patch call for value.py made no change"
STABLE = "--stable-prefix"
DEDUP = "--dedup-commands"
REUSED = "HOST_RECORD: this exact command already ran at action"


def probe(*argv):
    return call("run_command", {"argv": list(argv)})


class CommandDedupTests(BoundedRepairTests):
    """--dedup-commands serves a stored verdict for an identical run_command
    issued while no edits, commands, or validations ran since, instead of
    re-executing. Policy, deadlines, artifacts, and downstream metadata are
    unchanged; the complete action stays in session artifacts either way.
    Without the flag every command re-executes. Scripted runs assert the
    contract, not model quality."""

    def command_outputs(self, events):
        return [item["output"] for item in self.kind(events, "tool_result")
                if item["name"] == "run_command"]

    def test_identical_command_reuses_verdict(self):
        hello = [sys.executable, "-c", "print('hello')"]
        events, session, _, _ = self.run_script(
            [probe(*hello), read(), probe(*hello),
             patch("return 1", "return 2"), validate(), final()], DEDUP)
        self.assert_verified_repair(events)
        outputs = self.command_outputs(events)
        self.assertEqual(len(outputs), 2, outputs)
        self.assertIn("hello", outputs[0])
        self.assertNotIn("already ran at action", outputs[0])
        self.assertIn("hello", outputs[1])
        self.assertIn(REUSED, outputs[1])
        reused = self.kind(events, "command_verdict_reused")
        self.assertEqual(len(reused), 1, reused)
        calls = [item for item in self.kind(events, "tool_call")
                 if item["tool"] == "run_command"]
        self.assertEqual(len(calls), 2, calls)
        self.assertEqual(calls[0]["args"]["argv"], calls[1]["args"]["argv"])
        prompt = json.loads(sorted((session / "context").glob("*.txt"))[-1].read_text(
            encoding="utf-8"))
        self.assert_complete_pairs(prompt)

    def test_control_without_the_flag_reexecutes(self):
        hello = [sys.executable, "-c", "print('hello')"]
        events, _, _, _ = self.run_script(
            [probe(*hello), read(), probe(*hello),
             patch("return 1", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        outputs = self.command_outputs(events)
        self.assertEqual(len(outputs), 2, outputs)
        self.assertNotIn("already ran at action", outputs[1])
        self.assertEqual(self.kind(events, "command_verdict_reused"), [])

    def test_different_command_clears_the_slot(self):
        events, _, _, _ = self.run_script(
            [probe(sys.executable, "-c", "print('one')"),
             probe(sys.executable, "-c", "print('two')"),
             probe(sys.executable, "-c", "print('one')"),
             patch("return 1", "return 2"), validate(), final()], DEDUP)
        self.assert_verified_repair(events)
        outputs = self.command_outputs(events)
        self.assertEqual(len(outputs), 3, outputs)
        self.assertNotIn("already ran at action", outputs[2])
        self.assertEqual(self.kind(events, "command_verdict_reused"), [])

    def test_edit_clears_the_slot(self):
        events, _, _, _ = self.run_script(
            [probe(sys.executable, "-c", "print('hello')"),
             patch("return 1", "return 3"),
             probe(sys.executable, "-c", "print('hello')"),
             patch("return 3", "return 2"), validate(), final()],
            DEDUP, "--max-turns", "12")
        self.assert_verified_repair(events)
        outputs = self.command_outputs(events)
        self.assertEqual(len(outputs), 2, outputs)
        self.assertNotIn("already ran at action", outputs[1])
        self.assertEqual(self.kind(events, "command_verdict_reused"), [])

    def test_failing_verdict_reused_with_exit_code(self):
        events, _, _, _ = self.run_script(
            [probe(sys.executable, "-c", "import sys; print('boom'); sys.exit(3)"),
             read(),
             probe(sys.executable, "-c", "import sys; print('boom'); sys.exit(3)"),
             patch("return 1", "return 2"), validate(), final()], DEDUP)
        self.assert_verified_repair(events)
        outputs = self.command_outputs(events)
        self.assertEqual(len(outputs), 2, outputs)
        self.assertIn("exit_code=3", outputs[0])
        self.assertIn("boom", outputs[1])
        self.assertIn("exit_code=3", outputs[1])
        self.assertIn(REUSED, outputs[1])
        self.assertEqual(len(self.kind(events, "command_verdict_reused")), 1)

    def test_dedup_commands_requires_checkpoint(self):
        args = self.command([final()], DEDUP, checkpoint=False, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


class StablePrefixTests(BoundedRepairTests):
    """--stable-prefix keeps the bounded-repair admission window monotonic:
    a dropped exchange is never re-admitted when the per-turn budget loosens.
    Without the flag, and on every other rejection path, behavior is
    unchanged. These scripted runs assert the contract, not model quality."""

    def test_repair_passes_with_the_flag(self):
        events, session, _, _ = self.run_script(
            [read(), patch("return 1", "return 2"), validate(), final()], STABLE)
        self.assert_verified_repair(events)
        prompt = json.loads(sorted((session / "context").glob("*.txt"))[-1].read_text(
            encoding="utf-8"))
        self.assert_complete_pairs(prompt)

    def test_stable_prefix_requires_bounded_repair(self):
        args = self.command([final()], STABLE, checkpoint=True, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bounded", result.stderr.lower())

    def test_stable_prefix_requires_checkpoint(self):
        args = self.command([final()], STABLE, checkpoint=False, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


GUIDE = "--budget-guidance"
GATE = "--gate-noop-edits"


class NoopGateTests(BoundedRepairTests):
    """--gate-noop-edits disables apply_patch for the one ordinary turn after
    an apply_patch rejected specifically because its replacement equalled the
    replaced text. The host dispatcher enforces the exclusion; the registry
    is restored on the following turn; anchor-mismatch rejections and the
    unflagged control are unchanged. Scripted runs assert the contract, not
    model quality."""

    def test_gated_turn_has_no_patch_tool_and_control_line(self):
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 1"), read(), patch("return 1", "return 2"),
             validate(), final()], GATE)
        self.assert_verified_repair(events)
        gated = self.prompt(session, 2)
        self.assertNotIn("apply_patch", self.tools(gated))
        self.assertIn("NOOP_EDIT_GATE", self.host_text(gated))
        restored = self.prompt(session, 3)
        self.assertIn("apply_patch", self.tools(restored))
        self.assertNotIn("NOOP_EDIT_GATE", self.host_text(restored))
        self.assert_complete_pairs(gated)

    def test_host_rejects_patch_issued_on_gated_turn(self):
        events, _, _, _ = self.run_script(
            [patch("return 1", "return 1"), patch("return 1", "return 2"), read(),
             patch("return 1", "return 2"), validate(), final()], GATE)
        self.assert_verified_repair(events)
        rejected = [item for item in self.kind(events, "tool_result")
                    if item["name"] == "apply_patch" and item.get("status") != "ok"]
        self.assertTrue(any("disabled" in item.get("output", "") for item in rejected),
                        rejected)
        calls = [item for item in self.kind(events, "tool_call")
                 if item["tool"] == "apply_patch"]
        self.assertEqual(len(calls), 3, calls)

    def test_control_without_flag_gates_nothing(self):
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 1"), read(), patch("return 1", "return 2"),
             validate(), final()])
        self.assert_verified_repair(events)
        after = self.prompt(session, 2)
        self.assertIn("apply_patch", self.tools(after))
        self.assertNotIn("NOOP_EDIT_GATE", self.host_text(after))

    def test_anchor_mismatch_does_not_gate(self):
        events, session, _, _ = self.run_script(
            [patch("return 99", "return 2"), read(), patch("return 1", "return 2"),
             validate(), final()], GATE)
        self.assert_verified_repair(events)
        after = self.prompt(session, 2)
        self.assertIn("apply_patch", self.tools(after))
        self.assertNotIn("NOOP_EDIT_GATE", self.host_text(after))

    def test_gate_noop_edits_requires_bounded_repair(self):
        args = self.command([final()], GATE, checkpoint=True, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bounded", result.stderr.lower())

    def test_gate_noop_edits_requires_checkpoint(self):
        args = self.command([final()], GATE, checkpoint=False, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


class BudgetGuidanceTests(BoundedRepairTests):
    """--budget-guidance adds per-turn remaining token counts to the
    candidate control state plus concise tool-shaped work discipline to the
    instructions. Without the flag both texts are byte-identical to the
    existing control. Scripted runs assert the contract, not model quality."""

    def bodies(self, session):
        return [path.read_text(encoding="utf-8")
                for path in sorted((session / "context").glob("*.txt"))]

    def test_repair_passes_with_the_flag(self):
        events, session, _, _ = self.run_script(
            [read(), patch("return 1", "return 2"), validate(), final()], GUIDE)
        self.assert_verified_repair(events)
        prompt = json.loads(sorted((session / "context").glob("*.txt"))[-1].read_text(
            encoding="utf-8"))
        self.assert_complete_pairs(prompt)

    def test_control_carries_remaining_budgets_and_discipline(self):
        _, session, _, _ = self.run_script(
            [read(), patch("return 1", "return 2"), validate(), final()], GUIDE)
        bodies = self.bodies(session)
        self.assertTrue(any("input_left=" in body and "generated_left=" in body
                            for body in bodies), bodies[-1][-2000:])
        self.assertTrue(any("short decisive turns" in body for body in bodies))

    def test_control_without_the_flag_has_no_budgets(self):
        _, session, _, _ = self.run_script(
            [read(), patch("return 1", "return 2"), validate(), final()])
        bodies = self.bodies(session)
        self.assertFalse(any("input_left=" in body for body in bodies))
        self.assertFalse(any("short decisive turns" in body for body in bodies))

    def test_budget_guidance_requires_bounded_repair(self):
        args = self.command([final()], GUIDE, checkpoint=True, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bounded", result.stderr.lower())

    def test_budget_guidance_requires_checkpoint(self):
        args = self.command([final()], GUIDE, checkpoint=False, bounded=False)
        result = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                                timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


class NoopElisionTests(BoundedRepairTests):
    """--elide-noop-edits suppresses verbatim retention of an edit rejected
    specifically because its replacement equalled the text it replaced. Every
    other rejection, and every arm without the flag, stays unchanged."""

    def last_prompt(self, session):
        contexts = sorted((session / "context").glob("*.txt"))
        self.assertTrue(contexts, "no rendered prompt was retained")
        return json.loads(contexts[-1].read_text(encoding="utf-8"))

    @staticmethod
    def patch_arguments(prompt):
        """Every apply_patch call as it survives in the rendered conversation.

        The native renderer emits `arguments` as a JSON object, not a string."""
        return [message["tool_calls"][0]["function"]["arguments"]
                for message in prompt["messages"]
                if message["role"] == "assistant" and message.get("tool_calls")
                and message["tool_calls"][0]["function"]["name"] == "apply_patch"]

    def every_host_text(self, session):
        """Host text of every rendered prompt, not only the last one."""
        return [self.host_text(json.loads(path.read_text(encoding="utf-8")))
                for path in sorted((session / "context").glob("*.txt"))]

    @staticmethod
    def tool_replies(prompt):
        return [message.get("content", "") for message in prompt["messages"]
                if message["role"] == "tool"]

    def test_rejected_identical_edit_is_not_retained_verbatim(self):
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 1"), patch("return 1", "return 2"), validate(), final()],
            ELIDE)
        self.assert_verified_repair(events)
        prompt = self.last_prompt(session)
        arguments = self.patch_arguments(prompt)
        self.assertEqual(len(arguments), 2, arguments)
        self.assertEqual(arguments[0], {"path": "value.py"})
        self.assertEqual(arguments[1]["old_text"], "return 1")
        self.assertEqual(arguments[1]["new_text"], "return 2")
        self.assertTrue(any(OBSERVATION in reply for reply in self.tool_replies(prompt)),
                        self.tool_replies(prompt))
        self.assertEqual(len(self.kind(events, "noop_edit_elided")), 1)
        self.assert_complete_pairs(prompt)

    def test_control_without_the_flag_retains_the_identical_edit_verbatim(self):
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 1"), patch("return 1", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        prompt = self.last_prompt(session)
        arguments = self.patch_arguments(prompt)
        self.assertEqual(len(arguments), 2, arguments)
        self.assertEqual(arguments[0]["old_text"], "return 1")
        self.assertEqual(arguments[0]["new_text"], "return 1")
        self.assertFalse(any(OBSERVATION in reply for reply in self.tool_replies(prompt)))
        self.assertEqual(self.kind(events, "noop_edit_elided"), [])

    def test_anchor_mismatch_rejection_is_still_retained_verbatim(self):
        events, session, _, _ = self.run_script(
            [patch("return 99", "return 2"), patch("return 1", "return 2"), validate(), final()],
            ELIDE)
        self.assert_verified_repair(events)
        prompt = self.last_prompt(session)
        arguments = self.patch_arguments(prompt)
        self.assertEqual(len(arguments), 2, arguments)
        self.assertEqual(arguments[0]["old_text"], "return 99")
        self.assertEqual(arguments[0]["new_text"], "return 2")
        self.assertEqual(self.kind(events, "noop_edit_elided"), [])
        self.assertTrue(any("must match exactly once" in reply
                            for reply in self.tool_replies(prompt)), self.tool_replies(prompt))

    def test_raw_action_survives_in_session_artifacts(self):
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 1"), patch("return 1", "return 2"), validate(), final()],
            ELIDE)
        self.assert_verified_repair(events)
        calls = [item for item in self.kind(events, "tool_call") if item["tool"] == "apply_patch"]
        self.assertEqual(calls[0]["args"]["old_text"], "return 1")
        self.assertEqual(calls[0]["args"]["new_text"], "return 1")
        raw = sorted((session / "tool").glob("*.raw"))
        self.assertTrue(raw, "no raw tool artifact was retained")
        self.assertIn("old_text and new_text are identical", raw[0].read_text(encoding="utf-8"))
        self.assertEqual(len(self.kind(events, "noop_edit_elided")), 1)

    def test_previous_applied_delta_still_reflects_the_last_real_edit(self):
        """A rejected no-op must not disturb bounded repair's applied-delta evidence.

        The first edit applies but does not repair, so the failed validation
        keeps the episode open and the delta is rendered on later turns."""
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 3"), validate(), patch("return 3", "return 3"),
             patch("return 3", "return 2"), validate(), final()], ELIDE, "--max-turns", "12")
        self.assert_verified_repair(events)
        self.assertEqual(len(self.kind(events, "noop_edit_elided")), 1)

        deltas = [text for text in self.every_host_text(session)
                  if "PREVIOUS_APPLIED_DELTA" in text]
        self.assertTrue(deltas, "bounded repair never rendered an applied delta")
        # Every rendered delta describes an edit that actually applied. The
        # rejected no-op never becomes the "previous attempted delta".
        for text in deltas:
            block = text.split("PREVIOUS_APPLIED_DELTA", 1)[1]
            self.assertNotIn('"new_text":"return 3","old_text":"return 3"', block)
            self.assertNotIn('"old_text":"return 3","new_text":"return 3"', block)

    def test_elide_noop_edits_requires_checkpoint(self):
        result = subprocess.run(self.command([final()], ELIDE, checkpoint=False, bounded=False),
                                capture_output=True, text=True, encoding="utf-8", timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate", result.stderr.lower())


class HostRecordDefectTests(BoundedRepairTests):
    """W2 defect repairs. These claim nothing for model repair quality."""

    def last_prompt(self, session):
        contexts = sorted((session / "context").glob("*.txt"))
        self.assertTrue(contexts, "no rendered prompt was retained")
        return json.loads(contexts[-1].read_text(encoding="utf-8"))

    def test_applied_delta_excludes_model_prose_and_keeps_the_arguments(self):
        """Verbose prose used to consume the compressor's head, clipping old_text."""
        prose = "I will now describe my reasoning at length. " * 120
        events, session, _, _ = self.run_script(
            [patch("return 1", "return 3", prose), validate(),
             patch("return 3", "return 2"), validate(), final()], "--max-turns", "12")
        self.assert_verified_repair(events)
        deltas = [self.host_text(json.loads(path.read_text(encoding="utf-8")))
                  for path in sorted((session / "context").glob("*.txt"))]
        deltas = [t.split("PREVIOUS_APPLIED_DELTA", 1)[1] for t in deltas
                  if "PREVIOUS_APPLIED_DELTA" in t]
        self.assertTrue(deltas, "bounded repair never rendered an applied delta")
        block = deltas[0]
        # The arguments survive; the model's own prose is not competing for the
        # compressor's budget.
        self.assertIn("old_text", block)
        self.assertIn("new_text", block)
        self.assertNotIn("describe my reasoning at length", block)

    def test_silent_successful_command_is_annotated(self):
        events, _, _, _ = self.run_script(
            [call("run_command", {"argv": [sys.executable, "-c", ""]}),
             patch("return 1", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        outputs = [item["output"] for item in self.kind(events, "tool_result")
                   if item["name"] == "run_command"]
        self.assertTrue(outputs)
        self.assertIn("exited 0 and wrote nothing to stdout or stderr", outputs[0])
        # The annotation is an observation only. It fires in every mode,
        # including the minimal control, so it must not carry an inference:
        # that control is defined by never receiving a corrective instruction.
        self.assertNotIn("not evidence", outputs[0])
        self.assertNotIn("next_action_guidance", outputs[0])

    def test_command_with_output_is_not_annotated(self):
        events, _, _, _ = self.run_script(
            [call("run_command", {"argv": [sys.executable, "-c", "print('hello')"]}),
             patch("return 1", "return 2"), validate(), final()])
        self.assert_verified_repair(events)
        outputs = [item["output"] for item in self.kind(events, "tool_result")
                   if item["name"] == "run_command"]
        self.assertIn("hello", outputs[0])
        self.assertNotIn("exited 0 and wrote nothing", outputs[0])

    def test_agent_mode_reports_only_enabled_capabilities(self):
        events, _, _, _ = self.run_script(
            [patch("return 1", "return 2"), validate(), final()])
        mode = self.kind(events, "agent_mode")[0]
        self.assertEqual(mode["name"], "bounded-repair")
        # --failure-reflection and --semantic-loops are not in this profile, so
        # neither capability may be advertised.
        self.assertFalse(mode["recovery"])
        self.assertFalse(mode["corrective_prompts"])

    def test_agent_mode_reports_capabilities_that_are_enabled(self):
        events, _, _, _ = self.run_script(
            [patch("return 1", "return 2"), validate(), final()],
            "--failure-reflection", "--semantic-loops")
        mode = self.kind(events, "agent_mode")[0]
        self.assertTrue(mode["recovery"])
        self.assertTrue(mode["corrective_prompts"])


if __name__ == "__main__":
    unittest.main()
