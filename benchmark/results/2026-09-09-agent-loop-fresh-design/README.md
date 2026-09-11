# Forge agent-loop fresh-design campaign

Executed September 9–10, 2026, against
[`docs/plans/agent-loop-fresh-design.md`](../../../docs/plans/agent-loop-fresh-design.md).

**Outcome: the loop is not the ceiling on this task family, and this campaign
says so with evidence rather than absorbing the result into another round of
changes.** The plan's central hypothesis was implemented, confirmed to activate,
and refuted. The escalation arm the evidence pointed at was executed and
confirmed the ceiling rather than lifting it.

No gate was attempted: neither screened candidate met the preregistered
promotion bar, and the rule that says so was written before any run.

## What was run

| Package | What it did | Result |
| --- | --- | --- |
| [W0](w0-baseline/README.md) | 36 runs, three arms, unchanged candidate-2 runtime | plain 0/12, checkpoint 1/12, candidate-2 **4/12** |
| [W1](candidate-04-elide-noop/README.md) | `--elide-noop-edits`, G0 5/5, 14-run screen | **H-ECHO refuted** — 2/12, repeat rate unmoved |
| [W2](candidate-05-host-defects/README.md) | cache prerequisite + 4 defect repairs, G0 5/5, 14-run screen | 1/12; claims nothing, and nothing is claimed |
| [W4 arm 1](w4-temperature-zero/README.md) | temperature 0 at 32 actions, 12 runs | **0/12**, and repetitions still diverged |
| [W4 arm 2](w4-penalty/README.md) | repetition penalty 1.05 / last-64, 12 runs | **1/12**; penalty axis closed, and it never engaged its target |

88 model runs in total (36 + 14 + 14 + 12 + 12), all retained, none re-run or
discarded. The last 12 are a W4 penalty arm added after this record was first
written.

**A seventh intervention has now failed.** The repetition-penalty arm scored
1/12 against the 4/12 comparator and its preregistered rule closes the penalty
axis. Its instrumentation is the interesting part: the identical-replacement
rate was *exactly* unchanged, 40.2% → 40.2% (p = 1.00), and the repeat rate
60.0% → 58.6% (p = 1.00). A 64-token penalty window cannot see a repetition made
of 720–993 byte edit spans separated by whole turns, so the sampler never had
the chance to act. This is the second time a mechanism was confirmed not to
engage — or, for H-ECHO, confirmed to engage perfectly — while the behaviour
stayed put.

## The three findings that matter

**1. H-ECHO is refuted, and refuted well.** The plan proposed that failing runs
are poisoned by their own retained no-op edits acting as worked examples. The
intervention was built, and it provably did its job: 43 `noop_edit_elided`
events, exactly equal to the 43 identical replacements counted independently.
With the exemplars gone, the no-op-after-no-op transition rate was **59.0%
against a matched baseline of 60.0%** — unmoved. The identical-replacement rate
did not fall either.

This is stronger than a null result on pass rate would have been, because the
mechanism is confirmed to have worked. The degeneracy is internal to the model,
not auto-catalysed through the transcript.

**2. Sampler variance was never the thing to control.** At temperature 0 with a
fixed seed, all three repetitions of every manifest still produced different
trajectories. Combined with the existing finding that `--seed 42` is
byte-identical across repetitions, run-to-run variance here is neither seed- nor
temperature-controllable; what remains is GPU batching nondeterminism, which
`AGENTS.md` already documents. Removing sampler variance also did not help:
0/12 against 4/12.

**3. What the loop machinery demonstrably does is suppress false completion, not
improve repair.** W0's plain control declared the task finished 7 times and was
refuted by the independent verifier all 7 times. Across the 24 gated runs there
were **zero** false completions (Fisher exact two-sided p = 9.5e-05). That is a
far cleaner effect than any pass-rate difference measured in this campaign, and
it was not what W0 set out to measure.

## Where this leaves the plan

The plan's completion clause is satisfied: W0 produced a calibrated baseline, W1
and W2 were each screened under W3's preregistered rule, and W4's chosen axis was
executed and reported. The plan permits at most two screened candidates before
W4 must fire; two were used.

