"""Diagnose the completed paraphrased r001 only, without model or fixture writes."""
import hashlib
import importlib.util
import json
import marshal
from pathlib import Path
import runpy
import sys

HERE = Path(__file__).resolve().parent
helper_path = HERE / "compare-completed-g1.py"
helper = runpy.run_path(str(helper_path))
run = helper["analyze"]("generalize_retractions_paraphrased", "service", "balances")
root = helper["ROOT"]
read = helper["read"]
read(helper_path)
identity_helper_path = HERE / "analyze-renamed-r001.py"
identity_helper = runpy.run_path(str(identity_helper_path))
read(identity_helper_path)
code_identity = identity_helper["code_identity"]
harness = HERE.parent / "runs/development-G1-loop-pilot-generalize_retractions_paraphrased-s42-r001/harness/generalize_retractions_paraphrased-loop-repair-r001"
cache = []
for module in ("service", "test_service"):
    source = read(harness / f"terminal-workspace/{module}.py")
    path = harness / f"failed-workspace/__pycache__/{module}.cpython-311.pyc"
    data = read(path)
    assert data[:4] == importlib.util.MAGIC_NUMBER, "Use Python 3.11"
    cache.append({"path": path.relative_to(root).as_posix(),
                  "executable_structure_equals_current_source": code_identity(marshal.loads(data[16:])) == code_identity(compile(source, "retained-source", "exec", dont_inherit=True))})
assert all(x["executable_structure_equals_current_source"] for x in cache)
run["cache_probe"] = {"python": sys.version, "records": cache,
                      "limit": "Retained terminal pyc matches fresh source executable structure; no claim about every earlier import"}
run["terminal_reason"] = read(harness / "stderr.txt").decode(errors="replace").splitlines()[-1]
run["source_sha256"] = hashlib.sha256(read(harness / "terminal-workspace/service.py")).hexdigest()
run["input_sha256"] = helper["READS"]
run["all_read_inputs_unchanged"] = all(hashlib.sha256((root / p).read_bytes()).hexdigest() == h for p, h in helper["READS"].items())
assert run["all_read_inputs_unchanged"]
assert not any(e["after_tests"]["passed"] for e in run["edits"] if "after_tests" in e)
(HERE / "paraphrased-r001-analysis.json").write_text(json.dumps(run, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"inputs": len(helper["READS"]), "unchanged": True,
                  "edits": [(x["action"], x["ast_unchanged"], x.get("after_tests", {}).get("cases")) for x in run["edits"]],
                  "cache_equal": True, "verification": run["verification"]["passed"]}))
