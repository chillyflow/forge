# Routed vs inline thought sweep — 2026-08-30 (WSL 2 + RTX 5090)

Evidence for §32 (`docs/DESIGN_CHECKLIST.md`), phase 25. Supersedes the arm
comparisons in `../2026-08-29-thought-ablation/`, which measured a different
binary; that run's conclusions are re-tested here rather than assumed.

Two questions:

1. Does `--thought-routed` (llama.cpp lazy grammar patterns, plain-text prefix
   until a `tool`/`memory`/`final` object triggers the action GBNF) actually
   elicit reasoning where the inline optional field did not?
2. Does retaining a thought in the stored `ACTION` segment cost accuracy, and is
   that effect real or an artefact of the 16-turn cap?

## Environment

| Item | Value |
|---|---|
| Platform | WSL 2, Ubuntu 24.04, kernel 6.18.33.2-microsoft-standard-WSL2 |
| GPU | NVIDIA GeForce RTX 5090 Laptop, 24,463 MiB, driver 616.56 (per `environment.json`) |
| Model | `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` |
| Forge binary SHA-256 | `24bf9a188b81d741…` (all eight arms) |
| Context / turn cap | 16,384 tokens / `--max-turns 16` |
| Fixtures | `utf8-lf-gofmt-v1`, ten Go repair tasks |

**All eight arms ran from one binary, one model and one fixture set**; `benchmark/consolidate_arms.py`
verifies this and refuses to merge arms that disagree; `environment.json` records
both the shared values and the key list it checked. That matters here: the same inline arm scored 1/10 on the previous binary and
4/10 on this one, so cross-binary comparisons are not valid.

## Results

| Arm | Passed | Prompt tokens | Generated | Turns | Actions with thought | Statuses |
|---|---|---|---|---|---|---|
| `optimized` (offered, optional) | **10/10** | 91,233 | 2,743 | 63 | **0 / 63** | ok 10 |
| `no-thought` | **10/10** | 87,908 | 2,818 | 62 | 0 / 62 | ok 10 |
| `thought-required` (inline, retained) | 4/10 | 280,976 | 12,571 | 114 | 114 / 114 | limit 6, ok 4 |
| `thought-required-decode-only` | 7/10 | 123,355 | 9,944 | 76 | 76 / 76 | ok 7, limit 3 |
| `thought-routed` (retained) | 9/10 | 105,136 | 3,069 | 67 | **0 / 67** | ok 9, limit 1 |
| `thought-routed-decode-only` | 9/10 | 105,136 | 3,069 | 67 | **0 / 67** | ok 9, limit 1 |
| `thought-routed-required` | **0/10** | 10,892 | 100 | 10 | 0 / 10 | **parse 10** |
| `thought-routed-required-decode-only` | **0/10** | 10,892 | 100 | 10 | 0 / 10 | **parse 10** |

Per task:

| Task | opt | no-th | req | req+DO | rout | rout+DO | rout+req | rout+req+DO |
|---|---|---|---|---|---|---|---|---|
| add | PASS | PASS | FAIL | PASS | PASS | PASS | FAIL | FAIL |
| average | PASS | PASS | PASS | PASS | PASS | PASS | FAIL | FAIL |
| ceil_div | PASS | PASS | PASS | PASS | PASS | PASS | FAIL | FAIL |
| clamp | PASS | PASS | FAIL | PASS | FAIL | FAIL | FAIL | FAIL |
| contains | PASS | PASS | FAIL | FAIL | PASS | PASS | FAIL | FAIL |
| last_index | PASS | PASS | FAIL | PASS | PASS | PASS | FAIL | FAIL |
| prefix | PASS | PASS | FAIL | FAIL | PASS | PASS | FAIL | FAIL |
| range_sum | PASS | PASS | PASS | PASS | PASS | PASS | FAIL | FAIL |
| reverse | PASS | PASS | PASS | PASS | PASS | PASS | FAIL | FAIL |
| unique | PASS | PASS | FAIL | FAIL | PASS | PASS | FAIL | FAIL |

## Turn-cap control

The 16-turn cap was the obvious alternative explanation for the retained arm's
failures — "it does not diverge, it just needs more room". Both inline arms were
re-run unchanged at `--max-turns 32` (`turncap-control.json`):

| Arm | 16 turns | 32 turns | Turns consumed at cap 32 |
|---|---|---|---|
| `thought-required` (retained) | 4/10 | **4/10** | 194 |
| `thought-required-decode-only` | 7/10 | **7/10** | 108 |

Both plateau exactly, and on the **same tasks** — not merely the same count.
`thought-required` passes `average`, `ceil_div`, `range_sum` and `reverse` at both
caps; `thought-required-decode-only` passes the same seven at both. Doubling the
budget bought the retained arm 80 extra turns (114 to 194) and zero extra passes.
`turncap-control.json` records the passing task sets so a coincidental count match
could not be mistaken for a plateau.

**What this control does and does not establish.** `status=limit` is not a
turn-cap-specific code: `FORGE_ERR_LIMIT` also covers the generated-token budget,
the input-token budget, and a full context with compaction disabled
(`src/core/agent.c:513-540`). The control therefore rules out turn starvation
specifically, which was the obvious alternative explanation; it does not
separately rule out token or context exhaustion for every individual failure.
Read it as "more turns do not help", not as "every failure was non-convergence".

**Statistical strength.** The retention effect is a paired comparison over ten
tasks with three discordant pairs, all in the same direction (three tasks pass
only when the thought is stripped, none only when it is retained). Exact McNemar
two-sided **p = 0.25**. The direction is consistent and reproduces across two
binaries and two turn caps, but n = 10 on one model and one seed cannot make it
statistically significant, and this report does not claim it is.

