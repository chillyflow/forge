# jev_global.md -- cross-cutting Jev augmentation seams (units no single module owns)

Run: wf_forge_perf_978e2fd5. Repo: C:/Users/flowc/dev/forge. Revision: d16e6736918808daa9c1f1993093a6dcd6ea0af8.
Read-only sweep: no edits, builds, tests or model runs. All anchors are lines actually read at that revision.
Scope: evaluation/benchmarking (Q1), experience+memory A7 (Q2), routing/escalation A8/M6 (Q3),
threshold calibration loop (Q4), human-facing surfaces (Q5), cross-module budgets/decomposition (Q6).
Cap 12 entries, best-first. Entry format = task_jev-reconcile.md section D plus WHY NOT DECIDABLE BY CODE
and OFFLINE/FALLBACK. Jev is advisory only; host validation keeps authority everywhere.

G01 | ADVISORY ACCEPT/ABSTAIN CALIBRATION AND DECISION RECORD | PHASE post-failure (feedback arm) + retrieval (rerank arm) | ANCHOR src/core/agent.c:1107-1110; src/core/agent.c:1041-1067; src/judge/judge.c:134-148; include/forge/judge.h:113-118 | Q_TYPE batch
WHAT CODE CANNOT DECIDE: whether a judge answer is reliable enough to act on for this episode. The acceptance rule is hardcoded (uncertain if next_action_confidence < 0.55, failure_confidence < 0.45, repair_readiness_confidence < 0.45; inspect if evidence_gap >= 0.60 or the family/next_action says so) and no retained data links answers to episode outcomes.
WHY NOT DECIDABLE BY CODE: confidence is a statistic derived from the answer distribution, not a proven probability of correctness (docs/research/jev-integration-2026-09-17.md:19); code can compute the distribution but cannot decide whether acting on it repays its cost. That needs outcome-linked calibration, which the host does not record today.
HOST KEEPS: thresholds, guidance text, reserved turns, validation authority. Jev never gets success authority.
EXPECTED BENEFIT: quality + wall (advisory calls help or waste latency depending on this gate); unmeasured.
COST: no new calls (the arm already sends one batch per complete failed validation: 6 records, 11,512 input / 918 output tokens, latency 125-516 ms median 422 ms -- benchmark/results/2026-09-18-typesafe-repair-feedback/README.md:51-56).
RISK: acting on an uncalibrated answer steers a repair turn wrong; abstaining keeps current behavior.
STATUS: existing-arm-extension | EVIDENCE: repair-feedback arm continue bar met (README.md:3-4, 38-47); rerank v2 material at retrieval level (benchmark/results/2026-09-17-judge-rerank-v2/README.md:9-17). The record is missing: the judge_feedback event carries answers but no threshold values and no episode linkage (agent.c:1047-1059); forge_judge_metrics has no production caller (tests/unit/test_judge.c only; grep over src/, include/, tests/, benchmark/) and metrics.json carries no judge counters (src/core/session.c:78-153).
OFFLINE/FALLBACK: judge absent, failed or cancelled means append_judge_feedback returns early (agent.c:1097-1098) and the loop keeps its deterministic path; thresholds stay as written.
RECOMMENDATION: experiment -- record-only first: emit the accept/abstain branch and threshold version with the judge_feedback event, link the episode outcome (candidate_checkpoint/tool_result/judge events), and call forge_judge_metrics at session finish; calibrate only on retained records.

