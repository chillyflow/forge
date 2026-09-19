# attempt_01-inference-hotpath - RECOVERED PARTIAL DRAFT

<!-- provenance: recovered by the parent from the subagent's final summary (delegation deleg_5b98df66, task 0) after a global tool-budget gate blocked the unit's write; content = the unit's full final summary, verbatim (draft; follow-up unit to complete). No content altered. -->

All tool access has remained blocked for ~50 attempts across every tool class. Finalizing with an honest status report.

---

STATUS=blocked; claims=0; ok=0; jev=0; top3=(no claim lines filed — deliverable file could not be written; see below)

**Blocker:** From the write phase onward, every tool call (terminal, read_file, write_file, search_files, execute_code, browser_exec) was rejected by a global daily budget gate: `[BUDGET] Global is over its daily budget: spent $5.91 of $5.00 (118%). Tool calls are blocked until the daily window resets.` ~50 retry attempts across ~30 minutes; the gate never opened. No repo edits, no builds, no model runs, no git writes. Nothing was written to the run dir.

**Completed before the block (all read in full):**
- BRIEF.md + task_01-inference-hotpath.md; src/inference/llama_backend.c (1502 lines), inference.c (286), routing.c (160)
- Pinned llama.h context-param fields (.scratch/llama.h:340-400); pinned llama.cpp defaults (build/_deps/llama-src/src/llama-context.cpp:3528-3544: type_k/v=F16, flash_attn=AUTO, offload_kqv=true, defrag_thold=-1); P0.1 text (docs/plans/agent-performance-implementation.md:52-95)

**UNFILED evidence (drafted claims, ready to write; deliverable NOT written):**
- GAP llama_backend.c:1431 — load sets only n_ctx/n_batch=512/n_ubatch=256/threads; type_k/type_v/flash_attn_type/offload_kqv never set (grep: zero hits); P0.1 unimplemented
- COST routing.c:33 — fg_action_complete tries yyjson_read at every `{` candidate; called per `}` token (llama_backend.c:1139) → superlinear on patch-heavy actions
- COST routing.c:60 — fg_action_decode_phase per token (llama_backend.c:1021-1023) scans action text up to 3× → O(n²) over the action
- COST llama_backend.c:388 — llama_batch_init/free around every single-token decode (call sites 1145/950/662)
- GAP llama_backend.c:897 — decode_ms spans whole loop incl. sampling_ms (1019-1036); no decode-only timer
- WRONG llama_backend.c:1028 — greedy fast path not gated on penalties-disabled while reduced path is (1030-1034); penalty silently bypassed in fast regions
- GAP llama_backend.c:143 — grammar_fallback_tokens conflates reduced-prefix successes and full-vocab fallbacks
- COST llama_backend.c:265 — count_prompt re-renders+re-tokenizes; generate repeats both (704/713) → duplicate per-turn work
- COST llama_backend.c:397 — logits requested at every prefill chunk tail; only final chunk's consumed
- COST llama_backend.c:1094 — fg_action_begin(out.data) per think token → O(n²) scan
- JEV seams identified: think-cap force decision (llama_backend.c:974, noul), native force timing (routing.c:47, noul), envelope choice at force (llama_backend.c:555, choice)
- VERIFIED-OK material: sampler chain built once (763-896); reduce buffers sized once (1449-1455); fast path validates via grammar apply (135-137); ladder exact (routing.c:148-160); prefix-reuse guards (417-421); exact-hit re-decode (437-438); seq_rm fallback (442-445); stack piece buffer (1041-1053) + ~4 more

**To resume:** raise the limit (`/budget set global daily <usd>`), then re-dispatch this unit. Remaining work: reads of checkpoint_cache.c, chat_template.cpp scan_stop, include/forge headers, config defaults (threads/gpu_layers), spikes/benchmark docs (~3 batched calls), then write `attempt_01-inference-hotpath.md` (~1 call). All three scope files were fully read and the claim set above is ready to file.
