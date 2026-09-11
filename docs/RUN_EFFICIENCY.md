# Run efficiency: measured process failures and the rules that prevent them

Status: standing protocol, September 11, 2026. This document records how the
September campaigns spent effort inefficiently and turns each failure into a
checkable rule. It documents **process**, not results, and it supersedes nothing:
the evidence rules in the existing plans remain in force and are referenced, not
restated.

Scope: the repair and agent-loop campaigns of September 8–11, 2026 — approximately
**314 model runs in three days** — plus the comparative campaigns that preceded
them. Every failure below is drawn from a retained artifact; none is a general
observation about research practice.

## 0. What went well, and must not be broken by these rules

These are load-bearing and several of the rules below could accidentally erode
them. Preserve them deliberately.

- **Nothing was discarded.** Every failed run, aborted freeze and wrong result was
  retained. No batch was re-run for a better repetition.
- **Bars were preregistered** before the first run of each screen and gate.
- **Fixtures were frozen with hashes**, and preflight asserted that a broken
  fixture fails and its supplied oracle passes.
- **Provenance was honest**, including dirty working trees, uncommitted sources
  and reused development tasks.
- **Screening evidence was kept out of `outcomes.json`**, so an unscheduled
  record cannot retroactively invalidate a gate.
- **Corrections were published**, not quietly fixed — including a wrong
  extractor figure and an overstated cost objection.
- **One binary served every arm**, so arm differences were not binary differences.
- **Host validation was independent of the model's claims.**

## 1. The failures

### 1.1 Decisions taken on one repetition per fixture

**What happened.** Screens ran 14 real-model runs — four Python manifests × three
repetitions plus two Go manifests × one — and were then closed or promoted on that.

**Sharpest evidence.** Screen c09 and its own preregistered confirmation c09b ran
a **byte-identical binary, profile and schedule**. c09 scored 5/12; c09b scored
1/12. Four of the five passes were variance, and c09 had been recorded as "the
first favorable success signal in eleven loop screens."

**Cost.** Roughly 28 runs and an entire favorable finding, consumed to learn
nothing about the flag — the same conclusion a second repetition would have
produced directly.

**Rule.** A mechanism is not closed or promoted on one repetition per fixture.
When the population is small, buy **repetitions before arms**: fewer arms, more
reps, and a stated minimum detectable effect sized against the observed variance.

### 1.2 Model time used as the first instrument

**What happened.** Every screen was 14 real-model runs. The deterministic layer —
`--script actions.json` (`src/cli/main.c:101`) and the 24 GPU-free unit and
integration tests — was used only as a pass/fail preflight, never as the primary
screening surface.

**Cost.** 30–70 minutes of GPU per question, including for questions that are
decidable by construction.

**Rule.** Decide by construction first. Tool semantics, edit application, address
validation, refusal behaviour and grammar shape are all deterministically
testable at thousands of cases with zero sampling variance. A model run is for
what the deterministic layer provably cannot answer.

### 1.3 Screens serialised, each with its own comparator

**What happened.** c06, c07, c08, c09 and c09b were five separate 14-run screens,
launched one after another, each measured against a different baseline (c06, W0
candidate-2, candidate-05).

**Cost.** Five launches, five independent baselines, 70 runs — and results that
still cannot be pooled, because they are different binaries measured at different
times.

**Rule.** One interleaved multi-arm screen against **one** frozen control. Arms
that answer related questions belong in the same schedule block, not in a series.

### 1.4 No shared frozen control across campaigns

**What happened.** W0 candidate-2 (4/12), candidate-05 (1/12) and c06 (2/12) are
the same population and profile on three binaries, and are explicitly marked
"do not pool".

**Cost.** Each campaign spends 12–14 runs re-measuring a baseline that a carried
control would have supplied.

**Rule.** Carry one frozen control binary across a campaign series and re-measure
it only when the profile, fixtures or sampling parameters change.

### 1.5 A gate runner that cannot resume

**What happened.** The all-green campaign's G1 batch executed 12 of its 18
scheduled runs and was then interrupted. The runner **deliberately refuses** to
execute a gate that already has outcomes, so the batch could not be resumed or
retried: six cells were lost permanently — **33% of the gate**.

**The contrast is inside this same repository.** `run_models.py` skips completed
runs (`if results.exists(): continue`) and resumes cleanly. Two runners, two
policies, and the stricter one destroyed a third of a gate.

**Rule.** Every runner resumes. An interruption is not a failure, and it must
never be able to consume scheduled cells. If a runner cannot resume, it is not
ready to launch.

### 1.6 Six attempts on one fixture family before the review rule fired

**What happened.** repair-recovery v2, v3, v4 (reverted), bounded repair, c04
elide-noop and c05 host-defects were all aimed at the retraction family and were
all of the same class — host evidence presentation. The plan already contained
the rule *"three unproductive experiments trigger a design review and a different
hypothesis, not relaxation of the target."* It fired after roughly five.

**Rule.** Count attempts in the campaign record, not in the operator's memory.
Fire the review automatically at three, and make "same class as a previous
attempt" an explicit field in the experiment record.

### 1.7 Fixture population too narrow — and one family arguably degenerate

**What happened.** The conclusion *"the loop is not the ceiling on this task
family"* rests on six fixtures, four of which are wordings of a single Python
task. That task's `if True:` scaffolding makes byte-identical no-op edits
especially likely, which is the very behaviour the campaign then studied.

**External contrast.** The edit-format benchmark cited in
[agent-performance-avenues.md](plans/agent-performance-avenues.md) used 180 tasks
across 16 models on real sources.

**Rule.** No population-level conclusion from a fixture set small enough that one
fixture can dominate it. Before a family becomes the basis of a claim, check
whether it is degenerate — whether its structure coerces the failure being
measured.

