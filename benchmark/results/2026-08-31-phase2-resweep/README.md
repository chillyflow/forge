# Phase 2 all-arm re-sweep

This is the scheduled same-binary re-sweep after bounded routed reasoning and
action-completion early stop changed every grammar arm, including the baselines.
It supersedes earlier smoke-suite numbers for comparisons involving the Phase 2
decode mechanism. All 100 records use one Qwen3-Coder-30B-A3B-Instruct Q4_K_M
model and one Forge executable.

## Identity

| property | value |
| --- | --- |
| Forge binary SHA-256 | `d92057a498fa21921248c91f10ef40a1a879f6dc66a0c44daeb68083abd04fef` |
| Model SHA-256 | `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad` |
| Platform | WSL2 Linux, NVIDIA RTX 5090 Laptop GPU, driver 616.56 |
| Context / output / turns | 16,384 / 2,048 / 16 |
| Sampling | temperature 0, seed 42 |
| Fixtures | `utf8-lf-gofmt-v1`, ten smoke tasks |

`consolidate_arms.py` verified the binary, model, preparation, task set,
context, output reserve, turn cap, template, sampling, platform, Go version,
and GPU identity across every arm. Raw consolidated records and the action
census are retained in this directory.

## Results

| arm | passed | turns | prompt tokens | generated | thought actions |
| --- | ---: | ---: | ---: | ---: | ---: |
| no-thought | 10/10 | 62 | 87,908 | 2,848 | 0/62 |
| optimized | 10/10 | 63 | 91,233 | 2,743 | 0/63 |
| thought-optional-decode-only | 10/10 | 63 | 91,233 | 2,743 | 0/63 |
| thought-required | 4/10 | 115 | 286,994 | 12,724 | 115/115 inline |
| thought-required-decode-only | 7/10 | 76 | 123,355 | 9,944 | 76/76 inline |
| thought-routed | 7/10 | 65 | 122,017 | 15,252 | 64/64 routed |
| thought-routed-decode-only | 8/10 | 65 | 126,435 | 13,867 | 65/65 routed |
| thought-routed-required | 8/10 | 61 | 120,207 | 12,862 | 60/60 routed |
| thought-routed-required-decode-only | 10/10 | 46 | 65,370 | 11,649 | 46/46 routed |
| thought-routed-unbounded-decode-only | 8/10 | 52 | 80,195 | 10,092 | 51/51 routed |

The bounded routed arms forced seven action transitions. All seven recorded a
first action-progress token immediately after the swap, so the forced state did
not remain stuck in leading grammar whitespace. `action_stops` tracks completed
objects in every arm; for example, it is 62/62 for `no-thought` and 63/63 for
`optimized`.

The executable in this sweep used the Phase 2 provisional default of half the
2,048-token turn reserve (1,024 reasoning tokens). The separate reasoning-gated
calibration found no success difference among the bounded budgets and selected
256 as the cheaper final default; this historical data is intentionally not
rewritten. See `../2026-08-31-reasoning-gated/`.

These fixtures remain saturated for the baselines. The 10/10 routed-required
result shows that the corrected mechanism can retain baseline smoke success; it
does not establish that reasoning caused the success.
