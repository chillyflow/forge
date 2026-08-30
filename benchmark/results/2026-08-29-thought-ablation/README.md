# Thought-channel ablation — 2026-08-29 (WSL 2 + RTX 5090)

> **Superseded for arm comparisons** by
> [`../2026-08-30-routed-sweep`](../2026-08-30-routed-sweep/README.md), which re-runs
> these arms plus the routed ones on a single later binary. The inline
> `thought-required` arm scores 1/10 here and 4/10 there, so figures from the two
> runs must not be mixed. The retention effect reproduces in both; the
> `optimized` 10/10 vs `no-thought` 9/10 split here does not.
> Its **Arms** table below is also stale in a second way: retention in the stored
> ACTION segment was the default when this ran, and is now off by default. The flag
> sets listed there no longer produce the configurations they describe — `optimized`
> and `thought-required` would today need `--thought-history` to retain. The runner
> arms were renamed and made explicit accordingly.

Evidence for §32 (`docs/DESIGN_CHECKLIST.md`), phase 25: does the bounded
`thought` reasoning channel help, and should a thought be retained in the
stored `ACTION` segment or stay purely decode-side?

All four arms ran back to back from one binary, one model, and the same ten
prepared Go fixtures. Only the named flags differ.

## Environment

| Item | Value |
|---|---|
| Platform | WSL 2, Ubuntu 24.04, kernel 6.18.33.2-microsoft-standard-WSL2 |
| GPU | NVIDIA GeForce RTX 5090 Laptop, 24,463 MiB, driver 610.88 |
| GPU offload | `-1` (all layers) |
| Model | `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` |
| Model SHA-256 | `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad` |
| Forge binary SHA-256 | `cd3b9b9e0fca18a91b7b183429bd587639537b0a4df14727594b90c70efa37cc` |
| Forge commit | `bb8e0e7` plus the uncommitted §32 switches |
| Context | 16,384 tokens; `--max-turns 16`; `--wall-ms 600000` |
| Go | go1.27.0 linux/amd64 |
| Fixtures | `utf8-lf-gofmt-v1`, ten tasks |

**This is WSL 2, not native Linux**, and the model, seed, and fixture set are
single. Read the numbers as one measured configuration, not a general law.

## Arms

| Variant | Flags | Channel offered | Thought required | Kept in history |
|---|---|---|---|---|
| `optimized` | (none) | yes | no | yes |
| `no-thought` | `--no-thought` | no | — | — |
| `thought-required` | `--thought-required` | yes | yes | yes |
| `thought-required-decode-only` | `--thought-required --thought-decode-only` | yes | yes | no |

## Results

| Variant | Passed | Wall (s) | Prompt tokens | Generated | Turns | Thoughts / actions |
|---|---|---|---|---|---|---|
| `optimized` | **10/10** | 153.9 | 87,876 | 2,725 | 61 | **0 / 61** |
| `no-thought` | 9/10 | 157.5 | 90,832 | 2,695 | 63 | 0 / 63 |
| `thought-required` | **1/10** | 400.6 | 472,048 | 29,324 | 150 | 150 / 150 |
| `thought-required-decode-only` | 7/10 | 202.2 | 115,059 | 7,925 | 68 | 68 / 68 |

Per task (`PASS`/`FAIL`):

| Task | optimized | no-thought | required | required + decode-only |
|---|---|---|---|---|
| add | PASS | PASS | FAIL | PASS |
| average | PASS | PASS | FAIL | PASS |
| ceil_div | PASS | PASS | FAIL | PASS |
| clamp | PASS | FAIL | FAIL | FAIL |
| contains | PASS | PASS | FAIL | FAIL |
| last_index | PASS | PASS | FAIL | PASS |
| prefix | PASS | PASS | PASS | FAIL |
| range_sum | PASS | PASS | FAIL | PASS |
| reverse | PASS | PASS | FAIL | PASS |
| unique | PASS | PASS | FAIL | PASS |

