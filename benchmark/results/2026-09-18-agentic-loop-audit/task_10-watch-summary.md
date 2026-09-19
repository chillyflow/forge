# task_10-watch-summary

Read BRIEF.md first (same directory). Domain: 10-watch-summary.

## Scope files
- src/repo/watch.c (1636)
- src/repo/summary.c (1360)

## Questions (answer with located claims)
1. watch.c: enrollment cost (recursive handles), per-turn drain/poll cost, event buffer handling, per-event allocation; full-scan fallback triggers and their cost; coordinator poll frequency; anything blocking the loop.
2. summary.c: when summaries are generated (per index change? per turn? on demand?), cost (tree walks, token counting?), caching keyed by generation; whether summary generation blocks the loop or repeats per prompt render.
3. The repository-map prompt segment: built from summary -- per-render cost vs cached; size accounting.
4. Any per-turn full-tree work in either file.
5. Windows-specific costs (ReadDirectoryChangesW buffering).

## Output
- Write <RUN>/attempt_10-watch-summary.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
