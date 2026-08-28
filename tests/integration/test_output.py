"""Output text boundaries and byte-preserving artifacts, without model inference."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())
PROCESS_HEADER = b"exit_code=0 timeout=false cancelled=false truncated=false\nstdout:\n"


class OutputTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="forge-output-")
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_script(self, actions, *options):
        script = self.root / "script.json"
        script.write_text(json.dumps(actions), encoding="utf-8")
        result = subprocess.run(
            [FORGE, "run", "Inspect the recorded command output.", "--script", str(script),
             "--workspace", str(self.root), "--no-config", "--allow-exec", "--json",
             "--no-auto-validation",
             "--context", "65536", "--output-reserve", "1024", *options],
            capture_output=True, timeout=90,
        )
        # Strict decoding makes a partial or invalid UTF-8 code point a failure.
        stdout, stderr = result.stdout.decode("utf-8"), result.stderr.decode("utf-8")
        self.assertEqual(result.returncode, 0, stdout + stderr)
        events = [json.loads(line) for line in stdout.splitlines() if line.startswith("{")]
        outputs = [event["data"]["output"] for event in events if event["type"] == "tool_result"]
        sessions = list((self.root / ".forge" / "sessions").iterdir())
        self.assertEqual(len(sessions), 1)
        # Persisted events must be valid UTF-8 as well, without replacement bytes.
        (sessions[0] / "events.jsonl").read_bytes().decode("utf-8")
        return outputs, sessions[0]

    @staticmethod
    def command(code):
        return {"tool": "run_command", "args": {"argv": [sys.executable, "-c", code]}}

    @staticmethod
    def expand(offset):
        return {"tool": "expand_output", "args": {"id": 1, "offset": offset}}

    @staticmethod
    def page(raw, offset):
        while offset < len(raw) and raw[offset] & 0xC0 == 0x80:
            offset += 1
        end = min(offset + 8192, len(raw))
        while end < len(raw) and end > offset and raw[end] & 0xC0 == 0x80:
            end -= 1
        return raw[offset:end].decode("utf-8")

    def test_binary_and_unicode_streams_keep_exact_artifacts(self):
        out = b"begin\x00after\xff\xed\xa0\x80 " + "\u20ac\U0001f30d".encode("utf-8") + b"\n"
        err = "\u03a9".encode("utf-8") + b"\x00stderr-tail\x01\xc2"
        code = (f"import sys; sys.stdout.buffer.write(bytes.fromhex('{out.hex()}')); "
                f"sys.stderr.buffer.write(bytes.fromhex('{err.hex()}'))")
        outputs, session = self.run_script([
            self.command(code), self.expand(0), {"final": "Recorded both streams."},
        ])
        self.assertEqual((session / "tool" / "000001.stdout").read_bytes(), out)
        self.assertEqual((session / "tool" / "000001.stderr").read_bytes(), err)
        raw = (session / "tool" / "000001.raw").read_bytes().decode("utf-8")
        self.assertEqual(outputs, [raw, raw])
        self.assertIn("begin\\x00after\\xff\\xed\\xa0\\x80 \u20ac\U0001f30d", raw)
        self.assertIn("\u03a9\\x00stderr-tail\\x01\\xc2", raw)

    def test_go_json_decodes_output_past_nul(self):
        event = {"Action": "output", "Output": "before\x00sample.go:7: error after NUL \u03a9\n"}
        out = (json.dumps(event) + "\n").encode("utf-8")
        code = f"import sys; sys.stdout.buffer.write(bytes.fromhex('{out.hex()}'))"
        outputs, session = self.run_script([
            self.command(code), {"final": "Read the diagnostic."},
        ])
        self.assertEqual((session / "tool" / "000001.stdout").read_bytes(), out)
        self.assertIn("before\\x00sample.go:7: error after NUL \u03a9", outputs[0])
        self.assertNotIn('"Action"', outputs[0])

    def test_nonsemantic_clip_does_not_split_unicode(self):
        payload = ("\u20ac" * 341 + "a").encode("utf-8")
        code = "import sys; sys.stdout.buffer.write(('\\u20ac'*341+'a').encode('utf-8'))"
        outputs, session = self.run_script([
            self.command(code), {"final": "Read the bounded output."},
        ], "--no-semantic", "--max-tool-bytes", "1024")
        self.assertEqual((session / "tool" / "000001.stdout").read_bytes(), payload)
        raw = (session / "tool" / "000001.raw").read_bytes()
        self.assertTrue(raw.startswith(PROCESS_HEADER))
        self.assertEqual(raw[1024] & 0xC0, 0x80)
        cut = 1024
        while raw[cut] & 0xC0 == 0x80:
            cut -= 1
        expected = raw[:cut].decode("utf-8") + "\n[truncated; use expand_output]\n"
        self.assertEqual(outputs[0], expected)

    def test_expand_output_aligns_start_and_end_to_unicode(self):
        prefix = 8191 - len(PROCESS_HEADER)
        payload = b"A" * prefix + "\u754c".encode("utf-8") + b"B" * 80
        code = (f"import sys; sys.stdout.buffer.write(b'A'*{prefix}+"
                "'\\u754c'.encode('utf-8')+b'B'*80)")
        outputs, session = self.run_script([
            self.command(code), self.expand(0), self.expand(8192),
            {"final": "Read both pages."},
        ])
        self.assertEqual((session / "tool" / "000001.stdout").read_bytes(), payload)
        raw = (session / "tool" / "000001.raw").read_bytes()
        self.assertEqual(raw.index("\u754c".encode("utf-8")), 8191)
        self.assertEqual(outputs[1], self.page(raw, 0))
        self.assertEqual(len(outputs[1].encode("utf-8")), 8191)
        self.assertEqual(outputs[2], self.page(raw, 8192))
        self.assertTrue(outputs[2].startswith("B"))

    def test_capture_truncation_escapes_partial_character(self):
        payload = b"a" * 1023 + "\u20ac".encode("utf-8")
        code = "import sys; sys.stdout.buffer.write(b'a'*1023+'\\u20ac'.encode('utf-8'))"
        outputs, session = self.run_script([
            self.command(code), self.expand(0), {"final": "Read the captured prefix."},
        ], "--max-tool-bytes", "1024")
        self.assertEqual((session / "tool" / "000001.stdout").read_bytes(), payload[:1024])
        raw = (session / "tool" / "000001.raw").read_bytes().decode("utf-8")
        self.assertIn("truncated=true", raw)
        self.assertIn("\\xe2", raw)
        self.assertIn("\\xe2", outputs[0])
        # The expansion is itself subject to the agent's visible-byte limit.
        self.assertTrue(outputs[1].startswith("exit_code=0"))

    def test_read_file_rejects_invalid_utf8(self):
        (self.root / "invalid.txt").write_bytes(b"before\xffafter\n")
        outputs, _ = self.run_script([
            {"tool": "read_file", "args": {"path": "invalid.txt", "start": 1, "end": 2}},
            {"final": "Invalid text was rejected."},
        ])
        self.assertIn("TOOL_ERROR [parse]", outputs[0])
        self.assertIn("invalid UTF-8", outputs[0])


if __name__ == "__main__":
    unittest.main()
