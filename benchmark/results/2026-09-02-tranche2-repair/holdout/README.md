# Fresh holdout: complete execution, rejected comparison

**No promotion claim or validated comparative confidence interval.** All 108
scheduled runs completed, but the frozen reporter rejected a protected-file
mutation. The clean freeze alone does not make an invalid measurement acceptable.

## Per-task results

Each cell is passes out of three scheduled repetitions. All failures remain.

| Task | Domain | Forge | OpenCode | Aider |
| --- | --- | ---: | ---: | ---: |
| `holdout_go_reservations` | Atomic seat batches | 3/3 | 3/3 | 0/3 |
| `holdout_py_versions` | Atomic optimistic updates | 3/3 | 3/3 | 3/3 |
| `holdout_go_build_waves` | Dependencies | 3/3 | 3/3 | 3/3 |
| `holdout_py_closure` | Dependencies | 3/3 | 3/3 | 0/3 |
| `holdout_go_projection` | Revision/tombstone replay | 3/3 | 3/3 | 0/3 |
| `holdout_py_retractions` | Out-of-order/idempotent replay | 0/3 | 1/3 | 0/3 |
| `holdout_go_mounts` | Path component matching | 3/3 | 3/3 | 3/3 |
| `holdout_go_window` | Time-window boundary | 0/3 | 1/3 | 3/3 |
| `holdout_go_lru` | Update recency | 3/3 | 3/3 | 3/3 |
| `holdout_py_overlay` | Recursive merge/copy | 3/3 | 0/3 | 0/3 |
| `holdout_py_records` | Quoted multiline CSV | 3/3 | 3/3 | 3/3 |
| `holdout_py_queue` | Stale heap entries | 3/3 | 3/3 | 3/3 |
| **Total** | | **30/36** | **29/36** | **21/36** |

All-run E2E medians were Forge 23.39 s, OpenCode 32.43 s, and Aider 16.20 s.
These medians describe the retained records; they are not a validated latency
advantage claim. [Descriptive data](descriptive-summary.json) includes every run.

## Why analysis was rejected

`holdout_go_window-opencode-r001` changed a protected test's diagnostic message.
The [diff](opencode/holdout_go_window-opencode-r001/workspace.diff) records the
exact change. The run's `passed` value is false, and it remains in the 36-run
OpenCode denominator. The preregistered plan invalidates measurements for any
protected-file edit, not only edits to assertions.

The unmodified reporter exited 2:

```text
OpenCode changed a protected file for holdout_go_window-opencode-r001
```

[Report invocation and rejection](report-check.json), [strict audit](audit.json),
and [verbatim error](report-rejection.txt) are retained. No fallback interval,
post-hoc exclusion, replacement repetition, or mutation of a recorded outcome
was used. Re-running the reporter against the exported evidence reproduces
the rejection.

Forge's retraction task also lost 0/3 to OpenCode's 1/3. All three Forge repairs
left incorrect balances and exhausted 16 actions. All three rolling-window runs
stopped at action six after exhausting generation before a complete native call;
their diffs are empty. Independent verification failed in every case. The other
ten Forge tasks passed 3/3 with normal completion and protected files unchanged.

## Freeze and scope

- Clean revision: `5222616b2aebeae91d4942fadbc64f9a1d79c371`.
- [Original lock](protocol-lock.json):
  `e32927b0e334a5706bb38ad457218d9e0ad9294e64524bcbeb95bf75e9e2c73f`.
- [Preregistered plan](../../../holdout/2026-09-02/ANALYSIS.md), committed before
  any holdout model run; all twelve broken/reference fixture preflights passed.
- Qwen3-Coder-30B-A3B-Instruct Q4_K_M, RTX 5090 Laptop GPU, Windows; full
  model/runtime/hardware identities are in the lock and environment records.
- Cold lifecycle, context 16,384, output reserve 2,048, maximum 16 actions,
  temperature 0, decode seed 42, schedule seed 20260902.
- Forge optimized/native, OpenCode 1.18.25, Aider 0.86.2; serial harness order.
- The primary 20,000-resample task-cluster bootstrap was planned in fixed
  reporter label order OpenCode, Forge, Aider. Validation rejected the data
  before that calculation. The old 29-task reporter metadata does not define
  this population; the linked 12-task plan does.

The archive preserves complete schedules and original per-run records,
verifier streams, diffs, and failed fixture sources. The
[export integrity manifest](export-integrity.json) verifies 616 copied files
against their originals. It verifies retention, not evaluation validity.
Full raw sessions remain at the path in [audit.json](audit.json).

This observed population requires a new untouched holdout after any further
tuning. It consists of synthetic fixtures, not representative large repositories.
Forge remains a development preview.
