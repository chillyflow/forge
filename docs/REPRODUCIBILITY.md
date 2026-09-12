# Reproducibility of agent runs

Two independent defects made otherwise-identical agent runs render different
prompts and diverge. Both were host artefacts — neither was model sampling, and
neither was GPU nondeterminism. Both are fixed; the evidence for each is
recorded here because the diagnosis is easy to lose and was twice misattributed
to the sampler.

## Why this matters

The campaign record attributed run-to-run variance to the sampler:

> "run-to-run variance is neither seed- nor temperature-controllable; what
> remains is GPU batching nondeterminism."

That was wrong, and it was load-bearing: it framed a fixable host defect as an
irreducible property of local inference, which made every downstream comparison
noisier than it had to be. Reproducibility is also the precondition for any
comparative claim — if two runs of the same task render different prompts, no
gate measured against them means what it appears to mean.

## Cause 1 — derived bytecode in the input snapshot

The bounded input snapshot hashed every file in the workspace. Running the
fixture's own test suite writes `__pycache__/*.pyc`, and a `.pyc` header embeds
the **source modification time**. Two byte-identical workspaces therefore
produced different snapshot hashes.

That hash is rendered into the prompt as `HOST_OBSERVATIONS:
current_inputs=<hash>`, so the model saw a different prompt, took a different
action, and the run diverged.

Evidence from the c10 screen (three repetitions of one task):

- Actions 1-4 identical across repetitions; prompts byte-identical through
  turn 4.
- Divergence began at **turn 5** — the first prompt rendered after the first
  mutating action, `run_command: {"argv": ["python", "test_service.py"]}`.
- Byte differences confined to `.pyc` header offsets **8, 9, 12, 13** — the
  mtime field is bytes 8-11 and the source size 12-15.
- Embedded mtimes differed per repetition (`service.cpython-311.pyc`
  `21:42:38` vs `22:04:12`; `test_service.cpython-311.pyc` `21:42:10` vs
  `22:01:39`).

**Fix.** `src/core/input_snapshot.c` excludes `__pycache__`, `*.pyc` and `*.pyo`
at any depth, in both the Windows and POSIX walks. The pre-existing
`snapshot_metadata` exclusion fired only at the snapshot root and only for
directories.

**Guard.** `tests/unit/test_snapshot.c::test_derived_bytecode_exclusions_are_exact`
rewrites a `.pyc`'s bytes and asserts the snapshot is unchanged, then writes one
at depth and asserts the same, then changes real source and asserts the snapshot
*does* change.

## Cause 2 — host paths in tool output

Tool output carries absolute paths. Python tracebacks from `run_command` embed
the file's absolute path, and the benchmark harness creates each workspace at
`tempfile.TemporaryDirectory(prefix='forge-bench-')` — a **randomly named**
directory.

So even with Cause 1 fixed, two identical runs rendered prompts differing by:

```
run1: ...\Temp\forge-bench-b78h39cb\test_service.py
run2: ...\Temp\forge-bench-l2wk5hjv\test_service.py
```

**Fix.** `src/core/agent.c` rewrites the workspace root to `.` in every text that
can reach the model, via `normalize_workspace_paths()`: the minimal agent's tool
dispatch, the text `candidate_validate` returns, and the two retained validation
feedback strings. This makes the prompt independent of where the workspace lives
rather than merely of what it is named, so it holds across machines and users,
not just across repetitions on one host.

### The fix needed four sites, and finding them was the hard part

The path leaked from more than one producer, and locating them was not
guesswork that could be shortcut:

- The agent's own `run_command` results are produced at `minimal_run`'s tool
  dispatch (`fg_tool_execute`, and the `tool/%06zu.raw` artefact is written from
  the same value).
- The verifier's output arrives as the **`validate_candidate` tool result**,
  which is built in a **separate branch** of `minimal_run` that appends
  `candidate_validate`'s return value directly — it never reaches the normal
  tool-dispatch path.
- `candidate_validate` also *retains* two feedback strings that are re-rendered
  into every later prompt, so normalising only the returned value left the
  HOST_OBSERVATIONS section leaking.

Two traps made this slow, and both are worth remembering because a green build
says nothing about either:

1. **The configured agent is not the obvious one.** `loop-repair` passes
   `--minimal-agent`, and `forge_agent_run` returns early into `minimal_run`.
   A correct-looking change placed in `forge_agent_run` is unreachable dead
   code in that configuration. Verify which function actually runs before
   trusting a patch site.
2. **A same-file function is not a same-behaviour site.** The dispatch that
   *looks* like the tool path (`fg_tool_execute` in `forge_agent_run`) is not
   the one the minimal agent uses.

