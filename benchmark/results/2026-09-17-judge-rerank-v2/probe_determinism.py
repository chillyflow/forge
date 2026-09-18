#!/usr/bin/env python3
"""Off-arm determinism probe for the 2026-09-17 judge rerank screen.

Re-runs the first eight queries without --judge and compares the ordered result
paths against the recorded off cells. Any difference is reported; the probe is
evidence, not a re-run of the screen.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--forge", required=True)
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--query-set", required=True)
    parser.add_argument("--run-root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--count", type=int, default=8)
    args = parser.parse_args()

    queries = json.loads(Path(args.query_set).read_text(encoding="utf-8"))["queries"][:args.count]
    results = []
    for query in queries:
        recorded = Path(args.run_root) / "cells" / f"{query['id']}-off-r1" / "output.json"
        expected = [row["path"] for row in json.loads(recorded.read_text())["results"]]
        completed = subprocess.run(
            [args.forge, "retrieve", query["query"], "--workspace", args.workspace,
             "--config", args.config, "--json"],
            cwd=args.workspace, capture_output=True, text=True, timeout=300)
        observed = [row["path"] for row in json.loads(completed.stdout)["results"]]
        results.append({"id": query["id"], "equal": expected == observed,
                        "expected": expected, "observed": observed})
        print(f"{query['id']}: {'EQUAL' if expected == observed else 'DIFFERS'}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "determinism_probe.json").write_text(json.dumps(results, indent=2) + "\n")
    differing = [item["id"] for item in results if not item["equal"]]
    print(f"determinism probe: {len(results) - len(differing)}/{len(results)} equal"
          + (f"; DIFFERS: {differing}" if differing else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
