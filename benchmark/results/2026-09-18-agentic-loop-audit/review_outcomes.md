# Review outcomes (recorded by the parent from the reviewer units)

## I1 (eight safe-now fixes) - two-stage review

- Spec compliance: GAPS=2, both test-evidence gaps with no code defect:
  (1) the text.c skip branch had no committed test (the named test uses a
  backslash root); (2) no test asserted the visible/raw tool-token invariant.
  Both closed by the fixup task (see impl_fixups_report.md).
- Code quality: APPROVED; critical=0, important=1 (= spec gap 1), minor=4:
  add an equivalence test for fg_tool_signatures; port quote_arg edge cases
  into repo tests; document command_line's buffer preconditions; comment
  wording at agent.c:3654 and the analogous still-re-tokenizing site at
  agent.c:2095-2106 (flagged out of scope).

## I2 (P0.1 + three hot-path fixes) - two-stage review

- Spec compliance: PASS, gaps=0. Acceptance criteria verified with independent
  recomputation: planner arithmetic reproduced in Python (f16 8,704; 6 GiB case
  7,168; q8_0 16,384, estimated_kv_bytes 855,638,016), defaults byte-compared
  against the pinned llama.cpp defaults, refusal enforced at config validation
  and load, TOML round-trip, docs updated. Hot-path claims all match (reusable
  batch lifecycle; single-pass tokenize; preserved-token cache with warm==cold
  md5 evidence).
- Code quality: APPROVED; critical=0, important=0, minor=6:
  1. The tokenize len+4 bound is sound for the pinned vocabularies but not
     provably universal (byte-fallback tokenizer edge); the failure mode is a
     clean FORGE_ERR_MODEL/LIMIT, never corruption. Follow-up if a
     byte-fallback vocab is ever targeted: len+8 or one exact-size retry.
  2. The single-pass allocation is an upper bound, so checkpoint probes budget
     roughly 5x prompt bytes in the worst case (tracked as skipped_budget);
     peak temporary accounting differs from the pre-fix exact-size allocation.
     Behaviour note, not a defect.
  3. Cross-source refusal: validation runs per file, so a profile selecting a
     non-f16 KV type is refused even when --flash-attn on is passed on the
     CLI; docs/CONFIG.md now states this explicitly.
  4. Doc/comment nits (search-floor wording; duplicated sentence) - fixed in
     docs/CONFIG.md by the parent.
  5. Coverage gaps: the q5_0 planner path, a K!=V planner call, and
     kv_type_matches_ggml have no direct tests; the q8_0 per-token value is
     computed with the planner's own helper (anchored by a literal 34/32
     assertion and the f16 8,704 result).
  6. Empty text now returns count==0 success instead of a sizing error; all
     callers reject count==0, so only the error text differs.
