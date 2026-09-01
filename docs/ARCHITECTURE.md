# Architecture

The CLI is an adapter over `Forge::forge`. The implementation follows the local
inference design from the [original plan](https://chatgpt.com/share/6a917dda-943c-83e9-974c-7cfcfd6dc7bf).

```text
CLI / embedding program
          |
    agent state machine ---- session event log / replay
          |
    context planner -------- Tree-sitter Go / SQLite repository index
          |
    direct llama.cpp ------- persistent token sequence / KV prefix
          |
    constrained action ----- policy ----- native tools
          |                                  |
          +----------- compact result -------+
```

## Inference and cache correctness

Each model owns a llama model, inference context, tokenizer, and evaluated token
sequence. Generation applies the model's supported chat template, tokenizes the
entire requested prompt, and compares **token IDs**, not text lengths, against
the previous sequence. Only the common sequential prefix is retained. The tail
is removed with `llama_memory_seq_rm` and evaluated again. Exact prompt hits
re-evaluate the last token to obtain valid logits.

Cache accounting uses actual token counts: `prompt_tokens = cached_tokens +
prefill_tokens`. Generated tokens are appended only after successful decode.
An error clears the physical cache. Recurrent and hybrid models conservatively
disable partial-prefix reuse. Arbitrary file KV blocks are never spliced.

llama.cpp backend registration has process-wide state upstream. Forge has no
global mutable session state; concurrent initialization or generation on the
same model is not supported. The selected model owns one active context.

Explicit [checkpoint handles](CHECKPOINTS.md) own independent host copies of
that context's sequence state. Capture/restore checks exact token prefixes,
model-instance identity, repository generation, bounds and physical positions.
Restored exact hits still recompute the final token for logits. Handles do not
contain sampler state, resume a partial generation, survive model reload, or
provide an automatic semantic-boundary cache/eviction policy.

Greedy generation first checks the highest-logit token against the grammar. If
allowed, it is the same maximum the full grammar mask would select. Otherwise,
the full vocabulary is masked and sampled. Every accepted token advances grammar
state exactly once. Stochastic sampling retains the full mask. `--grammar-first`
disables this optimization for comparison; sampling time and path counts are
recorded separately (`decode_ms` includes sampling and streaming overhead).

## Logical context

Segments have stable IDs, kind, generation, content fingerprint, priority, token
cost, and dependencies. System/tool/task/working-state segments are pinned.
The latest tool result is pinned with its parent action. Remaining segments are
selected by utility per token, including a recency term. Prompt ordering is
stable: system, tools, repository map, task, chronological history, then volatile
working state. Final rendered prompt size is checked with the actual tokenizer.

Known patches, launched commands and native external notifications conservatively
invalidate all source-bound segments: aliases and case-folding prevent a path
spelling from proving which views depend on the changed object. The context API
supports selective dependency invalidation, but the runtime needs portable file
identity before safely narrowing these invalidations. Commands trigger a full
index refresh. A mutation of unindexed inputs, or a launched command whose
effects are unknown, still advances the generation when indexed bytes match.
Context
compaction retains typed working state and drops lower-value history. Multiple
dependencies share one budgeted closure; stale inputs invalidate their transitive
dependents. Immutable system/tool/task segments cannot be mutated. Versioned
logical snapshots preserve text, metadata, selection and dependencies and can be
restored using the same token counter. They do not contain physical KV state.

The noncryptographic FNV fingerprint is for cache invalidation, not security or
artifact integrity. Download/benchmark provenance uses SHA-256 separately.

## Repository intelligence

Git file enumeration honors ignored files when Git is available. A conservative
walk is used outside Git. Files up to 2 MiB in supported text formats are indexed;
Go is parsed with Tree-sitter. SQLite transactions update changed files only,
delete vanished files, and persist repository generations. Full scans enumerate
and hash candidates. Explicit path batches inspect only named candidates and
their current Git eligibility after a full baseline. Observed changes to
unindexed inputs also advance the generation.

A bounded, per-handle source/tree cache supplies an edited copy of the previous
Go syntax tree to Tree-sitter. Cache publication follows the SQLite commit;
rollback cannot publish an uncommitted tree. Source, exposed-AST, declaration
and per-symbol hashes are syntactic fingerprints, not resolved semantic identity.
Cache counters include failed work and bound retained source bytes/nodes, not
Tree-sitter's opaque allocation sizes or process RSS. See [INDEX.md](INDEX.md).

The [native watcher](WATCH.md) uses recursive inotify enrollment on Linux,
FSEvents on macOS, and one recursive ReadDirectoryChangesW root handle on
Windows. Loss, directory topology and ignore-policy changes trigger full scans
and reopening where required. The coordinator starts watching before its index
baseline and drains after full scans; changes observed during a scan require a
follow-up scan. Agent runs fall back to bounded all-input snapshots if native
coverage cannot be established. An incomplete fallback fails closed.
Notifications are not an atomic snapshot or validation evidence.

Declarations, signatures, byte spans, identifier occurrences, and imports are
stored. Go identifier occurrences are syntactic: shadowed names and identically
named symbols can appear together. No type checker, cross-package name resolver,
or sound call graph is claimed. A conservative package import graph includes test
imports, nested modules and reverse dependents for staged validation. Unresolved
inputs trigger explicit fallback reasons and broad checks. Other supported text
languages have literal search, not AST symbol navigation.

## Tools and execution

One declarative registry defines field types, capability, and prompt description.
The GBNF action grammar is generated from it. Every action envelope accepts an
optional leading `thought` string, bounded at 2048 bytes: a free-text reasoning
channel written before the constrained action. Thought is never executed and
grants no authority. It is retained verbatim in session evidence — the raw
response is emitted to the session log before any normalization or stripping —
but by default it is dropped from the stored ACTION segment and so never
re-enters a later prompt. Host-enforced switches control the channel:
`--no-thought` removes the field from the generated grammar and schema and
rejects any action still carrying one (the §32 ablation control, valid for
unconstrained backends too), `--thought-history` re-injects the thought into
later prompts, `--thought-decode-only` states the default explicitly, and
`--thought-required` rejects an empty or absent thought. Both are enforced by the
host after parsing, not only by the grammar, so they hold for an unconstrained
backend as well. Retention defaults off
on measured grounds: with reasoning actually elicited it cost three of ten
benchmark tasks and 2.3x the prompt tokens, and doubling the turn budget did not
recover them.

`--thought-routed` uses llama.cpp's lazy grammar sampler instead of putting the
thought inside the action object. Output is unconstrained until a JSON object
starting with `tool`, `memory`, or `final` triggers the generated action grammar;
the host then bounds and validates the preceding UTF-8 text and normalizes it
into the ordinary thought field. The trigger needs steering to elicit
anything: measured without it, the model opened the action object on its first
token in every routed action, and merely banning `{` tips greedy decoding into
prompt echo that burns the whole turn budget (10/10 benchmark limit deaths).
The llama backend therefore force-decodes `FG_THOUGHT_CUE` (`Thought: `) at the
start of every routed generation, so the greedy continuation is a reasoning
sentence; for the next `FG_THOUGHT_MIN_PREFIX_TOKENS` (32) sampled tokens
(bounded by a quarter of the turn budget) every token containing `{` is
excluded via logit bias so some reasoning text must follow, and
end-of-generation tokens stay excluded until the action object actually begins,
so a routed generation cannot end actionless; the turn's token budget is the
backstop. The cue is scaffold — streamed and recorded in the raw response, but
stripped by the host before the prefix is bounded, validated, or normalized, so
it never satisfies `--thought-required` on its own. Required routed thought
remains a host validation gate on top: a whitespace-only or cue-only prefix
still fails it. A cue that cannot be force-decoded (untokenizable, over the
16-token cue bound, or rendering an action-opening token) fails loudly rather
than silently arming the bans without steering text, and a turn budget too
small for the cue is an ordinary limit failure. `--thought-cue` replaces the
cue; an empty cue drops the cue and the `{`-ban window together, since the ban
without steering text is the measured prompt-echo configuration.

Routed decoding is a bounded state machine, not an open-ended free phase. The
reasoning phase ends at the think budget (`--thought-budget`, default at most
256 tokens and reduced to half the remaining turn budget near exhaustion;
`--no-thought-budget` is the unbounded ablation): if the action has
not begun by then, the never-triggered lazy grammar sampler is replaced in the
chain by an eager grammar over the same GBNF, so every subsequent token is
grammar-sanctioned and the action opens under full tool constraints, with all
three envelope alternatives still available. If the swap lands mid-character,
host normalization drops the stranded trailing bytes instead of failing the
turn. After the swap, leading-whitespace tokens are biased out until the first
action-progress token, closing the padding loophole without removing legal
whitespace later in the grammar. Once the action region begins —
at the lazy trigger, the forced swap, or token 0 under an eager grammar — the
host ends generation at the first token that completes the action object
(objects can only close on a `}` piece, and the completeness scan starts at
the recorded action offset, never inside reasoning prose, which may legally
contain complete JSON objects that never armed the grammar). Waiting for an
end token instead would leave the post-close whitespace tail legal until the
budget dies. Per-state metrics (`think_tokens`, `forced_actions`,
`action_stops`) record the machine's behavior in `metrics.json`;
`think_tokens` is tokenizer-relative and never comparable across models.
`forced_action_progress_tokens` proves a forced swap produced an action token.

The action state is further classified as envelope selection, tool arguments,
patch content, final prose, or memory content. Selection and ordinary arguments
use a deterministic sampler policy; patch and final prose retain the configured
sampling policy. Inline-thought envelopes are classified from the top-level
action object, so strings inside the thought cannot impersonate an action key.
The generated GBNF remains active through every substate, and the corresponding
token counters are serialized in `metrics.json`. JSON is parsed by yyjson and
all required field types/cardinality are validated before policy or execution.

For chat templates that natively think, `--thought-native` uses a cue-free lazy
grammar and requests `enable_thinking`; `--disable-thinking` supplies a
grammar-matched safe control. The same template switch is available as
`--enable-thinking` / `--disable-thinking` and `model.enable_thinking`.

`apply_patch` requires one unique exact match. `apply_hunk` instead replaces an
inclusive line range only when the caller supplies the full-file SHA-256 emitted
by `read_file`; this makes narrow edits unambiguous and rejects stale line
coordinates. Both tools stage output in a sibling file, record the proposed
bytes, check Go syntax in-process, recheck the old content, and atomically
replace the file. A syntax-invalid Go candidate is recorded as aborted and
never replaces the target. Neither tool is a unified-diff parser. Empty old text
creates a missing file only through `apply_patch`.

The process runner uses `fork/exec` and process groups on POSIX, or `CreateProcess`
and kill-on-close Job Objects on Windows. It captures stdout/stderr separately,
drains output after reaching its byte cap, and kills descendants on completion or
timeout. It passes a limited environment. See the security document for limits.

[Diagnostic adapters](DIAGNOSTICS.md) normalize supported Go test/vet/lint,
compiler text/legacy GCC JSON, Cargo/rustc JSON, Rust test text and pytest text.
They retain observed locations and labelled values without inventing test
outcomes or interpreting ambiguous assertion operands. Limits, malformed input
and omitted records remain explicit. Exact captured streams are kept separately;
a shortened view never replaces them or proves that a command succeeded.

## Memory and work accounting

[Scoped arenas and slices](MEMORY.md) expose explicit ownership and bounds.
The agent parses each generated action in a reusable, 64 MiB committed-byte
arena; data that outlives the turn is copied. `read_file` uses an owned binary
file view and bounded slices, not a mutable mapping. Other allocation paths
have not all migrated to arenas.

Session metrics report arena high-water bytes, actual index attempts/cache
counters, observed filesystem events, watcher reopens and rejected stale
generations. `index_ms` includes coordinator enrollment/poll/index work and
direct post-tool indexing; validation work is timed under `validation_ms`.
Retained source/node counters are not peak RAM or VRAM measurements.

## State machine and durability

Runs transition through init, prefill, generating, tool request/running/result,
recontextualization, and done/error. Events are flushed before proceeding.
Random session IDs avoid collisions. JSON-lines replay checks schema version and
sequence continuity and does not call the agent or process runner.

Limits cover turns, input/generated tokens, per-turn output, file sizes, captured
output, command runtime, and total wall time. Repeated action plus repository
generation and normalized diagnostic signatures trigger a warning and eventually
stop loops. Identical patches are conflicts. Typed model memory is kept separate
from observed changes, failures and generation-bound validation evidence.

The coordinator checks before generation, after generated output, before final
verification and after verification. Observed external changes discard the
generated action/final and rebuild source context; the discarded inference
still counts toward token and turn limits. Native notification delivery and
filesystem reads are not atomic against concurrent writers.

Before accepting a final answer after edits, the host runs the deterministic
Go/Python validation plan with the same process policy and deadlines. Go uses
its syntactic package graph; Python uses non-importing, in-memory compiler
syntax checks, filename-related tests, broad pytest discovery, and explicit
execution of every indexed unittest file. A missing Python runtime blocks an
applicable plan. Validation stops at the first failure and returns diagnostics
for repair; mutation during validation invalidates the result. Other languages
and explicit `--no-auto-validation` runs have no such automatic proof. Even
passing checks does not prove arbitrary task correctness.
`bench` additionally runs its explicit independent verification command.
