# I1 implementation report — eight safe-now fixes

Repo: C:/Users/flowc/dev/forge (branch main, HEAD d16e6736, uncommitted working tree)
Implementer: subagent I1. No commit, no push. Build: `build-gpu` Release.
Build command: `.tools/bin/cmake.exe --build build-gpu --config Release --parallel` (PYTHONPATH=C:/Users/flowc/dev/forge/.tools) → **exit 0**.
Warning scan of a forced full recompile of the touched files: only 4 pre-existing warnings, all in `build/_deps/llama-src` headers (C4305/C4244/C4505). None from the edited files.

## Files changed (git diff --stat)

```
 src/core/agent.c                | 26 +++++++++++------
 src/core/process.c              | 64 ++++++++++++++++++++++++++++++-----------
 src/core/text.c                 |  8 ++++++
 src/inference/chat_template.cpp | 23 ++++++++++-----
 src/internal.h                  |  4 +++
 src/judge/judge.c               | 16 ++++++++++-
 src/repo/watch.c                | 15 ++++++----
 src/tools/tools.c               | 43 ++++++++++++++++++++++-----
 8 files changed, 155 insertions(+), 44 deletions(-)
```

## Per item

### 1. src/core/text.c — fg_normalize_workspace_paths (was :172)
Changed: the second `replace_all` pass is now skipped when the escaped pattern equals the
literal root. Guard tightened beyond the audit wording: skip requires
`strcmp(pattern, root) == 0 && strchr(root, '.') == NULL`.
Why the extra condition: the audit claim "when pattern==root the second pass cannot match
anything" is not universally true. A replacement "." can re-form the root when root itself
contains a '.'; an exact replica of text.c's `replace_all` found 67,324 differing small cases
and 204 differing fuzz cases, **all** with a '.' in the root (e.g. root="/a/b.c",
text="/a/b/a/b.cc": baseline ".", skipped "/a/b.c"). A 200,000-case fuzz over dotless roots
(and an exhaustive search over the alphabet "/.ab" up to length 7/8) shows **zero** cases
where the skip changes output, so the tightened guard is byte-identical by construction.
Roots containing '.' (and Windows roots with backslashes, where pattern != root) keep both
passes exactly as before.
Test: forge_unit (covers test_core.c test_workspace_path_normalization) PASS; full suite PASS.

### 2. src/inference/chat_template.cpp — capability caching (was :299-307)
Changed: `fg_chat_templates` now caches `supports_system_role`, `supports_tools`,
`supports_tool_calls`, computed once in `fg_chat_templates_create` (alongside the existing
`supports_thinking` / `rejects_user_after_tool` caches) from
`common_chat_templates_get_caps`; the native renderer reads the cached booleans instead of
re-fetching the capability map per render. Verified in the vendored llama.cpp source that
`common_chat_templates_get_caps` is a pure accessor over the parsed template objects (caps are
fixed at init; no throwing, no mutation), so create-time caching is equivalent to render-time.
Test: forge_chat_template_unit PASS; chat_template CTest PASS.

### 3. src/core/agent.c — tool-output token counts (was :3626/:3664/:3686)
Changed: the raw capture's token count is computed once (`raw_tokens`) and still added to
`metrics.raw_tool_tokens`. After the visible view is built, `visible_is_raw` is computed by
byte comparison against the raw capture; when identical, the budget loop and the
`visible_tool_tokens` metric reuse `raw_tokens` instead of re-tokenizing the same bytes.
When the loop cuts (or the marker is appended after a cut), the count is recomputed, so
`metrics.visible_tool_tokens` still equals a fresh `fg_model_count` of the final visible bytes
on every path (identical+unreduced: same bytes; compressed/truncated: fresh count; reduced:
fresh count after the marker is appended). `visible_is_raw` is NULL-guarded and computed
before `free(raw)`.
Test: forge_unit PASS (agent metrics assertions), agent integration tests PASS
(minimal_agent_integration, loop_extensions, bounded_repair, agent_loop_acceptance).

### 4. src/core/agent.c + src/tools/tools.c + src/internal.h — one signature serialization
Changed: `fg_tool_signature`'s field loop was extracted into a static `signature_fields()` in
tools.c (behaviour of `fg_tool_signature` unchanged — test_core.c:423-429 still passes), and a
new `fg_tool_signatures(name, args, generation, diagnostic_hash, &signature, &strategy)`
serializes the registry-ordered argument fields **once** and hashes the same canonical text
under both prefixes (`name:gen:diag` and `name:0:0`). agent.c:3017-3019 now calls it once
instead of calling `fg_tool_signature` twice. Both hash values and the `!signature ||
!strategy_signature` failure gate are preserved bit-for-bit (failure/OOM still yields 0 for
both).
SCOPE NOTE (deviation): this item required adding the helper where the field registry lives —
`src/tools/tools.c` (+ its declaration in `src/internal.h`), which are outside the task's
listed files. The audit's own recommendation is "add a fg_tool_signature variant taking the
prebuilt field string" and its refuter recorded "an internal canonical-text helper is safe to
add"; `fg_tool_signature` and all existing callers are untouched.
Test: forge_unit PASS (test_core signature stability asserts), agent integration tests PASS.

