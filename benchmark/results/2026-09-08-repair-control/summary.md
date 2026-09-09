# Repair control diagnostic

Failures, crashes and interruptions remain in the scheduled denominator. Time summaries contain only available end-to-end measurements, across passes and failures. Warnings and context exhaustion are trace observations, not a repair-success measure. This diagnostic comparison makes no promotion or statistical advantage claim.

The historical checkpoint predates native decoding fixes and has different thinking/tool-choice defaults despite identical CLI settings. Its comparison is descriptive of the whole system; current/minimal share the current decoder.

| Task | Arm | Passes / scheduled | Observed | Median end-to-end seconds |
| --- | --- | ---: | ---: | ---: |
| generalize_retractions_distractor | checkpoint | 0/3 | 3 | 49.922 |
| generalize_retractions_distractor | current | 2/3 | 3 | 26.718 |
| generalize_retractions_distractor | minimal | 0/3 | 3 | 27.485 |
| generalize_retractions_original | checkpoint | 0/3 | 3 | 48.406 |
| generalize_retractions_original | current | 0/3 | 3 | 55.312 |
| generalize_retractions_original | minimal | 0/3 | 3 | 34.000 |
| generalize_retractions_paraphrased | checkpoint | 3/3 | 3 | 22.156 |
| generalize_retractions_paraphrased | current | 1/3 | 3 | 43.516 |
| generalize_retractions_paraphrased | minimal | 0/3 | 3 | 30.109 |
| generalize_retractions_renamed | checkpoint | 0/3 | 3 | 51.188 |
| generalize_retractions_renamed | current | 0/3 | 3 | 57.407 |
| generalize_retractions_renamed | minimal | 0/3 | 3 | 30.484 |
| go_api_pagination | checkpoint | 3/3 | 3 | 36.063 |
| go_api_pagination | current | 3/3 | 3 | 32.860 |
| go_api_pagination | minimal | 3/3 | 3 | 24.267 |
| go_multifile_transfer | checkpoint | 3/3 | 3 | 22.204 |
| go_multifile_transfer | current | 1/3 | 3 | 48.344 |
| go_multifile_transfer | minimal | 3/3 | 3 | 16.531 |

Population complete: True.
