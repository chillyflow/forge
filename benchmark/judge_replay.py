#!/usr/bin/env python3
"""Jev shadow replay over retained TypeSafe repair-feedback records.

De-risking step for the larger A2 (repair-feedback) A/B campaign: re-sends
every retained feedback request VERBATIM N times to the live System One
endpoint and records the raw outcome per attempt, so the stability of the
judge's signal on this population can be measured before campaign budget is
committed. This script never gates anything and never touches the repository
outside its own output directory.

Inputs (read-only):
  benchmark/results/2026-09-18-typesafe-repair-feedback/raw/*.json              (6)
  benchmark/results/2026-09-18-typesafe-repair-feedback/engagement-screen/raw/*.json (9)

Outputs (all under benchmark/results/2026-09-18-judge-shadow-replay/):
  raw/replay-<record>-<n>.json   one file per sample attempt (n = 1..N)
  replay-calls.jsonl             append-only call log (one line per attempt)
  replay-summary.json            replay-side summary (written by `replay`)
  analysis.json                  computed analysis (written by `analyze`)

The API key is read from the environment variable TYPESAFE_API_KEY at call
time and is never printed, logged, or written to disk anywhere. If the
variable is unset, `replay` refuses to start.

Subcommands:
  replay   (default)  re-send each record N times; resumable, skips existing
                      sample files unless --force is given.
  analyze             read the replay outputs plus the retained run artifacts
                      and write analysis.json; prints a digest to stdout.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time  # noqa: F401 (kept for future pacing knobs)
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FEEDBACK = ROOT / "benchmark" / "results" / "2026-09-18-typesafe-repair-feedback"
OUT = ROOT / "benchmark" / "results" / "2026-09-18-judge-shadow-replay"
RAW_OUT = OUT / "raw"
DEFAULT_ENDPOINT = "https://api.typesafe.ai/v1/systemone"
DEFAULT_MODEL = "jev-1.13.0"
TIMEOUT_S = 30.0            # hard timeout per call
MAX_ATTEMPTS = 2            # initial call + at most one retry, transport errors only
RETRY_BACKOFF_S = 0.5
DEFAULT_N = 4
REPLAY_RE = re.compile(r"^replay-(judge-\d{8}T\d{6}Z-\d{4})-(\d+)\.json$")
KEY_ENV = "TYPESAFE_API_KEY"


# --------------------------------------------------------------------------- io

def now_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def read_json(path: Path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def rel(path: Path) -> str:
    return str(Path(path).resolve().relative_to(ROOT)).replace("\\", "/")


def percentile(values, q: float):
    xs = sorted(values)
    if not xs:
        return None
    if len(xs) == 1:
        return float(xs[0])
    k = (len(xs) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(xs) - 1)
    return float(xs[lo] + (xs[hi] - xs[lo]) * (k - lo))


# ------------------------------------------------------------------- record set

def input_records():
    """All 15 retained feedback records, in timestamp order."""
    found = []
    for directory in (FEEDBACK / "raw", FEEDBACK / "engagement-screen" / "raw"):
        found.extend(sorted(directory.glob("*.json")))
    return found


def flat_from_answers(answers: dict) -> dict:
    """Normalize the nested response.answers shape into a flat dict."""
    answers = answers or {}
    ff = answers.get("failure_family") or {}
    eg = answers.get("evidence_gap") or {}
    rr = answers.get("repair_readiness") or {}
    na = answers.get("next_action") or {}
    return {
        "failure_family": ff.get("choice"),
        "failure_confidence": ff.get("confidence"),
        "evidence_gap": eg.get("noul"),
        "repair_readiness": rr.get("score"),
        "repair_readiness_confidence": rr.get("confidence"),
        "next_action": na.get("choice"),
        "next_action_confidence": na.get("confidence"),
    }


def flat_tuple(flat: dict) -> tuple:
    return (
        flat.get("failure_family"), flat.get("failure_confidence"),
        flat.get("evidence_gap"), flat.get("repair_readiness"),
        flat.get("repair_readiness_confidence"), flat.get("next_action"),
        flat.get("next_action_confidence"),
    )


def flat_from_event(data: dict) -> dict:
    """Normalize a shipped judge_feedback event's flat data shape."""
    return {
        "failure_family": data.get("failure_family"),
        "failure_confidence": data.get("failure_confidence"),
        "evidence_gap": data.get("evidence_gap"),
        "repair_readiness": data.get("repair_readiness"),
        "repair_readiness_confidence": data.get("repair_readiness_confidence"),
        "next_action": data.get("next_action"),
        "next_action_confidence": data.get("next_action_confidence"),
    }


