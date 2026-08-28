"""CLI configuration contracts; no model downloads, and inference is explicitly simulated."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


FORGE = str(pathlib.Path(sys.argv.pop(1)).resolve())
FALLBACK_FORGE = None
if "--fallback-forge" in sys.argv:
    index = sys.argv.index("--fallback-forge")
    FALLBACK_FORGE = str(pathlib.Path(sys.argv.pop(index + 1)).resolve())
    sys.argv.pop(index)


class ConfigCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="forge-config-cli-")
        self.base = pathlib.Path(self.temporary.name)
        self.workspace = self.base / "workspace"
        self.workspace.mkdir()

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return str(path)

    def cli(self, *arguments, success=True, executable=FORGE):
        result = subprocess.run(
            [executable, "--workspace", str(self.workspace), *arguments],
            cwd=self.workspace,
            capture_output=True,
            encoding="utf-8",
            timeout=35,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def plan(self, *arguments):
        result = self.cli("hardware-plan", "--json", *arguments)
        # A machine-readable command must emit one complete document, not logs
        # interspersed with JSON. Backend diagnostics belong on stderr.
        return json.loads(result.stdout)

    def fixture(self, actions):
        return self.write(self.workspace / "script.json", json.dumps(actions))

    def test_hardware_without_model_reports_unknown_fit(self):
        report = self.plan("--no-config", "--gpu-layers", "auto")
        self.assertEqual(report["type"], "hardware_plan")
        hardware = report["hardware"]
        self.assertGreaterEqual(hardware["logical_cpus"], 1)
        if hardware["ram_total_known"]:
            self.assertGreater(hardware["ram_total_bytes"], 0)
        if hardware["ram_total_known"] and hardware["ram_available_known"]:
            self.assertLessEqual(hardware["ram_available_bytes"], hardware["ram_total_bytes"])
        if not hardware["gpu_detection_available"]:
            self.assertEqual(hardware["gpus"], [])
        self.assertIsNone(report["model"]["path"])
        self.assertFalse(report["model"]["metadata_available"])
        self.assertEqual(report["plan"]["fit"], "unknown")
        self.assertEqual(report["plan"]["gpu_layers"], 0)
        self.assertEqual(report["plan"]["context_tokens"], 4096)
        self.assertFalse(report["plan"]["kv_estimate_available"])
        self.assertIsNone(report["plan"]["estimated_kv_bytes"])
        self.assertFalse(report["plan"]["draft_enabled"])
        self.assertTrue(report["plan"]["assumptions"])

    def test_workspace_discovery_and_no_config(self):
        self.write(self.workspace / "forge.toml", "model.context=3072\nagent.output_reserve=512\n")
        self.assertEqual(self.plan()["plan"]["context_tokens"], 3072)
        self.assertEqual(self.plan("--no-config")["plan"]["context_tokens"], 4096)
        selected = self.base / "selected"
        selected.mkdir()
        self.write(selected / "forge.toml", "model.context=2560\nagent.output_reserve=512\n")
        self.assertEqual(self.plan("--workspace", str(selected))["plan"]["context_tokens"], 2560)

    def test_profile_project_and_cli_precedence(self):
        profile = self.write(self.base / "base.toml", "model.context=2048\nagent.output_reserve=512\n")
        self.write(self.workspace / "forge.toml", "model.context=3072\n")
        self.assertEqual(self.plan("--profile", profile)["plan"]["context_tokens"], 3072)
        self.assertEqual(self.plan("--profile", profile, "--no-config")["plan"]["context_tokens"], 2048)
        # Put the CLI override before the file flag: argument order cannot allow
        # the profile/project file to overwrite an explicit CLI value.
        self.assertEqual(
            self.plan("--context", "3584", "--profile", profile)["plan"]["context_tokens"],
            3584,
        )
        explicit = self.write(self.base / "explicit.toml", "model.context=2560\n")
        self.write(self.workspace / "forge.toml", "unknown_field=true\n")
        self.assertEqual(
            self.plan("--profile", profile, "--config", explicit)["plan"]["context_tokens"],
            2560,
        )

    def test_cli_values_override_file_limits_and_profile_still_applies_with_no_config(self):
        profile = self.write(self.base / "base.toml", "model.context=2048\nagent.output_reserve=512\n")
        result = self.plan(
            "--context", "8192", "--output-reserve", "5000",
            "--profile", profile, "--no-config",
        )
        self.assertEqual(result["plan"]["context_tokens"], 5001)
        self.assertTrue(result["plan"]["context_reduced"])
        # Numeric GPU settings must not trigger metadata loading for a fixture.
        fixture = self.fixture([{"final": "done"}])
        complete = self.cli(
            "complete", "prompt", "--script", fixture, "--gpu-layers", "0",
            "--seed", "7", "--temperature", "0.5", "--profile", profile, "--no-config",
        )
        self.assertEqual(json.loads(complete.stdout), {"final": "done"})
        self.assertNotIn("hardware auto", complete.stderr)

    def test_explicit_file_failures_and_invalid_discovery(self):
        self.write(self.workspace / "forge.toml", "inference.speculative=true\n")
        self.assertIn("not implemented", self.cli("hardware-plan", success=False).stderr)
        self.plan("--no-config")
        missing = str(self.base / "missing.toml")
        self.assertIn("missing.toml", self.cli("hardware-plan", "--config", missing, success=False).stderr)
        self.assertIn("missing.toml", self.cli("hardware-plan", "--profile", missing, success=False).stderr)
        self.assertIn("cannot be combined", self.cli(
            "hardware-plan", "--no-config", "--config", missing, success=False,
        ).stderr)
        self.assertIn("Options:", self.cli("--help").stdout)

    def test_physical_checkpoint_configuration_is_explicit_and_never_simulated(self):
        fixture = self.fixture([{"final": "done"}])
        self.write(self.workspace / "forge.toml", "[inference.checkpoints]\nenabled=true\n")
        failed = self.cli("complete", "prompt", "--script", fixture, success=False)
        self.assertIn("does not support automatic physical checkpoint", failed.stderr)
        completed = self.cli("complete", "prompt", "--script", fixture, "--no-checkpoint-cache")
        self.assertEqual(json.loads(completed.stdout), {"final": "done"})
        failed = self.cli("complete", "prompt", "--script", fixture, "--no-config",
                          "--checkpoint-cache", success=False)
        self.assertIn("does not support automatic physical checkpoint", failed.stderr)
        failed = self.cli("complete", "prompt", "--script", fixture, "--no-checkpoint-cache",
                          "--checkpoint-cache-entries", "0", success=False)
        self.assertIn("inference.checkpoints", failed.stderr)
        completed = self.cli("complete", "prompt", "--script", fixture, "--no-checkpoint-cache",
                             "--checkpoint-cache-entries", "2", "--checkpoint-cache-bytes", "4096",
                             "--checkpoint-cache-min-tokens", "1", "--checkpoint-cache-captures", "1")
        metrics = json.loads(completed.stderr)
        self.assertTrue(metrics["simulated"])
        self.assertEqual(metrics["checkpoint_hits"], 0)

    def test_unknown_malformed_and_ambiguous_options(self):
        cases = [
            (("hardware-plan", "--unknown"), "Unknown option"),
            (("hardware-plan", "--config"), "Missing value"),
            (("hardware-plan", "--threads", "oops"), "Invalid numeric"),
            (("hardware-plan", "--temperature", "nan"), "Invalid numeric"),
            (("hardware-plan", "--context", "-1"), "Invalid numeric"),
            (("hardware-plan", "--gpu-layers", "AUTO"), "Invalid numeric"),
            (("hardware-plan", "--threads", "1025"), "inference.threads"),
            (("hardware-plan", "--profile", "a", "--profile", "b"), "only one"),
            (("hardware-plan", "--config", "a", "--config", "b"), "only one"),
            (("hardware-plan", "model.gguf"), "positional argument"),
        ]
        for arguments, expected in cases:
            with self.subTest(arguments=arguments):
                self.assertIn(expected, self.cli(*arguments, success=False).stderr)
        # A value that happens to look like a flag is still a path, not an
        # implicit --no-config permission/discovery override in the first pass.
        self.write(self.workspace / "--no-config", "model.context=2048\nagent.output_reserve=512\n")
        self.assertEqual(self.plan("--profile", "--no-config")["plan"]["context_tokens"], 2048)

    def test_configuration_does_not_grant_tool_permissions(self):
        config_path = self.workspace / "forge.toml"
        for text in ("agent.allow_write=true\n", "tools.shell.allow_exec=true\n"):
            self.write(config_path, text)
            self.assertIn("unknown", self.cli("hardware-plan", success=False).stderr)
        self.write(config_path, "tools.shell.network=true\n")
        self.write(self.workspace / "note.txt", "original")
        fixture = self.fixture([
            {"tool": "apply_patch", "args": {"path": "note.txt", "old_text": "original", "new_text": "changed"}},
            {"tool": "run_command", "args": {"argv": [
                sys.executable, "-c", "from pathlib import Path; Path('unexpected.txt').write_text('executed')",
            ]}},
            {"final": "Operations were denied."},
        ])
        # This exact two-action fixture tests permission dispatch, not native
        # notification timing. A delayed native event may correctly discard a
        # fixed script response as stale. Keep both denial assertions and use
        # the real bounded snapshot fallback when supplied by CTest.
        result = self.cli("run", "Check permissions", "--script", fixture, "--json",
                          executable=FALLBACK_FORGE or FORGE)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        if FALLBACK_FORGE:
            scans = [event["data"] for event in events if event["type"] == "repository_scan"]
            self.assertTrue(scans, result.stdout + result.stderr)
            self.assertTrue(all(not scan["watch_available"] for scan in scans))
            self.assertIn("fallback test fixture", scans[0]["watch_fallback"])
        names = [event["data"]["name"] for event in events if event["type"] == "tool_result"]
        outputs = [event["data"]["output"] for event in events if event["type"] == "tool_result"]
        self.assertEqual(names, ["apply_patch", "run_command"], result.stdout + result.stderr)
        self.assertEqual(len(outputs), 2, result.stdout + result.stderr)
        self.assertTrue(all("TOOL_ERROR [policy]" in output for output in outputs))
        self.assertEqual((self.workspace / "note.txt").read_text(), "original")
        self.assertFalse((self.workspace / "unexpected.txt").exists())

    def test_network_false_blocks_unsandboxed_exec_before_session_creation(self):
        self.write(self.workspace / "forge.toml", "tools.shell.network=false\n")
        fixture = self.fixture([{"final": "done"}])
        result = self.cli("run", "task", "--script", fixture, "--allow-exec", success=False)
        self.assertIn("cannot enforce a network sandbox", result.stderr)
        self.assertFalse((self.workspace / ".forge").exists())
        # Read-only operation remains available under the same restriction.
        self.plan()

    def test_explicit_fixture_overrides_configured_model_without_implicit_simulation(self):
        self.write(self.workspace / "forge.toml", "model.path='absent.gguf'\ninference.gpu_layers='auto'\n")
        fixture = self.fixture([{"final": "explicit fixture"}])
        result = self.cli("complete", "prompt", "--script", fixture)
        self.assertEqual(json.loads(result.stdout), {"final": "explicit fixture"})
        metrics = json.loads(result.stderr[result.stderr.index("{"):])
        self.assertTrue(metrics["simulated"])
        self.assertIn("explicit simulated fixture", result.stderr)
        both = self.cli("complete", "prompt", "--script", fixture, "--model", "absent.gguf", success=False)
        self.assertIn("not both", both.stderr)
        # This command never attempts a model load, even with an invalid model
        # path and automatic placement configured for future inference commands.
        plan = json.loads(self.cli("validation-plan").stdout)
        self.assertFalse(plan["applicable"])

    def test_relative_model_path_uses_defining_file_directory(self):
        config = self.base / "profiles" / "local.toml"
        self.write(config, "model.path='../models/absent.gguf'\n")
        result = self.cli("hardware-plan", "--config", str(config), success=False)
        expected = (config.parent / "../models/absent.gguf").resolve()
        prefix = "Model must be a readable regular GGUF file: "
        self.assertIn(prefix, result.stderr)
        reported = result.stderr.split(prefix, 1)[1].strip()
        # macOS /var and Windows short-name temp paths may have different
        # spellings while resolving to the same defining-file directory.
        self.assertEqual(pathlib.Path(reported).resolve(), expected)


if __name__ == "__main__":
    unittest.main()
