# task_05-agent-loop-core

Read BRIEF.md first (same directory). Domain: 05-agent-loop-core.

## Scope files
- src/core/agent.c (3878 -- read in passes; prioritize minimal_run, repair/candidate_validate, budget accounting, render path, event writing)
- src/core/working_state.c (715)

## Questions (answer with located claims)
1. Turn loop: enumerate host work between generations; anything repeated per turn that could be incremental (rebuilds, rescans, re-serialization).
2. Repair cycle: candidate_validate + bounded repair + reserved turns -- what work repeats per failed episode; the reserved validation/final arithmetic (double work when a reserved final is rejected?); reflection action cost.
3. Repeat/no-op detection: exact cost per turn (workspace hashing? diagnostic normalization? full scans?) -- cost only; presentation changes are a closed axis.
4. Budget accounting: cumulative input tokens; verify a discarded generation is counted once; any place the same tokens are counted or re-sent twice.
5. Render path: what is rebuilt per turn vs cached; verify the identical-prompts fix (commit 9592088f) still holds; name any volatile text.
6. working_state.c: per-turn serialization cost/size; growth behavior.
7. Events/session writes per turn: flush/fsync policy, JSON building cost (cite what agent.c does).
8. minimal_run vs forge_agent_run: duplicated logic and divergence risk (repair profile enters minimal_run).
9. Disabled-by-default machinery (candidates, semantic loops, reflection, symbol impact): cost when off (should be ~0 -- verify no hidden work).

## Output
- Write <RUN>/attempt_05-agent-loop-core.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
