# Automatic checkpoint correctness

Source `028082cfc9e55134b9d2da374a65a41f54b00f38` passed the optional automatic
checkpoint test and a read-only agent smoke on Windows with the Qwen3-Coder
30B-A3B Q4_K_M model and RTX 5090 Laptop GPU. All 49 layers were offloaded.
[Environment and binary/model hashes](environment.json) identify the exact run.
The model SHA-256 was rechecked. No model, KV bytes, raw sessions or credentials
are included.

## Automatic A/B/A/B

The test first generates both references with prefix reuse disabled, unloads
that instance, then generates A/B/A/B on one new instance with the automatic
manager. It makes no explicit checkpoint save call. Both initial prompts are
captured during normal prefill; both displaced prefixes must restore, match the
cold output bytes, and account for all prompt tokens. A subsequent generation
change must invalidate the old entries and preserve output parity.

| Variant | Prompt / cold prefill | Restored | Reused | Recomputed | Additional reuse over live state | Output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A | 150 | 150 | 149 | 1 | 141 | 1 |
| B | 150 | 150 | 149 | 1 | 141 | 1 |

[Numeric records](results.json) were emitted only after all assertions passed;
the executable returned zero. Peak manager allocation was 29,500,755 bytes under
the 268,435,456-byte cap. These are small correctness cases, not task timings.
Capture/restore timing excludes model loading and is affected by the coarse
Windows clock. It must not be interpreted as an overall speedup.

```text
forge_checkpoint_model --automatic /path/to/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf -1 1024
```

## Agent opt-in smoke

Create an otherwise empty workspace with `message.txt` containing the UTF-8
bytes `violet-ember-4721` and a trailing LF. Run:

```text
forge run "Use read_file to read message.txt, then return its exact contents. Do not modify files or run commands." --workspace /path/to/fixture --no-config --model /path/to/model.gguf --gpu-layers -1 --context 4096 --output-reserve 512 --max-tokens 2048 --max-input 18000 --max-turns 4 --wall-ms 180000 --threads 1 --temperature 0 --seed 42 --checkpoint-cache --no-auto-validation --json
```

The [smoke record](agent-smoke.json) verifies the expected final text, one actual
`read_file` call, two inference turns, one automatic capture, and unchanged
fixture bytes. No write/process permission was granted and automatic validation
was explicitly disabled for this reading task. It is not validation evidence.
The second turn used the live prefix, so zero physical hits is expected here.
This check covers agent integration; the A/B/A/B case covers displaced-state
restoration. Neither establishes long-generation quality or durable resume.

See the [checkpoint contract](../../../docs/CHECKPOINTS.md) for scopes, memory
accounting, failure behavior and remaining platform/semantic-boundary limits.
