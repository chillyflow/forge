# Candidate 1 implementation review

The opt-in bounded repair policy is the first candidate. The original minimal
and checkpoint-only paths remain controls, and no default changes.

The implementation uses actual rendered prompt counts for mandatory evidence and
then admits a newest-first suffix of complete tool exchanges. All raw segments
remain available to context export and conversation capture. Current host state
uses one replaceable memory segment, separate from model hypotheses. Task and
user clarification evidence remain mandatory. Current source reads recheck the
read policy, safe path and authorized line range. A complete validation record
keeps its command, diagnostics, input identity and validation ID together;
incomplete later attempts are separately labelled.

Prompt allocation has a soft per-action target and a hard reserve for subsequent
validation/final prompts. Output allocation also preserves validation/final
capacity. A reserved final remains a model call subject to host validation. A
rejected reserved final stops the run instead of looping. Malformed reflection
consumes its allowed action/tokens and returns to ordinary repair when budgets
permit, without executing a substitute tool or granting another attempt.

The review found and corrected five boundaries before candidate freeze:

1. Early budget closure could otherwise latch repeated rejected final calls.
2. An optional null error pointer could otherwise disable the soft-budget retry.
3. A source excerpt could otherwise exceed the authorized inclusive line range.
4. User answers and prior requests could otherwise be evicted with tool history.
5. Incomplete validation could otherwise mix a new identity with an old command.

The context tests and 12 new bounded-repair integration cases exercise the
observable contracts, alongside the existing minimal, ordinary, interactive,
semantic, candidate-search and validation checks. Pre-freeze focused checks
passed. These are implementation observations; frozen G0 and model gates have
their own records and acceptance status.
