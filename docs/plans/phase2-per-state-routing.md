# Phase 2 — per-state decode routing (§32 literal) — plan (written 2026-08-31)

Self-contained plan. Goal: make the routed generation's decode phases an explicit,
bounded, testable state machine, resolving the mechanism defects phase 1 and the
tier-1 sweep recorded, and unblocking natively-thinking models mechanically.
Branch `codex/thought-channel`, local only — NEVER push. The design was
adversarially reviewed (three lenses: llama.cpp mechanics, host protocol,
scope/conventions) before implementation; the fixes that review forced are
folded in below and marked (review).

## Requirements this phase answers

1. **Early stop on action completion** (tier-1 sweep requirement, recorded for
   routed generation): the backend samples until an EOG token or budget
   exhaustion even after the action object is syntactically complete. Verified
   against the pinned llama.cpp: after forge's root rule completes, the grammar
   still permits unlimited whitespace tokens besides EOG (`root ::= ws (...)
   ws`; empty-stack rejection intersected with the ws stack), so a model that
   does not emit EOG promptly can legally pad until the budget dies.
   **Decision (review): early stop applies to eager-grammar (baseline)
   generations too, not only routed ones.** This extends the recorded
   requirement — deliberately: the defect class is identical in eager mode, and
   stopping only routed arms would make within-model token comparisons
   mechanism-asymmetric. Consequence: every grammar arm's decode mechanics
   change, so the stale-evidence statement and the follow-up re-sweep cover all
   grammar arms, not just routed ones.
2. **Bound the reasoning phase** (elicited-sweep turncap control): routed
   failures are within-turn token-budget deaths — reasoning past the turn
   budget before the action begins. Llama-3.1-8B lost 10 of 20 routed fixtures,
   each dying at turn 1 having generated the full 2048-token budget without
   opening an action.
