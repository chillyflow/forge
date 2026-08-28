# Context dependencies and snapshots

Forge stores prompt material as typed segments. Selection includes each chosen
segment's dependency closure, then renders a stable order within the token
budget. Context snapshots preserve this semantic state for inspection and
replay. They do **not** contain inference KV buffers, model weights, executable
callbacks, or an inference-engine checkpoint.

## Dependency API

The existing `forge_context_add` accepts an optional first dependency. That ID
must already exist; a missing dependency now fails the addition instead of
silently dropping the relationship. Additional dependencies use:

```c
forge_status forge_context_add_dependency(forge_context *, uint64_t id,
                                          uint64_t dependency);
size_t forge_context_dependency_count(const forge_context *, uint64_t id);
bool forge_context_get_dependency(const forge_context *, uint64_t id,
                                   size_t index, uint64_t *dependency);
```

Every edge must point to an existing **older ID**. Self references and newer
IDs return `FORGE_ERR_CONFLICT`; missing IDs return `FORGE_ERR_NOT_FOUND`.
The ordering rule makes cycles impossible without a recursive cycle search.
Dependencies are kept sorted and duplicate additions are successful no-ops.

For source compatibility, `forge_segment_view.dependency` is still available
and contains the first/oldest dependency, or zero. The appended
`dependency_count` field and accessors expose the full graph. Segment IDs remain
stable across updates and snapshot import/export. The APIs do not remove or
renumber segments.

### Selection

Pinned segments and their closures must fit before optional material is
considered. Optional segments are ranked by priority, recency, and local token
cost. This remains a deterministic greedy planner, not an optimal knapsack or
coverage algorithm.

An iterative traversal calculates each candidate's marginal closure cost.
Every reachable segment is counted once, including shared ancestors in a
diamond, and already-selected ancestors cost nothing again. A closure containing
stale material cannot be selected. Failed planning clears the selection and
reports no prompt or token count; it does not leave a partial successful plan.

The per-segment estimate is the caller's token count plus the existing 16-token
framing allowance. The complete rendered prompt is counted again and must fit
`capacity - reserve`; the final check is authoritative when token boundaries or
framing differ from the sum of estimates.

## Metadata, updates, and invalidation

`forge_segment_view` exposes `immutable`, `cacheable`, `stale`, `source_hash`,
and `dependency_count` in addition to the existing text, IDs, content hash,
version, generation, priority, pin, and selection fields. New segments start
mutable and not cacheable. `cacheable` is an explicit semantic hint; setting it
does not create an inference cache or claim any particular KV reuse.

```c
forge_status forge_context_set_flags(forge_context *, uint64_t id,
                                     bool immutable, bool cacheable);
```

Sealing a segment is irreversible. Immutable text, dependencies, and source
bindings cannot change, and its cacheable flag cannot be changed after sealing.
Attempted text/dependency/flag mutations return `FORGE_ERR_POLICY`. The legacy
void `forge_context_bind_source` ignores changes to immutable bindings.
Repeating the existing text or an existing dependency is a successful no-op;
an identical immutable text update does not change generation, version,
staleness, or selection. Pinning, selection, and invalidation remain planner
metadata, so immutable evidence can still become stale.

For mutable segments, an update with identical text can refresh generation and
clear staleness without incrementing the content version or changing the hash.
A changed text increments version and stales every transitive dependent. A
dependent cannot be refreshed while any of its dependencies remains stale;
`forge_context_update` returns `FORGE_ERR_CONFLICT` without changing the segment.
Refreshing a dependency does not automatically certify old summaries derived
from it: those summaries must be refreshed explicitly.

`source_hash` is an **opaque source identity**, often a hash of a file path. It
is separate from the segment's `content_hash` and is not a cryptographic proof
of source freshness. `forge_context_bind_source` attaches that identity;
`UINT64_MAX` means repository-wide evidence. A zero binding is unbound.

The existing `forge_context_invalidate(context, source_hash, generation)` marks
older generations bound to that source, plus repository-wide bindings, stale.
A zero source argument invalidates all older bound sources. Staleness then
propagates through every transitive context dependency, regardless of each
dependent's own generation. Stale segments are unpinned and unselected; attempts
to pin them are ignored. Callers remain responsible for notifying the context
when source data changes.

