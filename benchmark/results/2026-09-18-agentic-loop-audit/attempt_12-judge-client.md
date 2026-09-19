# attempt_12-judge-client -- Forge agentic-loop performance audit, domain 12-judge-client
Run: C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5
Repo: C:/Users/flowc/dev/forge @ d16e6736918808daa9c1f1993093a6dcd6ea0af8 (read-only)
Scope: src/judge/judge.c (850 lines, read in full), include/forge/judge.h (123 lines, read in full),
call sites (src/cli/main.c, src/core/agent.c, src/tools/tools.c), the rerank path (src/repo/retrieval.c,
include/forge/retrieval.h), tests/unit/test_judge.c (header + first 80 lines), the three retained judge
campaigns (2026-09-17-judge-rerank, -v2, 2026-09-18-typesafe-repair-feedback) and all 176 retained raw
records (machine-summarized: candidate counts, request bytes, input tokens, latency, record sizes).

### CLAIMS

1|WRONG|src/judge/judge.c:131|forge_judge_budget_ms() returns 2*timeout_ms+500 = 4500 ms at the default 2000 ms timeout, but http_transport sets four phase timeouts of timeout_ms each (judge.c:654-655: resolve, connect, send, receive), so one attempt can consume up to about 4*timeout; the retained record benchmark/results/2026-09-17-judge-rerank-v2/raw/judge-20260918T004421Z-0000.json has latency_ms=8016 with error "WinHttpSendRequest failed (12002)", 78% above the budget, and the v2 README (lines 32-35) records the same overrun as a residual tuning note.|Derive the budget from the phase structure (2*(resolve+connect+send+receive)+backoff) or set resolve/connect/send to a small fixed slice and leave the remainder to receive; also enforce an elapsed cap inside the retry loop.|deadline accounting: src/repo/retrieval.c:592-593 extends the snapshot window by this budget and src/core/agent.c:1077 refuses the call only when the budget fits, so both seams can overrun the deadline they promise to respect (v1 lost 1 of 120 cells to this class).|measured-perf-baseline + needs-verify|high|Show that the 8016 ms record includes work outside judge_transport, or that WinHttpSetTimeouts phases cannot sum to more than timeout_ms per attempt.

2|GAP|src/tools/tools.c:1823|the rerank callback is wired only where retrieve_context runs, and the measured agent populations never call that tool: benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md records 2 of 2 preflight cells with zero raw records and zero retrieve_context tool calls, and no session in the repair-control, agent-loop or beat-contemporaries campaigns contains one either (same file, lines 29-34).|Extend the seam to a surface the agent actually uses (search_text at tools.c:1810, or host-routed read_file selection) or select a retrieval-forcing population before any further judged campaign.|prevents campaign spend on a mechanism that cannot fire: the material retrieval-level result (MRR +0.427) still has no measured agent-level counterpart.|none-observable + needs-decision|high|Find one retained session in those populations with a retrieve_context tool call.

3|COST|src/judge/judge.c:546|record_run re-parses the request that was just serialized (judge.c:192) and the response that was just parsed (judge.c:209) to embed both as JSON, then writes the record pretty-printed (judge.c:569, flag 1 = YYJSON_WRITE_PRETTY); one file per call, measured 9241-29583 bytes across 161 retained rerank records and 1.10-1.41x the request+response bytes.|Reuse the built yyjson doc (or embed the raw strings) and write with YYJSON_WRITE_NOFLAG; the record schema and fields stay unchanged.|host overhead per call whenever record_dir is set (every judged campaign set it): two extra JSON parses plus a roughly 1.3x larger file write per call; CPU unmeasured, sizes measured.|unit-tests + safe-now|high|Show a reader that requires pretty-printing or re-serialized JSON (tests/unit/test_judge.c asserts fields, not formatting).

4|GAP|src/judge/judge.c:790|no request cache or dedup exists in the client: every call rebuilds and re-sends, while the research doc proposes exact-input caching (docs/research/jev-integration-2026-09-17.md:74) and the needed keys already exist (rerank: query plus candidate paths/snippets plus the retrieval generation in the output; feedback: input_hash, initial_hash, validation summary).|Memoize by exact inputs (model, question text, query, candidate hashes, index generation; feedback: input_hash, initial_hash, summary hash) with a small LRU invalidated on generation or hash change.|unmeasured: a repeated identical call costs a full round trip (measured 359-8016 ms rerank, 125-547 ms feedback); no within-session duplicate has been measured, so the saving is contingent on one existing.|unit-tests + needs-verify|medium|Extract within-session repeated identical (query, candidate-set) or (input_hash, summary) pairs from retained sessions; zero pairs means zero value.

