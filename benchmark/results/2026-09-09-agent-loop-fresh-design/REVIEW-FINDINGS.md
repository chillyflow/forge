# Adversarial review of the campaign, and what was done about it

Two independent reviews were run against the finished work: one recomputing
every published number from raw artifacts, one attacking the code changes.

## Evidence audit: 7 of 7 claims confirmed

Every published figure was independently recomputed with re-implemented
predicates rather than by re-reading this campaign's own extractor output: W0's
0/12, 1/12, 4/12 and its per-manifest cells; the 7-versus-0 false-completion
cross-tab; candidate-04's 2/12 and 2/2; the 43 elisions equalling 43 identical
replacements (which holds **per run**, not only in aggregate); both transition
rates; W4's 0/12 and its four diverged trajectory triples; candidate-05's 1/12
and its 79 → 123 ms validation cost; and gate hygiene — all three
`outcomes.json` are literally `[]`, and neither acceptance report contains a
single `screen-`, `w0-` or `w4-` prefixed run.

Nothing was refuted. Two presentation issues were raised and both are fixed: the
transition statistic is now computed by `extract_instrumentation.py` with its
denominator convention documented in code, and a 221,048/221,049 rendering
mismatch between two records is reconciled.

## The one finding that threatened the conclusion, and how it was settled

The code reviewer argued that the H-ECHO test never actually removed the
exemplar: `minimal_elided_action` preserves `assistant_content` verbatim, and a
model that restates its intended code in prose would keep the identical
old/new text in the transcript, defeating the intervention.

This is a sound hypothesis and it was checked rather than argued about. Across
all 43 elisions in the screen, the retained prose averages 890 characters and
reproduces the suppressed span in **0 of 43** cases, tested as an exact
substring or as at least half the non-blank lines of `old_text` surviving. The
model states intent in prose without restating the code.

**The exemplar was genuinely removed, and the H-ECHO refutation stands.**

## Defects fixed

| Defect | Fix |
| --- | --- |
| `fg_buf_clear` before `fg_buf_take` in the `agent_mode` emission zeroed the failure flag, so an allocation failure produced an empty payload instead of NULL and surfaced later as a misleading parse error | Take first, clear only if NULL — matching the correct pattern used elsewhere in the same file |
| `run.py` leaked its bytecode-prefix temp directory on every non-normal exit, including the common "Run output already exists" re-run guard | `try/finally` around the run loop. Fixing this initially introduced a `NameError` on that very path; caught by static analysis before it shipped |
| `run.py`'s comment claimed the shared prefix meant "the standard library compiles once" — false, since writes are disabled and the directory stays empty by design | Comment rewritten to say it exists to be missed, not reused |
| `bytecode_cache_prefix` reported the whole `-X` option string while `benchmark/common.py` reported a filesystem path under the same key | Split `VP_PYCACHE_DIR` from `VP_PYCACHE`; both now report a path |
| The `limitations` string claimed no workspace `__pycache__` "can satisfy" an import — false for an interpreter a test spawns itself, since `-B`/`-X` are process-local and the environment allowlist carries no bytecode policy | Reworded to scope the guarantee to the test process, and to state the grandchild gap explicitly |
| The silent-command annotation carried an inference ("not evidence that any test was executed") while firing in **every** mode, including the minimal control, which is defined by never receiving a corrective instruction | Reduced to a pure observation. A test now asserts it carries no inference |

## Accepted as real, deliberately not fixed

- **The retained elided ACTION is schema-invalid.** It renders as
  `apply_patch{"path": …}` with no `old_text`/`new_text`, while the tool schema
  declares all three required, and the paired observation refers to replacement
  text no longer visible. A reasonable critique of the design. Since H-ECHO is
  refuted the flag is not a promotion candidate, and reshaping it now would
  change an arm whose only measurement is already recorded. If any future work
  revives it, this is the first thing to fix.
- **Bytecode isolation was applied to the Forge harness only.** `aider.py`,
  `opencode.py` and the `run_missing*` scripts still verify without it. That is
  a real fidelity asymmetry for any Forge-versus-other comparison; this campaign
  makes no such comparison. It is recorded here as a prerequisite for the
  reliability campaign rather than fixed out of scope.
- **`bytecode_cache_isolated` is hardcoded true with no interpreter probe.**
  `-X pycache_prefix` needs CPython ≥ 3.8 and unknown `-X` keys are silently
  ignored, so an older interpreter would degrade to `-B` alone while still
  reporting isolation.
- **`replay_loop_pressure.py` does not pass `--thought-history`**, which every
  archived arm it replays includes. Pre-existing, and unrelated to this
  campaign's changes.
- **The `--minimal-agent` usage text is stranded.** Its continuation line now
  sits under `--history-bytes`. Confirmed pre-existing: at `HEAD` the line
  directly followed `--minimal-agent`, and the options between them come from
  earlier uncommitted work, not from this campaign.
- **`agent_mode` key sets differ across modes under `"version": 1`**
  (`semantic_context` absent from `bounded-repair`, `raw_history_retained`
  present only there). Pre-existing shape; this campaign changed only the two
  capability booleans.

## Status of these fixes

**They are unscreened.** Every fix above landed *after* candidate-05 was frozen
and screened, so the current working tree no longer matches candidate-05's
frozen identity. Candidate-05's evidence and binary remain immutable and its
screen stands for what it measured.

The plan permits at most two screened candidates before W4 fires, and both were
used, so no candidate-06 was frozen. These are post-campaign corrections
verified by the deterministic suites only: full GPU CTest 34/34, bounded-repair
integration 47/47, benchmark unit 30/30. No model runs were made against them,
and none of this campaign's measurements were recomputed under them.
