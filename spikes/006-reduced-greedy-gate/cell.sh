#!/usr/bin/env bash
# One A/B cell for the reduced-candidate greedy sampling gate.
#
#   cell.sh <label> <temperature> <forge.exe>
#
# Resets the fixture workspace to a byte-identical state, runs the repair task in
# the arm configuration, and copies the session metrics and the run log into this
# directory under <label>. The workspace path is fixed so the prompt is identical
# across cells; only the binary changes.
set -euo pipefail

label="$1"
temperature="$2"
forge="$3"
extra="${4:-}"
task="${5:-Inspect calc.py, repair the implementation so the tests pass, and run the tests.}"
here="$(cd "$(dirname "$0")" && pwd)"
ws="C:/Users/flowc/AppData/Local/Temp/forge-ab6-ws"
model="C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf"

rm -rf "$ws"
mkdir -p "$ws"
cat > "$ws/calc.py" <<'EOF'
def add(a, b):
    return a - b

def total(values):
    result = 0
    for v in values:
        result = add(result, v)
    return result
EOF
cat > "$ws/test_calc.py" <<'EOF'
import unittest
from calc import add, total

class TestCalc(unittest.TestCase):
    def test_add(self):
        self.assertEqual(add(2, 3), 5)

    def test_total(self):
        self.assertEqual(total([1, 2, 3]), 6)

if __name__ == "__main__":
    unittest.main()
EOF

"$forge" run "$task" \
    --workspace "$ws" --model "$model" --gpu-layers -1 \
    --context 16384 --temperature "$temperature" --seed 42 \
    --allow-write --allow-exec --max-turns 6 --max-tokens 8192 --wall-ms 600000 \
    $extra \
    > "$here/$label.run.txt" 2>&1 || true

session="$(ls "$ws/.forge/sessions" | head -1)"
cp "$ws/.forge/sessions/$session/metrics.json" "$here/$label.metrics.json"
printf '%-12s gen=%-5s decode_ms=%-8s sampling_ms=%-8s prefill_ms=%-8s fallback=%-5s\n' \
    "$label" \
    "$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_tokens'])" "$here/$label.metrics.json")" \
    "$(python -c "import json,sys;print(int(json.load(open(sys.argv[1]))['decode_ms']))" "$here/$label.metrics.json")" \
    "$(python -c "import json,sys;print(int(json.load(open(sys.argv[1]))['sampling_ms']))" "$here/$label.metrics.json")" \
    "$(python -c "import json,sys;print(int(json.load(open(sys.argv[1]))['prefill_ms']))" "$here/$label.metrics.json")" \
    "$(python -c "import json,sys;print(json.load(open(sys.argv[1]))['grammar_fallback_tokens'])" "$here/$label.metrics.json")"