G02 | PRE-CLOSING CONTROL POINT (same questions, earlier trigger) | PHASE budget-closing, per turn | ANCHOR src/core/agent.c:1663-1667; src/core/agent.c:1069-1075; src/core/agent.c:1637-1644 | Q_TYPE choice
WHAT CODE CANNOT DECIDE: whether another ordinary turn repays its cost before the host closes with validation/final. Closing triggers on fixed arithmetic (remaining_output <= output_reserve + 2*256, or remaining_input <= 2*input_capacity; validation_only at turn+1 == max_turns, agent.c:1663-1667) and the judge batch fires only after a complete failed validation (!validated || passed returns, agent.c:1074).
WHY NOT DECIDABLE BY CODE: the remaining-value question is semantic -- is the candidate one edit from passing, or one edit from another cap-death? The host arithmetic knows remaining budget, not remaining distance.
HOST KEEPS: reserved validation/final turns, budgets, validation authority; judge advice cannot skip required validation.
EXPECTED BENEFIT: turns/wall (avoid a doomed validation cycle: repo index then validation commands, agent.c:1146-1156) and reduce cap-deaths (6/6 Python failures ended at 32/32 actions, docs/plans/agent-loop-fresh-design.md:52-55); unmeasured.
COST: one batch call per closing episode (same payload class as the shipped arm, <=4 questions); latency bounded by forge_judge_budget_ms = 2*timeout + 500 ms backoff (src/judge/judge.c:130-132).
RISK: an extra call at the most budget-scarce point; a wrong "keep going" spends the last turn, a wrong "close now" abandons a winnable repair. Gate timing is measured machinery: changing it invalidates measured-perf-baseline.
STATUS: existing-arm-extension | EVIDENCE: mechanism engages (4 judge_feedback events in 2/4 treatment cells, README.md:43-45); no measured baseline for a pre-closing trigger.
OFFLINE/FALLBACK: current fixed gates; judge failure returns early (agent.c:1097).
RECOMMENDATION: experiment (own candidate + screen; do not bundle with any evidence-presentation change).

G03 | PASSING-CANDIDATE SELECTION | PHASE run-end (candidate search) | ANCHOR src/core/candidate_search.c:265-277; src/core/candidate_search.c:314-318; docs/plans/agent-performance-avenues.md:205-212 | Q_TYPE choice
WHAT CODE CANNOT DECIDE: which of several host-validated passing candidates better satisfies the task. The host picks "passing_then_smallest_content_change_then_first" (candidate_search.c:314-318); the cost proxy never reads the diffs.
WHY NOT DECIDABLE BY CODE: all eligible candidates passed real validation (candidate_search.c:226-246); nothing deterministic ranks repairs that both pass. Semantic comparison of each diff against the request is the missing judgment.
HOST KEEPS: validation authority (only passing candidates are eligible), shared budget accounting, apply/restore mechanics; the winner is re-validated after apply (candidate_search.c:300-308).
EXPECTED BENEFIT: quality; unmeasured.
COST: one batch call per multi-candidate run that yields 2+ passing candidates; payload = task + candidate diffs (bounded).
RISK: choosing a worse passing candidate. Not a closed-axis re-proposal: "best-of-N as budget splitting" closed the success mechanism (BRIEF.md:30-35; avenues.md:50); this seam only orders already-passing outputs.
STATUS: new-proposal | EVIDENCE: docs/research/jev-integration-2026-09-17.md:99 names "choosing among completed passing candidates" a later experiment; M3 records best-of-N failed on allocation, not mechanism (avenues.md:205-212). Prerequisite unmeasured: how often 2+ candidates pass in one run.
OFFLINE/FALLBACK: smallest-content-change rule unchanged.
RECOMMENDATION: hold -- measure multi-pass frequency on retained candidate runs first (falsifier: if multi-pass is rare, the seam is worthless); then experiment.

