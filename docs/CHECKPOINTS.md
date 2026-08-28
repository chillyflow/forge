# Physical prefix checkpoints

`forge/checkpoint.h` provides explicit in-memory capture and restore of a model's
computed sequence state. A checkpoint is separate from the context DAG and its
JSON snapshots. Context JSON describes prompt selection; it does not contain KV
state or establish that a model has computed those tokens.

## API and ownership

```c
forge_checkpoint_options options = forge_default_checkpoint_options();
options.repo_generation = current_generation;
options.max_state_bytes = 256u * 1024u * 1024u;

forge_checkpoint_stats stats;
forge_checkpoint *saved = forge_checkpoint_save(model, prefix_prompt, &options, &stats, &error);
if (saved) {
    /* Other prompts may now replace the live model state. */
    forge_status status = forge_checkpoint_restore(model, saved, &options, &stats, &error);
    /* Inspect status before relying on the restored state. */
    forge_checkpoint_destroy(saved);
}
```

Save applies the model's chat template, tokenizes the whole supplied prompt, and
prefills it through the same implementation used by normal generation. It does
not sample or advance a grammar or sampler. Save deliberately replaces the live
prompt state. If prefill succeeds but a later allocation, cap, parent, timeout,
or serialization check fails, that prefilled state may remain active.

The returned opaque handle owns its token IDs and host state bytes. Separate
handles remain usable after other captures/restores. Handles can outlive the
model and can always be destroyed without accessing it. Restore requires the
original loaded model instance: a fresh random 128-bit instance nonce prevents
a destroyed/reloaded model at the same allocation address from matching. A
checkpoint cannot be moved between two independently loaded copies of the same
GGUF. The current model/context/template configuration is fixed within that
instance.

`forge_checkpoint_get_info` reports an instance-local monotonically increasing
ID, optional parent ID, token range `[0, token_end)`, repository generation,
untemplated prompt hash (`context_hash`), token-ID hash, and state byte count.
The actual token IDs are retained; hash equality is not used to authorize reuse.
`valid` means a complete physical copy was captured. It does not mean repository
files remain current or that a subsequently loaded model can use the handle.

`options.parent` is optional on save. It must belong to the same instance, and
its actual token sequence must be an exact prefix of the child's. The child
contains a complete independent copy, not a delta; destroying the parent does
not invalidate the child. Parent selection is not an automatic cache policy.

Restore checks `options.repo_generation` against the saved generation. This is
an explicit caller binding, not a filesystem scan or verification claim. Use
generation zero consistently for deliberately repository-independent prefixes.
`options.parent` is ignored on restore.

## Sequential correctness and supported models

The pinned llama.cpp revision is `bb4caa7540188872173c44d161602d9271386413`.
Forge uses `llama_state_seq_get_size`, `llama_state_seq_get_data`, and
`llama_state_seq_set_data` for sequence zero, with ordinary host memory. It does
not use `PARTIAL_ONLY` or `ON_DEVICE`; the latter invalidates earlier saved
device states for the same sequence in this pin.

The sequence serialization contains memory state, not logits or sampler/RNG
state. Following restore, ordinary generation still tokenizes its requested
prompt and compares the actual token IDs. It trims an unmatched suffix and
always re-decodes the final prompt token on an exact hit to produce valid logits.
Tokens loaded by restore are not automatically counted as avoided prefill.
The model's `reuse_prefix=false` ablation remains in force after an explicit
restore and will therefore recompute the next requested prompt.

Initial support is intentionally limited to text decoder models with complete
sequence memory. Scripted backends, recurrent/hybrid models, sliding-window
attention, diffusion, and encoder/non-decoder models return
`FORGE_ERR_UNSUPPORTED`. Capture and restore also require the physical sequence
to cover positions zero through the last stored token. Active prefix reuse now
checks that its claimed prefix still exists in physical memory.

Do not add separately tokenized segment lengths to locate physical boundaries:
chat templates, their assistant suffixes, and token merges can change the full
token sequence. A parent made from a shorter chat-wrapped prompt may therefore
fail the exact-prefix check even when the untemplated source text is a prefix.
There is no arbitrary middle-segment KV splicing or mid-generation continuation.

## Limits and failure behavior

Defaults are a 256 MiB host state cap and a 120-second cooperative timeout. The
public limits allow at most 1 GiB per state copy and one hour per operation.
Zero or larger limits are argument errors. Token arrays are limited to the
runtime's maximum context of 1,048,576 tokens, independently of the state cap.
For explicit handles, the caller owns aggregate checkpoint memory budgeting and
destruction. The per-handle cap does not bound the existing model/KV allocation
or a collection of retained handles. The separate opt-in manager below owns its
own aggregate budget; `forge_complete()` still never requests automatic capture.

