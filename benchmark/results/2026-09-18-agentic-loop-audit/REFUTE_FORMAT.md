# Refuter instructions (read with BRIEF.md)

You are a REFUTER. Your job is to BREAK the claims in the input attempt file(s), not to
confirm them. Adjudicate every claim line in the CLAIMS section (ignore VERIFIED-OK and JEV
sections except to report a falsified VERIFIED-OK line under NEW:).

Method per claim:
1. Re-open the cited FILE:LINE. A wrong line number is itself a finding (give the correct
   one). If the code does not say what the claim says, the claim is REFUTED or PARTIAL.
2. Check the claim against the CLOSED AXES and SETTLED mechanisms in BRIEF.md. A claim that
   re-proposes a closed axis without new measured evidence is REFUTED (cite the doc).
3. Deliberate-decision check: grep docs/, spikes/, and git log for a rationale that settles
   the point. If found, DOWNGRADE (settled by design) unless the claim's evidence
   contradicts the recorded measurement.
4. Consequence check: what breaks or needs re-running if the recommendation lands? Name the
   evidence class (measured-perf-baseline | gpu-suite | unit-tests | frozen-candidate |
   engagement-screen | none-observable). If applying it needs a decision the claim did not
   acknowledge, APPLY_SAFETY = needs-decision.
5. Verify complexity and count claims yourself (if it says O(n^2) or "twice", check the loop).

Output file (one line per claim; ASCII only; forward slashes):
CLAIM_ID|VERDICT|COUNTER_EVIDENCE|CORRECTED_RECOMMENDATION|APPLY_SAFETY
- CLAIM_ID: <domain>#<N> exactly as numbered in the attempt file (e.g. 02-template-input#7)
- VERDICT: SURVIVES | PARTIAL | REFUTED | DUP-OF <id> | DOWNGRADE
- COUNTER_EVIDENCE: the exact thing you read that decides the verdict (file:line or doc
  quote). For SURVIVES, state the check performed and what it showed.
- CORRECTED_RECOMMENDATION: only when PARTIAL or DOWNGRADE changes the action; else '-'.
- APPLY_SAFETY: safe-now | needs-verify(<suite|re-measure|re-cook|smoke>) | needs-decision

Then:
NEW:  -- claims found while checking, in the attempt-file claim format
        N|TYPE|FILE:LINE|OBSERVATION|RECOMMENDATION|IMPACT|CHANGE_RISK|CONF|FALSIFIER
NOTES: -- uncertainty, checks you could not complete, files skipped.

Rules: read-only on the repo; no builds, tests, or model runs; terminal read-only commands
allowed (grep, git log/show). Write only your refute_*.md file in the run dir. Do not edit
the attempt file. Adjudicate EVERY claim in the input; do not stop early.