### 1.8 Instrumentation published before validation

**What happened.** The metric extractor read a `command` argument key; the schema
is `argv`. Every call normalised to the empty string and all but the first
counted as a repeat. Published figures of 113/126/149 were later corrected to
31/20/17.

**Rule.** Validate every extractor against a hand-checked table — reproduced to
the last digit — **before** any of its numbers are published or acted on.

### 1.9 The measurement instrument had a correctness hole

**What happened.** Python `-B` suppresses bytecode writes but can still load
existing cached code, so a stale `.pyc` could produce a false pass in **both** the
independent verifier and the host checkpoint, even with stable input hashes. It
was found after the batch.

**Severity.** The highest of any failure here. A false pass is indistinguishable
from a real one, and it silently validates the model and the host simultaneously.

**Rule.** The verifier gets adversarial tests of its own: a known-failing fixture
must fail, a known-passing fixture must pass, and the test must assert that no
cached artifact can be consulted. A verifier without a failing control is not a
verifier.

### 1.10 Mechanism engagement never preflighted

**What happened, three times.**

- candidate-checkpoint v2 tied the control at 7/18 but had *invalidated most KV
  reuse* by placing changing control text in the system prefix — found after the
  full matrix.
- The c08 command-dedup screen found **one** reuse event in twelve runs: the
  machinery worked, the workload did not contain what it cached.
- The c07 prefix screen had *no floor-engagement telemetry*, so "engaged but
  useless" could not be separated from "rarely engaged".

**Rule.** Before any matrix, a one- or two-run telemetry preflight must prove the
mechanism fires on this workload. A mechanism that cannot be counted cannot be
evaluated, and a matrix run without that counter is a wasted matrix.

### 1.11 Clustered runs used for significance

**What happened.** p-values were reported on clustered runs — c09 (Fisher exact
two-sided p = 0.371) and c06 (p ≈ 0.64) — where the twelve runs are three
repetitions of four fixtures, not twelve independent observations. The plans
acknowledge clustering in prose while the numbers were still reported per-run.

**Rule.** Report cluster-level effects. Do not compute per-run p-values on
clustered data; if a p-value is reported at all, report it on the number of
independent clusters and say so explicitly.

### 1.12 Protected-file violations discovered at analysis time

**What happened.** A 12-task, three-repetition, three-agent holdout completed all
108 scheduled runs and produced **no usable comparative result**, because one
agent edited a protected test's diagnostic message and the frozen rule invalidates
the comparison. The rejection was correct; discovering it after 108 runs was not.

**Rule.** Hash protected files after **every** run and flag at run time, not at
analysis time. Preregister what a violation does to the comparison *before*
running, so the answer is not invented under pressure afterwards.

### 1.13 Fixture reuse and diagnostic-specific guidance contaminating gates

**What happened.** The tranche-2 lead could not be promoted because the campaign
"froze an uncommitted tree, reused development tasks, and includes
diagnostic-specific repair guidance". The clean holdout that followed was then
itself consumed by repair, becoming development evidence.

**Rule.** A fixture used for tuning is development evidence **permanently**. Keep
a sealed set that no repair campaign may touch, and treat every use as burning it.

### 1.14 Per-candidate runtime duplication

**What happened.** Each frozen candidate copied its full CUDA runtime bundle —
**673 MB per candidate**, dominated by an identical `cublasLt64_13.dll`. Three
candidates cost about 2.0 GB of which ~1.3 GB was pure duplication.

**Rule.** Content-address or share the runtime bundle; hard-link or symlink the
identical parts. Record the bundle hash once and reference it.

### 1.15 Operator interruption with no recovery

**What happened.** The all-green campaign process was terminated mid-batch,
immediately after a run had started, leaving a run directory with only
`command.json` and `started.json`. That cell is reported as missing and is not a
result.

**Rule.** Launch multi-hour batches detached from the driving session, with a
resume-safe runner, so a closed terminal is not an experiment outcome.

### 1.16 Measurements taken while other work was running

**What happened.** Not confirmed in the retained record — recorded as a standing
hazard rather than a known failure.

**Rule.** Never build, index or run other GPU or CPU-heavy work during a timed
batch. The campaigns record cold latency and end-to-end wall time; a parallel
compile silently corrupts those numbers while every outcome still looks valid.

## 2. Preflight checklist

Run before launching any model batch. If any line is "no", do not launch.

1. **Question** — is this decidable by the deterministic layer instead?
2. **Control** — is there a frozen control from the same binary pair, or is one
   being measured in this batch?
3. **Engagement** — do 1–2 preflight runs show the mechanism's counter firing?
4. **Repetitions** — does the schedule buy reps before arms, sized against
   observed variance?
5. **Population** — is the fixture set large enough that no single fixture
   dominates, and is it free of degenerate structure?
6. **Resumption** — does the runner skip completed cells, and is it launched
   detached?
7. **Protected files** — hashed per run, with the violation policy preregistered?
8. **Verifier** — does it have a known-failing control that must fail?
9. **Extractor** — validated against a hand-checked table before publication?
10. **Exclusivity** — is the machine otherwise idle for the duration?
11. **Bar** — written and frozen before the first run, with the stopping rule?

## 3. When a batch fails

Report what changed, whether the mechanism activated, which previously passing
inputs regressed, and the next testable hypothesis. Three unproductive phases on
one avenue trigger a design review, not a fourth attempt. Budgets and
denominators are never relaxed to turn a failing gate green, and no result is
re-run in the hope of a better repetition.

A failure that is honestly recorded and closed is a successful experiment. The
failures catalogued above are not failures of rigour — the rigour was
consistently higher than the sample sizes could support. They are failures of
**instrument choice and sequencing**, which is why they are fixable by rule.
