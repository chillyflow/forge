# task_12-judge-client

Read BRIEF.md first (same directory). Domain: 12-judge-client.

## Scope files
- src/judge/judge.c (850)
- include/forge/judge.h (123)
- call sites: search for forge_judge_ in src/ and benchmark/

## Questions (answer with located claims)
1. Request construction cost: JSON building (yyjson? manual?), allocation churn, body size; response parse cost; per-call work.
2. The two seams: retrieval rerank (forge_judge_rerank_retrieval) and repair feedback (forge_judge_feedback) -- where called, per-invocation cost, whether latency is budgeted outside host deadlines (commit 5520c33a fixed rerank; verify the repair path), retry/backoff math (forge_judge_budget_ms).
3. Fail-open paths: cost when the service is down or the key is absent (should be ~0; verify).
4. Telemetry/raw-record IO: record_dir writes per call (size, frequency).
5. Batching opportunities: could either seam batch more per call (fewer round trips); input token counts (cost) and whether payloads are minimal.
6. Caching: any dedup of identical requests (the research doc proposes exact-input caching; verify current state).
7. Windows-only transport: what non-Windows builds lose and whether any perf-relevant path is affected.
8. JEV section: enumerate EXTENSIONS of the two existing arms and any missing wiring (judge feedback only in bounded repair? rerank only in retrieval tool?).

## Output
- Write <RUN>/attempt_12-judge-client.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
