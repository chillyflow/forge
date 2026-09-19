# RECOVERY.md - budget-block incident and recovered deliverables

At ~18:40-19:09 on 2026-09-18 the Hermes global daily budget gate (5.00 USD cap) blocked
every tool call for all wave-1 units and the parent, so units could not write their attempt
files. The parent recovered all 10 units' final summaries verbatim from the session store
(state.db, async_delegations.result_json) and wrote them into this run directory:

- attempt_02-template-input.md .. attempt_08-process-diagnostics.md, attempt_10-watch-summary.md
  -- complete content as delivered by the units (fenced deliverable, verbatim).
- attempt_01-inference-hotpath-recovered.md -- draft claims; unit 01b finalizes.
- attempt_04-context-planner-recovered.md -- prose findings; unit 04b converts and finalizes.
- attempt_09-repo-retrieval-recovered.md -- candidate findings; unit 09b finalizes.

No repo file was touched by any wave-1 unit (all read-only). Wave 2 adds final files for
01, 04, 09; first files for 11-runtime-config and 12-judge-client; and refutation of
02, 03, 05, 06, 07, 08, 10. Partial files keep the -recovered suffix; final files use the
plain attempt_<domain>.md name.
