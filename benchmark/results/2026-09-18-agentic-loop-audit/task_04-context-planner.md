# task_04-context-planner

Read BRIEF.md first (same directory). Domain: 04-context-planner.

## Scope files
- src/context/context.c (1254)
- src/core/conversation.c (326)
- src/core/semantic_state.c (511)

## Questions (answer with located claims)
1. Segment selection: algorithm and complexity per turn (sorting, scoring loops, repeated rescans); utility-per-token scoring and recency term; any O(n^2).
2. Budget fit: how final rendered size is checked (tokenizer calls per candidate); any fit loop that re-tokenizes repeatedly.
3. Invalidation: dependency closure cost per turn; conservative full invalidation on patches/commands -- what is walked and its cost; transitive dependent walks.
4. Snapshots: versioned logical snapshots -- what is copied per capture, when captures happen, cost.
5. Compaction: triggers, cost, and the measured 16.5%-of-one-run re-prefill under compaction pressure -- locate the mechanism in code that causes re-prefill (which segment text changes).
6. conversation.c: append/eviction cost (memmove?), byte accounting per append; retention limits.
7. semantic_state.c: per-turn serialization cost and size.
8. Stable prompt ordering (system, tools, map, task, history, working state): verify; flag any unstable element.

## Output
- Write <RUN>/attempt_04-context-planner.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
