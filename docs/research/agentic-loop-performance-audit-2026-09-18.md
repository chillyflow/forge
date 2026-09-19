# Agentic-loop performance audit: method, findings, remediation

Status: audited and partially remediated, September 18, 2026. Repository
inspected at revision `d16e6736918808daa9c1f1993093a6dcd6ea0af8` (branch main).
This document records a read-only audit of Forge's local-model agentic loop,
its adversarial refutation, and the deterministic-layer fixes implemented in
the same session. It asserts no speedup: every unmeasured impact is labelled
unmeasured, per the standing evidence rules in
[RUN_EFFICIENCY.md](../RUN_EFFICIENCY.md) and the plans under `docs/plans/`.
The companion Jev deliverable is
[jev-augmentation-avenues-2026-09-18.md](jev-augmentation-avenues-2026-09-18.md).

## 1. What was asked, what was delivered

Question: is the implementation as performant as it can be with respect to the
local-model agentic loop, and what can still be improved without weakening the
project's evidence discipline?

Delivered:

- A 12-domain audit producing 129 located, falsifiable claims
  (74 survive refutation, 40 partial, 13 downgraded, 2 refuted), plus 11
  further claims found by the refuters themselves and 59 Jev seam candidates.
- P0.1 from [agent-performance-implementation.md](../plans/agent-performance-implementation.md)
  implemented: the KV cache type / flash-attention / offload surface, the
  enforced dependency, and a KV-aware `--gpu-layers auto` planner. This is the
  plan's highest-value item: its measured consequence was 11 all-green runs
  that died against a 14,336-token input capacity while roughly 5 GiB of VRAM
  sat unused, because `context = 16384` was a constant rather than a computed
  fit.
- Twelve verified hot-path fixes (section 3) and review-driven test additions.
- A ranked backlog (section 4), the refuted/corrected list (section 5), and
  the retained evidence (section 7).

## 2. Method and evidence discipline

Three waves of read-only subagent units (34 in total), orchestrated as a
dynamic workflow with a shared brief and per-domain task files:

1. **Domain audits** (12 units): one per subsystem slice (inference hot path,
   template/tokenization, KV/prefix/checkpoints, context planner, agent loop,
   validation/snapshots, tools/edits, process/diagnostics, repo/retrieval,
   watch/summary, runtime/config, judge client). Each produced at most 18
   located claims in a fixed format (TYPE, FILE:LINE, observation,
   recommendation, impact, change risk, confidence, falsifier), at least 8
   VERIFIED-OK coverage lines, and at most 8 Jev seam candidates.
2. **Adversarial refutation** (8 units, two rounds): every claim was re-opened
   at its cited line and attacked with counter-evidence; verdicts
   SURVIVES / PARTIAL / REFUTED / DOWNGRADE / DUP-OF, each with an
   APPLY_SAFETY tag. Refuter-found claims went through their own round.
3. **Implementation** with two-stage review (spec compliance, then code
   quality) per task, and parent-side verification.

Discipline notes: the audit re-asserted the closed axes from
[agent-performance-avenues.md](../plans/agent-performance-avenues.md) section 1
and the settled spike mechanisms (001-008); no surviving claim re-proposes one
without new measured evidence. One brief error was caught by a unit and
corrected: the first rerank screen (v1) was NOT MATERIAL; the MATERIAL result
is the post-fix v2 directory. A tool-budget incident interrupted the first
wave; all ten deliverables were recovered verbatim from the session store and
the incident is recorded in the run directory's `RECOVERY.md`.

## 3. Implemented in this session

### 3.1 P0.1 memory controls and KV-aware planner

- `[inference] cache_type_k`, `cache_type_v` (`f16`, `q8_0`, `q4_0`, `q5_0`),
  `flash_attn` (`auto|on|off`), `offload_kqv`; CLI `--cache-type-k/v`,
  `--flash-attn`, `--offload-kqv/--no-offload-kqv`; TOML round-trip.
- The llama.cpp dependency is enforced, not documented: a non-f16 KV type
  without flash attention is refused with an explicit error and a nonzero exit
  at both config validation and load; a test proves the refusal.
- The `--gpu-layers auto` planner sizes against measured free VRAM and the
  selected KV type (block-layout-derived ratios; K and V sized independently
  against the larger-cost type), binary-searches the largest fitting
  128-token context in `[minimum, requested]` instead of halving, and never
  exceeds the requested context.
