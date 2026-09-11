# Repair control diagnostic

Failures, crashes and interruptions remain in the scheduled denominator. Time summaries contain only available end-to-end measurements, across passes and failures. Warnings and context exhaustion are trace observations, not a repair-success measure. This diagnostic comparison makes no promotion or statistical advantage claim.

Same-binary minimal versus opt-in candidate checkpoints; no historical arm.

| Task | Arm | Passes / scheduled | Observed | Median end-to-end seconds |
| --- | --- | ---: | ---: | ---: |
| generalize_retractions_distractor | candidate | 0/3 | 3 | 43.828 |
| generalize_retractions_distractor | minimal | 0/3 | 3 | 28.251 |
| generalize_retractions_original | candidate | 1/3 | 3 | 41.016 |
| generalize_retractions_original | minimal | 0/3 | 3 | 33.031 |
| generalize_retractions_paraphrased | candidate | 0/3 | 3 | 55.078 |
| generalize_retractions_paraphrased | minimal | 1/3 | 3 | 30.156 |
| generalize_retractions_renamed | candidate | 0/3 | 3 | 46.328 |
| generalize_retractions_renamed | minimal | 0/3 | 3 | 27.000 |
| go_api_pagination | candidate | 3/3 | 3 | 41.313 |
| go_api_pagination | minimal | 3/3 | 3 | 26.187 |
| go_multifile_transfer | candidate | 3/3 | 3 | 28.140 |
| go_multifile_transfer | minimal | 3/3 | 3 | 19.345 |

Population complete: True.
