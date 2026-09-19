#!/usr/bin/env bash
# Interleaved A/B decode+prefill benchmark for the NVIDIA llama.cpp question.
# Frozen schedule: A,B x3. No re-runs for a better repetition.
set -u
MODEL="C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf"
OUT="C:/Users/flowc/dev/forge/benchmark/results/2026-09-13-nvidia-llamacpp-support"
A="C:/Users/flowc/dev/forge/.tools/llama-cuda/llama-bench.exe"
B="C:/Users/flowc/AppData/Local/Temp/forge-llama-b10948/B/llama-bench.exe"
i=0
for rep in 1 2 3; do
  for arm in A B; do
    i=$((i+1))
    if [ "$arm" = "A" ]; then exe="$A"; else exe="$B"; fi
    printf '=== cell %d | arm %s | repetition %d | %s | exe=%s ===\n' \
      "$i" "$arm" "$rep" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$exe" >> "$OUT/schedule.log"
    "$exe" -m "$MODEL" -p 512 -n 128 -ngl 99 -r 3 -o md \
      > "$OUT/arm${arm}-rep${rep}.md" 2> "$OUT/arm${arm}-rep${rep}.stderr"
    printf 'cell %d exit=%d bytes=%d\n' "$i" "$?" "$(stat -c %s "$OUT/arm${arm}-rep${rep}.md")" >> "$OUT/schedule.log"
  done
done
printf 'DONE %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$OUT/schedule.log"
