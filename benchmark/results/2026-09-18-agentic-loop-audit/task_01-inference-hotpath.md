# task_01-inference-hotpath

Read BRIEF.md first (same directory). Domain: 01-inference-hotpath.

## Scope files
- src/inference/llama_backend.c (1502 lines)
- src/inference/inference.c (286)
- src/inference/routing.c (160)

## Questions (answer with located claims)
1. Load-time params: list exactly what is set on llama_model_params / llama_context_params and identify performance-relevant knobs left at llama.cpp defaults (KV type type_k/type_v, flash attention, offload_kqv, n_batch/n_ubatch, threads, mmap/mlock, defrag). Verify field names against the pinned llama.h you can find on disk (.scratch/llama.h or a dependency checkout); cite what you read. Cross-check docs/plans/agent-performance-implementation.md P0.1 and report the exact current implementation state.
2. Prefill: how the prompt is submitted (single llama_decode over n_batch chunks? per-token?), whether logits are requested only where needed, what is re-evaluated on a cache hit, and the exact KV sequence operations per turn (seq_rm/seq_keep/defrag) with expected cost.
3. Decode hot path per token: sampler chain construction (rebuilt per token or reused), grammar state advance, buffer allocations/copies, streaming callback cost, stop/action-stop scans, any per-token O(vocab) or O(context) work not inherent to sampling.
4. Greedy grammar fast path and reduced-prefix fallback: where implemented, what remains on the full-mask path, whether the fast path validates every accepted token.
5. Allocation churn in generation: per-call mallocs (token buffers, string building), arena usage.
6. Metrics: which timers exist (decode_ms, sampling_ms, prefill, cached tokens) and which hot costs have NO counter (measurement gaps are valid GAP claims).
7. Anything that scales worse than linearly with context or vocab per turn.

## Output
- Write <RUN>/attempt_01-inference-hotpath.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
