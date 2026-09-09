# Independent final evidence review

The completed diagnostic supports an accurate descriptive report. No missing run,
crash, timeout, protected-file mutation, simulated inference, or recorded token /
turn budget overrun was found. This review changed no source, harness, fixture,
runtime, or model-run evidence.

| Arm | Primary composite passes | Terminal-code test passes | Median original end-to-end seconds |
| --- | ---: | ---: | ---: |
| Historical checkpoint | 9/18 | 9/18 | 41.8125 |
| Current | 7/18 | 7/18 | 48.2265 |
| Minimal | 6/18 | 8/18 | 28.7735 |

The preregistered 54 cells match all 54 retained outcomes exactly. All are completed
harness invocations, with 18 per arm and three per task. I checked each outcome
against its individual result file, aggregate result, environment, stored trace
notes, settings, preflight fixture identity, and matching secondary result. All
54 record real inference with positive generated-token counts. Original process
returns are only zero or one. Maximum generated tokens were 5843; maximum prompt
tokens were 126095, below the fixed cumulative limits 32768 and 262144. No run
exceeded 16 turns. Every recorded protected-file check passed.

The stored before/after frozen identities are identical and the recorded protocol
hash matches the protocol. The source ZIP member population and every contained
file hash match the source inventory; the retained source diff also matches.
Model/runtime/environment fingerprints were checked across all 54 individual
environments against the frozen protocol. This audit used those recorded model
hashes rather than independently rehashing the model weights again.

Of 32 primary failures, 31 reached the turn limit. Historical checkpoint original
retractions repetition 1 ended at turn 13 with a native-response parser error
after its per-generation limit; it did not exhaust the cumulative generation
budget. No explicit context-exhaustion error was observed. Loop warnings are
6/8/0 and attempted unchanged string replacements are 4/11/13 for
checkpoint/current/minimal respectively. Every counted replacement actually
contains equal old_text and new_text fields.

Minimal's additional terminal-code passes are paraphrased retractions repetition
1 and distractor retractions repetition 2. Both original independent verification
logs already show the two tests passing. Their primary failures remain failures
because the agent exited at the 16-turn limit without a final answer. The later
secondary checks confirm the same preserved code and immutable tests; no extra
model repair attempt was made. Report 8/18 only as the separately labeled terminal
test outcome, alongside the preregistered 6/18 composite outcome.

Interpretation remains limited: the historical checkpoint has different native
decoder defaults, these are six selected development cases, and three repeated
runs per task do not establish a correctness or latency advantage. The minimal
control also bundles orchestration changes, so its contrast does not identify a
single causal mechanism. No promotion or changed-model conclusion follows from
this comparison alone.
