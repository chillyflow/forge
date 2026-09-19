# task_06-validation-snapshot

Read BRIEF.md first (same directory). Domain: 06-validation-snapshot.

## Scope files
- src/core/verification.c (681)
- src/repo/validation.c (1233)
- src/core/input_snapshot.c (768)

## Questions (answer with located claims)
1. Validation plan construction: per-final cost (graph walks, full scans?), staged Go/Python scheduling; process spawns per validation and whether stages short-circuit.
2. input_snapshot.c: what is captured (which files, hashing?), when (before commands, before validation, per turn?), cost per capture and per compare; bounds; whole-tree hashing frequency (a per-turn whole-tree hash is a hot cost).
3. verification.c: checks run after edits; spawns; redundant re-verification between candidate and final.
4. Mutation-during-validation detection cost.
5. Deadline/cancellation check frequency.
6. Python checks: non-importing syntax checks, unittest execution -- spawn counts.
7. Go checks: package graph usage; cached graph vs rebuilt per validation.
8. Any repeated normalization/hashing of the same bytes across stages.

## Output
- Write <RUN>/attempt_06-validation-snapshot.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
