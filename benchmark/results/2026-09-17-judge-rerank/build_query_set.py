#!/usr/bin/env python3
"""Build the frozen query set for the 2026-09-17 judge rerank screen.

Deterministic: identifier candidates are sorted by name before selection, and
concept queries are fixed here with a verification substring that must appear
on the target line. Writes query_set.json beside this script. Read-only against
the repository.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]

DEF_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_ \t\*]*\b([a-z][a-z0-9_]*)\s*\(")

CONCEPTS = [
    ("cq01", "where is the retrieval source digest verified", "src/repo/retrieval.c",
     "fg_sha256_hex(source, n, computed)"),
    ("cq02", "where does the context planner score candidate segments", "src/context/context.c",
     "view.priority + 1.0"),
    ("cq03", "where are tool calls dispatched after policy approval", "src/tools/tools.c",
     'if (!strcmp(name, "read_file"))'),
    ("cq04", "where is the pre-command input snapshot taken", "src/core/agent.c",
     "before_command = !strcmp(tool"),
    ("cq05", "where does the checkpoint cache decide invalidation", "src/inference/checkpoint_cache.c",
     "static bool set_scope(fg_checkpoint_cache"),
    ("cq06", "where is the GBNF grammar built from the tool registry", "src/tools/tools.c",
     "char *fg_tool_grammar(bool thought"),
    ("cq07", "where is the KV prefix anchor nominated", "src/context/context.c",
     "forge_status forge_context_cache_anchor(const forge_context"),
    ("cq08", "where does forge validate the config schema", "src/core/config.c",
     "forge_status forge_config_validate(const forge_config"),
    ("cq09", "where is the edit journal prepared before an edit lands", "src/tools/edit_journal.c",
     "bool fg_edit_prepare(fg_tool_context"),
    ("cq10", "where is the final answer validation gate", "src/core/agent.c",
     "if (validation_required && !a->config.skip_validation) {"),
]


def rel(path):
    return path.relative_to(ROOT).as_posix()


def scan_sources():
    files = []
    for base in (ROOT / "src", ROOT / "include"):
        for path in sorted(base.rglob("*")):
            if path.suffix in (".c", ".h") and path.is_file():
                files.append(path)
    return files


def definitions(files):
    found = {}
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if not line or line[0] in " \t#/":
                continue
            match = DEF_RE.match(line)
            if not match:
                continue
            brace = "{" in line
            following = ""
            for probe in range(index + 1, min(index + 4, len(lines))):
                if lines[probe].strip():
                    following = lines[probe].strip()
                    break
            if not brace and not following.startswith("{"):
                continue
            found.setdefault(match.group(1), []).append((rel(path), index + 1, line.strip()))
    return found


def tracked_file_count(name):
    result = subprocess.run(["git", "grep", "-l", "--fixed-strings", name],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise SystemExit(f"git grep failed for {name}: {result.stderr}")
    return len([line for line in result.stdout.splitlines() if line.strip()])


def main():
    defs = definitions(scan_sources())
    identifiers = []
    for name in sorted(defs):
        sites = defs[name]
        if len(sites) != 1:
            continue
        if not (name.startswith("fg_") or name.startswith("forge_")):
            continue
        if len(name) < 8:
            continue
        files = tracked_file_count(name)
        if files < 4 or files > 30:
            continue
        path, line, text = sites[0]
        identifiers.append({"id": "", "kind": "identifier", "query": name,
                            "target_path": path, "target_line": line, "target_text": text,
                            "occurrence_files": files})
        if len(identifiers) == 30:
            break
    for index, item in enumerate(identifiers, 1):
        item["id"] = f"id{index:02d}"
    queries = list(identifiers)
    failures = []
    for qid, query, path, needle in CONCEPTS:
        text = (ROOT / path).read_text(encoding="utf-8", errors="replace").splitlines()
        hits = [index + 1 for index, line in enumerate(text) if needle in line]
        if not hits:
            failures.append(f"{qid}: needle not found in {path}: {needle}")
            continue
        line = hits[0]
        content = text[line - 1].strip()
        queries.append({"id": qid, "kind": "concept", "query": query, "target_path": path,
                        "target_line": line, "target_text": content})
        print(f"{qid}: {path}:{line} (hits={len(hits)}): {content[:100]}")
    if failures:
        print("VERIFICATION FAILURES:")
        for failure in failures:
            print(" ", failure)
        return 1
    document = {
        "schema_version": 1,
        "note": "Frozen 2026-09-17 query set for the judge rerank screen. Identifier "
                "queries select unique column-0 function definitions whose names occur in "
                "4..30 tracked files; concept queries fix the target line and verify its "
                "content. Coverage rule: a result row covers the target when its path "
                "matches and its snippet contains the first 60 normalized characters of "
                "the target line.",
        "queries": queries,
    }
    out = HERE / "query_set.json"
    out.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out} with {len(queries)} queries "
          f"({len(identifiers)} identifier, {len(CONCEPTS)} concept)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
