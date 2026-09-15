# Spike 007 - grammar-forced runs (elision census)

## Question

When Forge's native tool grammar is live, how much of a *real* agent generation is
**forced** - i.e. the grammar admits exactly one token, so the token is the same
whatever the logits say? And in what **run lengths** does forced-ness come?

The prize, if it is large: a forced token does not need to be *sampled*, and a run
of L forced tokens does not need L forward passes. Walk the grammar for the run,
emit L tokens, then re-sync the KV cache with **one batched decode**. Decode of a
batch is far cheaper per token than decode of one token (prefill pp512 measured
5,150 tok/s = 0.19 ms/token against 4.1 ms/token for single-stream decode), so the
saving is the (L-1) forward passes a run avoids.

Nothing in the existing evidence measures this. Spike 002 measured *costs* per state
(1-3 allowed tokens in the states it sampled) and spike 005 measured reduced-candidate
*cover*; neither walked a real generation counting forced steps and forced runs.

## Method

`spikes/002-grammar-mask-cost/sweep.cpp` gains a `--census` mode (the P6/P7 arms),
built by the existing `forge_sampler_attribution` target against the shipping
prebuilt CUDA runtime - no source patch, no nvcc.

- Model: `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`, all layers GPU, n_ctx 16384,
  n_batch 512, n_ubatch 256.
- Prompt: `long-prompt.txt` (2,753 tokens), the prompt the spike 002-005 arms ran on.
- Chain: Forge's campaign chain - `[grammar] top_k 20, top_p 0.8, temp 0.6, dist(42)`,
  i.e. the loop-pilot configuration, where the sampler was 59-77% of decode.
