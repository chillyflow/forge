# Typed working state

`include/forge/state.h` provides a bounded state component for one agent run. The goal and observed evidence are owned by host code; model updates can change only explicitly designated notes and arrays. It does not execute tools, decide whether a test passed, load a model, or implement session/KV resume.

## API

| Function | Purpose |
| --- | --- |
| `forge_working_state_create(goal, error)` | Copy a nonempty immutable goal into a new state at generation zero. |
| `forge_working_state_update_json(state, memory_value_json, error)` | Transactionally update model-authored notes or typed fields. Pass the JSON value of a memory action, not its outer action wrapper. |
| `forge_working_state_observe(state, observation, error)` | Add deterministic host evidence about a tool or automatic validation action. |
| `forge_working_state_set_validation(state, generation, status, detail, error)` | Set host-observed validation for a particular repository generation. |
| `forge_working_state_json(state, error)` | Return the complete retained audit state as compact, deterministic, schema-versioned JSON. |
| `forge_working_state_context_json(state, max_bytes, error)` | Return a compact prompt view within an exact serialized-byte budget. |
| `forge_working_state_destroy(state)` | Release all owned memory; a null pointer is allowed. |

Inputs are copied. Returned JSON is caller-owned and must be released with `forge_free`. A state may be used without terminal or inference components, but callers must serialize access; concurrent mutation and serialization of the same instance are not supported.

## Model update contract

An update accepts either a legacy JSON string or an object with exactly these five arrays:

```json
{
  "facts": ["The failing package is storage"],
  "hypotheses": ["The delete path may skip cleanup"],
  "decisions": ["Keep the existing public API"],
  "relevant_files": ["storage/delete.go"],
  "remaining": ["Run the affected package tests"]
}
```

All five fields are required. Unknown, duplicate, missing, or incorrectly typed fields are rejected. Array elements must be strings. JSON strings containing embedded NUL, invalid Unicode, malformed JSON, and exceeded limits are rejected. Failed updates leave the entire state unchanged, including model memory, observed evidence, staleness, and validation.

A legacy string replaces only `model_notes` and preserves typed fields. An object replaces all five typed arrays and preserves existing legacy notes. Both share one cumulative 8192-byte decoded UTF-8 payload budget. To reclaim old legacy notes, submit the empty JSON string `""`; the typed arrays remain intact.

These fields are **model claims**, even when an item says that tests passed. An update cannot change `goal`, `generation`, `observed_changes`, `recent_outcomes`, `validation`, or overflow counters. Model text is never parsed as an instruction to update observed evidence.

## Observations and validation

`forge_state_observation` carries `tool_call_id`, `tool_name`, an optional workspace-relative `path`, a `forge_status result`, optional `detail`, repository `generation`, and `changed`. A changed observation requires a path. Call ID zero is permitted for synthetic host actions.

`result` describes the host-observed tool outcome. A caller can use `FORGE_ERR_CONFLICT` for a command that ran but returned a failing exit status. `FORGE_OK` does not promote validation to passed: reading a file or successfully launching a process is not proof of correctness.

Only the host validation API accepts the following statuses:

| Enum | JSON status |
| --- | --- |
| `FORGE_STATE_UNVERIFIED` | `unverified` |
| `FORGE_STATE_PASSED` | `passed` |
| `FORGE_STATE_FAILED` | `failed` |
| `FORGE_STATE_DENIED` | `denied` |
| `FORGE_STATE_NOT_APPLICABLE` | `not_applicable` |

Generations must not go backward. A newer generation invalidates old validation and marks existing model memory stale without deleting its contents. A host-reported change also invalidates validation and model memory at the same generation, conservatively covering an index integration that has not advanced its generation yet.

Legacy notes and typed fields have separate update generations and stale flags. Updating only legacy notes cannot make stale facts or decisions current. Updating the typed object cannot refresh untouched legacy notes. The combined `model_memory_stale` flag is true if either retained part is stale.

After an independent verifier finishes, host code may set validation and then record an `automatic_validation` observation with `changed = false` at the same generation. That observation preserves the explicit validation result. An older verifier result is rejected with `FORGE_ERR_CONFLICT`.

## Bounds and overflow

