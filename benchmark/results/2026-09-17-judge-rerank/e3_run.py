#!/usr/bin/env python3
"""E3 agent-screen cell runner (2026-09-17 judge campaign).

One run.py invocation per (task, variant) cell; a cell whose results.json
exists is skipped. Resume-safe by construction; every cell is retained.
"""
import argparse
import subprocess
import sys
from pathlib import Path

TASKS = ["generalize_retractions_original", "generalize_retractions_renamed",
         "generalize_retractions_paraphrased", "generalize_retractions_distractor",
         "go_api_pagination", "go_multifile_transfer"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--forge", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--task-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--variants", nargs="+", required=True)
    parser.add_argument("--tasks", nargs="*", default=TASKS)
    args = parser.parse_args()
    run_py = Path(__file__).resolve().parents[2] / "run.py"
    output_root = Path(args.output)
    output_root.mkdir(parents=True, exist_ok=True)
    for task in args.tasks:
        for variant in args.variants:
            cell = output_root / f"{task}-{variant}"
            if (cell / "results.json").exists():
                print(f"skip {cell.name} (complete)", flush=True)
                continue
            print(f"cell {cell.name}", flush=True)
            command = [sys.executable, str(run_py), "--forge", args.forge, "--model", args.model,
                       "--task-dir", args.task_dir, "--tasks", task, "--suite", "all",
                       "--variants", variant, "--output", str(cell), "--repetitions", "1",
                       "--no-randomize", "--retain-terminal", "--prompt-protocol", "native",
                       "--gpu-layers", "-1", "--context", "16384", "--output-reserve", "2048",
                       "--temperature", "0.6", "--max-turns", "32", "--max-tokens", "32768",
                       "--max-input", "262144", "--timeout", "600", "--verification-timeout",
                       "120", "--order-seed", "20260831", "--gpu-index", "0", "--seed", "42"]
            cell.mkdir(parents=True, exist_ok=True)
            completed = subprocess.run(command, capture_output=True, text=True)
            (cell / "cell.stdout.txt").write_text(completed.stdout, encoding="utf-8")
            (cell / "cell.stderr.txt").write_text(completed.stderr, encoding="utf-8")
            print(f"  exit={completed.returncode}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