G04 | REPEATED-FAILURE ESCALATION TRIAGE | PHASE per-episode, after repeated failure | ANCHOR src/core/agent.c:2156-2173; src/core/agent.c:1196-1230; src/core/agent.c:2774-2781; docs/plans/agent-loop-fresh-design.md:407-429 | Q_TYPE choice
WHAT CODE CANNOT DECIDE: whether a repeated failure is an approach error (change strategy, file, or requirement reading) or a capability limit (abort/escalate). The host detects repetition exactly (3 consecutive identical-replacement edits -> stop_loss_abort, agent.c:2161-2173; canonical workspace + diagnostic repeat -> semantic_loop, agent.c:1205-1230) but the response is fixed: abort the trial or keep going.
WHY NOT DECIDABLE BY CODE: the same repetition evidence is consistent with both causes; only semantic reading of the transcript, diff and task separates them. The design review's conclusion ("a reasoning gap, not a plumbing gap", fresh-design.md:419-421) is exactly the class assignment code cannot make.
HOST KEEPS: abort/continue control, budgets, validation authority. A8/M6 constraint: 23.89 GiB VRAM against 14-18 GiB per model means one resident model at a time, so model switching is a W4 decision (avenues.md:349-354), not a control this seam may take.
EXPECTED BENEFIT: wall/quality (cap-deaths and no-op degeneracy are the dominant measured failure classes: 28 byte-identical replacement calls across 8 runs, fresh-design.md:62-80; 11 runs at the pinned-context wall, avenues.md:138-143); unmeasured.
COST: one batch call per aborted/repeated episode (bounded excerpts).
RISK: mislabelling a winnable repair as a capability limit; keep advisory and record disagreement with W4 outcomes.
STATUS: new-proposal | EVIDENCE: no measured baseline for judge-informed escalation; W4 arms need an explicit decision before execution (fresh-design.md:407-411).
OFFLINE/FALLBACK: current stop-loss and semantic-loop behavior unchanged.
RECOMMENDATION: experiment in shadow form (triage retained aborted episodes); any control change needs the W4 decision.

G05 | FAILURE-TAXONOMY RESIDUAL CLASSIFICATION | PHASE analysis-time (post-run) | ANCHOR benchmark/analyze_failures.py:28-38; benchmark/analyze_failures.py:113; benchmark/export_evidence.py:103-121 | Q_TYPE choice
WHAT CODE CANNOT DECIDE: the cause inside the residual class "wrong_fix" (actions well formed and varied; the repair just failed). The mechanical taxonomy splits mechanism failures exactly (no_model_output, no_action, malformed_json, repeated_action, turn_cap, within_turn, forced_action_padding) and leaves every semantic cause in one bucket.
WHY NOT DECIDABLE BY CODE: distinguishing "misread requirement" from "wrong file" from "incomplete multi-file change" needs the task, the transcript and the retained failed workspace (export_evidence.py keeps the terminal workspace of every failed run); no deterministic predicate over those artifacts separates them.
HOST KEEPS: the mechanical taxonomy as-is; judge labels are advisory analysis fields, never gates or denominators.
EXPECTED BENEFIT: quality (campaign targeting: which failure class to attack next, fresh-design.md:312-350 W2/W4).
COST: one batch call per failed run; excerpts bounded (task + last actions + diff summary).
RISK: a mislabel steers remediation investment; retain both labels and require human acceptance before acting. Publishing judge-derived numbers is constrained by TypeSafe MCA 2.3(f) (docs/research/jev-integration-2026-09-17.md:103).
STATUS: new-proposal | EVIDENCE: taxonomy is derived from raw events (analyze_failures.py:1-15); no judge has been applied to it; no measured baseline for label accuracy.
OFFLINE/FALLBACK: current taxonomy unchanged; offline analysis keeps working.
RECOMMENDATION: experiment (offline shadow only; never enters a gate).