## What the numbers say

**1. As shipped, the channel is inert.** With `thought` merely *offered*, the
model opened with one in **0 of 61 actions** (`thought-census.json`, counted
from the `model_output` events of every run). The channel it was offered went
completely unused, so this arm measures no reasoning.

`optimized` and `no-thought` still are not a clean pair, and the difference is
not only prompt text. Turning the channel off changes **two** things at once:
one sentence of schema text disappears, *and* `fg_tool_grammar` drops the
`thought ::=` rule and the ` ws thought? ` prefix from `final`, `memory`, and
every `call` alternative — so the two arms decode under different GBNF token
masks at the start of every action. The single flipped task (`clamp`, 10/10 vs
9/10) is therefore attributable to prompt-plus-mask perturbation, not to
reasoning, and not to the schema sentence alone. Treat the 10/10-vs-9/10 gap as
run-to-run sensitivity of an unused feature, not as an effect.

The real lesson is methodological: an on/off ablation of an *optional* channel
cannot measure reasoning, because the treatment never occurs. That is why
`--thought-required` exists, and the comparisons that carry weight below are
`thought-required` against `thought-required-decode-only`, which share both the
schema text and the grammar and differ only in what is retained.

**2. Requiring a thought and keeping it in history is destructive.** 1/10
passed. The failures are not wrong answers: **8 of the 9 exhausted the 16-turn
cap** with `status=limit`, and most recorded zero loop warnings, meaning each
turn produced a genuinely new action and the agent simply never converged.
Cost against the `no-thought` baseline: **5.2× prompt tokens, 10.9× generated
tokens, 2.4× turns.** Retained reasoning compounds — every turn re-reads all
prior reasoning and adds more.

**3. Stripping the thought from history recovers most of that loss.** Same
generation-side treatment, thought dropped from the stored `ACTION` segment:
**1/10 → 7/10**, turns 150 → 68 (baseline 63), prompt tokens 472,048 → 115,059.
Seven of the nine lost tasks come back purely by not retaining the text
(`prefix` moves the other way), a net +6. The
remaining three failures look different: they fail at turns 10–13 with 3–4 loop
warnings, i.e. detected repetition rather than unbounded wandering.

**4. Even decode-only, thinking did not pay on these fixtures.** 7/10 against
9–10/10, for 2.9× the generated tokens. These are small single-function Go
repairs that the terse tool-calling path already solves; there is little headroom
for reasoning to help and plenty for it to hurt. This does not generalize to
harder tasks, and it is a single seed.

## Consequences

- The 2,048-byte bound is **not** the mitigation for retained reasoning. The
  bound caps one thought; the damage is cumulative across turns. Not retaining
  is the mitigation, and it is worth six net tasks here (1/10 to 7/10).
- Forcing a thought on *every* action is a blunt instrument: `prefix` was the
  one task where required reasoning passed and decode-only failed, so the useful
  setting is per-state, not global. This is the argument for the remaining §32
  work — routing decoding per state (thinking, tool selection, tool arguments,
  patch, final) via lazy grammar patterns — rather than a single global switch.

## Reproducing

```
python3 benchmark/run.py --forge build/forge \
  --model <model>.gguf \
  --output benchmark/results/2026-08-29-thought-ablation \
  --variants optimized no-thought thought-required thought-required-decode-only \
  --gpu-layers=-1 --context 16384 --timeout 600
```

`results.json` holds all 40 records; `environment.json` holds the model, binary,
and platform identity; `thought-census.json` holds, per run and per arm, how
many model actions were emitted and how many opened with a `thought`, which is
what makes the 0/61, 150/150 and 68/68 figures above checkable. The raw per-task
event logs were not published (32 MiB of session output), matching the practice
of the other result sets here — the census is the derived, checkable extract of
the one quantity those logs were needed for.
