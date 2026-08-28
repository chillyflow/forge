# Staged indexed retrieval

`forge/retrieval.h` retrieves source evidence in this order:

1. Exact, case-sensitive Go declaration name.
2. The declaration's package and its bounded import neighborhood.
3. Case-sensitive literal text in indexed files.
4. FTS5 matches for quoted query terms, ordered by BM25 and canonical path.

There is no embedding model, inference call, resolved call/type graph or measured
retrieval-quality claim. Go declarations and package imports are syntactic.
Other indexed text languages participate in literal/FTS search, without AST
semantics. An optional indexed `seed_file` can seed the graph when the query has
no exact symbol. A missing seed is an error, not an inferred directory.
The JSON explicitly reports whether the graph was loaded; incompleteness is
null when it was not inspected, rather than a claim that it is complete.

## Snapshot and source contract

The API consumes one committed SQLite snapshot. Exact lookup, graph loading,
literal search, FTS, serialization and optional token-budget checks all use that
same scope. It does not refresh the index, read live source, launch processes or
generate summaries. A second handle may commit new indexed state while the
query holds its old WAL snapshot; the returned generation remains the observed
one. Same-handle snapshot/index re-entry is rejected.

Every candidate's stored source is checked against its SHA-256 metadata before
publication. Missing digest metadata requires reindexing. Observed malformed
types, paths, spans, UTF-8, lengths, versions or digest mismatches fail without
partial output. A digest is a consistency check, not authentication of a mutable
database or evidence that the live filesystem has not changed.

The shared Go graph retains its fixed module/package/edge/text bounds and
incompleteness reasons. Default traversal includes incoming and outgoing imports
for one hop; the embedding API permits up to eight hops. With incoming traversal
enabled, distance is an undirected package-import neighborhood, not proof that a
symbol calls another symbol or that a package needs validation. Test imports are
part of the syntactic union. No resolved build-tag/type/call interpretation is
invented.

## Results and deterministic limits

The returned JSON has a generation, snapshot/graph qualifications, an ordered
`stages` trace, and `results`. Each result identifies its stage, path, source
digest, kind, excerpt and whether that excerpt is truncated. Exact declarations
also have their name and original symbol end. Located excerpts have line and
byte offsets into indexed source. FTS excerpts come from SQLite's token window;
their absolute line/byte fields are null rather than guessed.

Exact declarations are ordered by path, byte offset and kind. Graph results are
ordered by hop distance and package/file path. Literal results use path order;
FTS uses rank then path. Exact duplicate spans are removed. Later stages skip
files represented by an earlier stage. This favors structured evidence, but it
does not prove that every chosen excerpt is relevant to a natural-language task.

| Default | Meaning |
| --- | --- |
| 16 results | Retained before output-budget trimming; public maximum 256. |
| 64 KiB output | Complete serialized JSON, including metadata and escaping; maximum 1 MiB. |
| 2 KiB excerpt | UTF-8-safe byte prefix/window per result; maximum 8 KiB. |
| 256 candidates | Rows inspected across stages, including duplicates; maximum 4,096. |
| 16 MiB source | Candidate source bytes inspected, charged again for repeated rows; maximum 256 MiB. |
| 5 seconds | Cooperative query timeout; an absolute deadline may shorten it. |
| 50 million VM steps | Shared SQLite instruction budget; maximum one billion. |

Source budgets apply to result inspection; shared graph construction has its own
fixed bounds. These limits are not SQLite memory, process RSS, allocator-overhead
or live-model memory bounds. Intermediate JSON may exceed the final output cap
before tail results are removed, but remains bounded by the hard result/excerpt
and metadata limits.

Result, candidate and source exhaustion return explicitly limited output. The
lowest-priority tail results are removed until the complete JSON fits both its
byte budget and optional token budget. A budget too small even for metadata
returns `FORGE_ERR_LIMIT`. Candidate counters are observed rows, not a count of
all repository matches. The stage trace identifies disabled/skipped stages,
duplicates, limits and results omitted for the final output budget.

Token limits require a caller counter. It receives the exact complete JSON on
each bounded trimming attempt; counts are never added per result. Successful
`forge_retrieval_stats` reports final bytes/tokens, generation and observed work.
Stats are zero on failure. Explicit cancellation returns `CANCELLED`; deadlines
and VM limits follow the shared snapshot's `LIMIT` contract. A single SQLite,
hashing, serialization or caller-tokenizer call cannot be preempted, but a late
result is rejected. Snapshot callbacks and busy behavior are restored on exit.

FTS terms are extracted from a query of at most 1,024 UTF-8 bytes, with at most
32 terms. Each term is quoted and bound; operators, quotes, column selectors and
punctuation cannot become caller-supplied MATCH syntax. The resulting OR query
is a lexical fallback, not semantic similarity. Punctuation-only queries have
no FTS stage but may still match literal source text.

## CLI and agent

```text
forge retrieve DeleteRecord --workspace ./repository --depth 1
```

The CLI first refreshes the index, then returns JSON without loading a model.
`--depth 0..3` controls graph hops; zero disables that stage. `--max-tool-bytes`
can lower the final JSON byte cap. Its wall deadline includes the index/query
path, subject to cooperative backend calls. No model token budget is implied by
a CLI invocation without a tokenizer.

The agent's `retrieve_context(query:string)` is a READ-capability tool. It uses
the current indexed snapshot, the existing cancellation/deadline and the tool
byte cap. With a model, it counts the complete JSON through the same tokenizer
used by the agent and limits it to one quarter of context capacity. This avoids
cutting the JSON into invalid text during generic tool-output handling. A host
policy may deny the read; neither query text nor a cache hit grants permission.
Monitor and stale-action checks still decide whether subsequent actions may be
accepted. Existing `find_symbol`, `get_references` and `search_text` remain
available as narrower tools.

`tests/unit/test_index.c` exercises the new API using real SQLite FTS5 and
Tree-sitter fixtures: stage order, graph direction/seed behavior, source-only
snapshot reads, output/UTF-8/token/VM limits, cancellation, malformed metadata,
concurrent writer commits and same-handle re-entry. CLI fixtures cover JSON
retrieval and an explicitly scripted read-only tool call. These are correctness
fixtures, not model-quality or broad coding-task benchmark evidence.

The [2026-08-28 model smoke](../benchmark/results/2026-08-28-retrieval/README.md)
records one real Qwen3-Coder exact-symbol read and correct final answer on a
standalone Git fixture. Its initial ignored-directory setup failed and is
retained in the evidence notes. This does not establish retrieval quality.
