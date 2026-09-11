"""Deterministic contracts for loop experiments, never evidence of model superiority."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())


def call(name, arguments=None):
    return {"role": "assistant", "content": "", "tool_calls": [
        {"type": "function", "function": {"name": name,
         "arguments": json.dumps(arguments or {}, separators=(",", ":"))}}]}


def patch(old, new):
    return call("apply_patch", {"path": "value.py", "old_text": old, "new_text": new})


def validate():
    return call("validate_candidate")


def final(answer):
    return call("final", {"answer": answer})


class LoopExtensionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forge-loop-extensions-")
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

    def command(self, actions, *options, command="run", checkpoint=True):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        args = [FORGE, command]
        if command == "run":
            args.append("Repair value() without changing tests, then validate and finish.")
        args += ["--script", str(script), "--workspace", str(self.root), "--no-config", "--json",
                 "--minimal-agent", "--context", "65536", "--output-reserve", "4096",
                 "--allow-write", "--allow-exec", "--max-turns", "10", "--wall-ms", "60000"]
        if checkpoint:
            args.append("--candidate-checkpoint")
        return [*args, *options]

    def run_script(self, actions, *options, success=True, stdin=None, command="run", checkpoint=True):
        result = subprocess.run(self.command(actions, *options, command=command, checkpoint=checkpoint),
                                input=stdin, capture_output=True, text=True, encoding="utf-8", timeout=90)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        self.assertEqual((self.root / "test_value.py").read_text(encoding="utf-8"), self.tests)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        return result, events

    def root_sessions(self):
        path = self.root / ".forge" / "sessions"
        return sorted(path.iterdir()) if path.exists() else []

    @staticmethod
    def prompt(session, turn):
        return json.loads((session / "context" / f"{turn:04d}.txt").read_text(encoding="utf-8"))

    @staticmethod
    def kind(events, name):
        return [event["data"] for event in events if event["type"] == name]

    def test_second_candidate_starts_from_baseline_and_only_winner_survives(self):
        _, events = self.run_script([
            patch("return 1", "return 0"), validate(), final("Discarded failed candidate."),
            patch("return 1", "return 2"), validate(), final("Selected repaired candidate.")],
            "--candidates", "2", "--max-turns", "6")
        self.assertIn("return 2", (self.root / "value.py").read_text())
        self.assertEqual(self.kind(events, "candidate_selected")[0]["candidate"], 2)
        starts = self.kind(events, "candidate_start")
        self.assertEqual([item["candidate"] for item in starts], [1, 2])
        self.assertTrue(all(item["independent_baseline"] for item in starts))
        selected = self.kind(events, "candidate_selection")
        self.assertTrue(selected)
        self.assertTrue(all(item["real_workspace"] for item in selected))
        self.assertEqual(self.kind(events, "final"), ["Selected repaired candidate."])
        metrics = json.loads((self.root_sessions()[0] / "metrics.json").read_text())
        self.assertEqual(metrics["turns"], 6)
        self.assertTrue(metrics["simulated"])

    def test_all_failed_candidates_leave_original_workspace(self):
        _, events = self.run_script([
            patch("return 1", "return 0"), validate(), final("failed one"),
            patch("return 1", "return 3"), validate(), final("failed two")],
            "--candidates", "2", "--max-turns", "6", success=False)
        self.assertEqual((self.root / "value.py").read_text(), self.source)
        self.assertFalse(self.kind(events, "candidate_selected"))
        self.assertFalse(self.kind(events, "final"))
        self.assertEqual(len(self.kind(events, "candidate_generated")), 2)

    def test_ignored_trial_directory_has_independent_source_index(self):
        # A child below ignored .forge must not inherit the outer Git listing,
        # which succeeds with zero files even though the copied sources exist.
        subprocess.run(["git", "init", "-q", str(self.root)], check=True, capture_output=True)
        (self.root / ".gitignore").write_text(".forge/\n", encoding="utf-8")
        git_config = (self.root / ".git" / "config").read_bytes()
        for options in ((), ("--symbol-impact",)):
            with self.subTest(options=options):
                (self.root / "value.py").write_text(self.source, encoding="utf-8")
                _, events = self.run_script([
                    patch("return 1", "return 0"), validate(), final("failed trial"),
                    patch("return 1", "return 2"), validate(), final("passing trial")],
                    "--candidates", "2", "--max-turns", "6", *options)
                self.assertEqual(self.kind(events, "candidate_selected")[0]["candidate"], 2)
                self.assertEqual(self.kind(events, "final"), ["passing trial"])
                generated = self.kind(events, "candidate_generated")
                self.assertEqual(len(generated), 2)
                for item in generated:
                    child_session = pathlib.Path(item["session"])
                    trial = child_session.parents[2]
                    self.assertFalse((trial / ".git").exists())
                    plans = list((child_session / "validation").glob("*.plan.json"))
                    self.assertTrue(plans)
                    plan = json.loads(plans[0].read_text(encoding="utf-8"))
                    self.assertTrue(plan["applicable"])
                    self.assertEqual(plan["python"]["source_file_count"], 2)
                    if options:
                        impact = plan["structural_impact"]
                        self.assertTrue(any(symbol["name"] == "value"
                                            for symbol in impact["changed_symbols"]))
                        self.assertTrue(impact["fallback"])
                self.assertEqual((self.root / ".git" / "config").read_bytes(), git_config)

    def test_selection_prefers_smaller_passing_change(self):
        _, events = self.run_script([
            patch("return 1", "return 2  # unnecessary explanation"), validate(), final("larger"),
            patch("return 1", "return 2"), validate(), final("smaller")],
            "--candidates", "2", "--max-turns", "6")
        self.assertEqual(self.kind(events, "candidate_selected")[0]["candidate"], 2)
        self.assertEqual((self.root / "value.py").read_text(), self.source.replace("return 1", "return 2"))
        selections = self.kind(events, "candidate_selection")
        self.assertEqual([item["passed"] for item in selections], [True, True])
        self.assertGreater(selections[0]["changed_content_cost"], selections[1]["changed_content_cost"])

    def test_isolated_pass_cannot_bypass_actual_workspace_validation(self):
        self.tests += ("        from pathlib import Path\n"
                       "        self.assertIn('.forge', Path.cwd().parts, 'real-workspace gate')\n")
        (self.root / "test_value.py").write_text(self.tests, encoding="utf-8")
        _, events = self.run_script([
            patch("return 1", "return 2"), validate(), final("first isolated pass"),
            patch("return 1", "return 2"), validate(), final("second isolated pass")],
            "--candidates", "2", "--max-turns", "6", success=False)
        selections = self.kind(events, "candidate_selection")
        self.assertEqual(len(selections), 2)
        self.assertTrue(all(item["real_workspace"] and not item["passed"] for item in selections))
        self.assertFalse(self.kind(events, "candidate_selected"))
        self.assertEqual((self.root / "value.py").read_text(), self.source)

    def test_candidate_budget_is_shared_not_multiplied(self):
        result, events = self.run_script([final("never sampled")],
                                         "--candidates", "2", "--max-turns", "5", success=False)
        self.assertTrue(any(word in result.stderr.lower() for word in ["budget", "turn", "actions"]), result.stderr)
        self.assertFalse(self.kind(events, "candidate_selected"))
        self.assertEqual((self.root / "value.py").read_text(), self.source)

    def test_interactive_questions_cannot_be_discarded_with_a_candidate_branch(self):
        result, events = self.run_script([
            call("ask_user", {"question": "Which requirement should every candidate follow?"}),
            patch("return 1", "return 2"), validate(), final("Would hide the user's answer")],
            "--candidates", "2", "--max-turns", "6", command="chat",
            stdin="Repair value.\n/quit\n", success=False)
        self.assertIn("Interactive questions require one candidate", result.stderr)
        self.assertIn("--candidates 1", result.stderr)
        self.assertNotIn("Answer>", result.stderr)
        self.assertFalse(self.kind(events, "candidate_start"))
        self.assertFalse(self.kind(events, "inference"))
        self.assertEqual((self.root / "value.py").read_text(), self.source)

    def test_semantic_loop_recognizes_comment_only_revision_of_same_failed_candidate(self):
        _, events = self.run_script([
            patch("return 1", "return 0  # first explanation"), validate(),
            patch("first explanation", "rephrased explanation"), validate(),
            patch("return 0", "return 2"), validate(), final("Repaired after repeated failure.")],
            "--semantic-loops", "--max-turns", "8")
        semantic = self.kind(events, "semantic_loop")
        self.assertEqual(len(semantic), 2)
        self.assertTrue(all(item["complete"] for item in semantic))
        self.assertFalse(semantic[0]["repeated_failed_state"])
        self.assertTrue(semantic[1]["repeated_failed_state"])
        self.assertEqual(semantic[0]["canonical_hash"], semantic[1]["canonical_hash"])
        checkpoints = self.kind(events, "candidate_checkpoint")
        self.assertFalse(checkpoints[0]["passed"])
        self.assertFalse(checkpoints[1]["passed"])
        self.assertTrue(checkpoints[-1]["passed"])
        self.assertGreater(json.loads((self.root_sessions()[0] / "metrics.json").read_text())["loop_warnings"], 0)

    def test_semantic_detector_preserves_behavioral_literal_changes(self):
        _, events = self.run_script([
            patch("return 1", "return 0"), validate(),
            patch("return 0", "return 3"), validate(),
            patch("return 3", "return 2"), validate(), final("Correct literal selected.")],
            "--semantic-loops", "--max-turns", "8")
        semantic = self.kind(events, "semantic_loop")
        self.assertEqual(len(semantic), 2)
        self.assertNotEqual(semantic[0]["canonical_hash"], semantic[1]["canonical_hash"])
        self.assertFalse(any(item["repeated_failed_state"] for item in semantic))

    def test_failed_validation_grants_one_bounded_diagnostic_then_requires_repair(self):
        diagnosis = "The implementation returns zero but the existing test requires two; replace the literal."
        _, events = self.run_script([
            patch("return 1", "return 0"), validate(),
            call("reflect_failure", {"diagnosis": diagnosis}),
            call("read_file", {"path": "value.py", "start": 1, "end": 20}),
            validate(), patch("return 0", "return 2"), validate(), final("Validated repair.")],
            "--failure-reflection", "--reflection-tokens", "256")
        self.assertEqual(len(self.kind(events, "failure_reflection")), 1)
        session = self.root_sessions()[0]
        tools = self.prompt(session, 3)["tools"]
        self.assertEqual([tool["function"]["name"] for tool in tools], ["reflect_failure"])
        after = self.prompt(session, 4)
        self.assertIn(diagnosis, json.dumps(after))
        self.assertIn("episode_active=true", json.dumps(after))
        self.assertNotIn("reflect_failure", [tool["function"]["name"] for tool in after["tools"]])
        inference = self.kind(events, "inference")
        self.assertLessEqual(inference[2]["generated_tokens"], 256)
        self.assertEqual(len(inference), 8)

    def test_reflection_cannot_be_requested_before_a_host_failure(self):
        result, events = self.run_script([
            call("reflect_failure", {"diagnosis": "Unconditioned reasoning."}),
            patch("return 1", "return 2")], "--failure-reflection", success=False)
        self.assertIn("failure", result.stderr.lower())
        self.assertFalse(self.kind(events, "failure_reflection"))
        self.assertEqual((self.root / "value.py").read_text(), self.source)
        self.assertEqual(len(self.kind(events, "inference")), 1)

    def test_reflection_checkpoint_cannot_execute_a_patch_instead(self):
        result, events = self.run_script([
            patch("return 1", "return 0"), validate(), patch("return 0", "return 2")],
            "--failure-reflection", success=False)
        self.assertIn("Reserved checkpoint action", result.stderr)
        self.assertIn("return 0", (self.root / "value.py").read_text())
        self.assertFalse(self.kind(events, "failure_reflection"))

    def test_reflection_does_not_consume_reserved_validation_or_final_slots(self):
        _, events = self.run_script([
            patch("return 1", "return 0"), validate(),
            call("reflect_failure", {"diagnosis": "Return two to match the required result."}),
            patch("return 0", "return 2"), validate(), final("Done.")],
            "--failure-reflection", "--max-turns", "6")
        session = self.root_sessions()[0]
        self.assertEqual([tool["function"]["name"] for tool in self.prompt(session, 5)["tools"]],
                         ["validate_candidate"])
        self.assertEqual([tool["function"]["name"] for tool in self.prompt(session, 6)["tools"]], ["final"])
        self.assertEqual(len(self.kind(events, "failure_reflection")), 1)

    def test_loop_extensions_require_the_candidate_checkpoint(self):
        for flag in ["--semantic-loops", "--failure-reflection", "--stop-loss", "--candidates"]:
            options = [flag, "2"] if flag == "--candidates" else [flag]
            result = subprocess.run(self.command([final("unused")], *options, checkpoint=False),
                                    text=True, encoding="utf-8", capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("candidate", result.stderr.lower())

    def test_stop_loss_aborts_a_stuck_trial_and_spends_the_rest_on_trial_two(self):
        _, events = self.run_script([
            patch("return 1", "return 1"), patch("return 1", "return 1"),
            patch("return 1", "return 1"),
            patch("return 1", "return 2"), validate(), final("Selected repaired candidate.")],
            "--candidates", "2", "--stop-loss")
        self.assertIn("return 2", (self.root / "value.py").read_text())
        self.assertEqual([item["candidate"] for item in self.kind(events, "candidate_start")],
                         [1, 2])
        aborts = [item for item in self.kind(events, "candidate_event")
                  if item["type"] == "stop_loss_abort"]
        self.assertEqual(len(aborts), 1)
        payload = aborts[0]["data"]["data"]
        payload = json.loads(payload) if isinstance(payload, str) else payload
        self.assertEqual(payload, {"consecutive_identical_edits": 3, "limit": 3})
        self.assertEqual(self.kind(events, "candidate_selected")[0]["candidate"], 2)
        self.assertEqual(self.kind(events, "final"), ["Selected repaired candidate."])
        metrics = json.loads((self.root_sessions()[0] / "metrics.json").read_text())
        self.assertLess(metrics["turns"], 10)

    def test_stop_loss_ignores_a_single_identical_edit(self):
        _, events = self.run_script([
            patch("return 1", "return 1"),
            patch("return 1", "return 2"), validate(), final("Repaired.")],
            "--stop-loss")
        self.assertIn("return 2", (self.root / "value.py").read_text())
        nested = [item for item in self.kind(events, "candidate_event")
                  if item["type"] == "stop_loss_abort"]
        self.assertFalse(nested)
        self.assertFalse(self.kind(events, "stop_loss_abort"))
        self.assertEqual(self.kind(events, "final"), ["Repaired."])


if __name__ == "__main__":
    unittest.main()
