#!/usr/bin/env python3
"""Judge rerank screen runner (2026-09-17 campaign).

Resume-safe: every cell is one `forge retrieve` invocation; a cell with an
existing output.json is skipped. Arms: `off` (no --judge, one repetition) and
`on` (--judge, repeated). Off-arm determinism is probed on the first queries
with an extra repetition. Scoring uses the frozen coverage rule: a result row
covers the target when its path matches and its snippet contains the first 60
normalized characters of the target line.

Run root defaults to <workspace>/.forge/judge-raw/screen so run artifacts never
enter the indexed workspace. Usage:
  python run_screen.py --forge BIN --workspace WS --config CFG \
      --query-set PATH --run-root DIR [--repeats-on 2] [--limit N]
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def coverage_rank(document, query):
    prefix = normalize(query["target_text"])[:60]
    results = document.get("results") or []
    for index, row in enumerate(results):
        if row.get("path") == query["target_path"] and prefix in normalize(row.get("snippet") or ""):
            return index + 1
    return None


def run_cell(args, query, arm, rep, cell_dir):
    cell_dir.mkdir(parents=True, exist_ok=True)
    command = [args.forge, "retrieve", query["query"], "--workspace", args.workspace,
               "--config", args.config, "--json"]
    if arm == "on":
        command.append("--judge")
    start = time.perf_counter()
    try:
        completed = subprocess.run(command, cwd=args.workspace, capture_output=True, text=True,
                                   timeout=300)
        wall = time.perf_counter() - start
        stdout, stderr, code = completed.stdout, completed.stderr, completed.returncode
    except subprocess.TimeoutExpired:
        wall = time.perf_counter() - start
        stdout, stderr, code = "", "timeout after 300s", 124
    (cell_dir / "stderr.txt").write_text(stderr, encoding="utf-8")
    meta = {"cell": cell_dir.name, "query_id": query["id"], "arm": arm, "rep": rep,
            "wall_s": round(wall, 3), "exit_code": code}
    document = None
    if code == 0:
        try:
            document = json.loads(stdout)
            (cell_dir / "output.json").write_text(stdout, encoding="utf-8")
        except json.JSONDecodeError as error:
            meta["parse_error"] = str(error)
    else:
        meta["stderr_tail"] = stderr[-400:]
    if document is not None:
        meta["rank"] = coverage_rank(document, query)
        meta["rows"] = len(document.get("results") or [])
        rerank = document.get("rerank")
        if rerank:
            meta["applied"] = rerank.get("applied")
            meta["reason"] = rerank.get("reason")
            meta["model"] = rerank.get("model")
            meta["judge_latency_ms"] = rerank.get("latency_ms")
            meta["judge_input_tokens"] = rerank.get("input_tokens")
            meta["judge_output_tokens"] = rerank.get("output_tokens")
    (cell_dir / "meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    with open(Path(args.run_root) / "progress.log", "a", encoding="utf-8") as log:
        stamp = datetime.now(timezone.utc).strftime("%H:%M:%SZ")
        log.write(f"{stamp} {cell_dir.name} exit={code} wall={meta['wall_s']}s "
                  f"rank={meta.get('rank')} applied={meta.get('applied')}\n")
    return meta


def build_schedule(queries, repeats_on, limit):
    schedule = []
    for query in queries:
        schedule.append((query, "off", 1))
        for rep in range(1, repeats_on + 1):
            schedule.append((query, "on", rep))
    if limit:
        schedule = schedule[:limit]
    return schedule


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--forge", required=True)
    parser.add_argument("--workspace", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--query-set", required=True)
    parser.add_argument("--run-root", required=True)
    parser.add_argument("--repeats-on", type=int, default=2)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()

    query_document = json.loads(Path(args.query_set).read_text(encoding="utf-8"))
    queries = query_document["queries"]
    run_root = Path(args.run_root)
    cells_root = run_root / "cells"
    cells_root.mkdir(parents=True, exist_ok=True)

    manifest = {
        "campaign": "2026-09-17-judge-rerank",
        "query_set_sha256": sha256_file(args.query_set),
        "forge_sha256": sha256_file(args.forge),
        "config_sha256": sha256_file(args.config),
        "repeats_on": args.repeats_on,
        "queries": len(queries),
        "started": datetime.now(timezone.utc).isoformat(),
    }
    (run_root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    schedule = build_schedule(queries, args.repeats_on, args.limit)
    done = skipped = 0
    for query, arm, rep in schedule:
        cell = f"{query['id']}-{arm}-r{rep}"
        cell_dir = cells_root / cell
        if (cell_dir / "output.json").exists():
            skipped += 1
            continue
        meta = run_cell(args, query, arm, rep, cell_dir)
        done += 1
        print(f"[{done + skipped}/{len(schedule)}] {cell} exit={meta['exit_code']} "
              f"rank={meta.get('rank')} applied={meta.get('applied')}", flush=True)
    print(f"cells: {done} run, {skipped} already complete", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
