# task_09-repo-retrieval

Read BRIEF.md first (same directory). Domain: 09-repo-retrieval.

## Scope files
- src/repo/repo.c (1984)
- src/repo/retrieval.c (709)
- src/repo/graph.c (858)
- src/repo/impact.c (723)

## Questions (answer with located claims)
1. Index triggers: what calls indexing per turn (post-tool? every turn?), delta vs full, git subprocess cost (enumeration), SQLite transaction cost; index_ms attribution.
2. repo.c: full scans vs delta paths; hashing every candidate per scan (bounded?); the 2 MiB cap; unchanged-file fast path.
3. retrieval.c: query cost (FTS/BM25 scans), candidate counts, dedup, rerank hook budget handling, output trim; repeated queries per turn.
4. graph.c: import graph construction per validation? cached by generation? cost.
5. impact.c: snapshot capture/compare cost (up to 4096 files, hashing), per-edit cost; declared bounds vs actual work.
6. Any O(n^2) over files/symbols/occurrences; any full rescan where a delta exists.
7. Watch/notification integration points: generation bump cost.
8. Baselines: which retained artifacts measure index_ms / retrieval cost.

## Output
- Write <RUN>/attempt_09-repo-retrieval.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