def canon(t: tuple) -> tuple:
    """Canonicalize a flat tuple for exact matching (round floats to 6dp)."""
    out = []
    for v in t:
        out.append(round(v, 6) if isinstance(v, float) else v)
    return tuple(out)


# ------------------------------------------------------------------------ http

class TransportError(Exception):
    pass


def post_once(url: str, body: bytes, key: str):
    """One HTTP attempt. Returns (status, raw_bytes, latency_ms, headers)."""
    request = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            raw = response.read()
            latency_ms = (time.perf_counter() - t0) * 1000.0
            return response.status, raw, latency_ms, dict(response.headers)
    except urllib.error.HTTPError as exc:  # a real HTTP response, not a transport failure
        raw = exc.read()
        latency_ms = (time.perf_counter() - t0) * 1000.0
        return exc.code, raw, latency_ms, dict(exc.headers)
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as exc:
        raise TransportError(f"{type(exc).__name__}: {exc}") from exc


def call_with_retry(url: str, body: bytes):
    """At most MAX_ATTEMPTS attempts, transport errors only. Key read at call time."""
    attempts = []
    for attempt in range(1, MAX_ATTEMPTS + 1):
        key = os.environ.get(KEY_ENV)
        if not key:
            raise SystemExit(f"{KEY_ENV} is unset at call time; refusing to call.")
        try:
            status, raw, latency_ms, headers = post_once(url, body, key)
            attempts.append({"attempt": attempt, "http_status": status,
                             "latency_ms": round(latency_ms, 1), "transport_error": None})
            return {"status": status, "raw": raw, "latency_ms": latency_ms,
                    "headers": headers}, attempts
        except TransportError as exc:
            attempts.append({"attempt": attempt, "http_status": None,
                             "latency_ms": None, "transport_error": str(exc)})
            if attempt < MAX_ATTEMPTS:
                time.sleep(RETRY_BACKOFF_S)
    return None, attempts


# ---------------------------------------------------------------------- replay

