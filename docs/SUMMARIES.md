# Indexed summary inputs and cache

`forge/summary.h` exposes bounded preparation, generation and storage for
repository, Go module, Go package, file, and syntactic declaration summaries.
The original prepare/store API accepts caller-generated text without invoking a
model. `forge_repo_summary_generate` connects that cache to one bounded model
call on a miss. Both consume indexed snapshots; neither refreshes the index,
launches a command or reads live source files. The `summarize` CLI refreshes the
index first. An opt-in agent tool requests summaries for its context; automatic
hierarchy construction and summary selection inside the context planner remain open.

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
verification flag. Scope/symbol/full-source flags require the `summarize` command.
The producer identity can also enable the agent summary tool for `run` or `bench`.

The CLI returns JSON containing the complete summary input/manifest, text,
generation/cache statistics and inference metrics. It loads the configured model
even for a hit, but invokes no generation on that hit. The wall deadline starts
before hardware selection/model load and includes index refresh; load itself is
not preemptible. Generation uses the smaller of `--output-reserve` and
`--max-tokens` (maximum 8,192); `--max-input` limits the whole prompt.
`--max-tool-bytes` can lower the summary-text byte cap. Preparation/manifest
defaults still apply and evidence is not silently clipped. No agent session,
source mutation, model-requested process or validation command is produced.
Index discovery can execute Git. This command does not itself run the agent.

The top-level `model_load_ms` reports each CLI load even on a cache hit. It is
outside the helper's generation duration and must not be mistaken for zero load
cost just because that hit has no inference work.

## Agent summary context

`forge run ... --summary-producer ID` enables
`summarize_context(scope, path, symbol)`. Embedders may set
`forge_agent_config.summary_producer_id`; the agent copies that identity. The
default is disabled and the tool schema tells the model whether it is enabled.
The host identity has the same trust requirements as the generation API. There
is not yet a TOML setting for this opt-in.

The model chooses a scope/path, with an empty symbol except for symbol scope.
Aggregate summaries use syntactic outlines. This is lazy, model-requested
selection, not a background repository-wide summarization pass. It can provide
a reusable overview without replacing the exact source needed for patches or
verification. It does not claim resolved language semantics.

The READ policy runs before any cache lookup or inference. A hit never bypasses
policy. A miss permits at most one generation, with a maximum 512 output tokens,
the remaining run token budgets, context capacity and a 30-second cooperative
deadline, shortened by the run deadline. It requires at least 1 KiB of tool
output capacity. Input bytes obey the smaller of the summary default and agent
file limit. Summary-text and complete-JSON budgets are checked separately.
Final JSON is counted as a whole against one quarter of context capacity and is
rejected if it cannot fit; it is not cut into invalid JSON.

All nested prompt/generated/cached/prefill tokens and inference timings are
charged to the agent totals, including work consumed by failed attempts. The
existing hard input/output budgets remain in force. `turns` still counts outer
agent turns. `summary_lookups`, `summary_hits`, `summary_generations` (attempted
model calls), `summary_failures` and `summary_ms` expose this path. Summary time
also falls inside tool time, so those intervals must not be added together.

Each attempted summary lookup records a `summary` event with its inference
metrics and outcome. Successful cache/generation results retain full prepared
evidence under `tool/NNNNNN.summary.json`; the model receives compact JSON with
the unverified summary, scope, indexed generation and dependency/cache hashes.
These per-operation limits do not bound total session disk usage. A subsequent
known edit or observed filesystem change invalidates the context result before
another model action can use it. Current agent invalidation is conservative
across all source-bound results, even when an unrelated edit could still permit
a new cache hit on revalidation.

The tool performs no source write or command. Cache publication writes derived
metadata only; it does not advance source generation. Summaries remain untrusted
model text. Neither summary content nor a cache hit grants any permission or
marks validation as successful.

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

Automatic hierarchy/planner summary selection, resolved relationships and broad
measured cache benefits remain required work in the full
design. [Staged indexed retrieval](RETRIEVAL.md) now supplies bounded source
evidence separately; it does not generate or select semantic summaries.