### 5. src/core/agent.c — one diagnostic hash (was :2769/:2817)
Changed: `uint64_t failure_hash = fg_diagnostic_hash(verification.summary);` computed once;
`failure = failure_hash ^ generation` and, later on the same conflict path,
`diagnostic_hash = failure_hash`. Both statements are inside the same `verified != FORGE_OK`
block (the :2817 site is only reachable after the :2769 site executes), so the second hash
call is removed with identical values. `verification` is not mutated between the two sites.
Test: forge_unit PASS; bounded_repair + loop_extensions PASS (these exercise validation-failure
and recovery paths).

### 6. src/core/process.c — quote_arg / command line (was :163)
Changed: `quote_arg` (Win32) now writes escape runs with `memset` into capacity reserved by a
new `command_line()` helper, which sizes the whole command line once from the summed worst
case (`2*len+3` per argument + separators + NUL, with an overflow guard) instead of letting
`fg_buf` grow per append. Output is byte-identical by construction and by measurement: a
differential harness compiled with the **real** `src/core/process.c` included (MSVC, same
flags) compared the new `quote_arg`/`command_line` against the pre-fix byte-at-a-time
implementation over **400,047 cases — 0 failures**, including empty args, quote/backslash
mixtures, 1 MiB all-backslash and 1 MiB mixed runs; every single-argument case also ran with
a 4 KiB guard zone after the exact reserved capacity and no guard byte was ever touched.
Test: forge_process_unit PASS; process CTest PASS (real process spawns with
backslash-containing path arguments).

### 7. src/repo/watch.c — empty-batch fast path (was :1554)
Changed: `watch_json` skips the `qsort` when `event_count == 0` and skips the event free loop
plus `event_map` memset when `event_count == 0`; `event_count`/`event_bytes` are still reset
unconditionally. Verified invariant: `event_map` entries are only ever created together with
events (watch_event_add), and the only reset site is this function, so an empty event list
implies an all-zero map — the skipped memset is a no-op. Empty-batch JSON construction is
untouched, so output is identical.
Test: forge_watch_unit PASS; watch + agent_watch + monitor CTest PASS.

### 8. src/judge/judge.c — collision-free record names (was :533/:577)
Changed: record filenames are now `judge-<utc>-<pid>-<seq>.json` (pid from `_getpid()` on
Windows / `getpid()` on POSIX, new `process_id()` helper; `<process.h>` / `<unistd.h>`
includes added under the existing platform guards). Record JSON schema and fields unchanged;
only the filename gains the process-unique component, so concurrent processes can no longer
overwrite each other's `judge-<utc>-0000.json`.
Test: forge_judge_unit PASS — its record tests glob `judge-*.json`, count success+failure
records (== 2), and assert record fields, all still passing.

## Test results (exact)

Affected binaries run directly from `build-gpu/Release/` (all exit 0):

```
forge_unit              exit=0
forge_process_unit      exit=0
forge_watch_unit        exit=0
forge_judge_unit        exit=0
forge_agent_changes_unit exit=0
forge_chat_template_unit exit=0
forge_monitor_unit      exit=0
forge_agent_watch_unit  exit=0
```

Full CTest suite (`ctest --test-dir build-gpu -C Release --output-on-failure`), run in two
regex batches covering all 36 tests:

- unit tier (24 tests): **24/24 passed**, `checkpoint_model` skipped (needs a model, normal).
- integration tier (12 tests): **12/12 passed** (integration, config_integration,
  output_integration, minimal_agent_integration, interactive_integration, loop_extensions,
  bounded_repair, git_policy_integration, benchmark_fixtures, repair_control,
  engagement_screen, agent_loop_acceptance).
- Overall: **35 passed, 1 skipped, 0 failed.**

## Evidence artifacts (this run dir)

- `scratch_replace_all.c` — exact-replica harness for item 1 (run result recorded in NOTES below).
- `quote_diff_test.c`, `run_quote_test.bat`, `quote_diff_test.exe` — item 6 differential/bounds
  harness: "quote_arg differential: 400047 cases, 0 failures".
- `build_i1.log` — forced full-recompile log (warning scan).
- `ctest_i1.log`, `ctest_i1_integration.log` — CTest logs.
- `test_<name>.out` — direct binary outputs.

## NOTES / deviations

- Item 1 guard is stricter than the audit text (adds `strchr(root, '.') == NULL`); see item 1
  above for the measured counterexamples that motivate it. Skip behaviour is unchanged for
  the common dotless POSIX roots; Windows roots (backslashes) never took the skip path.
- Item 4 touched `src/tools/tools.c` and `src/internal.h` (helper + declaration) in addition to
  the listed files; the alternative was duplicating the field registry in agent.c. Flagged for
  the parent's review.
- Item 3 left the analogous metric site at agent.c:2095-2106 untouched (not in the audit's
  cited lines).
- Nothing was committed; `git status` shows only the 8 modified source files plus the
  pre-existing untracked evidence dirs (`benchmark/results/2026-09-13-nvidia-llamacpp-support/`,
  `docs/research/`).
