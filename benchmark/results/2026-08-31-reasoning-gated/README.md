# Reasoning-gated evaluation and mechanism follow-ups

Six deterministic Go fixtures were added to ask the previously unanswered
question: does routed reasoning improve success when the repair requires
tracking a logical invariant? Each fixture contains an otherwise complete
implementation with one logical defect, and its passing oracle differs by one
source line. `tests/integration/test_benchmark.py` proves every broken fixture
fails and every oracle passes under Go 1.27.0.

The answer from this pilot is **no measured lift**. On the better-matched
Devstral model, the no-thought arm passed 4/6 and routed decode-only passed 3/6.
On Qwen3-Coder, the ordinary no-thought and routed arms each passed 1/6; the
required-routed arm reached 2/6. Six tasks and one seed are enough to reject a
claim of demonstrated benefit, not to prove that benefit is impossible.

## Qwen3-Coder budget calibration

One Forge executable (`d92057a4…`), Qwen3-Coder-30B-A3B-Instruct Q4_K_M,
WSL2/RTX 5090 Laptop, greedy seed 42, 16,384-token context, 2,048-token output,
and a 16-turn cap produced all 42 records.

| arm | passed | turns | prompt tokens | generated |
| --- | ---: | ---: | ---: | ---: |
| no-thought | 1/6 | 81 | 281,194 | 20,304 |
| routed, budget 256 | 1/6 | 49 | 158,250 | 17,531 |
| routed, budget 512 | 1/6 | 69 | 283,910 | 26,556 |
| routed, then-default budget 1,024 | 1/6 | 70 | 299,559 | 29,591 |
| routed, budget 1,536 | 1/6 | 60 | 230,996 | 26,491 |
| routed required, budget 1,024 | 2/6 | 63 | 264,332 | 33,525 |
| routed, unbounded | 1/6 | 36 | 104,348 | 15,821 |

Success is flat at 1/6 for the directly comparable routed budget arms. The
256-token arm is the cheapest bounded arm by turns, prompt tokens, and generated
tokens, so the implementation now uses a 256-token default ceiling while
retaining every explicit budget and the unbounded ablation.

## Better-matched model

Devstral-Small-2-24B-Instruct-2512 Q4_K_M ran the same fixtures, rig, context,
turn cap, greedy seed, and Forge executable.

| arm | passed | turns | prompt tokens | generated |
| --- | ---: | ---: | ---: | ---: |
| no-thought | 4/6 | 72 | 216,703 | 7,374 |
| routed decode-only | 3/6 | 54 | 163,510 | 15,646 |

Routed reasoning used fewer turns and prompt tokens but more generated tokens
and passed one fewer task. This directly answers the central pilot question in
the negative for the present model/fixture/seed combination.

## Native-thinking and whitespace checks

The official Qwen3-4B Q8_0 GGUF was run at its recommended temperature 0.6.
Native template thinking passed `ceil_div` in 3 turns with 1,608 reasoning
tokens; the matched `--disable-thinking` lazy-grammar control reached the run
limit in 9 turns. This establishes that `enable_thinking` reaches the template
and that a thinking-safe baseline exists; one smoke task is not an accuracy
study.

The post-calibration Llama-3.1-8B forced-swap smoke recorded
`forced_actions = 3`, `forced_action_progress_tokens = 3`, and
`action_stops = 14`. The run eventually reached its global limit without a
valid repair, but every forced transition opened action progress instead of
consuming the rest of its turn in leading grammar whitespace. That is the
intended regression evidence for the whitespace fix.

Subdirectories retain environment identity, full result records, and censuses
where applicable. Token counts are only compared within a model.