G06 | EVALUATION-SET EQUIVALENCE AND CLUSTERING | PHASE analysis-time (benchmark reporting) | ANCHOR benchmark/generalization.py:13; benchmark/generalization.py:36-38; benchmark/holdout/2026-09-02/ANALYSIS.md:14; benchmark/report.py:67-94 | Q_TYPE noul
WHAT CODE CANNOT DECIDE: whether a renamed/paraphrased variant is truly equivalent to its source task, and whether two task ids are paraphrases that must share one bootstrap cluster. Labels are hand-assigned: relation = "contrast" if variant == "contrast" else "equivalent" (generalization.py:36-38); each task id is one bootstrap cluster (holdout ANALYSIS.md:14).
WHY NOT DECIDABLE BY CODE: equivalence is semantic; the variants differ in text by construction, and the contrast variant is deliberately non-equivalent (generalization.py:76-110). Code can hash and diff task bytes but cannot decide paraphrase. The research doc's own replay method says to split by task family "keeping paraphrases together" (research doc:86).
HOST KEEPS: the frozen labels and report structure; a judge disagreement is recorded, never auto-flips a label; the bootstrap stays deterministic (report.py:67-94).
EXPECTED BENEFIT: quality of inference (prevents a false generalization or independence claim; RUN_EFFICIENCY.md:177-186 bans per-run significance on clustered data; fresh-design.md:369-373 warns the effective sample size is nearer 4 than 12).
COST: O(n^2) pairwise nouls offline; for the 10 generalization variants about 10-25 calls with small payloads (prompt + oracle diff).
RISK: a wrong "not equivalent" could discredit a valid variant; advisory review only.
STATUS: new-proposal | EVIDENCE: no measured baseline for judge-assessed equivalence; hand labels are the current authority; scope note in generalization.py:195 ("development generalization regressions; not a promotion holdout").
OFFLINE/FALLBACK: labels unchanged; offline reporting unaffected.
RECOMMENDATION: experiment (offline shadow on the existing 10-variant set; report agreement rate before any use).

G07 | WORKING-MEMORY SALVAGE AFTER A CHANGE | PHASE post-edit (rich loop) | ANCHOR src/core/working_state.c:113-122; src/core/working_state.c:397-403; src/core/working_state.c:585-706 | Q_TYPE batch (noul per item)
WHAT CODE CANNOT DECIDE: which remembered items (hypotheses, decisions, relevant_files) remain valid after a specific diff. Any generation change marks all model memory stale and any observed change invalidates validation wholesale (working_state.c:113-122, 397-403); the model must re-derive.
WHY NOT DECIDABLE BY CODE: whether a given edit invalidates a given hypothesis is semantic; the host can compare generations and hashes, nothing more.
HOST KEEPS: staleness flags; validation invalidation still fires (a change forces re-validation; working_state.c:434-436 refuses passed on incomplete evidence); the model may always re-derive; judge output is advisory marking only.
EXPECTED BENEFIT: tokens/turns (avoid re-derivation after unrelated edits); unmeasured.
COST: one batch per change event over the memory items (bounded: 5 fields x 32 items, combined 8192 bytes; include/forge/state.h:10-11; working_state.c:200-201).
RISK: retaining a stale hypothesis steers the next repair wrong. Adjacent to the closed host-evidence-presentation axis: any rendered-memory change must be its own screened candidate (BRIEF.md:30-31).
STATUS: new-proposal | EVIDENCE: no measured baseline; rich-loop only -- working state is created only in forge_agent_run (agent.c:2297), not in the measured minimal loop.
OFFLINE/FALLBACK: wholesale invalidation unchanged.
RECOMMENDATION: hold (no baseline for re-derivation cost; needs its own candidate and screen if rendered).

