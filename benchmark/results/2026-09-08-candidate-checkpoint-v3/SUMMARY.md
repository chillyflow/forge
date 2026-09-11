# Repair control diagnostic

Failures, crashes and interruptions remain in the scheduled denominator. Time summaries contain only available end-to-end measurements, across passes and failures. Warnings and context exhaustion are trace observations, not a repair-success measure. This diagnostic comparison makes no promotion or statistical advantage claim.

Same-binary minimal versus opt-in candidate checkpoints; no historical arm.

| Task | Arm | Passes / scheduled | Observed | Median end-to-end seconds |
| --- | --- | ---: | ---: | ---: |
| generalize_retractions_distractor | candidate | 3/3 | 3 | 27.516 |
| generalize_retractions_distractor | minimal | 0/3 | 3 | 28.389 |
| generalize_retractions_original | candidate | 0/3 | 3 | 38.563 |
| generalize_retractions_original | minimal | 0/3 | 3 | 29.249 |
| generalize_retractions_paraphrased | candidate | 0/3 | 3 | 27.140 |
| generalize_retractions_paraphrased | minimal | 0/3 | 3 | 29.766 |
| generalize_retractions_renamed | candidate | 0/3 | 3 | 36.312 |
| generalize_retractions_renamed | minimal | 0/3 | 3 | 29.453 |
| go_api_pagination | candidate | 3/3 | 3 | 27.719 |
| go_api_pagination | minimal | 3/3 | 3 | 27.953 |
| go_multifile_transfer | candidate | 3/3 | 3 | 20.969 |
| go_multifile_transfer | minimal | 3/3 | 3 | 17.812 |

Population complete: True.
