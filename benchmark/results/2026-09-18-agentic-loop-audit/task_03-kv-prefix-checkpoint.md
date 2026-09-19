# task_03-kv-prefix-checkpoint

Read BRIEF.md first (same directory). Domain: 03-kv-prefix-checkpoint.

## Scope files
- src/inference/checkpoint.c (410)
- src/inference/checkpoint_cache.c (555)
- src/core/digest.c (144)
- targeted: KV/prefix regions of src/inference/llama_backend.c and src/inference/inference.c (find via search)

## Questions (answer with located claims)
1. Prefix comparison per generation: exact algorithm and complexity (token-ID compare); what invalidates reuse; is the retained prefix maximal after tool results?
2. Prompt ordering stability: verify in code that stable segments precede volatile ones; name any per-turn volatile text (timestamps, counters, absolute paths, random ids) that can land in the prefix and break reuse -- each is a claim with the exact write site.
3. Exact-hit path: re-eval of the last token -- is that minimal? any extra decode?
4. seq_rm/seq_keep/defrag usage and cost; recurrent/hybrid fallback behavior.
5. checkpoint.c: capture/restore mechanics -- what is copied (per-layer buffers?), memory traffic, hashing (digest.c: over what, how often, complexity), handle bounds.
6. checkpoint_cache.c: accounting, note_reuse, invalidation decisions, eviction policy cost per turn; any linear scans per generation.
7. Automatic checkpoint path (recent cache-integration work): what triggers a capture, cost per turn, on by default?
8. Candidate/best-of-N interaction: does selection restore a stored prefix or re-run?

## Output
- Write <RUN>/attempt_03-kv-prefix-checkpoint.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