The method that worked, repeatedly: diff two runs' rendered prompts, find the
first differing byte, then identify the producer of *that exact string* by
reading the prompt's own structure around it. Each fix moved the first
divergence later (turn 5 → 8 → 10 → …), which is what confirmed each producer
was real rather than assumed.

## Evidence: the divergence moved, then stopped

Same task, same profile, same seed, two runs, `--forge build-gpu/Release/forge.exe`:

| tree state | identical turns | first divergence |
| --- | --- | --- |
| pre-fix | 7/32 | turn 5 (`.pyc` mtimes) |
| Cause 1 fixed | 7/32 | turn 8 (temp-dir path) |
| + tool dispatch, escape-matcher bug | 7/32 | turn 8 (no effect) |
| + tool dispatch fixed | 9/32 | turn 10 |
| + `validate_candidate` return normalised | **32/32** | **none** |

Final state: **32 of 32 rendered prompts byte-identical across two independent
cold processes**, with zero occurrences of the workspace root remaining in any
prompt. Both runs also reached the same outcome (`rc=1`).

The monotone movement of the divergence boundary is what made each producer
trustworthy: every fix either moved it later or was provably a no-op, and a
no-op is what identified the wrong patch site rather than a wrong diagnosis.

Note that the retained evidence artifacts are deliberately **not** all
normalised — `tool/*.stderr` still contains the absolute path, because it
records the process's raw output. `tool/*.raw` is normalised because it is the
model-visible text. Artifacts that record ground truth should not be rewritten
to match what the model was shown.

## What is not claimed

- **The sampler is fine.** A high-entropy control (`Invent ten one-word names
  for imaginary sea creatures.`) produced **3 of 3 distinct outputs** across
  temperature 0 / 1.5 and seeds 42 / 99. An earlier conclusion that
  `--temperature` and `--seed` were inert on `forge complete` was **wrong** and
  has been retracted: the prompt used for that test was so peaked that no
  temperature changed the argmax over 96 tokens of boilerplate.
- **Single generations are bit-reproducible across cold processes** — 5/5 and
  5/5 identical md5 (`1fba41374449d6f86516c0d597283d46`) at temperature 0 and
  0.6. This is the `complete` path, which does not reuse a KV cache.
- **Nothing here speaks to cold-vs-*cached* generation**, which is a separate
  question and the one the pinned llama.cpp `tools/server/README.md` addresses
  when it documents batch-size-dependent logits under `cache_prompt`. Verify
  parsed tool calls and token bounds separately for that case.
- **Reproducibility is not correctness.** Two identically-wrong runs are
  reproducible. This work removes a source of variance; it does not add
  capability.

## What is guarded, and what is not

`tests/unit/test_core.c::test_workspace_path_normalization` covers the rewrite
itself: the literal spelling, the escaped spelling, every occurrence, and
passthrough when the root is absent. It lives in `src/core/text.c` as
`fg_normalize_workspace_paths` precisely so it can be tested — as a `static`
helper in `agent.c` it was unreachable from the suite.

The test was **falsified before being trusted**: disabling only the
escaped-spelling pass makes it abort. That is the exact bug that survived a
green build and 34 passing tests during development, so the test is known to
have teeth rather than merely to pass.

The **wiring** is not covered by any automated check, and cannot be cheaply:
Forge has no stub model backend, so exercising the agent loop in CI requires a
real model and a GGUF. The only instrument that sees the wired-up behaviour is
the two-run comparison above. Any change to which function renders model-visible
text should be re-verified with it, because four separate producers exist and
three of them are not on the obvious code path.

## Known limitation of the Cause 2 fix

The substitution is an exact string match against the canonical workspace root,
which on Windows is the backslash form. A command that prints the workspace in
another spelling — a POSIX-style path from a bash/MSYS tool, a short `8.3` path,
or a differently-cased path — will not be normalised. In this benchmark the
tools are invoked as native processes and emit the canonical form, so the fix is
sufficient for the runs measured here, but it is not a general path canonicaliser.

If a future divergence reappears with identical prompts otherwise, check for a
separator or case variant of the workspace root before looking anywhere else.

## Verifying instrument failure

Both invalid experiments in this investigation were caught by inspecting the
artefact rather than the exit code — worth repeating, because either would have
produced a confident wrong answer:

- A `complete` sweep returned `rc=1` with 2 bytes of output. Counting files
  would have reported "5 identical outputs" as a determinism result.
- A retry returned five *empty* files, all sharing md5 `d41d8cd9...` — the
  empty-string hash — and a script that printed "1 of 5 distinct" was reporting
  that nothing had run at all.

`RUN_EFFICIENCY.md` §1.9 says to validate an extractor against a hand-checked
table before publishing its numbers. These are why.
