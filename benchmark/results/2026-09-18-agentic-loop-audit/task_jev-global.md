# task_jev-global
Read BRIEF.md first (same directory). Unit: cross-cutting Jev augmentation seams that no single
module owns.

## Inputs
- docs/plans/agent-performance-avenues.md (A7 self-improving agent, A8 multi-model, M3 checkpoint
  tiering), docs/plans/beat-opencode-reliability.md, docs/plans/agent-loop-fresh-design.md
- benchmark/README.md and benchmark/ANALYSIS.md (measurement and evaluation surface)
- docs/research/jev-integration-2026-09-17.md sections on experiments and scope boundaries
- docs/REPRODUCIBILITY.md, docs/RUN_EFFICIENCY.md (evidence discipline)
- src/core/session.c and the agent.c metrics/event surface (what is recorded per run)
- src/repo/summary.c, src/core/candidate_search.c, src/judge/judge.c

## Questions (answer with anchored seam entries)
1. Evaluation and benchmarking: where could Jev judge augment model-based graders, task-family
   clustering, failure taxonomy, or holdout equivalence checks? Anchor each in the artifact it
   would judge and the host decision it would inform.
2. Experience and memory (A7): seams where Jev turns retained artifacts (working_state.json,
   edit journal, session events, failed-workspace evidence) into typed, reusable decisions;
   include the overfitting guard.
3. Routing and escalation (A8/M6): decisions about which model/state/profile to use next; what
   code cannot decide; what the local model currently guesses.
4. Training/calibration loop: using retained judge records to calibrate thresholds; what the
   host must record now to make that possible later.
5. Human-facing surfaces: ask_user question quality, decline/cancel triage, final-answer
   acceptance explanations - only if a typed judgment helps and stays advisory.
6. Cross-module seams (budget allocation across turns, task decomposition) that per-module
   sweeps could not see.

## Deliverable: <RUN>/jev_global.md
Same entry format as task_jev-reconcile.md section D, plus for each entry: WHY NOT DECIDABLE BY
CODE and OFFLINE/FALLBACK behavior. Cap: 12 entries, best-first. NOTES section with rejected
seams and reasons.
Return: STATUS=<word>; seams=<int>; rejected=<int>
