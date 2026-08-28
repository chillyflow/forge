# Real-model prefix checkpoint checks

All four cases passed on Windows with Qwen3-Coder-30B-A3B Q4_K_M, the RTX 5090
Laptop GPU, all 49 layers offloaded, a 1,024-token context and greedy decoding.
Source revision: `5b6d9d096c6657af86bcf24ade9081003e3fd833`.

| Prompt case | Variants | Prompt tokens each | Saved bytes each | Recomputed after restore | Cold prefill |
| --- | --- | ---: | ---: | ---: | ---: |
| Short instruction | A / B | 15 | 1,475,916 | 1 | 15 |
| Source context | A / B | 287 | 28,217,868 | 1 | 287 |

Each case captures independent A/B states, restores both twice, compares output
bytes between repeated restores, unloads the model, and compares both with cold
generation on a new instance. The new instance also rejects the old handles.
Only one model and at most two host checkpoints coexist. Cold prefix reuse is
disabled for both variants.

The test completed with exit code zero. [Numeric records](results.json) and
the [environment and hashes](environment.json) identify the measured binary/model.
The model SHA-256 was independently rechecked for this publication. No weights,
serialized state, raw session data or credentials are included.

## Reproduce

Build the optional test with the pinned direct llama backend, then supply an
existing local GGUF explicitly:

```text
forge_checkpoint_model /path/to/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
```

See [build instructions](../../../docs/BUILD.md) and the
[checkpoint contract](../../../docs/CHECKPOINTS.md). Ordinary CTest invokes the
test without a model and skips it; scripted tests are not a substitute.

## Limits

This is a small correctness check, not an agent or task-time benchmark. These
requested completions each generated one output token. The source case has 287
tokens, not a full context window. Longer continuations, other model families,
Linux NVIDIA and macOS Metal need separate runtime evidence.

State-copy timing does not include model loading or complete prefill. Windows
uses the coarse `GetTickCount64` clock; a zero duration means below its observed
resolution, not instantaneous work. Do not derive an overall speedup from these
values or combine them with the coding-task benchmark totals.

These are in-memory, same-instance prefix states. They do not resume a process,
restore a sampler, establish a cache-eviction policy or implement disk persistence.
