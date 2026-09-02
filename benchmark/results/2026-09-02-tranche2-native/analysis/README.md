# Local coding campaign

Lifecycle: cold. Comparable.

| Harness | Passed | Pass rate, 95% CI | E2E p50, 95% CI (s) | E2E p90 (s) | E2E stdev (s) | Generated p50 | Peak RSS p50 (GiB) | Peak VRAM p50 (GiB) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| OpenCode | 71/87 | 81.6% [69.0, 93.1] | 27.59 [25.24, 30.24] | 45.36 | 14.23 | 943 | 18.43 | 20.04 |
| Forge/optimized | 83/87 | 95.4% [87.4, 100.0] | 20.83 [19.34, 22.84] | 33.10 | 9.07 | 774 | 17.84 | 20.63 |
| Aider | 69/87 | 79.3% [62.1, 93.1] | 14.86 [14.70, 15.44] | 17.78 | 1.72 | 211 | 18.02 | 20.04 |

## Pairwise matched analysis

| Contrast | All pairs | Both pass | Pass-rate difference, 95% CI | Matched E2E difference, 95% CI (s) |
| --- | ---: | ---: | ---: | ---: |
| Forge/optimized minus OpenCode | 87 | 70 | 13.8% [3.4, 27.6] | -5.46 [-6.62, -4.04] |
| Aider minus OpenCode | 87 | 67 | -2.3% [-9.2, 3.4] | -10.48 [-12.52, -9.27] |
| Aider minus Forge/optimized | 87 | 68 | -16.1% [-31.0, -3.4] | -4.94 [-6.20, -3.72] |

Every run and failure remains in its source result directory. Aggregate timing covers all records; matched timing includes only pairs where both harnesses pass. Per-task, per-language, and per-category results are in summary.json.

Scope: this fixture suite, locked model, locked hardware, and cold lifecycle only.
