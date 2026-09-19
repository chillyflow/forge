# task_02-template-input

Read BRIEF.md first (same directory). Domain: 02-template-input.

## Scope files
- src/inference/chat_template.cpp (568)
- src/inference/chat_template.h (63)
- src/core/text.c (175)
- src/cli/interactive.c (382)

## Questions (answer with located claims)
1. Per-generation template rendering: is the full prompt re-rendered from segments each turn? Any caching keyed on template + message shape? Cost of string building/escaping per turn.
2. Tokenization: is the entire prompt re-tokenized every turn? Is there a token cache? How are cached_tokens computed (text compare or token IDs)? Where does tokenization happen relative to rendering (double work?).
3. Budget fitting: how often a candidate prompt is tokenized (fit loops, retries); whether rendering for counting and rendering for generation are the same work.
4. Forced-cue path (FG_THOUGHT_CUE): cost per routed turn (force-decode batching; separate decode vs merged into prefill).
5. Template recognition/detection (commit 455c6185): cost at load vs per turn; caching.
6. text.c helpers used per token or per turn; repeated scans/concat patterns.
7. interactive.c: per-request work that could be amortized.
8. UTF-8 boundary handling in hot paths.

## Output
- Write <RUN>/attempt_02-template-input.md (RUN = C:/Users/flowc/AppData/Local/Temp/wf_forge_perf_978e2fd5).
- Return message exactly: STATUS=<word>; claims=<int>; ok=<int>; jev=<int>; top3=<first three claim lines verbatim, separated by ' ;; '>
- Cap: 18 claims, 8 JEV lines, minimum 8 VERIFIED-OK lines.