| Data | Limit and behavior |
| --- | --- |
| Goal | 8192 UTF-8 bytes; nonempty; oversized/invalid input fails. |
| All model notes and array strings combined | 8192 decoded UTF-8 bytes; overflow fails transactionally. |
| Each model array | 32 items. |
| Each model array string | 512 decoded UTF-8 bytes. |
| Serialized model update | 64 KiB, including escaping and whitespace. |
| Tool name | 128 UTF-8 bytes, nonempty. |
| Observed path | 4095 UTF-8 bytes; backslashes normalize to forward slashes. Absolute paths, drive prefixes, and empty/dot/parent components are rejected. |
| Distinct changed paths | 1024 normalized textual paths. Repeated observations update an existing entry instead of consuming another slot. |
| Recent outcomes | 32 records; older records are evicted with an explicit count. |
| Outcome/validation detail | Retain at most 512 UTF-8 bytes, ending at a complete code point; record omitted byte counts. Inputs larger than 16 MiB fail. |

Changed-path equality is normalized textual equality, not filesystem identity or type resolution. This component does not resolve paths, touch files, or replace the tool runtime's security policy. Host strings are NUL-terminated UTF-8; the JSON update path additionally detects escaped embedded NUL.

An observation for a new 1025th changed path returns `FORGE_ERR_LIMIT`. It also increments `overflow.changed_paths_rejected`, marks `evidence_incomplete`, advances to a valid newer generation if supplied, and invalidates validation. This explicit fail-closed metadata change is intentional; the unretained path is not silently accepted. A later `PASSED` request is rejected for an incomplete state. Callers should stop or surface the limit, not ignore it.

Other overflow counters are cumulative: `recent_outcomes_evicted`, `outcome_detail_bytes_omitted`, and `validation_detail_bytes_omitted`. Counters saturate at their integer maximum and set `counters_saturated` instead of wrapping. Per-record detail omission counts remain in retained outcomes and current validation.

## JSON artifacts and prompt views

The full JSON contains schema version 1, immutable goal, current generation, legacy notes, all typed arrays (including `remaining`), memory update generations/staleness, distinct observed changes, recent tool outcomes, current validation, and overflow metadata. It is the full **retained state**, not an unbounded transcript: historical outcome evictions and detail omissions are explicit.

Observed changes record first/last generation and observation count. The full audit array keeps first-observed path order; full recent outcomes are chronological. JSON serialization does not mutate state.

Save the full result as `working_state.json` when using the prompt view. The prompt API always preserves the goal, **all** model notes/facts/hypotheses/decisions/relevant files/remaining items, current validation, staleness, and overflow metadata. It considers host evidence from newest to oldest, including a recently changed old path before an older untouched one. Whole records that do not fit may be omitted; smaller later candidates can still fit.

The view includes:

```json
{
  "full_state_artifact": "working_state.json",
  "context_omitted": {
    "observed_changes": 12,
    "recent_outcomes": 8
  }
}
```

These example counts describe omissions from the current full state. They are separate from historical outcome evictions. Included evidence in the prompt view is ordered by recency, newest first. The full retained state is unchanged.

If mandatory core data alone exceeds `max_bytes`, the prompt API returns null with `FORGE_ERR_LIMIT`. It never truncates the goal or model memory to make the view fit. The byte cap includes actual JSON escaping and metadata; callers must still check the final model token budget with the real tokenizer.

## Verification

The agent refreshes state before the first compacted prompt and retries smaller
optional-evidence views against the actual whole-prompt token count.
`forge_working_state_context_core_json` supplies the smallest view, preserving
every mandatory field and explicitly counting all omitted host evidence.
If this core plus other pinned context cannot fit, the run stops. A launched
command invalidates validation even when no indexed source changed.

[Unit tests](../tests/unit/test_state.c) cover transactional rejection after partial parsing, immutable/copied goals, duplicate/unknown/missing fields, forbidden evidence updates, NUL and invalid Unicode, exact cumulative memory limits, preservation of typed fields during legacy updates, separate staleness, generation-associated host validation, normalized distinct paths, evidence-capacity failure, Unicode-safe detail truncation, history accounting, and prompt byte limits/core retention/recency.

The integrated Windows Debug suite passes state tests and scripted agent tests
at 4,096- and 8,192-token capacities, including first-compaction freshness and
retained decisions. Real Go integration covers failed validation, repair, and
command-only fixture edits. Scripted inference does not establish model
accuracy or performance; physical session/KV resume remains separate work.
