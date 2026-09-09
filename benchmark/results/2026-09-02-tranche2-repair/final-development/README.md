# Final repair candidate: development gates

The unchanged final candidate passed **83/87** development repetitions from
clean revision `5222616b2aebeae91d4942fadbc64f9a1d79c371`. Atomic transfers
and quota allocation each passed **3/3**. All four regression tasks passed
**12/12** without loop warnings, and the invariant set passed **60/60**.
This dataset is development evidence; it does not satisfy the fresh-holdout gate.

| Development gate | Result |
| --- | --- |
| Route specificity, Python syntax, multi-file transfer, quota allocation | 12/12, no loop warnings |
| Previously reliable 20-task invariant set | 60/60 |
| Atomic transfers | 3/3, versus earlier OpenCode development 1/3 |
| Dependency ordering | 3/3, versus earlier OpenCode development 0/3 |
| Event replay | 2/3, versus earlier OpenCode development 1/3 |
| Full matrix threshold | 83/87, above the required 80/87 |

The earlier OpenCode counts are historical development observations from the
[previous campaign](../../2026-09-02-tranche2-native/README.md), not new matched
comparisons. They do not provide fresh comparative uncertainty estimates.

Atomic transfers completed in seven actions in every repetition. Quota completed
in 10, 10, and 11 actions, each with six context evictions. The aggregate all-run
E2E median was 20.03 seconds; this single-harness run is not a latency comparison.

All four failures remain in the denominator and in the exported artifacts:

- `py_api_query-optimized-r002`: malformed percent encoding still accepted;
  action limit reached without final completion.
- `reasoning_interval_union-optimized-r001` and `-r002`: interval merging still
  incorrect; both emitted one loop warning and reached the action limit.
- `reasoning_event_replay-optimized-r003`: replay still incorrect; generation
  ended without a complete native call.

Independent verification failed for all four, so none represents a successful
repair discarded solely by the old quota prompt-rendering bug. Successful runs
completed normally, passed independent verification, and retained protected files.

The [gate counts](development-gates.json) identify each task population. The
[audit](audit.json) verifies the clean freeze, all 72 frozen source hashes,
exact 87-run seeded schedule, model/runtime/configuration identities, fixture and
protected-file hashes, per-run/aggregate equality, failure artifacts, and required
measurements. The [original lock](protocol-lock.json),
[numeric analysis](analysis/summary.json), and every run's numeric record, diff,
verifier output, and failed fixture sources are retained. Full original sessions
remain at the source directory recorded in the audit.

The measured Forge executable SHA-256 is
`9abe126279d9d042a26afaf1983b1df1671092ffcd9ba6e8c0f47740c13d70be`.
The protocol SHA-256 is
`1b8b49b72a1a6d4fb887d0db933c30d19364fe7a39abffa1c1010d9bc43cd649`.
No source or executable changed during this run. Earlier candidates' successes
are not substituted for these failures.
