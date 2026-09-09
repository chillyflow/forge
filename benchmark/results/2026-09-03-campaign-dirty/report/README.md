# Local coding campaign

Lifecycle: cold. Comparable.

| Harness | Passed | Pass rate, 95% CI | E2E p50, 95% CI (s) | E2E p90 (s) | E2E stdev (s) | Generated p50 | Peak RSS p50 (GiB) | Peak VRAM p50 (GiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| OpenCode | 76/87 | 87.4% [75.9, 96.6] | 27.12 [24.69, 30.49] | 57.76 | 14.92 | 861 | 18.44 | 19.96 |
| Forge/optimized | 83/87 | 95.4% [87.4, 100.0] | 27.86 [25.03, 35.19] | 50.20 | 28.10 | 712 | 17.86 | 20.06 |
| Aider | 69/87 | 79.3% [62.1, 93.1] | 15.50 [15.25, 16.22] | 18.34 | 1.73 | 207 | 18.02 | 19.93 |

## Pairwise matched analysis

| Contrast | All pairs | Both pass | Pass-rate difference, 95% CI | Matched E2E difference, 95% CI (s) |
| --- | ---: | ---: | ---: | ---: |
| Forge/optimized minus OpenCode | 87 | 75 | 8.0% [0.0, 18.4] | 1.03 [-2.44, 5.03] |
| Aider minus OpenCode | 87 | 69 | -8.0% [-17.2, -2.3] | -9.83 [-12.11, -8.23] |
| Aider minus Forge/optimized | 87 | 69 | -16.1% [-29.9, -3.4] | -10.27 [-13.61, -7.56] |

Every run and failure remains in its source result directory. Aggregate timing covers all records; matched timing includes only pairs where both harnesses pass. Per-task, per-language, and per-category results are in summary.json.

Scope: this fixture suite, locked model, locked hardware, and cold lifecycle only.