G08 | RETRIEVAL EXCERPT INCLUSION BEYOND REORDER | PHASE retrieval, per retrieve_context call | ANCHOR src/repo/retrieval.c:393; src/repo/retrieval.c:416-474; src/repo/retrieval.c:506-519; src/tools/tools.c:1823-1827 | Q_TYPE noul
WHAT CODE CANNOT DECIDE: which retrieved excerpts are worth their bytes when the output budget trims. Scores only reorder (fg_rerank_permutation, retrieval.c:393, 440-474); the byte/token trim stays budget-deterministic and the per-excerpt probability is not used to drop anything.
WHY NOT DECIDABLE BY CODE: relevance is semantic; the deterministic trim knows size, not usefulness, and the research doc warns a wrong drop removes useful evidence (research doc:99).
HOST KEEPS: mandatory/latest evidence, deterministic fallback order, rerank_budget_ms outside the snapshot deadline (retrieval.c:592); Jev can only demote, never add candidates.
EXPECTED BENEFIT: tokens (smaller prompt) + quality (less irrelevant context); unmeasured.
COST: no new calls (the rerank request already scores every candidate, one noul per candidate, batched; cap 32 default, 256 max, include/forge/judge.h:29-30).
RISK: dropping a needed excerpt; a wrong inclusion keeps today's cost. Prerequisite: a population that calls retrieve_context -- the measured one does not (E3: zero engagement, benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md:29-31).
STATUS: existing-arm-extension | EVIDENCE: rerank v2 material at the retrieval level (MRR 0.716 vs 0.288; bar +0.427, judge-rerank-v2/README.md:9-17); zero agent-level engagement in the frozen population (E3-RESULTS.md:29-31, 44-46).
OFFLINE/FALLBACK: current order plus budget trim.
RECOMMENDATION: hold until a fresh population exercises retrieve_context (fresh experiment on the frozen instrument, E3-RESULTS.md:44-46).

G09 | SUMMARY-TEXT FAITHFULNESS | PHASE summary generation (CLI surface) | ANCHOR src/repo/summary.c:1322; src/cli/main.c:599 | Q_TYPE noul
WHAT CODE CANNOT DECIDE: whether a model-generated summary asserts things the syntactic index does not support. The artifact records the gap itself: claims = "syntactic_index; summary_text_unverified" (summary.c:1322); the text is generated by the local model (main.c:599) and cached.
WHY NOT DECIDABLE BY CODE: text faithfulness is semantic; the host holds only index facts and generated text.
HOST KEEPS: the unverified label and cache; a judge flag downgrades or holds a summary, never edits it.
EXPECTED BENEFIT: quality (prevents acting on or publishing an unsupported summary); low today -- summaries are not in the agent loop, whose repo summary is a deterministic file list (src/repo/repo.c:1886-1898).
COST: one call per generated summary (text + manifest, bounded).
RISK: false flags on a correct summary; keep advisory.
STATUS: new-proposal | EVIDENCE: no measured baseline; the claims string is the only current guard.
OFFLINE/FALLBACK: label unchanged; offline generation keeps working.
RECOMMENDATION: hold (CLI-only surface; revisit if summaries enter a loop context).

G10 | CROSS-RUN LESSON ADMISSION (A7) | PHASE run-end + run-start | ANCHOR docs/plans/agent-performance-avenues.md:342-347; src/core/agent.c:408-429; src/tools/edit_journal.c:209-214; benchmark/export_evidence.py:103-121 | Q_TYPE choice
WHAT CODE CANNOT DECIDE: whether a completed episode yields a reusable lesson, and which retained lesson applies to the current episode. Forge has zero cross-run learning; retained artifacts (working_state.json, session events, edit journal, failed workspaces) are never fed back (avenues.md:343-345).
WHY NOT DECIDABLE BY CODE: generality is semantic; hashing and dedup can group episodes but cannot decide that a lesson transfers.
HOST KEEPS: admission policy, the fixed control, evaluation on later unseen work; Jev never gets success authority. Overfitting guard: improvement must be shown on later, unseen work against a fixed control, and copying one lesson into many runs is not independent confirmation (avenues.md:346-347); a fixture used for tuning is development evidence permanently (docs/RUN_EFFICIENCY.md:199-207).
EXPECTED BENEFIT: quality/turns (highest ceiling, highest risk, avenues.md:345); unmeasured.
COST: one batch at episode end plus one at applicable-episode start (bounded lesson menu).
RISK: poisoning later runs with a false lesson; publishing measured deltas is constrained by MCA 2.3(f) (research doc:103).
STATUS: new-proposal | EVIDENCE: no measured baseline for cross-run learning in this repo; the M0 corpus is the stated prerequisite for any such claim (avenues.md:58-83, recommended-order row 1).
OFFLINE/FALLBACK: no library; every run starts cold (current behavior).
RECOMMENDATION: hold -- revisit only after the M0 corpus exists, with a preregistered unseen-task evaluation.