Capture checks the required size before allocating the host buffer and requires
the serializer to return that exact size. Restore requires an exact byte count
and complete position coverage before committing the saved token IDs. Any
failure or cancellation after beginning restore's state write clears the live
KV/token cache, so partially restored bytes cannot become a prefix-reuse claim.
Rejected instance/generation/cap checks leave the current state unchanged.

The timeout starts at API entry, including prompt validation. Prompt scanning
stops at the 16 MiB input bound. Cancellation and absolute deadlines are checked
before work, around every
capture/restore stage, and between prefill batches. A single llama.cpp decode,
synchronize, or state-copy call cannot be preempted by this wrapper and may run
past the deadline; its late result is rejected. There is no background worker.
Only one operation may use a model at a time. A guard rejects same-thread callback
re-entry; callers must still serialize threads and must not destroy a model from
an active callback.

## Automatic cache

`forge_checkpoint_cache_configure()` enables a model-owned RAM cache. Defaults
are 256 MiB total requested allocations, eight retained prefixes, a 128-token
minimum, and two capture attempts per prompt. Public hard limits are 1 GiB,
64 entries, and four anchors/capture attempts. Its budget includes the manager,
entry table, copied namespaces, retained token/state copies, and transient
template/token probes and pending captures. It excludes allocator overhead,
ordinary inference buffers, the live model/KV allocation and explicit handles.
It is not an RSS or VRAM bound. A valid reconfiguration discards the old entries
and counters; allocation failure leaves the manager disabled. Invalid options
and unsupported backends leave an existing configuration unchanged.

```c
forge_checkpoint_cache_options cache = forge_default_checkpoint_cache_options();
if (forge_checkpoint_cache_configure(model, &cache, &error) == FORGE_OK) {
    size_t anchor = strlen(prompt); /* A raw UTF-8 byte boundary, not tokens. */
    forge_checkpoint_cache_request request = {
        canonical_workspace, logical_context_id, repo_generation, &anchor, 1
    };
    forge_status status = forge_complete_with_cache(
        model, prompt, &request, max_tokens, on_token, userdata, &metrics, &error);
    /* Inspect status and metrics; a request does not promise a cache hit. */
}
```

Requests borrow exact nonempty UTF-8 workspace/context namespaces and increasing
raw-prompt byte endpoints. The embedding API does not canonicalize paths or
establish repository freshness. Changing either namespace or repository
generation invalidates all retained entries. A model-instance nonce also binds
the manager. This conservative epoch policy does not selectively reuse entries
across source generations.

The backend tokenizes the complete templated prompt once. Each shorter raw
prefix is separately templated/tokenized only as a probe; the actual cut is its
longest exact token-ID prefix shared with the complete prompt. This handles
assistant-template suffixes and token merges at a byte boundary. No estimated
or separately added token lengths authorize reuse. The longest eligible saved
token prefix is restored only if it improves on verified live state. Exact
full-prompt hits still recompute the last token for logits. Capture copies the
state at eligible cuts during ordinary prefill; it never prefills each prefix
again. Entries are independent complete host copies with deterministic LRU
eviction and stable-ID ties, not a graph of KV deltas.

Missing requests, zero eligible anchors, `reuse_prefix=false`, or a budget that
cannot fit a probe/state bypass cache work and preserve ordinary generation.
Failed probes/captures are counted. A failed restore removes that entry; a
partially written state is cleared before one ordinary prefill fallback, with
no second-entry restore retry. Cancellation and deadlines propagate, including
after a non-preemptible backend call. One model operation at a time remains
required. Configuration, clearing and generation re-entry are rejected while
an operation is active. Clearing the manager leaves ordinary live state intact.

The agent nominates the selected immutable, cacheable SYSTEM/TOOLS prefix via
`forge_context_cache_anchor()`. It verifies that the supplied prompt is the
actual selected context rendering, with valid dependencies. Each agent has a
fresh context namespace and uses the canonical workspace and current indexed
generation. Existing monitor/stale-action checks still decide freshness; a
cache hit cannot authorize an action. The CLI opts in with `--checkpoint-cache`
or `[inference.checkpoints] enabled = true`; see [configuration](CONFIG.md).
`complete` nominates its entire raw prompt with generation zero, but each CLI
process loads a fresh model, so separate invocations do not share checkpoints.

## Metrics and evidence

