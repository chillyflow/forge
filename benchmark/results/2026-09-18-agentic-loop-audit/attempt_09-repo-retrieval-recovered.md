# attempt_09-repo-retrieval - RECOVERED PARTIAL DRAFT

<!-- provenance: recovered by the parent from the subagent's final summary (delegation deleg_5b98df66, task 8) after a global tool-budget gate blocked the unit's write; content = the unit's full final summary, verbatim (draft; follow-up unit to complete). No content altered. -->

STATUS=BLOCKED; claims=0; ok=0; jev=0; top3=(none)

**Blocker (environmental, not task-level):** every Hermes tool call is hard-gated by a global daily budget — `[BUDGET] Global is over its daily budget: spent $5.27 of $5.00 (105%). Tool calls are blocked until the daily window resets.` Verified across 11 attempts / 6 tool types (`read_file`, `search_files`, `terminal`, `execute_code`, `browser_exec`, `tool_describe`), all rejected identically; spend kept rising from other sessions, so the block will not clear on its own before the daily window resets. No write path exists for me: the deliverable **was not created**.

**What was completed before the block (usable for a re-run):**
- Read BRIEF.md and task_09-repo-retrieval.md in full; read all four scope files in full: `src/repo/repo.c` (1984 lines), `src/repo/retrieval.c` (709), `src/repo/graph.c` (858), `src/repo/impact.c` (723).
- Read key integration sites: `src/core/agent.c` (per-edit delta/full index + `fg_repo_note_change_until` at ~3533–3559; candidate-validation full index ~1150; init full index + impact snapshot ~1585/2292; monitor polls ~2471/2602/2685/2846), `src/repo/monitor.c` 150–349, `src/repo/validation.c` 960–1232 (`fg_go_graph_load` per plan at 1033; `fg_impact_analyze` at 1150), `src/core/candidate_search.c` 84, `src/core/verification.c` 472/644, `src/tools/tools.c` 1828, `src/cli/main.c` 551/596/1179–1220.
- Candidate findings drafted from actually-read lines (not yet written): per-scan double hashing (fnv + SHA-256 + UTF-8 validate on every unchanged file, repo.c:687–693); LITERAL stage `instr(chunks.content,?)` full-corpus scan (retrieval.c:664–667); digest-verify before duplicate rejection and duplicates consuming `source_bytes` budget (retrieval.c:160–165 vs 203); `fg_go_graph_load` rebuilt per retrieval query/plan/impact with no generation-keyed cache (retrieval.c:280, validation.c:1033, impact.c:549, summary.c:330); `render()` re-serializing the whole doc per trimmed row (retrieval.c:541–577); impact.c O(A·S) sweeps sharing a 4M step budget (impact.c:478–522, 597–617 → cap-death at ~244 sweeps at 16,384 symbols); per-row SQLite prepare churn (repo.c:350/374/384/429).
- Not yet read (needed for completion): `repo_internal.h`, `graph.h`, docs (INDEX/RETRIEVAL/WATCH), benchmark baselines for index_ms, docs/plans closed-axes sections.

**Required action:** re-run unit 09 after the daily budget resets or after `/budget set global daily <usd>` raises the limit; then I can finish the remaining reads, self-refute, and write `attempt_09-repo-retrieval.md`.