G11 | VALIDATION-ORDER PRIORITIZATION | PHASE per validation cycle | ANCHOR src/repo/validation.c:1005; src/core/agent.c:109-145; docs/research/jev-integration-2026-09-17.md:97 | Q_TYPE score
WHAT CODE CANNOT DECIDE: which planned test best discriminates the suspected defect, so which command to run first. The planner returns a fixed staged plan (validation.c:1005) and repair guidance renders at most 3 planned commands (agent.c:133-140).
WHY NOT DECIDABLE BY CODE: informativeness is semantic; the plan knows commands and stages, not which failure hypothesis a command falsifies.
HOST KEEPS: broad validation remains mandatory and complete; ordering cannot drop a command ("cannot remove required broad validation", research doc:97); Jev never decides success.
EXPECTED BENEFIT: wall (earlier failure signal); unmeasured.
COST: one score call per validation cycle (bounded command list).
RISK: reordering delays broad validation; a wrong order changes no outcome because every command still runs.
STATUS: new-proposal | EVIDENCE: doc-sanctioned future experiment (research doc:97); no measured baseline.
OFFLINE/FALLBACK: current plan order.
RECOMMENDATION: experiment (cheap and bounded; must keep every planned command).

G12 | ASK_USER NECESSITY SCREEN | PHASE per question | ANCHOR src/core/agent.c:1942-1959; src/core/agent.c:2927-2941; src/cli/interactive.c:210-232; src/tools/tools.c:353-371 | Q_TYPE noul
WHAT CODE CANNOT DECIDE: whether the model's question is necessary (not already answerable from workspace evidence) and answerable by the user. The host validates only shape (one key, <=4096 bytes, agent.c:2929-2932) and always interrupts the user (agent.c:1958; interactive.c:213-216).
WHY NOT DECIDABLE BY CODE: necessity is semantic; code can check the question's form, never whether its answer already exists in the transcript.
HOST KEEPS: the interrupt policy; advisory annotation only -- never suppress a question without a visible user-facing notice; /decline and /cancel stay deterministic host semantics (interactive.c:221-227).
EXPECTED BENEFIT: wall (fewer user stalls) + user experience; unmeasured.
COST: one noul per ask_user call (tiny payload).
RISK: suppressing a question the user needed to answer; the wrong answer changes human interaction, so hold until a user-facing policy exists.
STATUS: new-proposal | EVIDENCE: no measured baseline; questions are model-authored with no frequency bound.
OFFLINE/FALLBACK: every question surfaces as today.
RECOMMENDATION: hold.

NOTES

