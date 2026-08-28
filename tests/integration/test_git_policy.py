"""Git filter authorization using private, benign fixtures and scripted inference.

No real model is loaded. The only filter created here copies stdin to stdout and
appends to a marker in this test's temporary directory. No caller repository,
hooks, remote, downloads or network service is used.
"""

import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())
GIT = shutil.which("git")


@unittest.skipUnless(GIT, "Git is required for the real filter policy regression")
class GitPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="forge-git-policy-")
        self.addCleanup(self.temporary.cleanup)
        self.base = pathlib.Path(self.temporary.name).resolve()
        self.workspace = self.base / "workspace"
        self.workspace.mkdir()
        self.private_home = self.base / "home"
        self.private_home.mkdir()
        self.hooks = self.base / "empty-hooks"
        self.hooks.mkdir()
        self.marker = self.base / "filter-invoked.txt"
        self.filter_script = self.base / "benign-filter.py"
        self.filter_script.write_text(
            "import pathlib, sys\n"
            "data = sys.stdin.buffer.read()\n"
            "with pathlib.Path(sys.argv[1]).open('ab') as marker:\n"
            "    marker.write(b'clean filter invoked\\n')\n"
            "sys.stdout.buffer.write(data)\n",
            encoding="utf-8",
        )
        # Keep fixture setup separate from user Git configuration. Forge itself
        # deliberately inherits a small environment; its HOME/USERPROFILE also
        # point at this empty private directory, never the caller's home.
        self.environment = {
            key: value for key, value in os.environ.items()
            if not key.upper().startswith("GIT_")
        }
        self.environment.update({
            "HOME": str(self.private_home),
            "USERPROFILE": str(self.private_home),
            "XDG_CONFIG_HOME": str(self.private_home / "config"),
            "APPDATA": str(self.private_home / "AppData" / "Roaming"),
            "LOCALAPPDATA": str(self.private_home / "AppData" / "Local"),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_ATTR_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
        })
        self.git("init", "-q", "--template=")
        self.git("config", "--local", "core.hooksPath", str(self.hooks))
        self.git("config", "--local", "core.autocrlf", "false")
        self.git("config", "--local", "core.fsmonitor", "false")
        self.git("config", "--local", "protocol.allow", "never")
        self.tracked = self.workspace / "tracked.txt"
        self.tracked.write_bytes(b"before\n")
        # Make the index's stat data observably different from the subsequent
        # same-length edit, even on filesystems with coarse mtime resolution.
        initial_time = self.tracked.stat().st_mtime - 3600
        os.utime(self.tracked, (initial_time, initial_time))
        (self.workspace / ".gitattributes").write_text(
            "tracked.txt filter=forge_policy_probe\n", encoding="utf-8"
        )
        (self.workspace / ".gitignore").write_text(".forge/\n", encoding="utf-8")
        self.git("add", "--", "tracked.txt", ".gitattributes", ".gitignore")
        # Install the filter only after creating the index baseline, so no
        # setup operation can produce the marker being tested.
        command = " ".join(shlex.quote(part) for part in (
            pathlib.Path(sys.executable).as_posix(),
            self.filter_script.as_posix(),
            self.marker.as_posix(),
        ))
        self.git("config", "--local", "filter.forge_policy_probe.clean", command)
        self.git("config", "--local", "filter.forge_policy_probe.required", "true")
        self.tracked.write_bytes(b"after!\n")
        self.assertFalse(self.marker.exists())

    def git(self, *arguments):
        result = subprocess.run(
            [GIT, "-c", f"core.hooksPath={self.hooks}", "-c", "core.fsmonitor=false",
             "-C", str(self.workspace), *arguments],
            cwd=self.workspace,
            env=self.environment,
            capture_output=True,
            encoding="utf-8",
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def cli(self, *arguments):
        result = subprocess.run(
            [FORGE, "--workspace", str(self.workspace), "--no-config", *arguments],
            cwd=self.workspace,
            env=self.environment,
            capture_output=True,
            encoding="utf-8",
            timeout=40,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def run_actions(self, actions, *options):
        script = self.base / "actions.json"
        # Native notifications can arrive after a process has already been
        # reindexed. Permit the successful final to be reconsidered against the
        # fresh generation, without repeating any tool or mutation action.
        scripted = list(actions)
        if scripted and "final" in scripted[-1]:
            scripted.extend([scripted[-1]] * 4)
        script.write_text(json.dumps(scripted), encoding="utf-8")
        sessions = self.workspace / ".forge" / "sessions"
        previous = set(sessions.iterdir()) if sessions.exists() else set()
        result = self.cli(
            "run", "Inspect the test repository without changing it.",
            "--script", str(script), "--json", "--max-turns", "6", *options,
        )
        events = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        created = set(sessions.iterdir()) - previous
        self.assertEqual(len(created), 1)
        session = created.pop()
        self.assertEqual(session.parent, sessions)
        metrics = json.loads((session / "metrics.json").read_text(encoding="utf-8"))
        self.assertTrue(metrics["simulated"])
        self.assertEqual(metrics["status"], "ok")
        snapshots = [event["data"] for event in events if event["type"] == "patch_snapshot"]
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(snapshots[0]["status"], "not_collected")
        self.assertEqual(snapshots[0]["reason"], "explicit_git_diff_required")
        self.assertFalse((session / "patch.diff").exists())
        messages = [event["data"] for event in events if event["type"] == "message"]
        self.assertEqual(messages, ["Inspection complete."])
        return events, session

    def tool_actions(self, name):
        return [{"tool": name, "args": {}}, {"final": "Inspection complete."}]

    def assert_denied(self, name, *options):
        events, _ = self.run_actions(self.tool_actions(name), *options)
        results = [event["data"] for event in events if event["type"] == "tool_result"]
        self.assertEqual(len(results), 1)
        self.assertEqual(results[0]["name"], name)
        self.assertEqual(results[0]["status"], "policy")
        self.assertIn("unsandboxed process approval", results[0]["output"])
        self.assertNotIn("exit_code", results[0])
        self.assertFalse(self.marker.exists(), "An unapproved path executed the clean filter")

    def test_git_diff_requires_process_approval(self):
        self.assert_denied("git_diff")

    def test_git_status_requires_process_approval(self):
        self.assert_denied("git_status")

    def test_write_approval_does_not_authorize_git_processes(self):
        for tool in ("git_diff", "git_status"):
            with self.subTest(tool=tool):
                self.assert_denied(tool, "--allow-write")

    def test_finalization_never_implicitly_executes_git(self):
        for options in ((), ("--allow-exec",)):
            with self.subTest(options=options):
                self.run_actions([{"final": "Inspection complete."}], *options)
                self.assertFalse(self.marker.exists(), "Finalization executed the clean filter")

    def test_explicit_git_diff_approval_executes_the_benign_filter(self):
        events, session = self.run_actions(self.tool_actions("git_diff"), "--allow-exec")
        result = next(event["data"] for event in events if event["type"] == "tool_result")
        self.assertEqual(result["name"], "git_diff")
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["exit_code"], 0)
        self.assertIn("diff --git a/tracked.txt b/tracked.txt", result["output"])
        self.assertIn(b"after!", (session / "tool" / "000001.stdout").read_bytes())
        self.assertIn(b"clean filter invoked\n", self.marker.read_bytes())

    def test_explicit_git_status_approval_executes_the_benign_filter(self):
        events, _ = self.run_actions(self.tool_actions("git_status"), "--allow-exec")
        result = next(event["data"] for event in events if event["type"] == "tool_result")
        self.assertEqual(result["name"], "git_status")
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["exit_code"], 0)
        self.assertIn("tracked.txt", result["output"])
        self.assertIn(b"clean filter invoked\n", self.marker.read_bytes())

    def test_index_enumeration_does_not_run_the_clean_filter(self):
        self.cli("index", "--json")
        self.assertFalse(self.marker.exists())

    def test_fixture_filter_is_live_for_both_git_commands(self):
        # Independent positive controls: the denied tests would be meaningless
        # if this Git version or fixture never invoked its configured filter.
        for arguments in (
            ("diff", "--no-ext-diff", "--no-textconv", "--"),
            ("status", "--porcelain=v1", "--untracked-files=normal"),
        ):
            with self.subTest(command=arguments[0]):
                self.marker.unlink(missing_ok=True)
                self.git(*arguments)
                self.assertIn(b"clean filter invoked\n", self.marker.read_bytes())


if __name__ == "__main__":
    unittest.main()
