# task_11-runtime-config

Read BRIEF.md first (same directory). Domain: 11-runtime-config.

## Scope files
- src/cli/main.c (1404)
- src/core/config.c (781)
- src/core/hardware.c (627)
- src/core/session.c (216)
- src/core/memory.c (547)
- src/core/util.c (496)

## Questions (answer with located claims)
1. Startup: config parse, model load, index baseline, hardware probe -- enumerate one-time costs and any duplicated work (double scans, repeated git calls, redundant file reads).
2. hardware.c: the --gpu-layers auto planner -- current arithmetic; does it size context against measured free VRAM and KV type (P0.1 requirement)? What metadata is available at planning time? Cite exact code.
3. session.c: event log writes per turn -- flush policy (fsync per event? buffered?), metrics JSON build cost, JSON escaping cost.
4. memory.c: arena discipline -- which per-turn allocations bypass arenas; high-water growth; per-turn allocation of large buffers.
5. util.c: hot helpers -- JSON parsing helpers, string ops, hash wrappers; O(n^2) patterns (repeated concat, strstr loops).
6. main.c: run/bench/retrieve entry points -- per-run setup, arg parsing, duplicate initialization.
7. Config validation cost at startup.

## Output
- Write <RUN>/attempt_11-runtime-config.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