- Defaults are unchanged: f16/f16/auto/true are byte-identical to the
  previous behaviour when the new flags are absent.
- Real-model confirmation on the campaign machine: a q8_0 KV + flash-attention
  GPU load allocated 952 MiB of KV against 1,792 MiB for f16 - exactly 34/64,
  matching the planner arithmetic. The planner now recommends 8,704 tokens
  (f16) where halving gave 8,192, and keeps 16,384 under q8_0.

### 3.2 Hot-path fixes (all verified by refutation before implementation)

| Claim | Location | Change |
| --- | --- | --- |
| 01#11 | `src/inference/llama_backend.c` | reusable scratch `llama_batch` in `llama_state` instead of init/free per decode call |
| 02#6 | `src/inference/llama_backend.c` | single-pass `tokenize_text_allocated` (bound widened to `len+4` for BOS+EOS/SEP, 1M post-check kept) |
| 02#9 | `src/core/text.c` | skip the provably-no-op second `replace_all` pass (dotless, backslash-free roots; dotted roots keep both passes) |
| 02#11 | `src/inference/chat_template.cpp` | capability booleans cached at create instead of re-fetched per render |
| 02#14 | `src/inference/llama_backend.c` | preserved-token ids cached in `llama_state` (fingerprint-keyed); warm generation byte-identical to cold |
| 05#7 | `src/core/agent.c` | tool-output tokens counted once; `visible_tool_tokens` still a fresh count of the final visible bytes |
| 05#12 | `src/core/agent.c`, `src/tools/tools.c` | one canonical serialization feeds both repeat-detection hashes |
| 08#9 | `src/core/agent.c` | one `fg_diagnostic_hash` computation per final-rejection path instead of two |
| 08#11 | `src/core/process.c` | `quote_arg` appends escape runs in bulk; byte-identical output (400,047-case differential harness) |
| 10#5 | `src/repo/watch.c` | empty watch batches skip the map clear and sort |
| 12#7 | `src/judge/judge.c` | record filenames carry a process-unique component (cross-process collision risk removed) |

Review-driven test additions close the two spec-review gaps (a committed test
for the path-normalization skip; an assertion that `visible_tool_tokens` is a
fresh count on both the byte-identical and truncated paths) plus signature
equivalence and `quote_arg` edge cases.

### 3.3 Verification

Build green; full CTest green on the integrated tree (36 targets; the opt-in
`checkpoint_model` real-model test skipped as normal). Review outcomes are
recorded in the evidence archive (`review_outcomes.md`): I1 and I2 both passed
spec compliance and code quality (0 critical, 0 important); the remaining minor
notes are documented there, including the tokenize single-pass bound's
byte-fallback edge (a clean error, never corruption) and the larger temporary
allocation the checkpoint probe budget now sees. Per the repository's
rules, the fixes are deterministic-layer changes with defaults preserved; the
planner's effect on a real campaign remains to be measured (P1's preregistered
bars), and no speedup is claimed here.

## 4. Verified findings not yet implemented (ranked backlog)

### 4.1 Next batch (deterministic, testable without a model)

