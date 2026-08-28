# Indexed summary inputs and cache

`forge/summary.h` exposes bounded preparation, generation and storage for
repository, Go module, Go package, file, and syntactic declaration summaries.
The original prepare/store API accepts caller-generated text without invoking a
model. `forge_repo_summary_generate` connects that cache to one bounded model
call on a miss. Both consume indexed snapshots; neither refreshes the index,
launches a command or reads live source files. The `summarize` CLI refreshes the
index first. Automatic agent/context selection remains open.

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

## Bounded generation

`forge_repo_summary_generate(repo, model, target, options, max_tokens, stats, error)`
prepares a fresh snapshot for every call. A validated hit returns without model
generation. A miss or corrupt cache row permits one inference call outside the
SQLite transaction, followed by dependency-checked publication. A concurrent
valid writer wins; the returned input contains that writer's committed text.
Changed/deleted dependencies reject the generated text, while unrelated indexed
changes can succeed. The returned input is owned and has `HIT` status.

The caller **must** provide an explicit `producer_id` identifying the actual
weights, backend and tokenizer/template versions. The default `caller` identity
is rejected. This is a host assertion, not an automatically verified model-file
digest; reusing an ID for different weights can reuse inappropriate text. Forge
also hashes its generation recipe version, selected chat-template string, context
capacity, sampling/thread/GPU/reuse configuration, simulated-backend flag and
generation token limit into the effective producer identity. Model path or file
timestamp alone is not used as weight identity. That effective SHA-256 appears
in the result; keep the original producer assertion with external run evidence.

Default recipe `forge.summary.v2` names the selected evidence mode explicitly
and requests concise source-supported behavior rather than metadata/digest
recitation. This followed a real-model trial whose correct return-value claim
was accompanied by an incorrect claim that file bodies were absent. A changed
recipe/prompt gets a new key; prior cached text is not silently reclassified.

The helper uses the model backend's full templated-prompt counter, leaving room
for the complete output reserve. A caller counter is rejected. The explicit
script backend still uses its documented simulated token estimates. One shared
deadline covers preparation, generation and publication; the existing SQLite
VM budget applies separately to each snapshot. Explicit cancellation is
`CANCELLED`, deadline exhaustion is `LIMIT`. Individual backend/tokenizer calls
remain cooperative and cannot be forcibly interrupted.

Streaming output is checked against the byte cap, then the completed text is
checked for UTF-8, NUL bytes and the stored-text token budget. Reaching the
generation token cap rejects publication instead of storing a clipped ending.
A valid ending is not proof of semantic accuracy or sufficient coverage. Errors
return no generated text and do not publish partial summaries. Stats retain
attempted inference work even on errors; a hit has zero model calls/tokens.
Summary text remains untrusted content and never grants a tool permission.

The model is exclusively borrowed through token counting, inference and
publication. Model operation re-entry is rejected; the model must outlive any
returned input later passed to `summary_store`, since its counter is borrowed.
Ordinary live-prefix reuse may occur. No new automatic checkpoint anchors are
nominated; generation may displace live agent KV state. No durable checkpoint or
agent resume claim is made.

## CLI

```text
forge summarize src/example.go --model MODEL --summary-producer VERIFIED_MODEL_AND_BACKEND_ID
forge summarize . --summary-scope repository --summary-full-source --model MODEL --summary-producer VERIFIED_MODEL_AND_BACKEND_ID
forge summarize src/example.go --summary-scope symbol --summary-symbol Example --model MODEL --summary-producer VERIFIED_MODEL_AND_BACKEND_ID
```

Scope defaults to `file`; other scopes are `repository`, `module`, `package` and
`symbol`. Aggregate scope defaults to outline evidence unless
`--summary-full-source` is selected. Ambiguous declarations fail rather than
guessing; the embedding target supports kind/byte-offset disambiguation.
`--summary-producer` is the same caller assertion described above, not a checksum
verification flag. All summary flags require the `summarize` command.

The CLI returns JSON containing the complete summary input/manifest, text,
generation/cache statistics and inference metrics. It loads the configured model
even for a hit, but invokes no generation on that hit. The wall deadline starts
before hardware selection/model load and includes index refresh; load itself is
not preemptible. Generation uses the smaller of `--output-reserve` and
`--max-tokens` (maximum 8,192); `--max-input` limits the whole prompt.
`--max-tool-bytes` can lower the summary-text byte cap. Preparation/manifest
defaults still apply and evidence is not silently clipped. No agent session,
source mutation, model-requested process or validation command is produced.
Index discovery can execute Git. This command does not yet select summaries for
the agent's context planner.

The top-level `model_load_ms` reports each CLI load even on a cache hit. It is
outside the helper's generation duration and must not be mistaken for zero load
cost just because that hit has no inference work.

## Coverage and remaining work

`test_summary.c` uses real SQLite, Tree-sitter and the shared graph with literal
summary text. It covers known SHA vectors, all scopes, deterministic keys,
unrelated edits, dependency changes/deletion/restoration, recipe identity,
corruption repair, trigger/commit failure, bounded eviction, token/byte/work
limits, cancellation, nested snapshots, concurrent indexing and writer locks.
Index tests cover digest metadata upgrades; validation tests retain the existing
graph fixtures and reject malformed graph rows. These are storage and
invalidation tests, not evidence of model summary quality or performance. An
explicit internal model seam additionally exercises one-call generation and
zero-call hits, all scopes, UTF-8 split across token pieces, competing cache
writers, edits/deletion during generation, corruption repair, model re-entry,
producer profile changes, output limits, cancellation and failed generation.
CLI tests use an explicitly simulated script backend, without quality claims.

Source `2184388` passed all 23 executed local groups in Debug (40.09 s) and
CUDA-linked Release (39.91 s); the optional model group skipped without its
arguments. Its [CI run](https://github.com/chillyflow/forge/actions/runs/33197412564)
passed all five jobs, including 100 macOS watcher and 30 configuration suites.
A separate [recorded real-model check](../benchmark/results/2026-08-28-summaries/README.md)
verified one file's cold generation, zero-generation reload/unrelated-edit hits,
and regeneration after changing its return value. It retains an initial v1
coverage-wording failure. This is not a broad quality or speedup benchmark.

A later test-only change gives the writer-lock fixture 500 ms instead of 15 ms:
the smaller limit could expire while preparing its input on Windows, before the
lock behavior was tested. The lock must still time out without publication;
runtime budgets are unchanged. Other later changes in this integration are
documentation/evidence only; the model record remains attached to `2184388`.

Automatic summary selection, semantic context integration, resolved
relationships and measured cache benefits remain required work in the full
design. [Staged indexed retrieval](RETRIEVAL.md) now supplies bounded source
evidence separately; it does not generate or select semantic summaries.