3. **Native-thinking groundwork** (tier-1 plan, "phase-2 work and phase 2's
   acceptance test"): the 2048-byte routed-thought cap is run-fatal; the forced
   `Thought: ` cue is wrong-distribution for models that open `<think>`; no
   `enable_thinking` template control exists at the C API of the pinned
   llama.cpp (verified: `common/chat.h` machinery is not compiled into forge's
   build; host prefill is the only mechanism). Special-token text divergence
   stays a recorded caveat.
4. **Silent cue-skip landmine** (2026-08-31 audit): if the cue cannot be
   force-decoded, the backend silently proceeds with bans armed — the
   documented 10/10-death configuration.

## Design

### Decode-state machine (llama backend)

States, each with its own sampler configuration — this is the §32 routing:

- `CUE`: host scaffold force-decoded. Hardened (review): a cue that cannot be
  tokenized or exceeds the 16-token cue bound is `FORGE_ERR_MODEL`; a cue that
  does not fit the remaining turn budget is `FORGE_ERR_LIMIT` (so ordinary
  global-budget exhaustion stays in the `limit` failure taxonomy); and a custom
  cue is additionally validated at token-piece level — any cue token whose
  rendered (special=true) piece contains `{` is rejected, because byte-level
  screening alone is defeated by NFKC-style tokenizer normalization.
- `THINK_MIN`: EOG ban + action-opening ban (existing suppress window,
  `FG_MIN(FG_THOUGHT_MIN_PREFIX_TOKENS, max_tokens/4)`, additionally clamped to
  the think budget). The brace-ban window exists only when a cue was actually
  force-decoded (review): an empty cue with the brace ban armed is byte-for-byte
  the refuted prompt-echo death configuration, so `--thought-cue ''` drops the
  window and keeps only the EOG ban.
- `THINK`: EOG ban only; the model may open the action at any time (lazy
  trigger arms the grammar).
- `ACTION`: grammar armed; EOG legal; **early stop** — on every sampled token
  whose piece contains `}`, the host checks completeness and ends generation
  when the action object is complete. Text-based, not a grammar-state probe:
  the lazy grammar's apply is a no-op if the trigger has not fired, so a
  grammar probe could read "accepted" on an unarmed grammar. Scoped (review):
  the completeness scan starts at the recorded action-begin offset (the host
  trigger-mirror match position, the forced-swap boundary, or offset 0 in eager
  mode) — never the whole buffer, because prose in the THINK region can legally
  contain a complete JSON object (e.g. one carrying a `thought` key) that never
  armed the grammar; a whole-buffer scan would stop generation mid-real-action
  and kill the turn. The scoped scan is also what bounds the check's cost. The
  stopping token's piece is appended and streamed before the break, unlike the
  EOG break, so the streamed output keeps the closing brace.
- Transition `THINK → ACTION` happens either when the action begins naturally
  (existing `action_begun` host scan, evaluated before the budget test on each
  token so a just-armed grammar is never replaced) or at the **think budget**:
  the never-triggered lazy grammar sampler is replaced in the chain by an eager
  grammar sampler over the same GBNF (chain surgery is established practice;
  the fresh eager grammar starts at root, so every subsequent token is
  grammar-sanctioned — tool constraints retained, all three envelope
  alternatives open, which force-decoding an opening literal would not allow).
  The EOG ban is dropped at the swap; the eager grammar masks EOG until the
  object closes. Surgery details (review): ban removals are conditional on the
  bans actually being present; selector samplers are popped and re-added in
  their original order so the eager grammar sits ahead of them; the local
  grammar pointer used by the greedy fast path is re-pointed to the eager
  sampler. Limitation (review): root's leading `ws*` keeps whitespace padding
  grammar-legal after the swap, so a degenerate model can still burn the
  remaining budget actionless; the turn budget is the backstop and the failure
  stays classified `within_turn`.

Pure, always-compiled routing logic in `src/inference/routing.c` (declared in
`internal.h`, unit-tested without a model): `fg_action_begin` (returns the
match position so the backend can record the action offset),
`fg_action_complete`, `fg_think_bounds` (min/cap arithmetic incl. clamping).
The llama backend consumes these; only sampler surgery stays backend-side.

### Configuration surface (CLI-only, like the rest of the reasoning channel)

- `--thought-budget N` — max sampled reasoning tokens before the action grammar
  is enforced (routed mode). N in [1, 2147483647]; 0 is rejected (it is the
  unset sentinel, and a zero budget with `--thought-required` is a guaranteed
  first-turn death — contradictory arms fail loudly). Default when absent:
  **half the per-call token budget** (`max_tokens/2`, where max_tokens =
  `FG_MIN(output_reserve, remaining global budget)`), a chosen, unmeasured
  fraction recorded as such — it mirrors the existing `max_tokens/4` suppress
  scaling, and it shrinks near the global budget end, which makes
  `think_tokens` a per-turn-varying bound (disclosed metrics confound).
- `--no-thought-budget` — unbounded reasoning (phase-1 behavior, ablation).
  Combined with `--thought-budget N`: last flag wins (checkpoint-cache flag
  precedent). Represented as a separate bool, NOT a SIZE_MAX sentinel (review:
  the CLI number parser accepts 18446744073709551615, which would silently
  alias the sentinel).
- `--thought-cue TEXT` — replace the forced `Thought: ` cue; empty string
  disables the cue (groundwork for natively-thinking models whose reasoning
  opens with their own markers; on non-thinking models an empty cue is an
  elicitation ablation — the brace-ban window is dropped with it, see above,
  so the refuted death configuration cannot be re-created). A cue containing
  `{` is rejected at config validation (byte level) and again at the backend
  (token-piece level).
- All three require `--thought-routed`, enforced at all three established
  sites: CLI diagnostics, `forge_config_validate`, and `forge_agent_create`
  (review: the third site keeps library embedders from arming a silent no-op).
  This is stricter than `--thought-history` (which is accepted as a no-op
  without routing) — deliberate, for arm integrity.
- Internal plumbing: the generate seam's `grammar_trigger` argument becomes a
  small `fg_decode_policy` (trigger, cue, think budget, unbounded flag); the
  scripted backend continues to ignore it. The configured cue also reaches host
  normalization (review): `routed_action_text` must strip the configured cue,
  not the compile-time default, or scaffold leaks into the recorded thought.

### Host normalization change

A routed reasoning prefix longer than `FG_THOUGHT_MAX_BYTES` is **truncated at
a UTF-8 boundary** instead of failing the run. Order of operations (review):
first trim a trailing incomplete UTF-8 sequence of at most 3 bytes (a
budget-forced swap can land mid-character; the eager grammar then rejects the
continuation bytes, so the stranded lead bytes would otherwise turn every such
swap into a run-fatal parse error), then validate the FULL prefix with
`fg_utf8_valid` (invalid UTF-8 beyond the cap must still fail —
`fg_utf8_prefix`'s contract presumes validated input), then truncate. The raw
text is already session evidence before normalization. Inline (non-routed)
thought validation is unchanged. `--thought-required` still rejects an empty
prefix. The combined size/UTF-8 error message is split: the surviving PARSE
branch no longer claims a byte bound it does not enforce.

### Metrics / evidence

New `forge_metrics` counters (appended, following the checkpoint-counter
precedent — no schema_version bump; consumers tolerate additive fields),
written to `metrics.json`: `think_tokens` (sampled before the action began;
tokenizer-relative, never comparable across models), `forced_actions`
(think-budget grammar swaps), `action_stops` (early stops on completion —
expected to approximate turns on every grammar arm; a mechanism-visibility
counter, not an anomaly signal). Scripted-backend runs report structural zeros.

### Benchmark harness

New arm `thought-routed-unbounded-decode-only` →
`['--thought-routed', '--no-thought-budget', '--thought-decode-only']` for the
same-binary A/B of the think budget (review: modifier placed before the
terminal `-decode-only` retention suffix, per the composed naming rule). All
existing grammar arms change decode mechanics (budget + early stop); published
sweeps predate the mechanism and every grammar arm needs re-measuring on the
new binary. `consolidate_arms.py` census is unaffected while the cue default is
unchanged; a future custom-cue arm must extend `classify()` (and
`analyze_failures.py`'s CUE constant goes stale harmlessly).

### Documentation deliverables (same feat commit)

- `src/cli/main.c` usage() lines for the three flags, established style.
- `docs/CONFIG.md:87-90` — the reasoning channel's whole-surface sentence
  gains the three flags; the channel stays CLI-only.
- `README.md` routed paragraph (152-169) and `docs/ARCHITECTURE.md` mechanism
  paragraph (133-155): budget, forced swap, early stop, truncation; closing
  honest-scope sentences updated.
- `docs/DESIGN_CHECKLIST.md` §32 row: rewritten per the update rules
  (implementing revision, exact source/test locations, test results), quoted
  routed numbers marked as predating this mechanism, closing gap statement:
  thinking versus action is now an explicit bounded decode-state machine with
  tool constraints retained through the budget-forced grammar swap, but tool
  selection, tool arguments, patch, and final-response decoding still share
  one undifferentiated action grammar with no per-state sampler policy, and no
  measurement yet motivates routing them separately — §32 remains Partial.
- `benchmark/README.md`: new arm, mechanism note, stale-evidence sentence for
  pre-budget published results, `think_tokens` tokenizer-relative caveat.
- `include/forge/forge.h`: struct comments for the new config fields and
  counters.

## Explicitly out of scope (recorded, not silently dropped)

- **Token-0 grammar amputation of native reasoning in baseline arms** (tier-1
  requirement). Not addressed: a "thinking-safe baseline" would need the lazy
  grammar, at which point it is no longer the baseline arm. Consequence,
  stated plainly: for natively-thinking models only routed arms are
  mechanism-honest, so phase 2's acceptance test on such a model is a
  routed-mechanism smoke, not a baseline-vs-routed lift measurement; the lift
  question for thinking models stays structurally open until a thinking-safe
  baseline mode is designed.
- Per-substate sampler parameters inside the action (tool selection vs
  arguments vs patch vs final prose): no measurement motivates different
  sampling policies there yet; the state machine is the seam they would plug
  into. §32 stays Partial until measured.
- Agent-state-conditioned routing (route by turn kind / after-failure): same.
- `enable_thinking` template control: impossible at the pinned C API; revisit
  at the next llama.cpp pin bump.
- Reasoning-gated fixtures (the lift question): fixture design, separate work.
- Pre-existing recorded caveats surfaced again by review, unchanged by this
  phase: embedded NUL truncates the host text scans; special-token
  piece-rendering asymmetry can desynchronize the host trigger mirror from the
  llama-side trigger (both end in caught errors); a lone UTF-16 surrogate
  escape is grammar-legal but yyjson-invalid, so such a turn still runs to the
  budget and fails at normalization exactly as before.

## Verification

- Unit: routing.c pure functions + the UTF-8 trailing-trim helper in
  `test_core.c`; CLI contradiction and truncation cases in `test_cli.py` (the
  2049-byte-prefix case flips from refusal to truncation-success asserting the
  recorded 2048-byte thought).
- Full ctest, Debug + Release, `build/` (FORGE_WITH_LLAMA=OFF) and `build-gpu/`
  (prebuilt CUDA DLLs) on Windows.
- Rig smoke (WSL2, per tier-1 rig map): one routed fixture on
  Qwen3-Coder-30B-A3B (early stop fires, pass preserved) and routed fixtures on
  Llama-3.1-8B (forced action replaces the turn-1 actionless 2048-token burn).
- Full re-sweep of all grammar arms is the follow-up data commit, not this
  change.

## Execution update — 2026-08-31

Every recorded follow-up above is now implemented or measured:

- The scheduled 100-record all-arm sweep is checked in at
  `benchmark/results/2026-08-31-phase2-resweep/` with exact binary, model,
  fixture, sampling, platform, Go, and GPU identity verified across arms.
- A forced swap now biases out leading grammar whitespace until the first
  action-progress token. The Llama smoke records a 1/1 forced transition,
  progress token, and completed action.
- Literal §32 routing now distinguishes selection, arguments, patch, final,
  and memory substates. Structured states are deterministic; patch/final prose
  retain configured sampling; the grammar remains active throughout.
- The pinned llama common Jinja renderer supports `enable_thinking` through
  TOML and CLI. `--thought-native` and its `--disable-thinking` control provide
  matched lazy-grammar arms; the Qwen3-4B smoke exercises both.
- Six reasoning-gated fixtures and their one-line oracles are covered by the
  benchmark integration test. Qwen3-Coder and Devstral show no routed success
  lift in this pilot; details are in
  `benchmark/results/2026-08-31-reasoning-gated/`.
- Budget arms at 256, 512, 1,024, 1,536, and unbounded had equal Qwen success.
  The measured default is therefore a 256-token ceiling, the cheapest bounded
  arm, with explicit controls retained.
- The census and failure-analysis classifiers now accept custom cues and native
  thinking instead of assuming the default scaffold.

The earlier "explicitly out of scope" section is retained as the historical
plan boundary; this update is its disposition and supersedes its statements
that these items remain open.
