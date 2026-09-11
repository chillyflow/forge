# W2 — prerequisites and demonstrated defects

Shipped as its own candidate, screened separately from W1, because two of these
items act on the same mechanism W1 tested and bundling them would have made the
W1 screen unattributable.

**These items claim nothing for the pass rate.** They are correct host
behaviour. Any pass-rate movement in the W2 screen is noise unless it is large,
and is reported as such.

## Built

### Prerequisite (blocking): the verification bytecode-cache hole

Closed. Full analysis and measurements in `W2-CACHE-PREREQUISITE.md`.

Python `-B` suppresses bytecode *writes* and has no effect on which bytecode is
*loaded*. A `__pycache__` written into the workspace by a model-issued
`run_command` — which inherits no bytecode policy, because the process
environment allowlist in `src/core/process.c:296` carries neither
`PYTHONDONTWRITEBYTECODE` nor `PYTHONPYCACHEPREFIX` — could therefore satisfy an
import during validation and produce a stale pass whose input hashes are
unchanged.

Fixed by **both** redirecting the cache and suppressing writes. Redirecting
alone is not sufficient and was shipped wrong once: a written prefix caches
workspace modules across a run, and because timestamp invalidation compares only
mtime and size, a later same-length edit inside the mtime resolution is not
detected — so a *correct* repair can be validated against the previous
candidate's bytecode. That was observed directly against the previous binary and
is now guarded by its own regression. See `candidate-05-host-defects/README.md`.

- `src/repo/validation.c` — the three Python test stages now pass **both** `-B`
  and `-X pycache_prefix=.forge/pycache`. The prefix keeps the workspace
  `__pycache__` out of the import path; `-B` keeps the prefix itself empty, so
  every import compiles from current source and no candidate's bytecode can
  outlive it. `.forge` is already excluded from the input snapshot at the root,
  so candidate input hashes are not perturbed. The syntax stage is unchanged: it
  calls `compile()` on bytes without importing and is genuinely immune.
- `benchmark/common.py` — new `verification_environment()` sets
  `PYTHONPYCACHEPREFIX` for the independent verifier.
- `benchmark/run.py` — one prefix per invocation, created outside every
  workspace and shared across that invocation's runs, removed at exit.
- `benchmark/replay_loop_pressure.py` — the third verifier path, which the
  prepared `cache-fix.patch` did not touch, gets the same isolation.
- Reported guarantees corrected: `bytecode_writes_disabled` (now false, and
  misleading) is replaced by `bytecode_cache_isolated` plus
  `bytecode_cache_prefix`, and the user-visible `limitations` string now
  describes what is actually guaranteed.

**On `cache-fix.patch`.** Its correctness approach — an unwritten prefix — was
right, and this campaign initially rejected it on cost grounds that turned out
to be overstated. A microbenchmark of a bare `import unittest` under a fresh
prefix suggested roughly +400 ms per command; **the measured cost on the real
validation workload is +44 ms** (79 → 123 ms per command, +0.33 s per run
against a 600-second task limit). Correctness and cost were never actually in
tension here.

Its genuine defects remain and are not adopted: the wrapper sets the prefix
*after* the runner is imported, its pytest half was never executed while its
user-visible `limitations` string asserted the bypass worked, it gates on
`task['language']` rather than the verifier's interpreter, and it leaves the
third verifier path untouched.

Regressions in `tests/unit/test_benchmark.py` fabricate the exact stale-cache
condition — a `.pyc` whose source changed underneath it with size and mtime
preserved — and assert both halves: that `PYTHONDONTWRITEBYTECODE` still loads
the **stale** value (which is why `-B` is not the fix), and that the isolated
prefix loads the **current** source.

### Model prose excluded from the applied-delta record

`src/core/agent.c` — `minimal_delta_record()` strips `assistant_content` and
`thought` before `fg_compress_output`. The compressor keeps a head and a tail
and clips the middle, so verbose prose at the front consumed the head and the
clip landed on `old_text` and often the argument envelope, leaving only the tail
of `new_text`. The prose is not lost: the complete action remains in the
`tool_call` event. An unparseable action keeps its original record rather than
losing it, so a compression failure cannot abort a run whose edit is already on
disk.

### Silent `run_command` annotated

`src/tools/tools.c` — a command that exits 0 having written nothing to stdout or
stderr now carries a host observation saying exactly that, and that empty output
is not evidence any test was collected or executed.

Deliberately **not** gated on the ordinary loop, unlike the adjacent
`next_action_guidance` (which is gated on `!minimal_agent` and therefore never
reaches the arms under test). It is an observation, not advice.

Per the plan, this does **not** discriminate: all six candidate-2 failures and
both passes issued at least one such command. Whether the model then formed a
false success belief is not established by retained evidence, and this change
must not be presented as if it were a lever.

### `agent_mode` capability claims made honest

`src/core/agent.c` — `recovery` and `corrective_prompts` were hardcoded `true`
for both the bounded-repair and candidate-checkpoint modes regardless of
configuration, while `--failure-reflection` was not in the profile and
`loop_warnings` was 0 in all twelve candidate-2 runs. They are now derived from
`config.failure_reflection` and `config.semantic_loops` respectively.

## Deliberately not built, with reasons

**Report whether the file already contains the intended `new_text`.** The
identical-replacement rejection fires in `fg_tool_execute`'s pre-dispatch gate
(`src/tools/tools.c:1636`), deliberately *before* policy and filesystem work, so
the file has not been read at that point. Adding the check means either reading
a file before its policy check or moving the rejection after path resolution —
a reordering of a security-sensitive path for an item that claims nothing.
Recorded as not done rather than done riskily.

**Stop the validation planner sweeping model-authored scratch scripts into the
authoritative `broad_tests` argv.** The planner sources test files from the repo
index (`src/repo/validation.c:275`) and has no notion of which files existed
when the run started; supplying it requires threading the run's initial input
snapshot into an index-driven layer. A rushed version risks *narrowing* the
authoritative broad check, which the parent plan forbids outright ("Keep
authoritative broad final verification"). Worth doing properly with its own
design pass.

Note also that inclusion alone cannot turn a failing suite green — `broad_tests`
fails if any test fails. The demonstrated harm is that the model's own
reassuring stdout is prepended to the host verdict it reads back, which is an
output-attribution problem as much as a selection problem. Whichever framing is
chosen should be settled before implementing.

**Compaction prefix-reuse oscillation.** The plan asks for an investigation
starting from the alternating pattern rather than a single collapse, and permits
recording the measured re-prefill cost as a known cost of bounded repair if
reuse cannot be preserved. Not investigated in this pass. The measured cost from
candidate 2 stands as recorded: in `renamed-s42-r002`, 112,206 tokens and
44.4 seconds — 16.5% of the run — spent re-prefilling.
