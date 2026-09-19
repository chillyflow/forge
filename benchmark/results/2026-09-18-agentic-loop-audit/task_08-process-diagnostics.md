# task_08-process-diagnostics

Read BRIEF.md first (same directory). Domain: 08-process-diagnostics.

## Scope files
- src/core/process.c (558)
- src/tools/diagnostics.c (1793)

## Questions (answer with located claims)
1. process.c: spawn path cost per command (CreateProcess/fork+exec, job objects/process groups), output drain loop (polling? busy wait? blocking reads), byte caps, timeout checks, environment setup; kill/cleanup cost; per-poll allocation.
2. Spawn frequency: which loop stages spawn processes (validation stages, run_command) and how many per task.
3. diagnostics.c: normalization cost per validation failure (stream parsing, scan patterns, hashing -- fg_diagnostic_hash, fg_compress_output); complexity over output size; repeated work across turns on the same failure.
4. Dedup/repeat-detection inputs (diagnostic signatures): cost of computing them.
5. Adapter parsing (Go test/vet/lint, GCC JSON, Cargo, pytest): per-parse cost, any O(n^2) scans over captured output.
6. Output compression (fg_compress_output): what it saves (visible_tool_bytes) vs what it costs; measured?
7. Bounds and truncation behaviors' cost.

## Output
- Write <RUN>/attempt_08-process-diagnostics.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