5|COST|src/judge/judge.c:611|the body is built before judge_transport runs, so a call that cannot succeed still pays a full build: the missing-key check (judge.c:611-613) and the non-Windows UNSUPPORTED path (judge.c:773-775) both run after build_request (judge.c:790); with record_dir set a failure record is written as well (judge.c:825).|When options.transport is NULL, check key presence and platform at the top of forge_judge_rerank and forge_judge_feedback before build_request.|unmeasured host CPU per call (yyjson alloc plus serialize of a measured 7868-25632 byte body) on every retrieve_context and feedback call while the key is absent or on non-Windows; no retry and no sleep, so the wall-clock cost stays sub-millisecond class.|unit-tests + safe-now|medium|Time a retrieve_context call with --judge and no TYPESAFE_API_KEY and show the build is below noise, or show the key can appear mid-process (the header promises call-time reads).

6|DEAD|src/judge/judge.c:134|forge_judge_metrics and forge_judge_stats have no production reader: outside tests/unit/test_judge.c and the benchmark live probe the only references are the declaration (include/forge/judge.h:118) and the definition; calls, failures, candidates_scored, input_tokens, output_tokens and total_latency_ms are accumulated (judge.c:505-507, 814-816) and never read by the agent, CLI or session.|Emit one judge_metrics session event at agent teardown (or fold the counters into fg_metrics_json) so hosted-judge calls, tokens and latency are attributable in a run; otherwise delete the surface.|measurement gap: no session-level judge accounting (per-call latency survives only in the retrieval output's rerank object and the judge_feedback event), so criterion 1's hosted-judge component cannot be reported from a session.|none-observable + safe-now|high|Show a production caller of forge_judge_metrics outside tests and the benchmark probe.

7|RISK|src/judge/judge.c:533|record filenames are judge-<utc-second>-<seq>.json; record_seq starts at 0 in every process (judge.c:577) and the stamp has one-second resolution, so two processes sharing a record_dir in the same second overwrite each other.|Include the process id (or a random suffix) in the record filename.|evidence loss at a stated scale: parallel cells sharing a record_dir silently drop records; no wall-clock effect.|unit-tests + safe-now|medium|Run two judges concurrently against one record_dir and count the files; no loss means the collision needs a same-second start.

8|RISK|include/forge/judge.h:29|the rerank cap (default 32, configurable 1..256, judge.h:30) and the retrieval result cap (default 16 at src/repo/retrieval.c:38, max 256 at include/forge/retrieval.h:9) are independent; a host that raises retrieval max_results above the judge cap makes every rerank fail with LIMIT (judge.c:785-787) and fall back to deterministic order with reason "failed" (retrieval.c:433-437); no current config path raises max_results above 16, so the default pair is safe.|Clamp the rerank request to max_candidates, or validate the pairing where the callback is wired (src/tools/tools.c:1823-1827, src/cli/main.c:1215-1219).|silent loss of the material rerank effect at a stated non-default scale (a host using the public forge_retrieval_options).|unit-tests + needs-decision|medium|Set retrieval max_results=64 with the default judge cap and inspect the output's rerank.reason/applied.

9|COST|src/judge/judge.c:18|the frozen question text costs exactly 429 bytes per candidate (constant across all 161 retained rerank records; min = median = max), 16.3-62.2% of the request body (median 18.4%), about 115 input tokens per candidate at the measured 0.267 tokens/byte; the state payload is otherwise dominated by snippets (median 1891 bytes per candidate).|If input cost ever matters more than instrument continuity, shorten rerank_instruction/rerank_true/rerank_false; these strings are the frozen instrument (judge.c:14-17), so any change must be recorded in the campaign identity.|unmeasured on host wall-clock; vendor-side only (a 16-candidate call carries about 1800 tokens of scaffolding, about $0.00008 at $0.042/M).|measured-perf-baseline + needs-decision|medium|Recount bytes per candidate in retained records after a change; re-run the frozen rerank bar to show MRR does not move.

### VERIFIED-OK

OK|src/judge/judge.c:801|the retry policy is exactly one retry and only on FORGE_ERR_IO; the single body built at judge.c:790 is reused for the retry (no re-serialization), and POLICY/PARSE/LIMIT/ARGUMENT/UNSUPPORTED are final with no backoff sleep (the feedback path at judge.c:492 mirrors this).
OK|src/repo/retrieval.c:591-597|the rerank budget extends only the snapshot's relative window (timeout_ms + rerank_budget_ms) and the absolute deadline still caps it through min(); without a rerank callback the deadline math is unchanged (commit 5520c33a verified in code).
OK|src/core/agent.c:1076-1078|the repair seam refuses the call when the remaining deadline is smaller than forge_judge_budget_ms, so the feedback call cannot consume a deadline it does not have; the repair path is budgeted like the rerank path.
OK|src/repo/retrieval.c:680-686|rerank_apply runs before render's output-budget trim, and the trim removes the array tail (retrieval.c:569-576), so omitted candidates are the lowest-scored ones; the doc's "reorder before trimming" is the implementation, not a pending item.
OK|src/judge/judge.c:785-787|count above the candidate cap fails with FORGE_ERR_LIMIT before any body build, and count==0 returns FORGE_OK with no call and no allocation.
OK|src/judge/judge.c:649-656|the WinHTTP session is opened lazily once per judge and its timeouts are set once; only the per-call connect and request handles are created and closed (judge.c:657-760).
OK|src/judge/judge.c:524-529|raw recording is best-effort by construction: a NULL record_dir disables it, mkdir/write failures are ignored, and no recording failure can fail the judged call (the header's promise holds).
OK|src/judge/judge.c:193-197|the request body is capped at FG_MAX_JSON before serialization returns and http_transport re-checks it (judge.c:616-617); the response body is capped while accumulating (judge.c:723-726), so both directions are bounded.
OK|src/judge/judge.c:765-776|non-Windows builds fail with FORGE_ERR_UNSUPPORTED before any transport allocation (no retry, no sleep) and the stub transport seam (include/forge/judge.h:43-48) keeps deterministic tests on every platform.
OK|src/judge/judge.c:842-848|the rerank callback fills forge_rerank_info from the judge's own telemetry only on success, and the retrieval output carries applied/reason/model/scored/latency/tokens in its rerank object (src/repo/retrieval.c:506-519), so per-call telemetry is retained even though the aggregate counters are not.
OK|src/core/agent.c:1074|the feedback seam is inert (zero calls, zero cost) unless a judge is configured, bounded repair is on, and a complete stable validation failed; passing and unvalidated turns never pay for it.
OK|src/judge/judge.c:728|the response buffer grows by exactly the available chunk with no amortized growth, but measured responses are small (55-282 output tokens, roughly 1-2 KB), so the un-amortized realloc is not a hot-path concern here.

### JEV

JEV|src/tools/tools.c:1810|arm-1 extension: search_text (fg_repo_search, BM25 order, 50 hits) has no rerank callback while retrieve_context does (tools.c:1823-1827); the frozen rerank question shape fits unchanged|noul|which returned text hit actually contains the definition or implementation the query asks for|the material retrieval-level rerank result applies only where retrieve_context runs, and E3 shows the agent works by direct reads and shell commands, so search_text is the surface it actually uses|advisory ordering only: a wrong top hit costs one read turn, not correctness; deterministic order stays the fallback|existing-arm-extension
JEV|src/core/agent.c:178|missing wiring: bounded_repair requires candidate_checkpoint requires minimal_agent (agent.c:178-183) and candidate_validate is called only from minimal_run (agent.c:1962, 1989), so the feedback arm is unreachable in the richer loop while the rerank arm is unreachable in the minimal loop (the minimal schema has no retrieve_context, tools.c:279-303); the research doc's MVP read routing is a choice over already-observed reads|choice|which bounded read (path or line range already observed in the episode) most likely resolves the failed validation when the model has no semantic retrieval|the judged repair population dies at the 32-turn cap (all 4 failures in the 2026-09-18 screen), so replacing one unproductive turn with a routed read attacks the measured failure mode; the same request can also carry the existing 4 feedback questions|the host must run the read through the existing tool path, policy, output bounds and action accounting; advisory with deterministic fallback|new-proposal
JEV|src/core/agent.c:1100|arm-2 extension: the feedback answers (failure_family, evidence_gap, repair_readiness, next_action) are rendered into prompt text only (agent.c:1111-1126); no host code consumes next_action to select the next action|choice|what the next action should be when the host cannot decide exactly between inspect_source, patch_code and run_validation|the answers already arrive in the same batched request (4 questions per call; 15 retained calls, 125-547 ms), so acting on them adds no round trip|low-confidence choices must defer; validation authority stays with the host|existing-arm-extension
JEV|src/core/agent.c:1074|arm-2 extension: feedback fires only on validated && !passed; the incomplete-validation branch retains host text and never asks the judge (agent.c:1268)|noul|whether the retained incomplete evidence (unstable inputs, missing tests, denied policy) is sufficient to choose a safe repair or needs another inspection|extends engagement to the class where the loop currently guesses, without touching failure classification|the research doc (line 38) requires incomplete validation to fall back rather than masquerade as an ordinary test failure; keep this question separate and advisory|existing-arm-extension
JEV|src/core/verification.c:182|new proposal: test-selection prioritization among applicable validation commands (the research doc lists this as follow-on work, line 97)|choice|which applicable validation command most likely exercises the failed behavior when several are available|validation is a top wall-clock component; running the covering command first can avoid a second full validation cycle|must not remove or weaken required broad validation; ordering only, advisory|new-proposal

### NOTES

Baselines (retained measurement artifacts that bear on this domain):
- benchmark/results/2026-09-17-judge-rerank/SUMMARY.md + E3-RESULTS.md: v1 retrieval screen NOT MATERIAL
  (MRR +0.424 but 1 of 120 cells died on the snapshot's 5 s scope; frozen rule retains failed cells);
  E3 agent screen did not run - zero engagement in both preflight cells, zero raw records, zero
  retrieve_context tool calls, no control arms.
- benchmark/results/2026-09-17-judge-rerank-v2/README.md + PREREGISTRATION.md: v2 MATERIAL, bar met on all
  three clauses (MRR +0.427, 27/40 queries improved, zero rank-1 regressions, 120/120 cells exit 0);
  engagement 79/80 = 98.75%; latency median 531 ms, max 984 ms; 291,845 input tokens = $0.0123; the
  retained 8,016 ms fail-open stall; judge-off output byte-identical to the pre-judge build.
- benchmark/results/2026-09-18-typesafe-repair-feedback/README.md + engagement-screen/ +
  population-manifest.json (sha f0661378): repair-feedback arm CONTINUE (n=4/arm, 2 tasks; 3P/1F vs 1P/3F);
  6 raw records, 11,512 in / 918 out tokens, latency 125-516 ms (median 422); engagement 4 events in 2 of
  4 treatment cells; failures are cap-deaths; the population manifest freezes 6 engaged + 2 inert fixtures.
- Recount of all 176 retained raw records (this audit): rerank 161 calls, 1 error, candidates 3-16,
  request 7868-25632 bytes, input tokens 2000-6720 (median 3332), latency 359-8016 ms (median 500),
  record files 9241-29583 bytes; feedback 15 calls, 0 errors, 4 questions each, request 3501-7798 bytes,
  input 1164-2277 tokens, latency 125-547 ms, record files 6600-10895 bytes.
- docs/research/jev-integration-2026-09-17.md is a proposal with no measurements (read, not relied on as
  evidence); spikes/ contains no judge artifacts (grep: zero files).

Self-refuted and not filed:
- Partial-answer handling: parse_response (judge.c:224-226) and parse_feedback_response (judge.c:434-437)
  discard the whole response if one answer is missing or malformed, but none of the 176 retained records
  shows a partial answers object, so a partial-credit path has no measured trigger; kept out of CLAIMS.
- The rerank request carries path and stage per candidate that the question text never references
  (judge.c:168-170); measured at roughly 50-70 bytes per candidate it is under 1% of the body, so it is
  not filed.
- candidate_feedback_source re-reads the current file from disk per failed validation (agent.c:1026-1027)
  but is deadline-guarded (agent.c:1022) and capped at 8192 bytes (agent.c:1032), so it is a bounded
  deliberate read, not a claim.
- Closed axes checked and not re-opened: nothing here touches host-evidence presentation, thought-history
  retention, sampler variance, repetition penalty, prefix-stability admission, exact-argv run_command
  dedup, best-of-N budget splitting, the retraction family, or CUDA Unified Memory. The budget finding (1)
  is the v2 README's own open tuning note, not a re-proposal.

Answers to the task's questions that are not claims:
- Q1 request construction: yyjson, one document per call, one serialization, freed on every path; response
  parse is a single yyjson_read; no allocation churn beyond the document (OK lines).
- Q5 batching: within-call batching is already maximal - 3-16 noul questions per rerank call in one request,
  4 questions per feedback call in one request; the only unused batch capacity is adding the read-routing
  choice to the existing feedback request (JEV line 2/3), which costs no extra round trip.
- Q7 Windows-only transport: the loss is feature parity, not a perf path; the deterministic loop is
  unaffected, and a wired judge on non-Windows costs one request build per call (claim 5).
- Retry/backoff math: 500 ms once, only after an IO failure; the success path never sleeps (measured
  feedback latencies 125-547 ms, which is below 2*timeout).

Skipped / not read: tests/unit/test_judge.c beyond the first 80 lines; the benchmark runners
(run_screen.py, e3_run.py, run.py profiles beyond the loop-repair flags); CMakeLists judge wiring;
docs/SECURITY.md and docs/plans/agent-performance-avenues.md judge mentions; spikes/* (no judge content).

Uncertainty and missing baselines:
- No host-side CPU measurement exists for request build or record write, so claims 3, 5 and 9 carry
  "unmeasured" impacts; the measured quantities are sizes, token counts and latencies.
- Vendor latency (70-500 ms reported) cannot be validated here: no network calls were made (read-only).
- No agent-level judge measurement exists in either direction: E3 never engaged, and no judge-off control
  arms ran, so nothing in this file claims an agent-level speedup or regression.
- The exact worst-case phase sum behind claim 1 is a code-based upper bound; the measured datum is the
  8,016 ms record, which is sufficient to falsify the declared 4,500 ms contract on its own.
