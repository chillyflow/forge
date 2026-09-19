# attempt_04-context-planner - RECOVERED PARTIAL DRAFT

<!-- provenance: recovered by the parent from the subagent's final summary (delegation deleg_5b98df66, task 3) after a global tool-budget gate blocked the unit's write; content = the unit's full final summary, verbatim (draft; follow-up unit to complete). No content altered. -->

Tool access is blocked by the daily budget gate, so I cannot read further files or write the deliverable. Writing the report with the evidence I gathered from the four files I read in full plus located lines elsewhere.

STATUS=blocked; claims=0; ok=0; jev=0; top3=none

**What happened**
- Read `BRIEF.md` and `task_04-context-planner.md` in full, then read all three scope files end to end: `src/context/context.c` (1254), `src/core/conversation.c` (326), `src/core/semantic_state.c` (511). Also read located excerpts of `src/core/agent.c` (planning/compaction/invalidation call sites), `include/forge/context.h`, `src/inference/inference.c`, `src/repo/monitor.c`, `src/core/input_snapshot.c`, and `docs/plans/agent-performance-avenues.md`.
- Mid-audit, every tool channel (`read_file`, `search_files`, `terminal`) began returning `[BUDGET] Global is over its daily budget: spent $5.24 of $5.00 (105%)`. Tool calls are blocked until the window resets.

**Deliverable: NOT produced.**
- `C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5/attempt_04-context-planner.md` was never written — `write_file` is blocked by the same gate. This is a hard failure of the task, reported as a failure, not papered over.
- No claim, VERIFIED-OK, or JEV line was filed, because filing any of them requires either a line citation I had already read or a write I cannot perform, and the output schema is only meaningful inside the deliverable file.

**Substantive findings I had already read, for the parent to reuse (all line-cited from files read in full or in located excerpts):**
- `context.c:690-806` — the priority planner is a selection-sort loop: `for (pass = 0; pass < count; pass++)` with an inner scan over all segments, i.e. O(n²) scoring passes per plan, each pass recomputing `(priority+1)*(1+i/(count+1))/tokens` for every unconsidered segment.
- `context.c:740-742` — that score has a recency term `(1.0 + i/(count+1))`, max ~2.0×, while the token-cost divisor is unbounded, so a long old segment can outrank a short recent one; the recency term is not the tie-breaker the docs-style ordering implies.
- `context.c:761-785` — the fit loop re-renders the *entire* selected prompt and re-tokenizes it (`c->count_prompt_tokens(out, ...)`) once per undo of an admitted bundle: up to `bundles+1` full-prompt tokenizations in the worst case.
- `context.c:893-923` — `plan_bounded_inner` re-renders and re-tokenizes the whole prompt *per admitted segment* inside the backwards walk, abandoning on the first overflow.
- `context.c:78-87` `invalidate_dependents` — O(n × parents) walk from the mutated index forward; `context.c:298-315` `forge_context_invalidate` walks all segments and then calls `propagate_stale` (`:66-77`), another full scan. Called at `agent.c:462`, `agent.c:548`, `agent.c:2727`, `agent.c:3586` — and every call passes `dependency = 0`, i.e. the conservative full-invalidation mode. `forge_context_bind_source` is called only at `agent.c:2421`, `agent.c:3746`, `agent.c:3750`, so `source_hash` is almost always 0 and the selective branch at `context.c:304-306` never narrows the walk.
- `context.c:298-310` — the predicate also fires on `s->view.source_hash == UINT64_MAX` (repository-wide), so one full invalidate marks both the bound source views and the wildcard set stale in one pass; this is the mechanism that makes a patch or command turn drop and later re-render working-state/source segments.
- `context.c:531-619` / `:621-658` — the renderer picks group order by an integer rank recomputed per segment, with `FORGE_SEG_MEMORY` reranked to 5 (volatile last) and everything past TASK to 4; `render_selected_native` is stable only when every selected segment with `kind <= FORGE_SEG_TOOLS` is immutable+cacheable+non-stale+parentless (`:546-548`), which is also the condition gating the cache anchor.
- `conversation.c:167-176` — `evict_turn` does `memmove(c->turns, c->turns + 1, count * sizeof(*turns))` on every eviction, inside a `while` loop at `:263-265` that can evict repeatedly; byte accounting (`c->bytes -= ...`, `:168`) is updated per eviction but `turn.bytes` accumulates `strlen(view.text)` only (`:245`).
- `conversation.c:190` — `calloc(FG_MIN(available, CONVERSATION_MAX_SEGMENTS))` allocates the full segment array up front per capture, and `:216-237` is an O(turn × results × parents) nested scan pairing each result to its action parent, repeated again at `:251-262`.
- `semantic_state.c:34-53` — FNV-1a over every byte via `sem_bytes`; `sem_canonicalize` (`:119-277`) re-hashes the same file bytes a second time when buffering, so a canonical C/Python/Go/Rust/TS file is read and hashed twice per scan (raw at `:287`, canonical frames at `:260-271`).
- `semantic_state.c:365-374` — `fg_semantic_state_take` walks the whole workspace with `fg_input_snapshot_take_visit` and then walks it again with `fg_input_snapshot_take` to prove stability, so each per-turn semantic scan is a double full-tree walk; `input_snapshot.c:667-736` hashes every file and `:711-712` qsorts the file list.
- `semantic_state.c:382-383` — `qsort` of file records by path, plus a second `qsort` in the diagnostic fingerprint (`:503`).
- `semantic_state.c:500` and `:384-394` — the state hash and diagnostic fingerprint are order-independent by sorting, which is correct, but `fg_semantic_state_equal` (`:397-407`) then compares every record including `strcmp` on paths, so the equality check is O(files × path length) even after the hash matched.

**Blockers and uncertainty**
- Root cause of the stop: the global daily budget cap (`$5.00`), not a repo or read-only-policy problem. Every remaining read (compaction call sites in `agent.c` around `:1776-1820` and `:2528`, `src/repo/repo.c:407`, the retained 16.5%-re-prefill artifacts under `benchmark/results/2026-09-09-agent-loop-fresh-design/`, `benchmark/results/2026-09-10-beat-contemporaries/c07-screen/README.md`) and the deliverable write are outstanding.
- The 16.5% re-prefill mechanism (task question 5) is only partly located: I confirmed the invalidation path that stales and re-renders segments, but did not read the retained measurement artifact that assigns the 16.5% figure, so I cannot yet name the single segment text that forces the re-prefill with a citation.
- To finish: raise the cap (`/budget set global daily <usd>`) or reset the window, then re-run this unit. All scope files are already read; roughly 8-12 further reads plus one write would complete it.
