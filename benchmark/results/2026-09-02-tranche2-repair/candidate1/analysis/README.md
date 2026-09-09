# Local coding campaign

Lifecycle: cold. Comparable.

| Harness | Passed | Pass rate, 95% CI | E2E p50, 95% CI (s) | E2E p90 (s) | E2E stdev (s) | Generated p50 | Peak RSS p50 (GiB) | Peak VRAM p50 (GiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Forge/optimized | 82/87 | 94.3% [86.2, 100.0] | 20.38 [18.95, 24.19] | 33.82 | 7.43 | 730 | 17.85 | 20.31 |

## Pairwise matched analysis

| Contrast | All pairs | Both pass | Pass-rate difference, 95% CI | Matched E2E difference, 95% CI (s) |
| --- | ---: | ---: | ---: | ---: |

Every run and failure remains in its source result directory. Aggregate timing covers all records; matched timing includes only pairs where both harnesses pass. Per-task, per-language, and per-category results are in summary.json.

Scope: this fixture suite, locked model, locked hardware, and cold lifecycle only.
