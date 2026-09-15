# Spike 005 — can Forge apply its grammar after top_k instead of before it?

**Question.** Spike 004 makes the grammar mask cheap in near-forced states, but it
lives in llama.cpp **and is inert for `FORGE_LLAMA_PREBUILT`** — the released
`llama.dll` that `build-gpu` and the documented release flow use. The permissive
half of the loop's masked tokens (JSON string bodies) is therefore still paid in
full by every Forge user: 21.7 ms per masked token at a 151,936-token vocabulary
(spike 004's P2b), against 0.027 ms for the same chain over a 64-candidate array.

Forge owns its sampler chain (`src/inference/llama_backend.c`, built as
`[bans][grammar][penalties][top_k 20][top_p 0.8][temp 0.6][dist]`). Moving `top_k`
**before** the grammar would let the mask run over 20-256 candidates instead of
151,936 — a Forge-side change that works with the shipped DLL and needs no
upstream work.

**What decides it.** That reorder is only equivalent while the raw top-K contains
every token the current chain could sample. The current chain samples from the top
20 *allowed* tokens; the reordered chain samples from the top 20 of
`raw-top-K ∩ allowed`. The two differ exactly by the allowed tokens that fall
outside raw top-K. This spike measures that count per grammar state.

## Pre-registered bar

Frozen before the run. Measurement is `sweep.cpp`'s P4 arm: for each state, take
the allowed top-20 of the **full** array and count how many of those tokens are
absent from the raw top-20 / top-64 / top-256 sets.

| # | Hypothesis | Bar | Meaning |
| --- | --- | --- | --- |
| H1 | Coverage | **0** allowed-top-20 tokens outside raw top-64, in every state (root, mid-action, post-action, permissive) | K=64 reorder is set-identical |
| H2 | Cost | the K=64 arm of the permissive state stays ≤ 1 % of its full-array arm | the win is real (spike 004 measured 0.027 ms vs 21.72 ms) |

**Falsifier:** any state with `outside > 0` at K=64 ⇒ a bare reorder is **not**
safe; the implementation must instead use expand-on-miss (start at K=20, expand
K x4 while the mask leaves too few candidates). That is a bigger change and must
not be presented as a two-line reorder.

**Limits recorded up front.** One fixture, one prompt, one logits snapshot per
state — this is a *decision* measurement, not a trajectory study. Spike 002
supplies the trajectory evidence for the real grammar (24 real steps: 0 allowed
tokens outside raw top-64, 44/44 top-1 agreement); the permissive-state figure
here is a single position and must be labelled as such. The run uses the
spike-004-patched build, which is verified exact on this fixture, so coverage is
unaffected by which build computes it.

Run: `forge_sampler_attribution --model <gguf> --prompt <file> --steps 24`

## Results

Run 2026-09-15, spike-004-patched source build, `--steps 24`, token hash
`21c6df5710bb7794` unchanged. **Two of the three measurements in this spike failed
first, both on the same class of mistake** — reading an "allowed" list from a
helper that assumes an array ordering. They are recorded because the trap is easy
to repeat: a grammar-only `apply` leaves the candidate array in **token-id** order,
so any "top-20 allowed" list derived from it is id-ordered, not logit-ordered, and
every comparison against a logit-ordered set is then nonsense. Both arms below read
the token a sampler would **actually draw**.

### P4 — truncation risk of masking a reduced array

| state | sampled set | outside raw top-20 | outside top-64 | outside top-256 |
| --- | --- | --- | --- | --- |
| root | 10 | 10 | 10 | 10 |
| mid-action | 5 | 5 | 5 | 4 |
| post-action | 6 | 6 | 6 | 6 |
| **permissive** | 20 | **0** | **0** | **0** |

In the native template grammar's states, the tokens the grammar will accept sit
**far down the logit ranking** — every one of them is outside the raw top-256, so a
bare reorder would sample a different token. In the wide-open (permissive) state
the opposite holds: all 20 are inside the raw top-20. **H1 is falsified for the
forced states and confirmed only for the permissive one**, which is exactly what
the pre-registered falsifier was written for: a bare two-line reorder is not safe.

### P5 — does reduced+expanded sampling draw the same token?

Greedy chain, same grammar state and same logits at every step, per-step comparison
of the token each side would draw:

| | result |
| --- | --- |
| same token as the full-array reference | **24/24 steps** |
| answered without reaching the full vocabulary | **24/24 steps** (median K **80**) |
| cost per step | reference **6.862 ms** → proposed **0.157 ms** (**43.7×**) |

The proposed scheme is the expand-on-miss rule: mask the raw top-K (start K=20),
expand K x4 while the mask leaves nothing selectable, and at K = n_vocab it *is*
the current behaviour by construction. It terminates early even on this
constrained trajectory, because it only has to reach the first acceptable token in
rank order — median K=80, not 151,936.

Note what the reference cost means: 6.862 ms per step is the **loop's own remaining
sampler cost** (the end-to-end run measured 6.7 ms per generated token with the
spike-004 patch in place). The prefilter does not engage in the states the loop
actually spends its masked tokens in, and this arm shows a Forge-side change that
does — measured on the same logits, with identical output.

## Verdict

- **H1 falsified as stated, and the falsification is the useful part**: forced
  states put the entire sampled set outside raw top-256, so reduction must expand,
  while permissive states are exactly covered at K=20.
- **The Forge-side design is validated where it can be**: reduced+expanded
  sampling draws the identical token on 24/24 real steps at 43.7× lower cost, and
  never falls back to the full vocabulary on this trajectory.
- **The three spikes now form one consistent picture**, and each fix covers what
  the other cannot:
  - forced states: dense first-byte prefilter (spike 004, upstream) — 45-51×;
  - permissive/other states: reduced+expanded masking (this spike, Forge-side,
    works with the **shipped prebuilt DLL**) — 43.7×;
  - allocation removal (spike 003) and bare top-K reduction (spike 002) are both
    dead ends, killed by measurement rather than argument.
- **Not measured, and required before a stochastic deployment**: the comparison
  above is **greedy** (temperature 0). At temperature > 0 the exactness condition
  is "the reduced set contains every token the full chain could sample" (the top-20
  allowed), which P4 shows holds at K=20 in the permissive state and fails in
  forced states — so the stochastic rule must expand while the reduced set holds
  fewer than `top_k` allowed tokens, and its cost in forced states is the
  spike-002 case (expansion to the full vocabulary, no saving, still exact).
- **Implementation shape** (for the next work item, not done here): split Forge's
  single chain into a mask chain ([bans][grammar]) and a sampling chain
  ([top_k][top_p][temp][dist]), reduce the candidate array before masking, expand
  on miss, and sample by reading the array's selected entry — which also deletes
  the `temperature <= 0 || (!native && structured)` fast-path cliff that started
  this investigation.