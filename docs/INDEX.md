# Incremental Go index

Forge keeps a bounded, in-memory Go source and Tree-sitter cache on each open
repository handle. SQLite remains the committed repository index. The cache is
optional: eviction, reopening the handle, or disabling retention causes a cold
parse when a file next changes.

Implicit Git enumeration disables fsmonitor and passes `--no-lazy-fetch` so
sparse-index expansion cannot silently fetch missing objects. This needs Git
2.45 or newer; unsupported Git uses the documented native fallback on a full
scan. A failing Git delta requires a full scan, never a less-restricted retry.

This implements incremental parsing at the **file** boundary. It still rebuilds
that changed file's symbol, reference, import, and metadata rows. It does not
implement type checking, resolved calls/implements edges, symbol-level database
updates, or semantic equivalence checking. The validation planner consumes the
indexed imports and retains its conservative fallback rules.

## Public API

Include `forge/index.h` alongside the existing repository API:

```c
forge_index_limits limits = forge_default_index_limits();
limits.max_cached_source_bytes = 8 * 1024 * 1024;
forge_repo_set_index_limits(repo, &limits, &error);

forge_repo_index(repo, &error); /* Discover the initial repository. */
const char *changed[] = {"service/handler.go"};
forge_repo_index_paths(repo, changed, 1, &error);

forge_index_stats stats;
forge_repo_get_index_stats(repo, &stats);
char *metadata = forge_repo_index_describe(repo, changed[0], &error);
/* Use metadata, then release it with forge_free(metadata). */
```

The example omits error handling. Check every status and nullable return value.
`forge_repo_set_index_limits(repo, NULL, ...)` restores defaults; setting any
retention dimension to zero disables the cache and frees retained entries.
Changing limits does not change the index generation.

| Retention dimension | Default | Maximum accepted |
| --- | ---: | ---: |
| Files | 256 | 4,096 |
| Source payload bytes | 16 MiB | 256 MiB |
| Exposed syntax nodes | 500,000 | 8,000,000 |

The three limits apply to **committed and transaction-staged entries combined**.
Source payload counts exclude one terminating NUL per entry. Retained source
allocations are resized to payload length plus that NUL; path strings and entry
structures are additional bounded overhead. Node counts include anonymous syntax
nodes, not just named nodes. Tree-sitter owns its allocations and may share
subtrees. Its opaque allocations, allocator overhead, and process RSS are not
measured by these counters.

Reading and parsing one file additionally requires transient source storage,
an edited old-tree copy, and a new tree. Those are not retained cache entries;
the parser's transient/internal allocation size is not a hard RSS bound.
An entry that cannot fit is not cached, but can still be indexed. Cache allocation
failure also falls back to indexing without retention. Source/index allocation
failures fail the transaction.

## Exact edit and transaction behavior

For each eligible path, Forge reads at most 2 MiB, hashes the bytes, and compares
them with the committed indexed bytes. Hash equality alone is insufficient for
an unchanged-file decision. An unchanged file updates its scan marker without
parsing. On reopen, unchanged persisted files are not parsed merely to warm the
cache.

For changed Go source with a usable retained tree:

1. Match the cached source against the indexed previous source with an exact
   byte comparison. This also rejects stale entries after another handle writes
   the same database.
2. Find the common byte prefix and suffix, expanding the replacement span to
   UTF-8 codepoint boundaries. Multiple separate source changes are represented
   by one enclosing span.
3. Calculate `TSInputEdit` byte offsets and points. Columns count **bytes**, and
   only LF advances the row; CRLF therefore counts CR as a byte before LF resets
   the column. Both old and new endpoints come from their respective source.
4. Copy the old tree with `ts_tree_copy`, edit the copy, and supply it to
   `ts_parser_parse_string`. The original retained tree is never edited.
5. Write the changed file's rows and hashes, and retain the new source/tree as a
   private staged entry if the combined cache budget permits.
6. Publish staged entries only after SQLite COMMIT succeeds.

On read/parse/SQL failure or interruption, SQLite rolls back and staged entries
are discarded. In-memory generation and scan state return to their pre-call
values. Budget eviction can remove an old committed cache entry while a
transaction is running; a failed transaction may therefore lose cache warmth,
but cannot expose an uncommitted tree. The next update cold-parses that miss.
Successful deletions and paths that cease to be eligible invalidate their cache
entries. Recreating a deleted file starts with a cold parse.

The parser and grammar are pinned in `cmake/Dependencies.cmake`:

- Tree-sitter: `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406`
- Go grammar: `1547678a9da59885853f5f5cc8a99cc203fa2e2c`

## Hash descriptions

`forge_repo_index_describe` returns schema-1 JSON for one indexed path. It reads
one database snapshot, not fresh filesystem content. Reindex before relying on
it for current source. Relevant fields include:

| Field | Meaning |
| --- | --- |
| `source_hash` | Hash of all indexed source bytes |
| `ast_hash` | Exposed tree structure, fields, positions, node flags, and leaf source bytes |
| `symbol_hash` | Ordered declaration names, kinds, and their source hashes |
| `symbols[].source_hash` | Complete declaration source span, including its body |
| `ast_nodes`, `symbol_count` | Complete exposed-node and extracted-symbol counts |
| `generation` | Generation when this file's indexed rows were written |
| `repo_generation` | Repository generation in the same database snapshot |
| `metadata_complete` | Whether this index format's metadata exists for the file |

