# Candidate 1 — G0 rejected

Frozen candidate `bounded-repair-01`, identity
`99e1479c51b31d6529772344291f7637e5bfaaae3da17209b9a59b851b14f628`.

The full GPU suite passed 33 checks with the opt-in checkpoint-model entry skipped
(96.97 seconds). The 22 acceptance tests and direct native-template model probe
passed. Both required direct checkpoint-model invocations failed, so **G0 is
not accepted and G1 was not started**.

The legacy physical-checkpoint probe constructs plain-text prompts but inherited
the newer default native JSON protocol. Explicit checkpoint setup reported this
parse failure as unsupported (exit 77); automatic generation failed on the same
input. This is a probe setup and error-classification defect, not evidence of an
unsupported local backend. The frozen gate correctly rejects the skip.

The next candidate will explicitly select the legacy flattened protocol for its
unchanged 16-token physical-checkpoint tests, retain all cache/identity/budget
assertions, and keep the separate native-template probe. Malformed native JSON
will be classified as an input error and tested without additional generation.
Required CLI invocations, model, context and coding profiles remain unchanged.
All logs, failed outcomes, source and runtime identities in this directory are
retained; none will be replaced by the next candidate's results.
