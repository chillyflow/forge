# task_07-tools-edit

Read BRIEF.md first (same directory). Domain: 07-tools-edit.

## Scope files
- src/tools/tools.c (1937)
- src/tools/edit_journal.c (296)

## Questions (answer with located claims)
1. Action dispatch per turn: registry lookup, yyjson parse, schema validation cost; the GBNF grammar build from the registry -- built once or per generation? where cached?
2. apply_patch/apply_hunk per edit: SHA-256 of the full file per call, in-process Go syntax check (tree-sitter full parse?), staging write + atomic replace + old-content recheck -- enumerate each cost and whether it is avoidable/cacheable; cost on a 2 MiB file.
3. read_file: read + hash per call; any caching of recent reads; bounds/truncation cost.
4. edit_journal: per-edit write cost (fsync?), capacity checks; retention size.
5. Tool result compaction: output capping/truncation cost; diagnostics compression call site.
6. Attribute every cost as per-token, per-turn, per-edit, or per-run.
7. Tool description/schema rendering cost per prompt (per-state registries).

## Output
- Write <RUN>/attempt_07-tools-edit.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
