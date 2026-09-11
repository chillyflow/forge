# Handoff: corrections from the Hermes session

Written 2026-09-11 by the Hermes session that has been committing to this
repository. Apply these before finishing Track D and before publishing any
write-up. Nothing here changes a bar's value or a denominator.

## 1. Do not describe the Stage 1 result as statistically significant

The Fisher test `[[10,8],[0,18]]` = **p 0.000344** is computed **per run**, but
those 18 runs are **3 repetitions × 6 fixtures** — clustered, not 18 independent
observations. Repetitions of one fixture are not independent samples.

Cluster-level (fixture-level) view: devstral-flat passed on 4 of 6 fixtures,
coder-flat on 0 of 6 → `[[4,2],[0,6]]` → **p = 0.061**.

**Report:** the directional result and the cluster-level p. **Do not report**
"significant", "clean capability signal", or the 0.000344 figure as the headline
statistic. Check the Stage 1 write-up and the capability READMEs for the same
error before they are published — this is the error catalogued as §1.11 of
`docs/RUN_EFFICIENCY.md`.

The directional finding is genuinely strong and should be stated plainly. Only
the significance claim is wrong.

## 2. c10-screen cannot promote or close anything

The design is right, and better than earlier screens: the bar is explicitly
*directional*, and it requires **gate engagement telemetry** ("gate fires in a
majority of failing Python runs"). Requiring engagement before reading an
outcome is correct practice.

But "Python ≥4/12" at one repetition per fixture is not a decision-grade
instrument. Screen c09 and its own preregistered confirmation c09b ran a
**byte-identical binary, profile and schedule** and scored **5/12 then 1/12**.
Four of the five passes were variance.

**Therefore:**

- If c10 **clears**: that justifies a preregistered confirmation on new run ids
  (as c09b was, and as c09b failed). It does **not** justify changing a default.
- If c10 **misses**: record the outcome, close the flag, and do not re-run for a
  better repetition.
- Either way, keep the screen out of `outcomes.json`.

## 3. The repository moved while you were running

- HEAD is now **`43065c88`** — five commits on top of `07c178a`.
- The working tree is now **clean**; it was dirty when the campaign started.
- Your **Stage 2 B-flattened evidence is committed as `43065c88`**.
- `git config core.longpaths` was enabled repo-locally so the deep campaign
  evidence paths are readable.
- 19 stale git worktrees under `.scratch/worktrees/` were removed. All branches
  were retained; no branch refs were deleted. Other `.scratch/` content was left
  alone.

Account for this if candidate-10's freeze records a source revision or tree
hash. **Do not use `git add -A`** — another session has been writing to this
tree, and a swept commit would mix the two.

## 4. Stage 2 is complete and independently verified

Group B retraction outcomes, all 24 scheduled runs retained:

| Arm | Result | Bar: ≥4/12 with ≥2 manifests |
| --- | --- | --- |
| `b1-devstral-flat` | **5/12**, passes on all four manifests | **cleared** |
| `b0-qwen-coder-flat` | **0/12** | not cleared |

Every run reports `protected_files_unchanged` and
`verification_inputs_unchanged` true — **zero protected-file violations**.

**Secondary finding, must not be substituted for a primary pass:** three further
Devstral runs left a *passing terminal workspace* without a successful agent run
— `original` r001, `original` r003, `renamed` r003. Combined, 8 of 12 Devstral
runs left a test-passing workspace but only 5 completed. Report those separately,
per the standing rule that a passing terminal workspace never overwrites a
failed primary result.

Stage 2 for **`A-deployable` has not been run**. `a0-qwen-coder` cleared G-Model
(6/18, 4 manifests) and is entitled to the retraction stage under G-Retract.

## 5. Keep the machine exclusive during a batch

Earlier you launched Stage 2 while a CTest run was in progress and noted the
"GPU vs CPU-only contention". The campaign records cold latency and end-to-end
wall time; concurrent GPU or CPU-heavy work corrupts exactly those numbers while
every outcome still looks valid. Do not launch a model batch alongside another
heavy job.
