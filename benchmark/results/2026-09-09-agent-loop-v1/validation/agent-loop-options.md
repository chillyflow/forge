# Agent loop options

The remaining loop mechanisms are implemented as selectable experiments on the
native minimal loop. The ordinary CLI also supports persistent conversations
and symbol-impact validation. Implementation and model accuracy are tracked
separately in [ROADMAP.md](ROADMAP.md).

```powershell
forge run "Repair the failing tests" --model MODEL.gguf --minimal-agent --candidate-checkpoint --candidates 2 --temperature 0.6 --semantic-loops --symbol-impact --failure-reflection --allow-write --allow-exec
forge chat --model MODEL.gguf
```

The repair options can be enabled individually. `--candidates`,
`--semantic-loops`, and `--failure-reflection` require the native minimal
candidate-checkpoint mode. Defaults preserve the existing repair policies;
the no-command `forge --model MODEL.gguf` entry point now starts the same
persistent conversation as `forge chat`.

## Candidate generation and selection

`--candidates N` accepts 1 through 8. Counts above one generate independent
trajectories from copies of the exact initial workspace, including initially
dirty and untracked files. One loaded model is used sequentially. Real sampling
requires positive temperature, with seed `base + index * 0x9e3779b9` modulo
2^32. Actions, generated tokens, cumulative input tokens and wall time are shared
across candidates, rather than multiplied by N. Each candidate needs at least
three actions. Up to five seconds, within the original wall limit, are reserved
for guarded cleanup after selection checks.

A candidate must produce a valid final after its local host checks. Completed
changed candidates are applied to the actual workspace and validated there.
The host restores initial contents between selections and chooses a passing
candidate with the smallest total changed-content byte cost; ties select the
earliest candidate. It then reapplies and validates that winner in the actual
workspace before accepting the winner's original model final. Discarded trials
do not enter the persistent conversation. No model final is synthesized.

The content store is bounded to 10,000 files and 64 MiB. Complete secure input
scans exclude root `.git` and `.forge`; staging copies preserve file bytes and
read-only protection. Every application records before/after content and intent
and outcome events. Write permission and journal capacity for both application
and restoration are checked before mutation; stale expected contents and
file/directory type changes refuse
application. Unexpected validation mutations refuse automatic restoration.
Each file replacement is atomic, but the complete multi-file operation is not
a filesystem transaction: unexpected write failures can leave a partial change
with its recovery journal retained. External process side effects outside the
workspace are not rolled back. Trial copies do not reproduce Git metadata and
use explicit filesystem indexing, independent of the enclosing Git repository.

## Failure detection and diagnosis

`--semantic-loops` compares canonical workspace evidence plus complete recognized
host diagnostics from raw validation output. Diagnostic source coordinates may
move, but messages, operands, paths and occurrence counts remain significant.
Unknown or incomplete diagnostic output cannot establish a repeated failure.
Ordinary comments and safe horizontal formatting can normalize; literals,
Python indentation, significant newlines,
paths and non-source inputs remain significant. Ambiguous syntax, directives,
unsupported inputs and source files over 4 MiB retain exact bytes. Incomplete
scans cannot establish equality. A repeated failed state produces a recorded
loop warning and concrete model feedback. This evidence never skips validation
or authorizes completion.

`--failure-reflection` grants one `reflect_failure` diagnostic action after a
failed validation or repeated failed candidate. It cannot read files, edit,
execute commands or declare success. The default bound is 256 tokens;
`--reflection-tokens 32..1024` overrides it. It consumes the existing action and
token budgets, and cannot take the reserved validation/final slots. Reads,
different tools and intermediate edits do not reset the episode or grant more
reflection. Passing validation closes the episode.

## Structural impact and validation

`--symbol-impact` captures indexed declarations before edits and compares them
with the changed repository. Go AST ranges identify added, changed and removed
declarations. Identifier occurrences provide explicitly labelled caller and
possible broken-reference candidates; they are not resolved call/type edges or
coverage measurements. When evidence permits, preliminary Go test stages use
anchored test-name selection and the existing reverse-import graph. The broad
final stage remains unchanged and authoritative.

Python supplies bounded lexical definition/caller evidence and retains broad
fallback. Unresolved, dynamic, deleted, ambiguous, unsupported or incompletely
indexed inputs also fall back. Unindexed resources remain part of complete
validation snapshots, but are not mapped to symbols. Full call/type resolution,
measured coverage mapping and additional language schedulers remain repository
intelligence work; this implementation must not be described as those features.

## Interactive sessions

`forge chat` keeps one loaded model across requests and retains actual user,
assistant and tool exchanges. `/new [task]` clears history; `/quit` exits.
`/begin` followed by `/end` submits multiline input. The `ask_user` tool waits
for an answer; `/decline` refuses and `/cancel` stops the session. Questions
never alter write or process permissions. Native prompting is required.

Interactive questions currently require one candidate. `forge chat --candidates 2`
and library configurations combining multiple candidates with an `ask_user`
callback are rejected explicitly: a real user's clarification must not be lost
when a sampled candidate is discarded. Collect requirements in a single-candidate
conversation before launching a separate multi-candidate run. Library hosts may
share persistent conversation history across multi-candidate runs when no question
callback is installed; only the selected candidate adds its exchanges to that history.

The caller owns `forge_conversation`; agent configurations borrow it for one
active operation at a time. Zero history limits select 256 KiB and 16 user
exchanges. Hard bounds are 16 MiB, 1024 exchanges and 2048 retained segments.
Eviction removes complete oldest exchanges and leaves a visible notice. An
exchange that cannot be retained blocks continuation until reset. Model
prompt-token limits apply separately, so `/new` can be needed before the byte
cap. History lives in memory in this process; audit-log replay is not resume.

Library hosts supply a synchronous `forge_question_fn` callback: fill the
provided UTF-8 answer buffer and return OK, POLICY for refusal, or CANCELLED for
EOF/cancellation. Callbacks must cooperate with host cancellation; the CLI
polls input and enforces EOF, cancellation and run deadlines itself. Callers
must not retain the borrowed buffer or destroy the conversation during a run.

The [seven-arm diagnostic protocol](../benchmark/LOOP_COMPLETION.md) separates
these mechanisms from performance claims and retains all failures.
