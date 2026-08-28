# Indexed summary inputs and cache

`forge/summary.h` exposes bounded preparation and storage for repository, Go
module, Go package, file, and syntactic declaration summaries. This is a library
API: the caller explicitly generates the summary between preparation and
publication. It does not invoke a model, refresh the index, launch a command, or
read live source files. CLI and automatic agent integration remain open.

## Lifecycle and identity

1. Index the repository with the existing index API.
2. Call `forge_repo_summary_prepare` with a target and recipe options.
3. Read the owned view with `forge_summary_input_get`. A hit contains validated
   cached text. On a miss, generate text from the supplied prompt.
4. Call `forge_repo_summary_store` with the same prepared input and UTF-8 text.
   The writer transaction rebuilds and compares the inputs before publishing.
5. Destroy the input. Its strings are owned; callback userdata remains borrowed.

The key combines SHA-256 digests of the versioned recipe and complete dependency
manifest. Recipe identity includes the caller's recipe, producer, instructions,
evidence mode, and output budgets. Callers must put model, profile, tokenizer,
and template versions in these identities when they affect generation.

Dependencies include the actual indexed bytes supplied, selected metadata,
aggregate membership and import-graph facts, and a digest of the complete prompt.
Source and declaration SHA-256 metadata are verified against indexed bytes;
the older FNV hashes remain available for existing index APIs. A global source
generation or SQLite row ID alone is never a summary key. Unrelated source
changes can reuse a summary; changed or deleted dependencies reject an old
publication with `CONFLICT`. Removing and restoring identical evidence can hit
the previous entry. A valid first writer wins for one key.

Preparations observe one SQLite read snapshot, and publications use a writer
snapshot. Source generation records indexed state, not unobserved disk edits.
Cache writes do not advance it. Reentrant indexing/snapshots on the same handle
are rejected. A second handle may commit while a read snapshot remains active;
the old prepared input cannot publish against changed indexed dependencies.

## Evidence and scope

| Scope | Supplied evidence |
| --- | --- |
| Repository | All indexed members and the bounded Go module/package/import graph. |
| Go module | Members in that module, excluding nested module contents, plus relevant graph and module metadata. |
| Go package | Eligible Go members in the directory, package imports and module metadata. |
| File | Complete indexed file bytes and ancestor module/workspace metadata. |
| Symbol | Selected declaration span, the header preceding the first declaration, file import/package flags, and ancestor module/workspace metadata. |

Aggregate `OUTLINE` includes complete member inventories, source digests, Go
imports and declaration signatures, but not bodies or non-Go file contents.
`FULL_SOURCE` includes member bytes. Inputs are rejected if they exceed their
budget; they are not silently clipped. File and symbol scopes always use their
complete selected source/span. A symbol summary does not claim resolved calls,
types, referenced global definitions, or access to surrounding declarations.
Ambiguous names require a declaration kind and/or current indexed byte locator.

Validation and summaries share one immutable Go import-graph loader. It retains
the existing conservative handling of nested/duplicate modules, external tests,
deleted packages, build constraints, unresolved imports, cgo and incomplete
indexing. This remains a syntactic graph, not a sound Go build/type/call graph.

## Limits and failure handling

Defaults are 64 KiB prompt, 8 KiB summary, 4,096 dependencies, 1 MiB manifest,
16 MiB source verification, 4,096 cache entries, 16 MiB cached manifest/text
payload, a five-second cooperative deadline and 50 million SQLite VM steps.
Public constants define the maximum accepted options. Token budgets require a
caller-supplied token counter; zero means unspecified, not estimated tokens.

The cache verifies version, recipe, manifest, content digest, lengths, UTF-8 and
generation ordering. A damaged row becomes a `CORRUPT` miss with no text and can
be replaced transactionally. Malformed source evidence is an error. Missing or
unsupported digest metadata requires reindexing. Cache writes cannot trigger
additional database mutations. SQL errors, cancellation, deadlines, VM limits,
publication verification failures and commit rejection roll back the write.

Eviction is deterministic insertion order, preserving the current publication;
it is not LRU and read hits do not write timestamps. Limits bound manifest/text
payload and query work, not database file size, total process RSS, OS filesystem
latency or a caller's token-counter/model work. The caller must not mutate or
close a repository handle concurrently. The index/cache is not a cryptographic
trust boundary against an adversary who can rewrite the whole database and
recompute its digests.

## Coverage and remaining work

`test_summary.c` uses real SQLite, Tree-sitter and the shared graph with literal
summary text. It covers known SHA vectors, all scopes, deterministic keys,
unrelated edits, dependency changes/deletion/restoration, recipe identity,
corruption repair, trigger/commit failure, bounded eviction, token/byte/work
limits, cancellation, nested snapshots, concurrent indexing and writer locks.
Index tests cover digest metadata upgrades; validation tests retain the existing
graph fixtures and reject malformed graph rows. These are storage and
invalidation tests, not evidence of model summary quality or performance.

Automatic summary generation/selection, semantic context integration, staged
symbol/graph/lexical/FTS retrieval, resolved relationships and measured cache
benefits remain required work in the full design.