Each API call resets its optional stats output. Save reports prompt, cached and
prefill tokens, prefill time, state-copy time and total duration. Prefill tokens
count successful decode batches; partial work in an aborted/failed upstream
batch is not known. Restore reports successfully restored token and state byte
counts and restore duration. Failed captures/restores do not report successful
state bytes or restored tokens. These counts remain separate from normal
generation's actual matched-prefix and recomputed-token metrics.

`tests/unit/test_checkpoint.c` injects a deterministic state-I/O seam. It covers
independent handle ownership, exact parent matching, nonce/address reuse,
generation/cap failures, short/zero/oversized serializer results, partial restore
cleanup, cancellation, post-write timeout, re-entry, oversized prompts,
unsupported backends and ID exhaustion.
Its fixture bytes are not a KV cache and are not real-model correctness or
performance evidence.

`forge_checkpoint_cache_get_stats()` exposes cumulative manager requests,
lookups, hits/misses, captures, evictions, invalidations, skip/failure reasons,
restored and subsequently matched tokens, and resident/pending/peak bytes.
Generation/session metrics contain the call deltas for cache counters and
probe/capture/restore times. `checkpoint_peak_bytes` is the manager lifetime high
water mark observed by that call, not the call's incremental allocation or RSS.
`checkpoint_additional_tokens` counts actual reuse beyond the live prefix that
was usable before restoration; it is not a speedup claim. Cache timings overlap
overall inference work and must not be added to the total again.

`tests/unit/test_checkpoint_cache.c` uses simulated host state I/O to test
token merges, template suffixes, A/B/A selection, live-prefix preference,
namespace/generation/nonce invalidation, deterministic eviction, allocation
caps including transient work, restore failure cleanup, cancellation, deadlines
and re-entry. Context and config/CLI fixtures cover anchor eligibility and
strict opt-in/override behavior. These fixtures are not physical KV evidence.

`tests/integration/checkpoint_model.c` is an optional real-model test:

```text
forge_checkpoint_model /path/to/local-model.gguf [gpu_layers] [context] [chat_template]
```

GPU layers default to zero; no model is downloaded. The test runs separate short
and source-like prompt cases, each with independent A/B checkpoint handles. It
restores and generates A/B twice, checks exact-hit final-token recomputation for
both variants, and compares repeated output bytes. It then unloads the model
before loading a fresh cold instance, rejects both old handles on that instance,
and compares each variant's actual greedy output bytes with cold generation.
Prefix reuse is disabled on the cold instance, so both A and B must report zero
cached tokens. Only one model and at most two host checkpoints coexist; each
case releases its checkpoints before the next case starts.

The source case adds/removes complete Go snippets using actual templated token
counts reported by checkpoint capture. At context sizes of 1,024 or more it
targets 200–500 prompt tokens; smaller contexts use a shorter source prompt that
still exceeds the short case. Every accepted prompt reserves 16 output tokens.
Fitting attempts and construction bytes are bounded; an impossible fit is a
failure, not a skip or silently truncated prompt.

Success prints four JSON records marked `real_model_checkpoint`, with explicit
`case` (`short`/`source`) and `variant` (`A`/`B`) names, actual token/byte counts,
capture/restore timings, repeated/cold cache accounting, and fitting-attempt
counts. Records for a case are emitted only after both variants pass. These are
correctness diagnostics, not end-to-end benchmark timings. Missing model
arguments or unsupported model architectures return 77 (skip). The default
CTest invocation supplies no model and therefore skips this test. Run the
executable explicitly with a local model for inference evidence; no automatic
download or scripted substitute is used.

[Recorded Qwen GPU checks](../benchmark/results/2026-08-28-checkpoints/README.md)
identify the exact tested revision, binary/model hashes and four short/source
A/B results. They are limited correctness checks, not end-to-end performance
or long-generation evidence.

The same executable has a separate automatic mode:

```text
forge_checkpoint_model --automatic /path/to/local-model.gguf [gpu_layers] [context] [chat_template]
```

It requires at least 512 context tokens. A cold instance with all reuse disabled
produces two reference outputs, then unloads. A new instance generates A/B/A/B
with the automatic manager and full-prompt anchors, without any explicit save
call. Both displaced-prefix hits must physically restore, match the cold output
bytes, account for all prompt tokens, and recompute exactly one final token.
The fixture then changes repository generation and requires invalidation and
output parity. Its two JSON records are correctness diagnostics, not a timed
task benchmark. Only one model is loaded at a time.

No checkpoint disk format, untrusted state import, process-restart resume,
model-reload restore or durable logical/physical session binding is implemented.
Agent selection currently covers only the stable SYSTEM/TOOLS boundary, not a
complete semantic checkpoint hierarchy for all repository/context segments.
