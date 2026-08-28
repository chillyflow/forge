# Benchmark methodology

`tasks/*.json` contains ten tiny Go repair tasks. They are a smoke suite, not
SWE-bench or representative evidence of general repository-level performance.
`generate_tasks.py` recreates them deterministically for review.

`run.py` creates a fresh temporary Git checkout for every task/variant, runs Forge,
and checks an independent verifier after the model finishes. Modifying the
original test file invalidates the result. Raw session artifacts are copied to
the output directory before removing the runner-owned temporary checkout.

```sh
python benchmark/run.py --forge ../build/forge --model /models/coder.gguf \
  --tasks add clamp --variants optimized no-kv no-semantic \
  --output /tmp/forge-bench
```

The Go executable must be on PATH. Fixtures use only the standard library.
The runner executes code; use trusted manifests and an isolated machine.
Do not point native `forge bench` at a valuable checkout without reviewing the
prompt and verifier.

## Reporting

Report GGUF filename/SHA-256, llama.cpp revision, GPU/driver, context size, output
budget, GPU layers, sampling settings, harness version, verification command,
task count, failure count and warm/cold conditions. Preserve all failures.

- `prompt_tokens`: logical prompt tokens across calls.
- `cached_tokens`: sequential prefix tokens not decoded again.
- `prefill_tokens`: new prompt tokens submitted for evaluation.
- `generated_tokens`: sampled tokens excluding end-of-generation.
- `raw_tool_bytes`: captured output, not bytes discarded after process limits.
- `visible_tool_bytes`: result bytes retained for model context.
- `wall_seconds`: driver time including model loading and verification.
- `duration_ms`: agent time; model loading is reported separately.

Never compare simulated token estimates with actual tokenizer counts. A no-KV
Forge ablation is not a comparison with another product. External comparisons
must use the same GGUF, hardware and tasks, preserve each harness's normal tool
protocol, and report configuration differences.

Disk KV caching, speculation, and OS isolation are not current variants. No
speedup is assumed before measurement. Review raw artifacts before publishing.
