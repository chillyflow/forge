"""Scripted conversation contracts; these are not model-quality measurements."""

import json
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest

FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())


def call(name, arguments=None):
    return {"role": "assistant", "content": "", "tool_calls": [
        {"type": "function", "function": {"name": name,
         "arguments": json.dumps(arguments or {}, separators=(",", ":"))}}]}


def final(answer):
    return call("final", {"answer": answer})


def read(path="sample.txt"):
    return call("read_file", {"path": path, "start": 1, "end": 200})


class InteractiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forge-interactive-")
        self.root = pathlib.Path(self.temp.name)
        (self.root / "sample.txt").write_text("the original workspace evidence\n", encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def command(self, actions, *options, minimal=True, implicit=False):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        args = [FORGE]
        if not implicit:
            args.append("chat")
        args += ["--script", str(script), "--workspace", str(self.root), "--no-config", "--json",
                 "--prompt-protocol", "native", "--context", "65536", "--output-reserve", "4096"]
        if minimal:
            args.append("--minimal-agent")
        return [*args, *options]

    def run_script(self, actions, stdin, *options, success=True, minimal=True, implicit=False):
        result = subprocess.run(self.command(actions, *options, minimal=minimal, implicit=implicit),
                                input=stdin, text=True, encoding="utf-8", capture_output=True,
                                timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        return result, events

    def sessions(self):
        directory = self.root / ".forge" / "sessions"
        if not directory.exists():
            return {}
        found = {}
        for path in directory.iterdir():
            events = [json.loads(line) for line in (path / "events.jsonl").read_text(
                encoding="utf-8").splitlines()]
            request = next((event["data"] for event in events if event["type"] == "request"), None)
            if request is not None:
                found[request] = path
        return found

    @staticmethod
    def prompt(session, turn=1):
        return json.loads((session / "context" / f"{turn:04d}.txt").read_text(encoding="utf-8"))

    @staticmethod
    def outputs(events):
        return [event["data"] for event in events if event["type"] == "tool_result"]

    def test_two_requests_keep_actual_assistant_and_tool_history_in_order(self):
        self.run_script([read(), final("First answer remains intact."), read(), final("Second answer.")],
                        "first-user-task\nsecond-user-task\n/quit\n")
        sessions = self.sessions()
        self.assertEqual(len(sessions), 2)
        prompt = self.prompt(sessions["second-user-task"])
        messages = prompt["messages"]
        first_user = next(i for i, message in enumerate(messages)
                          if message["role"] == "user" and "first-user-task" in message["content"])
        second_user = next(i for i, message in enumerate(messages)
                           if message["role"] == "user" and "second-user-task" in message["content"])
        read_result = next(i for i, message in enumerate(messages)
                           if message["role"] == "tool" and message["name"] == "read_file")
        self.assertLess(first_user, read_result)
        self.assertLess(read_result, second_user)
        self.assertIn("the original workspace evidence", messages[read_result]["content"])
        assistant = [message for message in messages if message["role"] == "assistant"]
        self.assertEqual([message["tool_calls"][0]["function"]["name"] for message in assistant],
                         ["read_file", "final"])
        self.assertIn("First answer remains intact.", json.dumps(assistant))
        self.assertEqual([message["tool_calls"][0]["id"] for message in assistant],
                         [message["tool_call_id"] for message in messages if message["role"] == "tool"])
        self.assertEqual(len({message["tool_calls"][0]["id"] for message in assistant}), len(assistant))
        for session in sessions.values():
            self.assertTrue(json.loads((session / "metrics.json").read_text())["simulated"])

    def test_default_model_invocation_is_persistent(self):
        self.run_script([final("One."), final("Two.")], "one\ntwo\n", implicit=True)
        self.assertEqual(set(self.sessions()), {"one", "two"})
        self.assertIn("One.", json.dumps(self.prompt(self.sessions()["two"])))

    def test_ordinary_loop_retains_final_and_question_evidence(self):
        self.run_script([call("ask_user", {"question": "Which naming convention?"}),
                         final("Use snake_case."), final("Naming retained.")],
                        "first\nsnake_case\nsecond\n/quit\n", minimal=False)
        prompt = self.prompt(self.sessions()["second"])
        assistant = [message for message in prompt["messages"] if message["role"] == "assistant"]
        self.assertIn("Use snake_case.", json.dumps(assistant))
        answer = next(message for message in prompt["messages"]
                      if message["role"] == "tool" and message["name"] == "ask_user")
        self.assertEqual(json.loads(answer["content"]), {"status": "answered", "answer": "snake_case"})

    def test_answer_is_returned_to_the_model_as_a_tool_result(self):
        result, events = self.run_script([call("ask_user", {"question": "Which color?"}),
                                          final("Selected blue.")],
                                         "choose-color\nblue\n/quit\n")
        self.assertIn("Question: Which color?", result.stderr)
        output = next(item for item in self.outputs(events) if item["name"] == "ask_user")
        self.assertEqual(output["status"], "ok")
        self.assertEqual(json.loads(output["output"]), {"status": "answered", "answer": "blue"})
        prompt = self.prompt(self.sessions()["choose-color"], 2)
        answer = next(message for message in prompt["messages"]
                      if message["role"] == "tool" and message["name"] == "ask_user")
        self.assertEqual(json.loads(answer["content"])["answer"], "blue")

    def test_refusal_is_visible_and_does_not_fabricate_an_answer(self):
        _, events = self.run_script([call("ask_user", {"question": "Optional preference?"}),
                                     final("The preference was declined.")],
                                    "optional\n/decline\n/quit\n")
        output = next(item for item in self.outputs(events) if item["name"] == "ask_user")
        self.assertEqual(output["status"], "policy")
        self.assertEqual(json.loads(output["output"])["status"], "declined")
        self.assertNotIn("answer", json.loads(output["output"]))
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 2)

    def test_question_eof_cancels_without_another_generation(self):
        result, events = self.run_script([call("ask_user", {"question": "Required answer?"}),
                                          final("Must not generate after EOF.")],
                                         "ask-then-eof\n", success=False)
        self.assertIn("cancelled", result.stderr.lower())
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 1)

    def test_question_cancel_stops_the_session(self):
        result, events = self.run_script([call("ask_user", {"question": "Continue?"}),
                                          final("Must not be sampled.")],
                                         "ask-then-cancel\n/cancel\n", success=False)
        self.assertIn("cancel", result.stderr.lower())
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 1)

    def test_unanswered_question_obeys_deadline_even_with_open_stdin(self):
        process = subprocess.Popen(self.command([call("ask_user", {"question": "Wait?"})],
                                                "--wall-ms", "500"),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8")
        output = {}
        readers = [threading.Thread(target=lambda name, stream: output.update({name: stream.read()}),
                                    args=(name, stream), daemon=True)
                   for name, stream in [("stdout", process.stdout), ("stderr", process.stderr)]]
        for reader in readers:
            reader.start()
        try:
            start = time.monotonic()
            process.stdin.write("wait-for-answer\n")
            process.stdin.flush()
            process.wait(timeout=8)
            self.assertLess(time.monotonic() - start, 8)
            self.assertNotEqual(process.returncode, 0)
            for reader in readers:
                reader.join(timeout=2)
            stderr = output.get("stderr", "")
            self.assertIn("deadline", stderr.lower())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            process.stdin.close()
            for reader in readers:
                reader.join(timeout=2)
            process.stdout.close()
            process.stderr.close()

    def test_new_clears_history_without_reloading_script_model(self):
        self.run_script([final("Unique prior answer."), final("Fresh answer.")],
                        "prior-task\n/new fresh-task\n/quit\n")
        prompt = self.prompt(self.sessions()["fresh-task"])
        self.assertNotIn("prior-task", json.dumps(prompt))
        self.assertNotIn("Unique prior answer.", json.dumps(prompt))
        self.assertEqual([message for message in prompt["messages"] if message["role"] == "assistant"], [])

    def test_history_limit_drops_only_complete_oldest_exchanges_with_notice(self):
        self.run_script([read(), final("answer-one"), read(), final("answer-two"), final("answer-three")],
                        "task-one\ntask-two\ntask-three\n/quit\n", "--history-turns", "1")
        prompt = self.prompt(self.sessions()["task-three"])
        text = json.dumps(prompt)
        self.assertIn("task-two", text)
        self.assertIn("answer-two", text)
        self.assertNotIn("task-one", text)
        self.assertNotIn("answer-one", text)
        self.assertIn("1 older user exchanges were omitted", text)
        assistant = [message for message in prompt["messages"] if message["role"] == "assistant"]
        self.assertEqual([message["tool_calls"][0]["function"]["name"] for message in assistant],
                         ["read_file", "final"])

    def test_oversized_exchange_blocks_continuation_until_explicit_new(self):
        result, _ = self.run_script([final("x" * 2048), final("After reset.")],
                                    "large\nblocked\n/new\nfresh\n/quit\n",
                                    "--history-bytes", "512", success=False)
        self.assertIn("conversation", result.stderr.lower())
        self.assertIn("/new", result.stderr)
        self.assertIn("fresh", self.sessions())
        prompt = self.prompt(self.sessions()["fresh"])
        self.assertNotIn("x" * 100, json.dumps(prompt))
        self.assertNotIn("large", json.dumps(prompt))

    def test_context_overflow_stops_instead_of_silently_dropping_retained_history(self):
        result, events = self.run_script([final("prior-answer-" + "x" * 500), final("Unreachable")],
                                         "small-task\n" + "z" * 12000 + "\n/quit\n",
                                         "--context", "4096", "--output-reserve", "1024", success=False)
        self.assertIn("context", result.stderr.lower())
        self.assertEqual(len([event for event in events if event["type"] == "inference"]), 1)

    def test_question_answer_never_grants_process_permission(self):
        marker = self.root / "unapproved.txt"
        _, events = self.run_script([
            call("ask_user", {"question": "May I run the command?"}),
            call("run_command", {"argv": [sys.executable, "-c",
                 "from pathlib import Path; Path('unapproved.txt').write_text('bad')"]}),
            final("Process permission remains denied.")],
            "permission-task\nyes\n/quit\n")
        output = next(item for item in self.outputs(events) if item["name"] == "run_command")
        self.assertEqual(output["status"], "policy")
        self.assertFalse(marker.exists())

    def test_multiline_task_preserves_newlines(self):
        self.run_script([final("Multiline received.")],
                        "/begin\nFirst requirement.\nSecond requirement.\n/end\n/quit\n")
        self.assertIn("First requirement.\nSecond requirement.\n", self.sessions())

    def test_eof_during_multiline_does_not_submit_partial_task(self):
        result, events = self.run_script([final("Never sampled.")], "/begin\npartial\n", success=False)
        self.assertIn("not submitted", result.stderr)
        self.assertFalse([event for event in events if event["type"] == "inference"])
        self.assertFalse(self.sessions())

    def test_empty_eof_is_a_clean_exit(self):
        _, events = self.run_script([final("Never sampled.")], "")
        self.assertFalse([event for event in events if event["type"] == "inference"])

    def test_flattened_protocol_is_rejected(self):
        result, _ = self.run_script([final("Never sampled.")], "task\n",
                                    "--prompt-protocol", "flattened", minimal=False, success=False)
        self.assertIn("native", result.stderr.lower())

    def test_noninteractive_question_fails_clearly_without_reading_stdin(self):
        args = self.command([call("ask_user", {"question": "Unavailable question?"}),
                             final("Question unavailable.")])
        args[1:2] = ["run", "single-task"]
        result = subprocess.run(args, input="this must not become an answer\n", text=True,
                                encoding="utf-8", capture_output=True, timeout=15)
        combined = result.stdout + result.stderr
        self.assertTrue("unavailable" in combined.lower() or "unsupported" in combined.lower(), combined)
        self.assertNotIn("Answer>", result.stderr)


if __name__ == "__main__":
    unittest.main()