def cmd_replay(args) -> int:
    if not os.environ.get(KEY_ENV):
        print(f"{KEY_ENV}: unset")
        print("Replay requires the key; stopping without any call.")
        return 2
    print(f"{KEY_ENV}: set")

    records = input_records()
    if args.records:
        records = [p for p in records if args.records in p.name]
    if not records:
        print("No input records matched; nothing to do.")
        return 2

    total_planned = len(records) * args.n
    print(f"records={len(records)} n={args.n} planned_calls={total_planned} "
          f"timeout_s={TIMEOUT_S} retry=1(transport) out={rel(RAW_OUT)}")
    if args.dry_run:
        for path in records:
            record = read_json(path)
            endpoint = record.get("endpoint") or DEFAULT_ENDPOINT
            model = (record.get("request") or {}).get("model", DEFAULT_MODEL)
            print(f"  would replay {path.stem} x{args.n} -> {endpoint} model={model}")
        print("dry-run: no calls made.")
        return 0

    RAW_OUT.mkdir(parents=True, exist_ok=True)
    log_path = OUT / "replay-calls.jsonl"
    log = log_path.open("a", encoding="utf-8")

    made = skipped = errors = 0
    first_call_done = False
    status_counts = {}
    latencies = []
    input_tokens = output_tokens = 0

    try:
        for path in records:
            record = read_json(path)
            stem = path.stem
            endpoint = record.get("endpoint") or DEFAULT_ENDPOINT
            payload = record.get("request")
            if not isinstance(payload, dict) or not payload:
                print(f"  {stem}: no request payload; skipped")
                skipped += args.n
                continue
            model = payload.get("model", DEFAULT_MODEL)
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            body_sha = hashlib.sha256(body).hexdigest()

            for sample in range(1, args.n + 1):
                target = RAW_OUT / f"replay-{stem}-{sample}.json"
                if target.exists() and not args.force:
                    skipped += 1
                    continue

                result, attempts = call_with_retry(endpoint, body)
                utc = now_utc()
                entry = {
                    "schema_version": 1,
                    "kind": "judge-shadow-replay",
                    "record": stem,
                    "source_record": rel(path),
                    "sample": sample,
                    "utc": utc,
                    "endpoint": endpoint,
                    "model_requested": model,
                    "request_sha256": body_sha,
                    "http_status": None,
                    "latency_ms": None,
                    "error": "",
                    "attempts": attempts,
                    "request_id": "",
                    "response_date": "",
                    "response": None,
                    "usage": None,
                    "response_sha256": "",
                }

                if result is None:
                    entry["error"] = "; ".join(a["transport_error"] for a in attempts
                                               if a.get("transport_error")) or "transport failure"
                    errors += 1
                    print(f"  {stem} n={sample}: TRANSPORT-ERROR {entry['error'][:120]}")
                else:
                    raw = result["raw"]
                    entry["http_status"] = result["status"]
                    entry["latency_ms"] = round(result["latency_ms"], 1)
                    entry["response_sha256"] = hashlib.sha256(raw).hexdigest()
                    entry["request_id"] = result["headers"].get("x-typesafe-request-id", "")
                    entry["response_date"] = result["headers"].get("date", "")
                    try:
                        entry["response"] = json.loads(raw.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError):
                        entry["response"] = None
                        entry["error"] = f"unparseable response body ({len(raw)} bytes)"
                    if isinstance(entry["response"], dict):
                        entry["usage"] = entry["response"].get("usage")
                    status_counts[result["status"]] = status_counts.get(result["status"], 0) + 1
                    latencies.append(result["latency_ms"])
                    usage = entry.get("usage") or {}
                    input_tokens += int(usage.get("input_tokens") or 0)
                    output_tokens += int(usage.get("output_tokens") or 0)
                    if result["status"] != 200:
                        errors += 1
                    print(f"  {stem} n={sample}: status={result['status']} "
                          f"latency={entry['latency_ms']}ms req_id={entry['request_id'] or '-'}")
                    if not first_call_done:
                        keys = sorted(entry["response"]) if isinstance(entry["response"], dict) else []
                        print(f"    first response top-level keys: {keys}")
                        if result["status"] in (401, 403):
                            print(f"    auth failure ({result['status']}); aborting replay.")
                            log.close()
                            return 3
                first_call_done = True
                made += 1
                log.write(json.dumps({
                    "utc": utc, "record": stem, "sample": sample,
                    "http_status": entry["http_status"], "latency_ms": entry["latency_ms"],
                    "error": entry["error"], "request_id": entry["request_id"],
                }) + "\n")
                log.flush()
                write_json(target, entry)
    finally:
        log.close()

    summary = {
        "schema_version": 1,
        "generated_utc": now_utc(),
        "records": len(records),
        "n_per_record": args.n,
        "calls_made": made,
        "skipped_existing": skipped,
        "errors": errors,
        "status_counts": {str(k): v for k, v in sorted(status_counts.items())},
        "latency_ms": {
            "p50": round(percentile(latencies, 0.50), 1) if latencies else None,
            "p90": round(percentile(latencies, 0.90), 1) if latencies else None,
            "max": round(max(latencies), 1) if latencies else None,
        },
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
    }
    write_json(OUT / "replay-summary.json", summary)
    print(json.dumps(summary, indent=2))
    return 0 if errors == 0 else 1


# --------------------------------------------------------------------- analyze

def run_artifacts():
    """Per-run outcome + shipped judge_feedback events, for record->run mapping."""
    runs = {}
    for result_path in sorted(FEEDBACK.glob("*/*/result.json")):
        run_dir = result_path.parent
        result = read_json(result_path)
        metrics = result.get("metrics") or {}
        events_path = run_dir / "session" / "events.jsonl"
        judge_events = []
        if events_path.exists():
            for line in events_path.read_text(encoding="utf-8").splitlines():
                if not line.strip():
                    continue
                event = json.loads(line)
                if event.get("type") == "judge_feedback":
                    judge_events.append({
                        "elapsed_ms": event.get("elapsed_ms"),
                        "flat": flat_from_event(event.get("data") or {}),
                    })
        runs[rel(run_dir)] = {
            "passed": bool(result.get("passed")),
            "turns": metrics.get("turns"),
            "status": metrics.get("status"),
            "judge_events": judge_events,
        }
    return runs