All hashes are explicitly labeled `fnv1a64`: deterministic, noncryptographic
FNV-1a-64 fingerprints. They are not collision-resistant signatures or proofs of
semantic equality. Source comparisons, including cache eligibility, use bytes
as well. AST hashes omit pointer identities and `has_changes`, which depend on
parse history. Whitespace absent from the exposed tree may affect only the source
hash. Moving an unchanged declaration can preserve its symbol source hash while
changing its byte positions and the file's AST hash.

Declarations include functions, methods, type specs/aliases, const specs, and
var specs, including multiple declared names and declarations inside functions.
References remain syntactic identifier occurrences. No name/type resolution is
implied. A syntactically invalid Go file can still have metadata and partial
symbols; `parse_error` records the syntax/package problem.

Descriptions return at most 4,096 symbols, with `symbols_truncated` and the
complete `symbol_count`. The aggregate symbol hash still covers every extracted
symbol. Serialized output is limited to 16 MiB. Text files have no AST or symbol
hash. Existing databases acquire the new metadata lazily on the next index pass;
`metadata_complete: false` makes that migration state explicit.

## Counters

Counters are per handle, reset on reopen, and saturate at `UINT64_MAX`. Work
counters include work later rolled back; they are not committed-change totals.

- `full_attempts` / `delta_attempts`: calls after basic argument validation.
- `commits` / `rollbacks`: successful or rolled-back index transactions. A
  zero-path delta does not start a transaction.
- `files_read` / `source_bytes_read`: completed bounded reads, including unchanged
  or subsequently rejected source. Partial failed reads are not counted.
- `unchanged_files`, `files_indexed`, `files_removed`: observed no-op files,
  completed per-file row writes, and pruned file rows, respectively.
- `cold_parses` / `incremental_parses`: actual parser calls without/with an edited
  previous tree. `cache_hits` / `cache_misses` count those usable-tree decisions.
  None of these measure how many internal nodes Tree-sitter actually reused.
- `cache_evictions`, `cache_invalidations`, `cache_skips`: budget removals,
  successful-index removals of obsolete entries, and skipped retention.
- `cached_*` / `peak_cached_*`: current and historical peak retained entries,
  source payload bytes, and exposed nodes. Historical peaks can exceed newly
  reduced limits.
- `observed_change_bumps`: successful explicit watcher generation bumps, separate
  from index transaction commits.

## Delta eligibility, generations, and interruption

Delta indexing preserves Git tracked/untracked/ignore eligibility and normalizes
relative path separators. Duplicate paths are processed once. Non-Git discovery
uses the same restricted exclusions as filesystem discovery (hidden paths,
build-prefixed names, vendor, node_modules, target, dist, and __pycache__). Its
directory traversal checks interruption between entries and limits nesting to
256. Git/filesystem discovery is not a filesystem snapshot; files can change
during a scan. Watcher loss, directory moves, and ignore-policy changes need a
full reconciliation scan.

The existing limits remain: 100,000 indexed files, 2 MiB per source file, 4,096
delta paths, and 1 MiB of combined delta path bytes. The total indexed-file limit
is checked for deltas too. Oversized, unreadable, NUL-containing, and invalid
UTF-8 source is not indexed. Omitted Go inputs mark the index incomplete; delta
updates cannot clear an unrelated omission, so a later full scan is needed.
A missing/deleted file is a tombstone, not an incomplete Go read.

Index transactions read persisted generation/scan metadata after `BEGIN
IMMEDIATE`, avoiding lost increments across separate repository handles. Neither
counter can exceed `INT64_MAX`. `forge_repo_generation` remains the handle's last
observed generation; it does not poll another handle's commits.

Internal coordinator APIs `fg_repo_index_until` and
`fg_repo_note_change_until` accept an absolute monotonic deadline, a cancellation
callback, and userdata. They temporarily install interruption state and a SQLite
busy handler, then restore the prior state/busy timeout. Checks occur during
directory/file/path traversal, each Git eligibility batch, before/after reads
and parsing, while extracting syntax, and before committing. Git subprocesses
receive the callback and at most `min(30 seconds, remaining time)`. SQLite lock
retries check interruption at intervals of at most 10 ms, subject to OS scheduling.
An individual read, parser operation, or SQL statement is not forcibly preempted;
the next check rejects and rolls back an overrun. Public indexing retains its
normal defaults when no coordinator scope is supplied.

`fg_repo_note_change` and its bounded variant atomically increment persisted
generation without pretending an unchanged source scan found a changed indexed
file. The coordinator uses them for watcher change/loss signals that indexing
did not represent, such as a changed `.txt` fixture. The agent also uses them
after a successful patch to an unindexed file, or a launched command whose
effects are unknown, when indexed bytes alone did not advance the generation.
The latter is a conservative invalidation, not an observed file modification.
They do not change scan markers or file hashes. Failed, cancelled, overflowing, ignored-row,
or rejected-commit updates do not publish a generation increment.

Repository handles, parser/cache state, and callbacks are not concurrently
thread safe. Callbacks must not re-enter indexing or destroy the handle. Cache
trees are not persisted or exported, and cache/description metadata makes no
inference KV claim.

## Verification

`tests/unit/test_index.c` uses actual SQLite and the pinned Tree-sitter parser in
isolated temporary workspaces. It compares cold and incremental exposed trees,
AST hashes, and symbols across Unicode, CRLF/LF, multi-span, deletion, empty-file,
and syntax-error edits. It covers hash stability, reopened handles, stale caches
across handles, limits, descriptor truncation, SQL/COMMIT fault injection,
rollback after cache eviction, cancellation after parsing, expired deadlines,
SQLite lock interruption, generation overflow, and watcher-only changes.

The Git eligibility case initializes a local temporary repository, uses no
network, and skips explicitly if Git is unavailable. No Go compiler, model,
GPU, or mocked parser is required by this suite.