REJECTED SEAMS (considered, not filed):
1. Final-answer acceptance explanations -- acceptance is exactly decidable by host validation (agent.c:2717-2759 runs the verifier before final; minimal loop routes final through candidate_validate, agent.c:1960-1977), and typed answers cannot produce the explanation text (Jev never generates text).
2. Decline/cancel triage -- /decline, /cancel and empty answers are deterministic host policy (interactive.c:221-227); no guess to replace.
3. Model/profile/gpu_layers selection per run -- gpu_layers auto is deterministic arithmetic (src/core/hardware.c:553-610); model choice is a human decision and switching is blocked by one-resident-model-at-a-time (avenues.md:349-354).
4. Best-of-N as budget splitting -- closed axis (BRIEF.md:30-35); the candidate split already exists (candidate_search.c:144-147) and was measured as failing (avenues.md:50). G03 is deliberately narrower (selection among passing outputs only).
5. Host evidence presentation -- closed axis (BRIEF.md:30-31): v2, v3, v4-reverted, bounded repair, c04 elide-noop, c05 host-defects. Any new in-prompt evidence rendering needs its own frozen candidate; G02/G07 note this adjacency explicitly.
6. Edit-channel / apply_hunk / content-hash addressing -- refuted before implementation (fresh-design.md:101-130); not a typed-judgment seam.
7. Sampler variance control / repetition penalty 1.05 / last-64 -- closed axes (BRIEF.md:31-32).
8. Per-episode thinking-budget selection -- measured: 256/512/1024/1536/unbounded arms had equal Qwen success; the measured default is the 256-token ceiling (docs/plans/phase2-per-state-routing.md:253-255).
9. Task decomposition into sub-runs -- no mechanism exists; a judge cannot execute or gate; it changes deliverable shape and control policy and needs an explicit W4-class decision (fresh-design.md:407-411).
10. Rerank on the current 29-fixture population -- zero engagement measured (E3-RESULTS.md:29-31); a population problem, not a judge seam.

BASELINES (retained measurement artifacts bearing on this domain):
- benchmark/results/2026-09-17-judge-rerank/ -- v1 NOT MATERIAL (hosted latency inside the retrieval deadline; one failed cell retained); E3-RESULTS.md records the agent-level screen with zero engagement (no retrieve_context calls in the population).
- benchmark/results/2026-09-17-judge-rerank-v2/ -- post-fix MATERIAL, bar met on all three clauses (MRR 0.716 vs 0.288; +0.427; survival equal; zero rank-1 regressions).
- benchmark/results/2026-09-18-typesafe-repair-feedback/ -- arm 2 continue bar met (directional, n=4 per arm over 2 tasks); 6 raw records; engagement screen across all 29 fixtures (6 engaged, 2 inert controls, 21 excluded); population manifest f0661378 frozen; larger campaign eligible, not run.
- benchmark/results/2026-09-02-tranche2-native/ (261-run matrix; Forge 83/87 vs OpenCode 71/87, promotion gate not closed) and 2026-09-02-tranche2-repair/ (108 holdout runs; comparison rejected on a protected-file mutation; every record retained).
- benchmark/results/2026-09-08-repair-control/ (temperature 0: current Forge 3/12, minimal 0/12), 2026-09-08-repair-validation-v1/ (7/10 vs 6/10), 2026-09-08-repair-recovery/ (7/10; focused 0/3).
- benchmark/results/2026-09-09-agent-loop-all-green/ (candidate 2 at 2/8; design review; L5 fired).
- benchmark/results/2026-08-31-phase2-resweep/ and 2026-08-31-reasoning-gated/ (grammar arms, budget arms; equal Qwen success across budgets).
- docs/plans/agent-performance-avenues.md -- Forge-measured numbers recomputed from retained artifacts (11 runs at the pinned-context wall; 28 byte-identical replacements; 16.5% re-prefill).
- No measured baseline for this domain: cross-run experience/lesson libraries (A7 is unimplemented, avenues.md:342-345); judge-assessed evaluation seams (failure taxonomy, equivalence/clustering, summary faithfulness -- no judge has been applied to analysis artifacts); pre-closing judge control points; passing-candidate selection (no retained run was checked for 2+ passing candidates in one run).

UNCERTAINTY / CHECKS NOT COMPLETED:
- summary.c was read structurally (claims string, generation and cache paths), not in full; watch.c, memory.c and the rich-loop context eviction internals were out of scope for this unit.
- Candidate-selection frequency (2+ passing candidates) was not measured here; it is the falsifier prerequisite for G03 and is stated as such.
- All cost and latency figures are quoted from retained artifacts or code constants; no call was made and no model was run.
- The judge unit-test transport is the only caller of forge_judge_metrics (tests/unit/test_judge.c:124-536); this was verified by grep over src/, include/, tests/ and benchmark/.

COUNTS: seams=12; rejected=10.