It also anticipated this ending: *"It is explicitly acceptable for this plan to
conclude that the loop is not the ceiling. That outcome must be reported with its
evidence rather than absorbed into another round of changes."*

Of the plan's four W4 options, one is now executed and negative, two remain:

- **A different or larger model** is the axis the design review said the evidence
  most supports, and nothing measured here contradicts it. It is not executable
  on the current machine: `C:/Users/flowc/models/forge/` holds only the one
  Qwen3-Coder-30B Q4_K_M GGUF.
- **L3 best-of-N under identical total budgets** remains untested, though it is a
  host-side allocation change and therefore the same broad class as the six
  interventions that have now failed.

## Corrections made during this campaign

Recorded because the campaign's own rules require failures to be reported as
failures.

- **My extractor's `run_command` repeat counts were wrong.** It read a `command`
  argument key; the schema is `argv`. Every call normalised to the empty string
  and all but the first counted as a repeat. Published figures of 113/126/149
  were corrected to 31/20/17. Call counts were never affected.
- **The first version of the cache fix was itself unsafe.** A persistent prefix
  with writes enabled cached a previous candidate's bytecode and failed a
  *correct* repair. Caught by two pre-existing tests, diagnosed by direct
  comparison against the previous binary, and replaced with `-B` plus the prefix
  so the cache is never populated.
- **The cost objection to that approach was overstated**, including where this
  campaign first raised it. A microbenchmark suggested ~400 ms per command; the
  measured cost on the real workload is **+44 ms** per command, 0.13% of run time.
- **Two plan statements did not match the tree.** The identical-edit rejection is
  in `src/tools/tools.c`, not `src/core/agent.c`; and the `last_patch_*`
  re-anchoring the plan asks to preserve lives in a loop `--bounded-repair` never
  enters. Both are documented in [W1-IMPLEMENTATION-MAP.md](W1-IMPLEMENTATION-MAP.md).
- **`candidate-03` is a retained, aborted freeze.** Its G0 was interrupted by a
  shell environment variable, not by a result. It is kept immutable rather than
  repaired in place, because the tooling's rule is to retain and re-freeze.

## Operational notes for the next session

- **`NoDefaultCurrentDirectoryInExePath=1` breaks `run-g0`.** The contract
  launches its three model probes by bare forward-slash relative path, and that
  variable removes the current directory from Windows' executable search.
  Clearing it in the child's environment does not help — Windows reads it from
  the calling process. Clear it in the invoking shell.
- **Disk.** This directory is 2.7 GB. 673 MB per candidate is the copied CUDA
  runtime bundle (`cublasLt64_13.dll` alone is 460 MB), so the three candidates
  carry roughly 2 GB of duplicated DLLs — including the aborted candidate-03.
  Worth a retention policy if more candidates follow.
- **Screening never touches `outcomes.json`.** All screening and baseline
  evidence lives at sibling paths, because one unscheduled record sets
  `accepted: false` for every gate permanently.

## Layout

- `extract_instrumentation.py` — the prespecified metric extractor. Validated
  against candidate 2's published tables before use: it reproduces all eight
  identical-replacement counts, the rejected-final multiset, every per-run
  outcome string, and the H-ECHO statistic to the last digit.
  `candidate-02-instrumentation.json` is that validation output.
- `screen.py` — the W3 screen runner, with the promotion rule recorded in each
  screen's frozen `protocol.json`.
- `W1-IMPLEMENTATION-MAP.md`, `W2-CACHE-PREREQUISITE.md`, `W2-CHANGES.md` —
  design and defect analysis.
- [`REVIEW-FINDINGS.md`](REVIEW-FINDINGS.md) — the adversarial review of this
  campaign: 7/7 evidence claims independently confirmed, six code defects fixed,
  and the findings accepted but deliberately not fixed. **The fixes it describes
  landed after candidate-05 was screened and are therefore unscreened**, verified
  by the deterministic suites only.
- `w0-baseline/`, `w4-temperature-zero/`, `candidate-03..05/` — evidence.