- Render/tokenize duplication (02#1, #2, #3, #8): memoize the planner's render
  in `llama_state`; admit by collected estimates and render once; cache the
  anchor render; cursor-based `strstr` in `fg_chat_render_action_started`.
- Context planner (04#6): the admission pass is a selection sort with an
  O(n^2) rescore; same order, cheaper pass.
- Agent loop (05#4, #5): move the gofmt post-edit check into `minimal_run`
  (behavior addition, deterministic test possible); normalize validation
  summaries at the four sites the reproducibility commit missed.
- Validation (06#5, #7, #9, #11, #14): thread the deadline into plan
  construction; stop embedding the full graph in the report event; cache the
  Go graph per repository generation; add per-capture snapshot counters so the
  domain becomes measurable.
- Tools/process (07#4, #7, #11, 08#2, #3, #4, #7): reuse one TSParser for
  syntax checks; return the parsed arguments alongside the normalized action;
  avoid the triple file read in re-anchor turns; index diagnostic-record
  identity checks; bulk-append in `dg_clean_line`; cache canonicalized PATH
  directories per spawn.
- Repo/retrieval (09#1, #2, #3, #8, #12, #13): cap the LITERAL-stage scan;
  run `search_text` under a snapshot/deadline; cache prepared SQLite
  statements; stop re-serializing the trimmed document per row; index the
  tree cache lookup; page the FTS stage before scoring.
- Watch/runtime/judge (10#4, #6, 11#3, 11#4, 12#1, #5, #6): cache the
  root-identity check between events; escape event paths into the batch
  buffer directly; drop the triple GGUF open in the auto path; batch session
  events and drop the per-event flush; derive `forge_judge_budget_ms` from the
  transport's actual phase timeouts (the retained 8,016 ms record exceeds the
  4,500 ms budget it promises); build the judge body after transport
  preconditions; surface judge metrics in `metrics.json` (currently no
  production reader).

### 4.2 Needs a decision (owner call)

- `inference.threads`/`gpu_layers` defaults of 0 (01#6); repo-segment rank
  before TASK in the chat path (03#1); per-transition tools-segment rewrite
  (03#5); full-repo index after every executed command and every final action
  (09#6, #11); duplicate-rejection charging and double hashing in indexing
  (09#7, #9); git eligibility batching (09#10); summary `prepare` cost on
  cache hits (10#7); validation file/byte caps as configurable (06#8, #10);
  the rerank arm's reachable surface (12#2; see the Jev catalogue).

### 4.3 Measurement gaps (instrument before optimising further)

No retained baseline exists for: planner/render/tokenize wall time
(02#1-3), per-edit host cost (07), snapshot per-capture cost (06#14),
retrieval-stage latency (09), watch poll cost (10), and per-spawn
canonicalization (08#7). A decode-only timer is missing from `decode_ms`
(01#9), and `grammar_fallback_tokens` conflates reduced-prefix successes with
full-vocab fallbacks (01#10). Several backlog items cannot be prioritised
honestly until these counters exist.

## 5. Refuted, downgraded and corrected

What the audit got wrong, retained as evidence:

- **04#1 REFUTED** - the proposed fix for the measured 16.5% re-prefill
  re-opens the closed prefix-stability admission floor; the retained c07
  screen already measured it (prefill +6%, oscillation unchanged). The
  diagnosis stands as evidence; the fix does not.
- **05#9 REFUTED** - the run-state segment renders last (rank 5), not ahead of
  history; the claimed per-turn history re-prefill does not occur.
- **Corrections that changed recommendations**: 03#3 (context-hash early
  reject is backwards; equality is an exact-prompt fast accept), 02#7 (keep
  the yyjson pre-validation - it pins the documented `FORGE_ERR_PARSE`
  contract), 12#3 (pretty-printed records are load-bearing for
  `test_judge.c`), 12#9 (per-candidate recount 420/422 bytes), 11#2 (q8_0
  slack was overstated), 06#1/#2 (snapshot hash reuse is contraindicated by
  the recorded content-identity contract; reuse the objects, not hashes),
  07#1/#2 (the edit-artifact IO and budget are the documented evidence
  contract; changes need a contract decision), 08#8 (the proposed fast path
  would reclassify scalar JSON lines), 10#1 (reusing the previous fallback
  snapshot would blind the monitor), plus documented downgrades where the
  behaviour is a recorded deliberate decision (03#4, 03#6, 05#8, 05#11, 06#10,
  06#13, 10#3, 10#9).

## 6. Closed axes respected

The audit re-confirmed, without re-proposing: host evidence presentation (the
v2/v3/v4/bounded-repair/c04/c05 family), thought-history retention, sampler
variance control, repetition penalty 1.05/last-64, prefix stability admission
floor, exact-argv dedup, best-of-N budget splitting, the retraction family,
CUDA Unified Memory. The settled sampler mechanisms (greedy grammar fast path,
reduced-prefix fallback, grammar-forced-run elision, first-byte prefilter,
grammar-after-topk, N-way batched decode, blocked speculative decode) were
checked and not touched.

## 7. Provenance and retained evidence

Audit run `wf_forge_perf_978e2fd5`: 12 domain audits, 8 refuters, 2 Jev
producers, 2 Jev refuters, 5 completion/producer units, 2 implementers, 5
review units. Retained in `benchmark/results/2026-09-18-agentic-loop-audit/`:
the 12 attempt files, 8 refute files, `merged_claims.tsv` (129 claims with
verdicts and apply-safety tags), the Jev draft/global/refute files, the brief,
task files and the recovery note. Review outcomes: I1 spec review PASSED with
two test-evidence gaps (closed by the fixup task); I1 quality review APPROVED;
I2 spec and quality reviews recorded in the same directory.
