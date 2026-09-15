# Spike 008 - does draft-model speculation pay on agent-shaped output?

## Question

The loop's output is dominated by tool-call text: an XML/JSON envelope around predictable
arguments. Draft-model speculation is exactly the technique that pays on predictable text -
it amortises the target's per-token weight read across several tokens - and it is
**available in the shipped runtime with no compiler** (`llama-cli` exposes
`--spec-draft-*`; `llama-common.dll` exports 136 speculative symbols). The earlier
conclusion was that Forge's remaining decode cost at temperature 0.6 is CPU masking, not the
GPU; if speculation works here it attacks the *GPU* side instead, which no other lever on the
table does.

Unknown, and the reason this is a spike: how much a 0.6B draft agrees with a 30B-A3B
instruct target on this workload, on this machine.

## Method

Shipped runtime only (`build bb4caa754`, the same library `forge.exe` links), one machine,
otherwise idle:

- Target: `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` (`-ngl 99`).
- Draft: `Qwen3-0.6B-Q8_0.gguf` (640 MB, official Qwen GGUF, `-ngld 99`). Same tokenizer
  family, so vocabularies match.
- Prompt: `task-prompt.txt` - a Qwen3 chat-format tool-using request that produces the
  XML tool call the census measured in spike 007.
- `-no-cnv -n 128 -s 42 -temp 0`, so both arms generate greedily and the comparison is
  about the sampler, not the dice.
- Arm A: no draft. Arm B: draft with `--spec-draft-n-max 3`, then `8` if 3 shows a gain.

## Bars (frozen before the first run)

- **P1 usefulness**: speculative decode tokens/s >= **1.30x** arm A on this prompt => worth
  integrating into Forge; **<= 1.05x means falsified** (the pairing does not agree well
  enough on agent output to pay for the draft's own compute).
- **P2 acceptance**: report the accepted-draft fraction the runtime prints (or compute it
  from its draft statistics). Below ~50% acceptance the technique cannot pay for a 3-token
  draft; that number is the explanatory variable either way.
- **P3 floor**: measure the draft model's own decode speed with `llama-bench` so the
  accounting is checkable: per speculative step the runtime spends (draft tokens x draft
  cost) + one batched verify pass.

## Falsifiers

1. Net decode tokens/s below arm A (speculation is a loss on this workload).
2. Acceptance so low that the projected step cost exceeds the baseline - reported as
   falsified even if the wall-clock happens to wobble above 1.05x.

## Scope

Measuring, not implementing. If it pays, the Forge-side version would be a draft context plus
grammar-filtered drafting - the grammar is what makes agent output *more* predictable than
the plain model, so this spike's numbers are a *lower* bound for that design. One model
download (640 MB) is the only side effect, and it stays in the models directory.

## Results

**Baseline measured, warm, 512 tokens** (`llama-server`, no draft): **226.3 tok/s**
(4.4 ms/token), matching `llama-bench`'s 243.65 and Forge's own 212-230. Two identical
repeats of the 512-token request on the draft-loaded server: **227.95 / 228.23 tok/s**; a
third configuration (`--no-spec-draft-backend-sampling -np 1`): **230.32 / 231.10 tok/s**.

**Speculation never engaged.** The draft loads
(`common_speculative_init_result: loading draft model 'Qwen3-0.6B-Q8_0.gguf'`), and that is
the *only* speculative line in any server log. No response carries draft statistics
(`timings` has no draft keys), no request logs an acceptance count, and the three
configurations land inside the baseline's noise band (+0.7%, +0.9%, +1.8%, +2.1%). The
machine was otherwise idle (VRAM 19.1 GB for the target alone, 19.6 GB with the draft).

**One measurement hazard worth recording**: a request's *first* token after a new prompt
shape costs **2.2-2.5 s** (graph re-reservation), so short requests report absurd rates -
13.5 tok/s over 35 tokens against 204.3 tok/s over the same 35 tokens once warm. Any
short-completion comparison through the server without repeated warm requests is measuring
warmup.

### Retained failures from the CLI path

`llama-cli` and `llama-completion` were tried first and are kept as failures:

- `fail-cli-interactive-armA.txt`: conversation mode, `-n 128` ignored, 2.3 MB of runaway
  output, the process had to be killed from outside the shell session (which then had to be
  recovered).
- `armA-noDraft.txt`: with `-no-cnv` the chat-format prompt's special tokens are escaped
  unless `--special` is passed, so the model saw markup as plain text and stopped after one
  token (`assistant: [end of text]`, 89 ms / 1 run).
- `armB-draft3.txt`: the draft arm looped on interactive `>` prompts and exited 1.

The server is the right instrument; the CLI flags are a trap.

### Anomaly, not explained

`armA-nodraft.txt` (2.7 MB) carries a timestamp *before* any CLI run recorded here. Windows
paths are case-insensitive, so it is also the same file my later `armA-noDraft.txt` wrote to.
It is left in place rather than deleted; no command in this spike accounts for its original
contents.

## Verdict

**Blocked - not falsified, and not measured.** No bar was evaluated: P1 (1.30x), P2
(acceptance) and P3 (the draft's floor) all require speculation to actually run.

The premise behind this spike - that the shipped binaries let us measure speculation today,
no compiler needed - is **wrong for this pin**. The machinery is in the library
(`llama-common.dll` exports 136 speculative symbols including draft, EAGLE-3, MTP and ngram
variants) and the server initialises the draft model, but the request path in `bb4caa754`
does not use it under any of the configurations tried. Something else gates it, and finding
that out is now a code-reading task in the pinned source, not a measurement task - the
harness/source is on disk (`build/_deps/llama-src`), so it is answerable without guessing.

What this spike does settle: single-stream decode of this model through the shipped runtime
is **226-231 tok/s warm**, the draft model loads beside it (19.6 GB total VRAM), and the
tooling paths above are the traps to avoid next time.

If the server-side gate is found and accepts a draft without a source build, the measurement
resumes from here; if it needs a source build (no CUDA toolkit on this machine), then
speculation for Forge means implementing draft+verify in Forge over `llama.dll`'s C API -
which is the prerequisite for the grammar-aware drafting variant anyway, and an
implementation task rather than a measurement one.