Text, dependency, pin, and freshness changes that affect selection clear the
last plan. Metadata-only flag/source-binding changes do not alter the rendered
text or the selection. Context instances are not safe for concurrent mutation.

## Stable rendering

Selected segments render in these groups:

1. `SYSTEM`, then `TOOLS`, then `REPOSITORY`, then `TASK`.
2. `SOURCE`, `ACTION`, and `TOOL_RESULT` together in insertion/ID order.
3. `WORKING_STATE` (`FORGE_SEG_MEMORY`) last.

Moving volatile memory behind source/tool history means changing only working
memory preserves the preceding prompt bytes when selection is unchanged.
This improves the opportunity for an inference adapter's sequential prefix
reuse; it does not make a semantic DAG directly reusable as a KV DAG. Changing
selection, updating an earlier segment, or adding new history can still change
the prefix. Dependencies control inclusion; role grouping controls rendering.

## Snapshot API

```c
char *forge_context_export(const forge_context *, forge_error *);
forge_context *forge_context_import(const char *json,
                                    forge_count_tokens_fn count,
                                    void *user, forge_error *);
```

Export returns caller-owned UTF-8 JSON; release it with `forge_free`. Import
creates an independent context with owned text/dependency storage; destroy it
with `forge_context_destroy`. The original context and input JSON may be freed
after import. The caller supplies the token counter and its borrowed userdata;
no function pointer is serialized. Counters must be deterministic for the
context lifetime.

Schema version 1 contains:

| Top-level field | Meaning |
| --- | --- |
| `schema_version` | Currently `1` |
| `capacity`, `reserve` | Total token capacity and reserved output budget |
| `next_id` | Next available segment ID |
| `planned` | Whether the saved selection is a successful current plan |
| `planned_tokens`, `planned_evicted` | Rendered token count and fresh unselected segment count, or zero when unplanned |
| `segments` | All segments, including stale and evicted material, in increasing ID order |

Each segment stores `id`, `kind`, full `text`, `content_hash`, `version`,
`generation`, estimated `tokens`, `priority`, `pinned`, `selected`, `immutable`,
`cacheable`, `stale`, `source_hash`, and the sorted `dependencies` ID array.
Kind values correspond to `forge_segment_kind`. Integers are serialized as
integers, including full-width IDs and hashes; consumers must not round them
through floating-point storage.

Import validates the schema, exact required key sets, field types/ranges,
budgets, increasing unique IDs, nonzero versions, content hashes, older existing
dependencies, sorted unique edges, transitive stale state, and selection
closure. Stale segments cannot be pinned or selected. A saved successful plan
must include every pinned segment, fit both estimated and actual budgets, and
have consistent eviction and rendered token counts. Unplanned snapshots have
no selected segments and zero saved plan counts.

The supplied token counter must match every saved segment estimate and, for a
planned snapshot, the complete rendered prompt count. A different tokenizer is
rejected with a counter-mismatch error rather than silently claiming exact
replay. To use another tokenizer, build a new context under that counter and
plan it explicitly. With the same counter and planner, an exported snapshot
imports and replans to the same selected prompt and counts; exporting the
imported state before changes yields the same JSON bytes.

Hashes detect inconsistent content; they do not authenticate the snapshot.
Snapshots contain the entire context, including evidence not selected for the
prompt. Treat them as potentially sensitive session artifacts. Import validates
structure and accounting, not the truth or authority of text inside a segment.

## Bounds and tests

A context holds at most 4,096 segments, 256 direct dependencies per segment,
65,536 total dependency edges, and 16 MiB of text. Snapshot input and output are
also bounded to 16 MiB, including JSON framing/escaping; a context at the text
limit can therefore exceed the snapshot limit and fail export explicitly.
Token and version arithmetic is checked for overflow. IDs never wrap.

`tests/unit/test_context.c` exercises shared closure costs, graph constraints,
immutability and identical updates, source/ancestor invalidation, explicit
refresh, stable memory placement, exact snapshot round trips and replanning,
malformed metadata/graphs/selections, tokenizer mismatch/overflow, empty and
failed-plan snapshots, dependency limits, a 4,096-node chain, and ID/version
rollover prevention. It uses deterministic counters
and no model, GPU, subprocess, or external fixture.