- Two grammar arms, one generation each: `native` (the native template render, lazy)
  and `flat` (Forge's eager tool grammar).
- Per step, on the same logits the real chain sees: apply a **grammar-only** sampler
  kept in sync with the generation and count finite logits in the full array
  (P6 census), plus time each single-token decode with a synchronize (P6 cost).
- P7: after the generation, time batched decodes of 1/2/4/8/16 tokens at continuing
  positions, synchronized, to get the real batching curve on this machine.

## Bars (frozen before the first run)

- **P1 forced share** - forced steps / generated steps >= **20%** and the technique is
  worth building; **< 5% means falsified** for this configuration. Between the two is
  "marginal, record and stop".
- **P2 run length** - mean forced run length >= **2.0** for the batching argument to
  have something to batch; a mean below that leaves only the sampling cost, not a pass,
  as the prize.
- **P3 premise** - on **every** forced step the chain must have drawn the grammar's
  single allowed token. One counterexample falsifies the premise (a token the grammar
  permits but the sampler would not take), and elision becomes unsound rather than
  merely unprofitable.
- **P4 projection** - using only measured numbers (single-token decode from P6, batch
  curve from P7, and the mask cost in each regime from spikes 002/004), report the net
  decode saving for (a) the pinned runtime and (b) the spike-004 prefiltered library.

## Falsifiers

1. Fraction of forced steps < 5% in both grammar arms.
2. A forced step whose sampled token != the single allowed token (premise broken).
3. Mean forced run length < 2.0 (nothing to batch).

Any of these is written up as a falsification, not reframed.

## What this spike does not do

It does not implement elision in Forge. It sizes the prize, and it must be honest that
its own census probe (a full-vocabulary grammar apply per step) is *expensive* in the
pinned runtime - that probe cost is reported per step and excluded from every
projection, so the numbers state what elision would cost *without* the probe
(`forge` would use the same mask application it already pays, or the spike-004 table).

## Results

Runs: `forge_sampler_attribution --model Qwen3-Coder-30B-A3B-Q4_K_M --census --census-steps 256`
(native arm; the flat arm is recorded below as a failure), then the same binary in legacy
mode for the regression check. All logs are in this directory.

**Rollout**: the model emitted a real Qwen3 native tool call, then ran past it because the
harness applies no end-of-generation bans (a fidelity note, not a Forge behaviour):

```
<tool_call>
<function=run_command>
<parameter=argv>
["python", "-m", "py_compile", "calc.py"]
</parameter>
</tool_call>
<|im_end|><|endoftext|><|im_end|><|im_end|>...   (the rest of the 256 steps)
```

**Allowed-set census over the 256 steps (native template grammar, lazy, as the loop runs it)**

| state | steps |
| --- | --- |
| asleep - no mask applied (grammar awaiting its trigger) | 1 |
| **== 1 (forced: token is deterministic)** | **1** |
| == 2 | 9 |
| 3-4 | 2 |
| 5-8 | 6 |
| 9-64 | 3 |
| > 64 (free text inside the tool arguments, and the degenerate tail) | 234 |

**Summary counters**: forced steps 1/256 (**0.4%**); forced runs 1, mean length **1.00**,
max 1; premise `sampled == only` **1 ok / 0 contradicting**; per-step medians sample
14.95 ms | census probe 14.63 ms | decode 4.90 ms; chain tail 0.32 ms.

**P7 batch curve (this machine, synchronized, median of 3, continuing positions)**

| batch | per token | vs single |
| --- | --- | --- |
| 1 | 4.540 ms | 1.07x |
| 2 | 4.883 ms | 1.00x |
| 4 | 3.249 ms | 1.50x |
| 8 | 2.234 ms | 2.18x |
| 16 | 1.170 ms | 4.16x |

**P4 projection**: today 20.56 ms/token (sample + decode) against an elided 17.49 ms
(probe + batched-8) = 1.18x on forced tokens, which at a 0.4% forced share is a **0.1% net
decode saving**; in the spike-004 prefilter regime the ratio improves to 2.07x but the
**net saving is still 0.2%** because there is nothing to elide.

### The flat arm failed, recorded as a failure

`--grammar flat` (Forge's eager JSON action grammar) paired with the native-protocol prompt
dies **at its first sample**: exit 127, no output beyond the setup headers, nothing printed
for step 0 (`census-flat.txt` is the 4-step attempt, `census-flat.stderr` the log). The
prompt asks for an XML-style tool call, the grammar admits only JSON, so no sampling-eligible
token survives. The mechanism is *the most likely* one, not independently confirmed - the
runtime has no graceful path for it, which is itself worth knowing. A flattened-protocol
variant would need its own prompt render; not attempted here.

### Byproducts worth keeping

- **The lazy grammar is free while asleep** (step 0: probe 0.00 ms, sample 0.68 ms) - the
  prose phase of a loop-pilot run pays nothing, matching the loop measurements.
- **Mask cost tracks candidates in the array, not the allowed count**: 14.63 ms median even
  in the 157-allowed free-text state, and 29-33 ms in the run-up to the action. This is
  spike 002/003's finding again from a different direction.
- **The structural region is narrow but not deterministic**: 1, then 2, 5, 15, 40, 6, 2, 2, 6,
  2, 4 allowed over the first twelve steps - which is why the recovered levers are the
  per-candidate prefilter (spike 004) and the reduced-candidate path (spike 005/landed), not
  elision.
- **Legacy mode still runs** after the harness change (`legacy-regression.txt`: P1 medians
  24.33 ms apply, K=64 0.008 ms, token hash emitted, P2 states and the JSON summary present).

### Process notes (not hidden)

- **The preregistered prompt was not the one used for the census.** The method section names
  `long-prompt.txt` (2,753 tokens, a summarisation task); the first pass showed it never
  enters the structural region at all, so the census renders its own task-shaped native
  prompt (`Read calc.py in the workspace and report the bug you find.`, 2,367 tokens) and
  reproduces the loop's lazy trigger semantics. `long-prompt.txt` is retained and was used
  for the legacy regression run. The bar did not move; the method did, before the run that
  counts, and both prompts are in this directory.

- The first census pass used the harness's degenerate probe prompt and an eager grammar
  probe, so it measured a free-text region (allowed constant at 157, forced 1/256). Both
  were fidelity bugs in the *measurement*, not the bar: the run that counts renders a
  task-shaped prompt and reproduces the loop's lazy trigger semantics. The first pass's log
  file was overwritten by the fixed run; its summary is quoted here from the session record.
- The census probe is a full-vocabulary grammar apply per step (~15 ms here) that `forge`
  would not need extra - it already applies a mask to know the allowed set. Every projection
  above uses the probe as the mask cost on *both* sides for exactly that reason.

## Verdict

**Falsified, on both preregistered counts.**

- **P1**: forced share is 0.4% of the rollout (about 3% if only the steps up to the end of
  the tool call are counted) - below the 5% falsifier, far below the 20% bar. There is
  nothing to elide.
- **P2**: mean forced run length is exactly 1.00 - even if forced steps were common, a run of
  one has no batching to amortize, which is what the 1.18x per-token ratio and the 0.1% net
  projection show.
- **P3 held**: the single forced step drew precisely the grammar's only allowed token, so the
  premise was never in question - it is only ever *available* on 0.4% of tokens.
- **P4**: projected saving 0.1% (pinned runtime) / 0.2% (prefilter regime) - not worth
  building.

The idea is dead as designed, and the reason is now measured rather than assumed: a live tool
grammar makes the *structural* region narrow (2-6 candidates) but almost never deterministic,
while roughly half a rollout's steps sit in free-text regions where the allowed set is wide.
Constrained decoding is therefore a *cost-per-candidate* problem and a *candidate-count*
problem - spike 004's prefilter and the reduced-candidate path attack both - and not a
*determinism* problem.

Nothing in Forge changes as a result. The harness gains the census mode (and the lazy-trigger
fidelity, the batch curve and unbuffered output), so the next person can re-ask this question
on a real loop-pilot trajectory without rebuilding the instrument.