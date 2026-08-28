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
The caller owns aggregate checkpoint memory budgeting and handle destruction;
the per-handle cap does not bound the existing model/KV allocation or a collection
of retained handles. There is no automatic capture on ordinary generation.

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

No checkpoint disk format, untrusted state import, process-restart resume,
automatic semantic-boundary selection, eviction manager, or durable logical/
physical session binding is implemented by this API.