def cmd_analyze(args) -> int:
    replays = {}   # stem -> {sample: entry}
    for path in sorted(RAW_OUT.glob("replay-*.json")):
        match = REPLAY_RE.match(path.name)
        if not match:
            continue
        stem, sample = match.group(1), int(match.group(2))
        replays.setdefault(stem, {})[sample] = read_json(path)

    shipped_paths = {path.stem: path for path in input_records()}
    shipped = {stem: read_json(path) for stem, path in shipped_paths.items()}
    runs = run_artifacts()

    # ---- record -> run mapping (exact flat-tuple match against shipped events)
    event_index = []
    for run_rel, info in runs.items():
        for i, event in enumerate(info["judge_events"], 1):
            event_index.append({"run": run_rel, "event_index": i,
                                "elapsed_ms": event["elapsed_ms"],
                                "tuple": canon(flat_tuple(event["flat"]))})
    mapping = {}
    used_events = set()
    problems = []
    for stem, record in sorted(shipped.items()):
        shipped_tuple = canon(flat_tuple(flat_from_answers(
            (record.get("response") or {}).get("answers") or {})))
        matches = [e for e in event_index if e["tuple"] == shipped_tuple]
        if len(matches) == 1:
            mapping[stem] = matches[0]
            used_events.add((matches[0]["run"], matches[0]["event_index"]))
        else:
            problems.append(f"{stem}: {len(matches)} event matches")
    duplicate_events = len(event_index) - len(used_events)
    if duplicate_events:
        problems.append(f"{duplicate_events} shipped judge events matched no record")

    # ---- per-episode sample statistics
    episodes = []
    all_flat = []            # every replayed sample (flattened)
    for stem, samples in sorted(replays.items()):
        record = shipped.get(stem, {})
        answers_list = []
        for sample in sorted(samples):
            entry = samples[sample]
            response = entry.get("response")
            answers = (response or {}).get("answers") if isinstance(response, dict) else None
            answers_list.append(answers)
        flats = [flat_from_answers(a) for a in answers_list]
        all_flat.extend(flats)

        choices_ff = sorted({f["failure_family"] for f in flats if f["failure_family"] is not None})
        choices_na = sorted({f["next_action"] for f in flats if f["next_action"] is not None})
        exact_equal = len({json.dumps(a, sort_keys=True) for a in answers_list}) == 1 and answers_list[0] is not None

        def spread(field):
            vals = [f[field] for f in flats if isinstance(f[field], (int, float))]
            return round(max(vals) - min(vals), 6) if len(vals) >= 2 else None

        if exact_equal:
            determinism = "deterministic"
        elif len(choices_ff) <= 1 and len(choices_na) <= 1:
            determinism = "mixed"
        else:
            determinism = "stochastic"

        matched = mapping.get(stem)
        run_rel = matched["run"] if matched else None
        run_info = runs.get(run_rel, {}) if run_rel else {}
        task = run_rel.split("/")[-1].split("-loop-repair")[0] if run_rel else None
        shipped_flat = flat_from_answers((record.get("response") or {}).get("answers") or {})
        episodes.append({
            "record": stem,
            "source_record": rel(shipped_paths[stem]) if stem in shipped_paths else None,
            "run": run_rel,
            "run_event_index": matched["event_index"] if matched else None,
            "run_event_elapsed_ms": matched["elapsed_ms"] if matched else None,
            "task": task,
            "run_passed": run_info.get("passed"),
            "run_turns": run_info.get("turns"),
            "run_status": run_info.get("status"),
            "samples": len(samples),
            "determinism": determinism,
            "exact_equal": exact_equal,
            "failure_family_choices": choices_ff,
            "next_action_choices": choices_na,
            "shipped": flat_tuple(shipped_flat),
            "replayed": [flat_tuple(f) for f in flats],
            "spread": {f: spread(f) for f in
                       ("failure_confidence", "evidence_gap", "repair_readiness",
                        "repair_readiness_confidence", "next_action_confidence")},
        })

    # ---- distributions (all replayed samples)
    def dist(field):
        vals = [f[field] for f in all_flat if isinstance(f[field], (int, float))]
        if not vals:
            return None
        return {"n": len(vals), "min": round(min(vals), 4), "median": round(percentile(vals, 0.5), 4),
                "mean": round(sum(vals) / len(vals), 4), "max": round(max(vals), 4)}

    distributions = {f: dist(f) for f in
                     ("failure_confidence", "evidence_gap", "repair_readiness",
                      "repair_readiness_confidence", "next_action_confidence")}

    # ---- conservative consumption rule
    def consumption(flat):
        fc, nac, rrc, eg = (flat.get("failure_confidence"), flat.get("next_action_confidence"),
                            flat.get("repair_readiness_confidence"), flat.get("evidence_gap"))
        uncertain = any(isinstance(v, (int, float)) and v < t
                        for v, t in ((nac, 0.55), (fc, 0.45), (rrc, 0.45)))
        inspect_adds = isinstance(eg, (int, float)) and eg >= 0.60
        return uncertain, inspect_adds

    rule = {"per_episode": {}, "samples_uncertain": 0, "samples_inspect_adds": 0,
            "samples": 0, "episodes_stable_uncertain": 0, "episodes_stable_inspect": 0}
    for stem, samples in sorted(replays.items()):
        flags = []
        for sample in sorted(samples):
            response = samples[sample].get("response")
            answers = (response or {}).get("answers") if isinstance(response, dict) else None
            flags.append(consumption(flat_from_answers(answers)))
        rule["per_episode"][stem] = {
            "uncertain": [int(f[0]) for f in flags],
            "inspect_adds": [int(f[1]) for f in flags],
        }
        rule["samples"] += len(flags)
        rule["samples_uncertain"] += sum(1 for f in flags if f[0])
        rule["samples_inspect_adds"] += sum(1 for f in flags if f[1])
        if len({f[0] for f in flags}) == 1:
            rule["episodes_stable_uncertain"] += 1
        if len({f[1] for f in flags}) == 1:
            rule["episodes_stable_inspect"] += 1

    # ---- latency / errors / tokens over the replayed live path
    latencies, errors, statuses = [], [], {}
    input_tokens = output_tokens = 0
    models = set()
    for samples in replays.values():
        for entry in samples.values():
            if entry.get("http_status") == 200 and entry.get("latency_ms") is not None:
                latencies.append(entry["latency_ms"])
            if entry.get("error") or entry.get("http_status") != 200:
                errors.append({"record": entry.get("record"), "sample": entry.get("sample"),
                               "http_status": entry.get("http_status"),
                               "error": entry.get("error")})
            statuses[str(entry.get("http_status"))] = statuses.get(str(entry.get("http_status")), 0) + 1
            usage = entry.get("usage") or {}
            input_tokens += int(usage.get("input_tokens") or 0)
            output_tokens += int(usage.get("output_tokens") or 0)
            response = entry.get("response")
            if isinstance(response, dict) and response.get("model"):
                models.add(response["model"])
    n_calls = sum(len(s) for s in replays.values())
    latency = {
        "n": len(latencies),
        "p50": round(percentile(latencies, 0.50), 1) if latencies else None,
        "p90": round(percentile(latencies, 0.90), 1) if latencies else None,
        "max": round(max(latencies), 1) if latencies else None,
        "min": round(min(latencies), 1) if latencies else None,
        "mean": round(sum(latencies) / len(latencies), 1) if latencies else None,
    }
    shipped_latencies = [r.get("latency_ms") for r in shipped.values()
                         if isinstance(r.get("latency_ms"), (int, float))]

    # ---- calibration vs run outcome
    FIELDS = ("failure_family", "failure_confidence", "evidence_gap", "repair_readiness",
              "repair_readiness_confidence", "next_action", "next_action_confidence")

    def group():
        out = {}
        for episode in episodes:
            if episode["run_passed"] is None:
                key = "unmapped"
            else:
                key = "passed" if episode["run_passed"] else "cap_death"
            out.setdefault(key, {"records": 0, "samples": [], "shipped": []})
            out[key]["records"] += 1
            for flat in episode["replayed"]:
                out[key]["samples"].append(dict(zip(FIELDS, flat)))
            out[key]["shipped"].append(dict(zip(FIELDS, episode["shipped"])))
        return out

    grouped = group()
    calibration = {}
    for key, info in grouped.items():
        rows = info["samples"]
        calibration[key] = {
            "records": info["records"],
            "samples": len(rows),
            "repair_readiness": dist_of(rows, "repair_readiness"),
            "repair_readiness_confidence": dist_of(rows, "repair_readiness_confidence"),
            "evidence_gap": dist_of(rows, "evidence_gap"),
            "next_action_confidence": dist_of(rows, "next_action_confidence"),
            "failure_confidence": dist_of(rows, "failure_confidence"),
            "failure_family_counts": counts(rows, "failure_family"),
            "next_action_counts": counts(rows, "next_action"),
            "shipped_repair_readiness": dist_of(info["shipped"], "repair_readiness"),
            "shipped_next_action_counts": counts(info["shipped"], "next_action"),
        }

    analysis = {
        "schema_version": 1,
        "generated_utc": now_utc(),
        "n_records": len(replays),
        "samples_per_record": sorted({len(s) for s in replays.values()}),
        "n_calls": n_calls,
        "n_ok": statuses.get("200", 0),
        "errors": errors,
        "status_counts": statuses,
        "models_returned": sorted(models),
        "latency_ms": latency,
        "shipped_latency_ms": {
            "n": len(shipped_latencies),
            "min": min(shipped_latencies) if shipped_latencies else None,
            "max": max(shipped_latencies) if shipped_latencies else None,
            "median": percentile(shipped_latencies, 0.5) if shipped_latencies else None,
        },
        "tokens": {"input": input_tokens, "output": output_tokens,
                   "estimated_cost_usd_input": round(input_tokens * 0.042 / 1_000_000, 6)},
        "determinism": {
            "per_episode": {e["record"]: e["determinism"] for e in episodes},
            "n_deterministic": sum(1 for e in episodes if e["determinism"] == "deterministic"),
            "n_mixed": sum(1 for e in episodes if e["determinism"] == "mixed"),
            "n_stochastic": sum(1 for e in episodes if e["determinism"] == "stochastic"),
        },
        "distributions": distributions,
        "consumption_rule": rule,
        "mapping_problems": problems,
        "episodes": episodes,
        "calibration": calibration,
    }
    write_json(OUT / "analysis.json", analysis)

    # ---- stdout digest
    print(f"records={analysis['n_records']} calls={n_calls} ok={analysis['n_ok']} "
          f"errors={len(errors)} models={analysis['models_returned']}")
    print(f"latency p50={latency['p50']} p90={latency['p90']} max={latency['max']} "
          f"(shipped median={analysis['shipped_latency_ms']['median']})")
    print(f"determinism: {analysis['determinism']['n_deterministic']} deterministic / "
          f"{analysis['determinism']['n_mixed']} mixed / {analysis['determinism']['n_stochastic']} stochastic")
    for episode in episodes:
        print(f"  {episode['record']}: {episode['determinism']:13s} "
              f"ff={episode['failure_family_choices']} na={episode['next_action_choices']} "
              f"rr_spread={episode['spread']['repair_readiness']} "
              f"run={episode['task']} passed={episode['run_passed']}")
    print("distributions (replayed samples):")
    for field, d in distributions.items():
        print(f"  {field:32s} {d}")
    print(f"consumption rule: uncertain {rule['samples_uncertain']}/{rule['samples']} samples, "
          f"stable per episode in {rule['episodes_stable_uncertain']}/{analysis['n_records']}; "
          f"inspect_adds {rule['samples_inspect_adds']}/{rule['samples']} samples, "
          f"stable in {rule['episodes_stable_inspect']}/{analysis['n_records']}")
    for key, c in calibration.items():
        print(f"calibration[{key}]: records={c['records']} samples={c['samples']} "
              f"rr={c['repair_readiness']} rrc={c['repair_readiness_confidence']}")
        print(f"   next_action={c['next_action_counts']} failure_family={c['failure_family_counts']}")
    if problems:
        print("MAPPING PROBLEMS:", problems)
    return 0


def dist_of(rows, field):
    vals = [r.get(field) for r in rows if isinstance(r.get(field), (int, float))]
    if not vals:
        return None
    return {"n": len(vals), "min": round(min(vals), 4), "median": round(percentile(vals, 0.5), 4),
            "mean": round(sum(vals) / len(vals), 4), "max": round(max(vals), 4)}


def counts(rows, field):
    out = {}
    for row in rows:
        out[str(row.get(field))] = out.get(str(row.get(field)), 0) + 1
    return out


# ------------------------------------------------------------------------ main

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command")
    replay = sub.add_parser("replay", help="re-send every retained request N times")
    replay.add_argument("--n", type=int, default=DEFAULT_N)
    replay.add_argument("--records", default=None,
                        help="substring filter on record file names")
    replay.add_argument("--force", action="store_true",
                        help="re-run samples whose output file already exists")
    replay.add_argument("--dry-run", action="store_true",
                        help="print the plan without making calls")
    replay.set_defaults(func=cmd_replay)

    analyze = sub.add_parser("analyze", help="analyze replay outputs into analysis.json")
    analyze.set_defaults(func=cmd_analyze)

    args = parser.parse_args(argv)
    if not args.command:
        args = parser.parse_args(["replay"])
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
