# Judge rerank screen — results (2026-09-17)

**Verdict: MATERIAL — preregistered bar met**

## Preregistered bar (frozen before the first cell)

- mean MRR (judge-on, two repetitions) ≥ judge-off MRR + 0.10 absolute
- target survival not lower under judge-on
- hit@1 not lower under judge-on

## Arms

| Arm | MRR | hit@1 | hit@3 | survival |
| --- | --- | --- | --- | --- |
| off (1 rep) | 0.288 | 5.0% | 55.0% | 72.5% |
| on r1 | 0.725 | 72.5% | 72.5% | 72.5% |
| on r2 | 0.706 | 70.0% | 70.0% | 72.5% |
| on mean | 0.716 | 71.2% | 71.2% | 72.5% |

MRR gain: +0.427 (required ≥ +0.10).

## Mechanism

- judge engagement: 98.8% of judged cells applied
- model ids returned: jev-1.13.0
- judge latency: median 531.0 ms, max 984.0 ms
- repetition rank flips: 1 of 40 queries ['id01']
- gains (best judge rep better than off): 27 queries ['id01', 'id02', 'id03', 'id04', 'id05', 'id06', 'id07', 'id11', 'id12', 'id13', 'id14', 'id15', 'id16', 'id17', 'id18', 'id19', 'id20', 'id21', 'id22', 'id23', 'id24', 'id25', 'id26', 'id27', 'id28', 'id29', 'id30']
- regressions (off rank 1, judge worse): 0 queries []
- missing cells: 0; rank mismatches vs runner metadata: 0

## Per-query ranks (off | on r1 | on r2)

| query | kind | target | off | on r1 | on r2 |
| --- | --- | --- | --- | --- | --- |
| id01 `fg_action_begin` | identifier | src/inference/routing.c:10 | 4 | 1 | 4 |
| id02 `fg_action_complete` | identifier | src/inference/routing.c:33 | 4 | 1 | 1 |
| id03 `fg_action_decode_phase` | identifier | src/inference/routing.c:105 | 2 | 1 | 1 |
| id04 `fg_agent_mark_independent_workspace` | identifier | src/core/agent.c:24 | 2 | 1 | 1 |
| id05 `fg_candidate_store_cost` | identifier | src/core/candidate_store.c:105 | 2 | 1 | 1 |
| id06 `fg_candidate_store_destroy` | identifier | src/core/candidate_store.c:77 | 2 | 1 | 1 |
| id07 `fg_candidate_store_equal` | identifier | src/core/candidate_store.c:97 | 2 | 1 | 1 |
| id08 `fg_candidate_store_materialize` | identifier | src/core/candidate_store.c:181 | None | None | None |
| id09 `fg_checkpoint_allocation_bytes` | identifier | src/inference/checkpoint.c:278 | 1 | 1 | 1 |
| id10 `fg_checkpoint_cache_note_reuse` | identifier | src/inference/checkpoint_cache.c:474 | 1 | 1 | 1 |
| id11 `fg_compress_output` | identifier | src/tools/diagnostics.c:1762 | 8 | 1 | 1 |
| id12 `fg_dedup_clear` | identifier | src/tools/tools.c:1671 | 3 | 1 | 1 |
| id13 `fg_diagnostic_hash` | identifier | src/tools/diagnostics.c:1732 | 5 | 1 | 1 |
| id14 `fg_go_graph_applicable` | identifier | src/repo/graph.c:820 | 3 | 1 | 1 |
| id15 `fg_go_graph_destroy` | identifier | src/repo/graph.c:797 | 3 | 1 | 1 |
| id16 `fg_go_graph_edges` | identifier | src/repo/graph.c:833 | 3 | 1 | 1 |
| id17 `fg_go_graph_excluded_path` | identifier | src/repo/graph.c:373 | 3 | 1 | 1 |
| id18 `fg_go_graph_find_package` | identifier | src/repo/graph.c:843 | 3 | 1 | 1 |
| id19 `fg_go_graph_module_for` | identifier | src/repo/graph.c:856 | 3 | 1 | 1 |
| id20 `fg_go_graph_modules` | identifier | src/repo/graph.c:823 | 3 | 1 | 1 |
| id21 `fg_go_graph_packages` | identifier | src/repo/graph.c:828 | 3 | 1 | 1 |
| id22 `fg_go_graph_reasons` | identifier | src/repo/graph.c:838 | 3 | 1 | 1 |
| id23 `fg_impact_snapshot_destroy` | identifier | src/repo/impact.c:156 | 3 | 1 | 1 |
| id24 `fg_input_snapshot_destroy` | identifier | src/core/input_snapshot.c:761 | 5 | 1 | 1 |
| id25 `fg_input_snapshot_equal` | identifier | src/core/input_snapshot.c:745 | 4 | 1 | 1 |
| id26 `fg_input_snapshot_hash` | identifier | src/core/input_snapshot.c:757 | 4 | 1 | 1 |
| id27 `fg_json_uint` | identifier | src/core/util.c:486 | 3 | 1 | 1 |
| id28 `fg_json_whitespace_only` | identifier | src/inference/routing.c:51 | 2 | 1 | 1 |
| id29 `fg_llama_init` | identifier | src/inference/llama_backend.c:1411 | 2 | 1 | 1 |
| id30 `fg_metrics_json` | identifier | src/core/session.c:78 | 2 | 1 | 1 |
| cq01 `where is the retrieval source digest verified` | concept | src/repo/retrieval.c:165 | None | None | None |
| cq02 `where does the context planner score candidate segments` | concept | src/context/context.c:740 | None | None | None |
| cq03 `where are tool calls dispatched after policy approval` | concept | src/tools/tools.c:1788 | None | None | None |
| cq04 `where is the pre-command input snapshot taken` | concept | src/core/agent.c:3078 | None | None | None |
| cq05 `where does the checkpoint cache decide invalidation` | concept | src/inference/checkpoint_cache.c:277 | None | None | None |
| cq06 `where is the GBNF grammar built from the tool registry` | concept | src/tools/tools.c:582 | None | None | None |
| cq07 `where is the KV prefix anchor nominated` | concept | src/context/context.c:663 | None | None | None |
| cq08 `where does forge validate the config schema` | concept | src/core/config.c:669 | None | None | None |
| cq09 `where is the edit journal prepared before an edit lands` | concept | src/tools/edit_journal.c:194 | None | None | None |
| cq10 `where is the final answer validation gate` | concept | src/core/agent.c:2592 | None | None | None |
