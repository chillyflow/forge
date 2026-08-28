# Staged retrieval smoke — 2026-08-28

This is a small correctness check on source `4cfd258`, using the exact executable
and model recorded in [environment.json](environment.json). It is not a broad
retrieval-quality evaluation or a speedup measurement.

A standalone Git fixture contains `go.mod` and one Go function, `Answer`, which
returns `37`. After a successful model-free `forge retrieve Answer --depth 0`
preflight, Qwen3-Coder on Windows/CUDA called `retrieve_context` exactly once.
The tool returned the exact declaration, source SHA-256 and indexed generation.
The model answered `37`; the fixture bytes remained unchanged. There were no
write/process tool calls or validation commands. Repository discovery still
uses Git. [agent-smoke.json](agent-smoke.json) records the selected events,
complete retrieval result, fixture and metrics.

The initial fixture was mistakenly placed under the parent repository's ignored
`.scratch` directory without its own Git root. It therefore indexed no files;
the model answered `0` after receiving an empty result. The process exited zero
but **the correctness check failed**. Initializing the fixture's own Git
repository and verifying indexed evidence corrected the setup. This failed run
is retained in the record; it is not included as a successful smoke. Runtime
ignore behavior was not weakened to include ignored files.

The passing run used two inference turns, 2,052 cumulative prompt tokens, 18
generated tokens, 607 live-prefix cached tokens and 1,445 prefilled tokens. It
captured one automatic checkpoint but needed no restore. Model load took
11,766 ms and the agent phase 1,734 ms. These one-run timings are descriptive,
overlap other measurements, and have no baseline or confidence interval.

## Reproduce

Create a fresh standalone Git repository with the two exact files in the record.
Use a Release build with the pinned llama.cpp CUDA backend and verified GGUF.

```text
forge retrieve Answer --depth 0 --workspace FIXTURE --no-config
forge run "Call retrieve_context exactly once with query Answer, then answer with only the integer returned by Answer based on the indexed excerpt. Do not use other tools, modify files, or run commands." --workspace FIXTURE --no-config --model MODEL --gpu-layers -1 --context 4096 --output-reserve 512 --max-tokens 2048 --max-input 18000 --max-turns 4 --wall-ms 180000 --threads 1 --temperature 0 --seed 42 --checkpoint-cache --no-auto-validation --json
```

Check the actual tool evidence, final answer and unchanged fixture bytes, not
only the process exit status. A different model/backend/hardware can choose
different actions even with the same seed.