## What the numbers say

**1. Routing does not elicit reasoning. It is as inert as the optional field.**
`thought-routed` produced a prefix in **0 of 67 actions**. The lazy-grammar
trigger fires the moment the model emits `{"tool":`, and nothing stops it doing
that immediately — so a model with a strong tool-call prior simply never writes a
prefix.

*How this is counted, and why the obvious objection does not apply.* The raw
`model_output` event is emitted **before** routed normalization, so a routed
thought appears there as plain text with no `"thought"` key at all. A census that
merely grepped for `"thought"` would therefore report zero for the routed arms no
matter what the model did — an artefact, not a measurement. `thought-census.json`
(schema 2) instead classifies every action three ways: a leading `"thought"` key
(inline), non-whitespace text before the action object (routed prefix), or
neither. Under that classification the routed arms record 0 routed prefixes and
67 actions that begin directly with `{`. Token accounting agrees independently:
routed spends 3,069 generated tokens over 67 actions (~46 per action) against the
`no-thought` baseline's 2,818 over 62 (~45), whereas genuinely elicited inline
reasoning costs ~110 per action.

`thought-routed` and `thought-routed-decode-only` are identical in prompt tokens,
generated tokens and turns (105,136 / 3,069 / 67), which is the expected signature
of a strip that never had anything to strip, and a useful internal consistency
check on the harness.

The 9/10 vs 10/10 gap for routed is therefore **not** a reasoning effect, but the
mechanism is not merely a different sampler path either: `fg_tool_schema` gives
the routed arms a different system instruction, which opens *"Reason in plain
text, then return exactly ONE JSON object"* (`src/tools/tools.c:53`). Routed and
`optimized` therefore differ in prompt text as well as in sampler configuration,
so the single flipped task is a treatment difference of unknown composition, not
an isolated sampler effect.

That instruction makes the inertness finding stronger rather than weaker: the
model was explicitly told to reason in plain text before acting, and still opened
with `{` in all 67 actions.


**2. `--thought-routed --thought-required` is a guaranteed-failure trap.** 0/10,
every task failing `parse` on the first action, 10 turns and 100 generated tokens
across the whole arm. This is the mechanism the §32 row already describes —
required routed thought is only a *host rejection gate* and cannot make a model
emit a prefix — but the measured consequence is worse than "ineffective": with a
model that never volunteers a prefix, the combination rejects every action and the
run dies immediately. It is not a usable ablation arm, and it should not be a
reachable configuration without a warning.

**3. The retention finding reproduces on a new binary.** Inline required
reasoning, retained, scores 4/10; identical treatment with the thought dropped
from the stored `ACTION` segment scores 7/10, at 2.3x fewer prompt tokens
(280,976 to 123,355) and 1.5x fewer turns. This is the one effect in the whole
study that survives a binary change, a turn-cap control and a paired design, though at
n = 10 it is not statistically significant (exact McNemar p = 0.25): a consistent
direction, not a demonstrated magnitude.

**4. Elicited reasoning still does not pay on these fixtures.** The best
reasoning arm (7/10) loses to both no-reasoning baselines (10/10). These are small
single-function Go repairs the terse path already solves. This does not generalize
to harder work, and it is a single model and seed.

## Consequences

- **Retention is the setting that matters, and it is only latent today.** With
  both elicitation mechanisms inert by default, `thought_in_history = true` costs
  nothing right now. The moment reasoning is actually elicited it costs three
  tasks out of ten. Defaulting retention off — or having any elicitation mode
  imply decode-only — is the change this data supports.
- **Lazy-grammar routing, as built, is a wire-format change, not a reasoning
  policy.** It relocates the thought from inside the action object to a prefix
  before it; it does not create a thinking state. Making the model think requires
  *suppressing* the trigger — a minimum prefix length, or a genuine per-state
  policy that forbids the action grammar until a planning budget is spent. That
  is the remaining §32 work, and it is now measured rather than assumed.
- The previous run's `optimized` 10/10 vs `no-thought` 9/10 split did not
  reproduce here (10/10 vs 10/10), confirming it was run-to-run sensitivity of an
  unused feature rather than an effect.

## Reproducing

Each arm was run separately, in the foreground, against the same binary:

```
python3 benchmark/run.py --forge build/forge --model <model>.gguf \
  --output <run-root>/<arm> --variants <arm> \
  --gpu-layers=-1 --context 16384 --max-turns 16 --timeout 600
```

The 32-turn control repeats the two inline arms with `--max-turns 32` into a
separate root. Both roots are then merged by a tool in this repository, which
refuses to merge arms whose binary, model, fixture preparation or turn cap differ
and records the identity it checked:

```
python3 benchmark/consolidate_arms.py <run-root> <out-dir> --control <control-root>
```

Every artifact here is that tool's output, and `environment.json` names it in
`consolidated_by` along with the `identity_verified` key list — so "all eight arms
ran from one binary" is a check you can re-run, not a claim you have to take:

- `results.json` — all 80 records from the eight 16-turn arms.
- `turncap-control-results.json` — all 20 records from the 32-turn control, in the
  same schema, each carrying its own metrics.
- `turncap-control.json` — the control summary, including `same_binary_as_baseline`
  and the passing task sets at both caps.
- `environment.json` — shared binary, model, fixture preparation, context and turn
  cap, plus the arm list.
- `thought-census.json` — per arm and per run, how many actions carried an inline
  thought, a routed prefix, or neither.

The raw per-task event logs are not published (they are the bulk of ~32 MiB of
session output); the census is the derived extract of the one quantity they are
needed for, and it is regenerated by the tool above from those logs.