# W1 implementation map, and two corrections to the plan

Read-only source investigation, September 9 2026, made while W0 was executing.
No code was changed. W1 is **not** built on the strength of this document: the
plan makes W1 depend on W0's result, not merely its existence.

## Two places the plan's text does not match the tree

**1. The identical-edit rejection is not in `src/core/agent.c`.**
The plan lists W1's primary file as `src/core/agent.c`. The rejection is
actually produced in `src/tools/tools.c` at three sites, all emitting the same
string:

- `fg_tool_execute` pre-dispatch gate, `src/tools/tools.c:1636`, which fires
  first in practice and rejects before policy or filesystem work;
- `patch()` head, `src/tools/tools.c:1155`;
- `patch()` post-splice byte-equality check, `src/tools/tools.c:1249`.

`agent.c` only formats the resulting `forge_error` into `TOOL_ERROR [...]` text
(`src/core/agent.c:1690`). The *append* site that W1 must intervene at is in
`agent.c` (see below), so both files are in scope — but the discriminator has to
be established in `tools.c`.

Critically, the identical rejection and the anchor-mismatch rejection
(`src/tools/tools.c:1207`) **share `FORGE_ERR_CONFLICT`**, so the error code
alone cannot tell them apart. W1 must key on the branch — a sentinel on the tool
context or a distinct error subcode set at `tools.c:1641` — not on the status.
String-matching the message would work but is fragile.

**2. The plan's `last_patch_*` constraint describes a loop `--bounded-repair`
never enters.**
The plan's mechanical constraints say the `last_patch_path`/`old`/`new` tracking
and stale-`old_text` re-anchoring "in `src/core/agent.c`" must keep working.
Those live in `forge_agent_run` (`src/core/agent.c:1847`, `:2634-2724`).
`--bounded-repair` requires `--candidate-checkpoint`, which requires
`--minimal-agent`, which routes to `minimal_run` (`src/core/agent.c:1245-1821`).
So that machinery is not on this path at all.

The "previous attempted delta" evidence that *is* on the path is
`checkpoint.last_delta` (`src/core/agent.c:1714`), and it is gated on the edit
**succeeding**. A rejected identical edit therefore already contributes nothing
to it. Two independent guards make this robust: the re-anchoring condition
explicitly excludes identical edits (`strcmp(patch_old, patch_new)` at
`agent.c:2654`), and `last_patch_*` is only written under `if (changed)`
(`agent.c:2922`), which a rejection never reaches.

Consequence: W1's acceptance item "the bounded-repair previous attempted delta
evidence remaining correct" is satisfied by construction, but must still carry a
regression asserting it is *unchanged*, because that is what the plan asks to be
demonstrated.

## Where the intervention goes

Retention happens at `src/core/agent.c:1750`:

```c
uint64_t id = minimal_append(ctx, FORGE_SEG_ACTION, action, 0);
if (!id || !minimal_append(ctx, FORGE_SEG_RESULT, visible, id))
```

`minimal_append` (`agent.c:816`) adds the segment pinned at priority 100 and
immediately seals it immutable, and `forge_context_update` refuses to change an
immutable segment's text (`src/context/context.c:225`). So the substitution must
happen **before** the append, not after.

The ACTION and RESULT are inseparable downstream: `src/context/context.c:508`
pairs them into `{"role":"assistant","tool_calls":[...]}` plus its matching
`{"role":"tool",...}`, `render_selected_native` fails an unpaired ACTION
(`context.c:519`), and `validate_native_history`
(`src/inference/chat_template.cpp:54-105`) throws for either half alone.

Two viable shapes, to be chosen if W1 is built:

- **Rewrite the ACTION's `args` in place** — keep a well-formed
  `apply_patch` call but replace `old_text`/`new_text` with a digest and byte
  length. Preserves the pairing, so no other site changes. Lowest risk.
- **Replace the pair with a single `FORGE_SEG_MEMORY` observation**, which
  renders as a plain user message (`context.c:526`) and is pairing-safe. But it
  drops the RESULT, so `latest_result` selection at `agent.c:1436-1446` would
  pin an older result. Higher blast radius.

An in-tree precedent for the wording and placement of a compact host observation
already exists: `PREVIOUS_APPLIED_DELTA`, `src/core/agent.c:1174`.

**The raw action survives regardless**, which is what the plan requires: the
`tool_call` event carries the full action JSON (`agent.c:1677`) and the raw
result is written to `tool/%06zu.raw` (`agent.c:1733`) *before* the visible text
is bounded and appended at `agent.c:1750`. The existing prompt text already
promises this contract at `agent.c:1157`.

## Flag plumbing, confirmed sites

- `include/forge/forge.h:181` — bool at the end of `forge_agent_config`,
  beside `minimal_agent` (`:162`), `candidate_checkpoint` (`:166`),
  `bounded_repair` (`:181`).
- `src/cli/main.c:50` — usage text.
- `src/cli/main.c:130` — the zero-arity `option_arity` table. Omitting this
  silently swallows the next argument (see the comment at `main.c:111`).
- `src/cli/main.c:729` — the parse block.
- `src/cli/main.c:968` — the compatibility validation requiring
  `--candidate-checkpoint`.
- **`src/core/agent.c:179` — a duplicate of that same validation inside
  `forge_agent_create`.** The plan's flag-site list omits this one; if only the
  CLI check is updated, the library accepts what the CLI rejects.
- `benchmark/run.py` `VARIANTS` — without it the harness cannot select the arm.

Not needed: `src/core/config.c` has no entries for these flags (CLI-only), and
`src/core/candidate_search.c` copies the whole config struct (`:124`, `:174`), so
a new bool propagates to trials automatically.

`validate_native_tools` (`src/inference/chat_template.cpp:107-148`) needs **no**
change: its checks are exact-size, exact-name whitelists over the tool schema
array, and a flag that changes no registry leaves the six-name `candidate` set
intact. The validator that *can* be tripped is `validate_native_history`, and
only if the ACTION/RESULT pairing is broken.
