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
stable: system, tools, repository map, task, working state, then chronological
history. Final rendered prompt size is checked with the actual tokenizer.

On a known patch, source-dependent segments for that file and broad source
queries are invalidated. Command changes trigger an index refresh. Context
compaction retains bounded working state and drops lower-value history. It is
not a general semantic DAG optimizer or model-generated summary engine.

The noncryptographic FNV fingerprint is for cache invalidation, not security or
artifact integrity. Download/benchmark provenance uses SHA-256 separately.

## Repository intelligence

Git file enumeration honors ignored files when Git is available. A conservative
walk is used outside Git. Files up to 2 MiB in supported text formats are indexed;
Go is parsed with Tree-sitter. SQLite transactions update changed files only,
delete vanished files, and bump repository generation only on content changes.
Every scan currently enumerates/hashes candidates; an OS watcher is future work.

Declarations, signatures, byte spans, identifier occurrences, and imports are
stored. Go identifier occurrences are syntactic: shadowed names and identically
named symbols can appear together. No type checker, cross-package name resolver,
or sound call graph is claimed. Other supported text languages have literal
search, not AST symbol navigation.

## Tools and execution

One declarative registry defines field types, capability, and prompt description.
The GBNF action grammar is generated from it. JSON is parsed by yyjson and all
required field types/cardinality are validated before policy or execution.

`apply_patch` requires one unique exact match, stages output in a sibling file,
checks the old content again, and atomically replaces the file. It is not a
unified-diff parser. Empty old text creates a missing file only.

The process runner uses `fork/exec` and process groups on POSIX, or `CreateProcess`
and kill-on-close Job Objects on Windows. It captures stdout/stderr separately,
drains output after reaching its byte cap, and kills descendants on completion or
timeout. It passes a limited environment. See the security document for limits.

Go JSON output is compacted to failures, locations, and summary counts. Raw
captured output is preserved, with a truncation marker if process capture hit a
limit. Other compiler failures use a generic diagnostic/tail adapter.

## State machine and durability

Runs transition through init, prefill, generating, tool request/running/result,
recontextualization, and done/error. Events are flushed before proceeding.
Random session IDs avoid collisions. JSON-lines replay checks schema version and
sequence continuity and does not call the agent or process runner.

Limits cover turns, input/generated tokens, per-turn output, file sizes, captured
output, command runtime, and total wall time. Repeated action plus repository
generation signatures trigger a warning and eventually stop loops. `run` ending
successfully means the model produced a final answer, not proof that the task is
correct. `bench` also checks an explicit independent verification command.
