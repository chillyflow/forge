# E3 agent screen — results (2026-09-18)

**Outcome: the screen did not run — frozen bar clause 1. Zero judge
engagement in both engagement-preflight runs; recorded result:
"the mechanism does not engage in this population."**

## What ran

- Launch identity per Amendment 1 in [PREREGISTRATION-E3.md](PREREGISTRATION-E3.md):
  post-fix binary `6a6b882378095b57c4a412256f8642546cb7df3c0e0027047c7d6365800c28dd`;
  every other frozen input re-verified at launch (run.py, judge-e3.toml, six
  task manifests, model file).
- The engagement preflight — the first two judge-arm cells of the 12-cell
  schedule — ran; the remaining 10 cells did not run, per bar clause 1. No
  control-arm runs were made.

| Cell | Result | Wall | Turns | Prompt / generated |
| --- | --- | --- | --- | --- |
| `go_api_pagination-optimized-judge` | **PASS** (protected files unchanged) | 54.6 s | 9 | 46,361 / 1,166 |
| `generalize_retractions_original-optimized-judge` | **FAIL** — loop stopped with status `limit` (input budget: 257,428 of 262,144 prompt tokens over 29 turns, 34 context evictions), then verification failed | 209.1 s | 29 | 257,428 / 7,619 |

## Engagement evidence (the preflight's question)

- Zero raw judge records under `.forge/judge-raw-e3/` — zero API calls, zero
  cost.
- The judge was armed: `command.json` records `--judge --config judge-e3.toml`
  on the exact invocation, and the rendered prompt's tool registry includes
  `retrieve_context`.
- The agents never called `retrieve_context`: session tool-call events contain
  only `read_file`, `run_command`, `apply_patch`/`apply_hunk`. Both cells
  worked the fixture by direct file reads and shell commands.
- Consistent with prior campaigns: no session in the repair-control /
  agent-loop / beat-contemporaries campaigns contains a `retrieve_context`
  tool call either.

## Interpretation

As anticipated by the prereg (clause 1 exists for exactly this outcome): the
judge rerank's agent-level value is conditioned on the agent using semantic
retrieval, and this fixture population is small enough that the rich loop
reads files directly. It is a population finding, not a judge finding — no
claim is made about task-level effect either way, since no control arms ran.
Measuring agent-level effect requires a population that exercises
`retrieve_context` (e.g., a workspace where direct reads do not scale, or a
task that forces retrieval); that would be a fresh experiment on this frozen
instrument.

## Retained evidence

- `e3/` — both full run directories (results.json, environment.json, session
  artifacts, verification).
- Amendment 1 (prereg); this file. The empty record directory is the
  engagement datum.
