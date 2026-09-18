#!/usr/bin/env python3
"""Score the 2026-09-17 judge rerank screen against its frozen preregistration.

Recomputes target ranks from each cell's output.json (independent of the
runner's meta.json, cross-checked against it) and applies the frozen bar:
MRR gain >= +0.10 absolute, survival not lower, hit@1 not lower. Writes
results.json and SUMMARY.md into --out (the campaign directory).
"""
import argparse
import json
import re
from pathlib import Path


def normalize(text):
    return re.sub(r"\s+", " ", text).strip()


def coverage_rank(document, query):
    prefix = normalize(query["target_text"])[:60]
    for index, row in enumerate(document.get("results") or []):
        if row.get("path") == query["target_path"] and prefix in normalize(row.get("snippet") or ""):
            return index + 1
    return None


def metrics(ranks):
    total = len(ranks)
    mrr = sum(1.0 / rank for _, rank in ranks if rank) / total
    hit1 = sum(1 for _, rank in ranks if rank == 1) / total
    hit3 = sum(1 for _, rank in ranks if rank and rank <= 3) / total
    survival = sum(1 for _, rank in ranks if rank) / total
    return {"mrr": mrr, "hit1": hit1, "hit3": hit3, "survival": survival}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--query-set", required=True)
    parser.add_argument("--run-root", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    queries = json.loads(Path(args.query_set).read_text(encoding="utf-8"))["queries"]
    cells_root = Path(args.run_root) / "cells"
    cells = {}
    for cell_dir in sorted(cells_root.iterdir()):
        output = cell_dir / "output.json"
        meta_path = cell_dir / "meta.json"
        meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
        cells[cell_dir.name] = {
            "meta": meta,
            "document": json.loads(output.read_text(encoding="utf-8")) if output.exists() else None,
        }

    mismatches, missing = [], []
    per_query = {}
    for query in queries:
        row = {"id": query["id"], "kind": query["kind"], "query": query["query"],
               "target": f"{query['target_path']}:{query['target_line']}"}
        for arm, rep in (("off", 1), ("on", 1), ("on", 2)):
            cell_id = f"{query['id']}-{arm}-r{rep}"
            cell = cells.get(cell_id)
            if not cell or cell["document"] is None:
                row[cell_id] = {"rank": None, "missing": True}
                missing.append(cell_id)
                continue
            rank = coverage_rank(cell["document"], query)
            if cell["meta"].get("rank") != rank:
                mismatches.append({"cell": cell_id, "meta_rank": cell["meta"].get("rank"),
                                   "recomputed": rank})
            row[cell_id] = {"rank": rank, "applied": cell["meta"].get("applied"),
                            "wall_s": cell["meta"].get("wall_s"), "rows": cell["meta"].get("rows"),
                            "judge_latency_ms": cell["meta"].get("judge_latency_ms"),
                            "model": cell["meta"].get("model"),
                            "exit_code": cell["meta"].get("exit_code")}
        per_query[query["id"]] = row

    def ranks_for(arm, rep):
        return [(query["id"], per_query[query["id"]][f"{query['id']}-{arm}-r{rep}"].get("rank"))
                for query in queries]

    off = metrics(ranks_for("off", 1))
    on_r1 = metrics(ranks_for("on", 1))
    on_r2 = metrics(ranks_for("on", 2))
    on_mean = {key: (on_r1[key] + on_r2[key]) / 2 for key in off}
    material = (on_mean["mrr"] - off["mrr"] >= 0.10 and on_mean["survival"] >= off["survival"] and
                on_mean["hit1"] >= off["hit1"])

    flips, regressions, gains = [], [], []
    for query in queries:
        qid = query["id"]
        on1 = per_query[qid][f"{qid}-on-r1"].get("rank")
        on2 = per_query[qid][f"{qid}-on-r2"].get("rank")
        off_rank = per_query[qid][f"{qid}-off-r1"].get("rank")
        if on1 != on2:
            flips.append(qid)
        best_on = min([r for r in (on1, on2) if r] or [99])
        if off_rank == 1 and best_on > 1:
            regressions.append(qid)
        if (off_rank or 99) > best_on:
            gains.append(qid)

    applied = [per_query[q["id"]][f"{q['id']}-on-r{rep}"].get("applied")
               for q in queries for rep in (1, 2)]
    engagement = sum(1 for value in applied if value) / len(applied)
    latencies = sorted(per_query[q["id"]][f"{q['id']}-on-r{rep}"].get("judge_latency_ms") or 0.0
                       for q in queries for rep in (1, 2))
    models = sorted({per_query[q["id"]][f"{q['id']}-on-r{rep}"].get("model")
                     for q in queries for rep in (1, 2)
                     if per_query[q["id"]][f"{q['id']}-on-r{rep}"].get("model")})
    wall_off = sorted(per_query[q["id"]][f"{q['id']}-off-r1"].get("wall_s") or 0.0 for q in queries)

    result = {
        "off": off, "on_r1": on_r1, "on_r2": on_r2, "on_mean": on_mean,
        "bar": {"mrr_gain_required": 0.10, "mrr_gain_observed": on_mean["mrr"] - off["mrr"],
                "survival_guard": on_mean["survival"] >= off["survival"],
                "hit1_guard": on_mean["hit1"] >= off["hit1"]},
        "material": material,
        "engagement": engagement, "flips": flips, "regressions": regressions, "gains": gains,
        "judge_latency_median_ms": latencies[len(latencies) // 2] if latencies else None,
        "judge_latency_max_ms": latencies[-1] if latencies else None,
        "models": models, "missing_cells": missing, "rank_mismatches": mismatches,
        "wall_off_median_s": wall_off[len(wall_off) // 2] if wall_off else None,
        "per_query": per_query,
    }
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    def pct(value):
        return f"{100 * value:.1f}%"

    lines = [
        "# Judge rerank screen — results (2026-09-17)",
        "",
        f"**Verdict: {'MATERIAL — preregistered bar met' if material else 'NOT MATERIAL — preregistered bar not met'}**",
        "",
        "## Preregistered bar (frozen before the first cell)",
        "",
        "- mean MRR (judge-on, two repetitions) ≥ judge-off MRR + 0.10 absolute",
        "- target survival not lower under judge-on",
        "- hit@1 not lower under judge-on",
        "",
        "## Arms",
        "",
        "| Arm | MRR | hit@1 | hit@3 | survival |",
        "| --- | --- | --- | --- | --- |",
        f"| off (1 rep) | {off['mrr']:.3f} | {pct(off['hit1'])} | {pct(off['hit3'])} | {pct(off['survival'])} |",
        f"| on r1 | {on_r1['mrr']:.3f} | {pct(on_r1['hit1'])} | {pct(on_r1['hit3'])} | {pct(on_r1['survival'])} |",
        f"| on r2 | {on_r2['mrr']:.3f} | {pct(on_r2['hit1'])} | {pct(on_r2['hit3'])} | {pct(on_r2['survival'])} |",
        f"| on mean | {on_mean['mrr']:.3f} | {pct(on_mean['hit1'])} | {pct(on_mean['hit3'])} | {pct(on_mean['survival'])} |",
        "",
        f"MRR gain: {on_mean['mrr'] - off['mrr']:+.3f} (required ≥ +0.10).",
        "",
        "## Mechanism",
        "",
        f"- judge engagement: {pct(engagement)} of judged cells applied",
        f"- model ids returned: {', '.join(models)}",
        f"- judge latency: median {result['judge_latency_median_ms']} ms, max {result['judge_latency_max_ms']} ms",
        f"- repetition rank flips: {len(flips)} of {len(queries)} queries {flips if flips else ''}",
        f"- gains (best judge rep better than off): {len(gains)} queries {gains}",
        f"- regressions (off rank 1, judge worse): {len(regressions)} queries {regressions}",
        f"- missing cells: {len(missing)}; rank mismatches vs runner metadata: {len(mismatches)}",
        "",
        "## Per-query ranks (off | on r1 | on r2)",
        "",
        "| query | kind | target | off | on r1 | on r2 |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for query in queries:
        row = per_query[query["id"]]
        qid = query["id"]
        off_rank = row[f"{qid}-off-r1"].get("rank")
        on1_rank = row[f"{qid}-on-r1"].get("rank")
        on2_rank = row[f"{qid}-on-r2"].get("rank")
        lines.append(f"| {qid} `{query['query']}` | {query['kind']} | {row['target']} | "
                     f"{off_rank} | {on1_rank} | {on2_rank} |")
    (out / "SUMMARY.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"verdict: {'MATERIAL' if material else 'NOT MATERIAL'}; "
          f"mrr off={off['mrr']:.3f} on={on_mean['mrr']:.3f}; "
          f"engagement={engagement:.2%}; flips={len(flips)}; missing={len(missing)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
