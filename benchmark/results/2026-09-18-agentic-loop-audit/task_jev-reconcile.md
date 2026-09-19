# task_jev-reconcile
Read BRIEF.md first (same directory). Unit: merge every JEV seam candidate in this run into one
deduplicated enumeration with status and evidence tags.

## Inputs (read in full)
- All attempt_*.md in the run dir: their JEV sections (the secondary lens), plus each file's
  CLAIMS where a seam is anchored.
- docs/research/jev-integration-2026-09-17.md (existing proposal: read-routing MVP; its
  "Where to integrate" table and "A concrete first decision" section).
- include/forge/judge.h and src/judge/judge.c (what the two shipped arms actually do).
- benchmark/results/2026-09-17-judge-rerank/ (v1: NOT MATERIAL) and benchmark/results/2026-09-17-judge-rerank-v2/ (MATERIAL, bar met on all three clauses); also benchmark/results/2026-09-17-judge-rerank/E3-RESULTS.md (agent-level screen: zero engagement -- the measured populations never call retrieve_context).
- benchmark/results/2026-09-18-typesafe-repair-feedback/README.md (arm 2: repair feedback,
  continue bar met; population manifest frozen; larger campaign eligible but not run).
- docs/CONFIG.md judge section and forge.toml.example [judge] section (config surface).

## Deliverable: <RUN>/jev_enumeration_draft.md
Structure:
### A. Shipped and screened (status: implemented)
For each: seam, call site (file:line), evidence artifact, verdict, remaining gap.
### B. Shipped but unscreened or partially screened
### C. Proposed by the existing research doc (status: proposed) - keep or amend, with reason.
### D. New seams from the domain sweeps (status: new-proposal)
One entry per seam. Dedupe: the same seam arriving from 2-3 domains must appear ONCE with all
cited locations. Entry format (ASCII, forward slashes):
SEAM_ID | NAME | PHASE | ANCHOR file:line(s) | Q_TYPE | WHAT CODE CANNOT DECIDE |
HOST KEEPS (deterministic fallback / authority) | EXPECTED BENEFIT (currency: turns/tokens/wall/quality) |
COST (calls per run, payload size) | RISK (what a wrong answer costs) | STATUS | EVIDENCE |
RECOMMENDATION (adopt-now / experiment / hold / reject+why)
Rules: every entry anchored at file:line; dedupe across domains; apply the falsifier (if code can
decide it exactly, reject it); note offline-mode behavior; Jev never gets success authority.
### E. Rejected candidates
Seams considered and rejected, one line each with the reason (code decides exactly / already
implemented / violates constraints / closed axis).
### F. Coverage check
List every JEV line in the run's attempt files and where it landed (A-D or E); state counts.
Return: STATUS=<word>; seams=<int>; deduped=<int>; rejected=<int>
