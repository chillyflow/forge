# Development model

The measured development setup uses **Qwen3-Coder-30B-A3B-Instruct, Q4_K_M GGUF**
from [Unsloth's conversion](https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF)
of the [Qwen model](https://huggingface.co/Qwen/Qwen3-Coder-30B-A3B-Instruct).

This is a practical fit for the tested 24 GB RTX 5090 Laptop GPU: the quantized
weights fit with a 16,384-token context. The model has about 30.5 billion total
parameters and 3.3 billion active parameters. It does not require a separate
thinking phase. This choice is not a claim that it is the strongest available
model on all coding tasks.

| Property | Value |
| --- | --- |
| Filename | `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf` |
| Size | 18,556,689,568 bytes (17.28 GiB) |
| Hugging Face revision | `b17cb02dd882d5b6ab62fc777ad2995f19668350` |
| SHA-256 | `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad` |
| Context used for measurements | 16,384 tokens; 2,048 reserved for each response |
| Offload | All layers (`--gpu-layers -1`) |
| Sampling | Greedy, seed 42 |
| Model license | Apache 2.0; consult the publisher's model card |

The weights were downloaded separately and checked against the publisher's
SHA-256. They are not committed to GitHub. No access token is required by Forge:
inference reads an existing local GGUF and makes no Hugging Face API requests.

## Additional measured models

The thought-channel mechanism was measured on three further non-thinking
instruct models to test whether it is portable across tokenizers and chat
template families rather than tuned to one model. All three were run on the
same rig and settings as the development model: 16,384-token context with
2,048 reserved per response, all layers offloaded (`--gpu-layers -1`), greedy
decoding with seed 42, and the chat template embedded in each GGUF (no
`--chat-template` override was needed). Results are in
`benchmark/results/2026-08-31-tier1-models/`.

| Property | Qwen3-4B-Instruct-2507 | Devstral-Small-2-24B-Instruct-2512 | Meta-Llama-3.1-8B-Instruct |
| --- | --- | --- | --- |
| Filename | `Qwen3-4B-Instruct-2507-Q8_0.gguf` | `Devstral-Small-2-24B-Instruct-2512-Q4_K_M.gguf` | `Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf` |
| Quantization | Q8_0 | Q4_K_M | Q4_K_M |
| Size (bytes) | 4,280,405,600 | 14,334,446,752 | 4,920,739,232 |
| Publisher (GGUF) | [unsloth](https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF) | [unsloth](https://huggingface.co/unsloth/Devstral-Small-2-24B-Instruct-2512-GGUF) | [bartowski](https://huggingface.co/bartowski/Meta-Llama-3.1-8B-Instruct-GGUF) |
| Hugging Face revision | `a06e946bb6b655725eafa393f4a9745d460374c9` | `6e458b8add42681bfd023de5eab93637694aaf82` | `bf5b95e96dac0462e2a09145ec66cae9a3f12067` |
| SHA-256 | `391c1e410fd9f4cf2de2b510273b56a84c19ce18f4fa3bfb3774031dac4ef068` | `d14ba9edee1bb4c4996a726deb81e49ae81800a3216f0774634238c380aee496` | `7b064f5842bf9532c91456deda288a1b672397a54fa729aa665952863033557c` |
| Architecture (GGUF) | `qwen3` | `mistral3` | `llama` |
| Chat template family | ChatML | Mistral | Llama-3 |
| Context used for measurements | 16,384 tokens; 2,048 reserved for each response | same | same |
| Offload | All layers (`--gpu-layers -1`) | same | same |
| Sampling | Greedy, seed 42 | same | same |
| Model license | Apache 2.0 | Apache 2.0 on the base model `mistralai/Devstral-Small-2-24B-Instruct-2512`; the GGUF repo tags `other` | Llama 3.1 Community License |

Each file's SHA-256 was checked against the value the Hugging Face repository
publishes for that exact revision before any measurement. Devstral's publisher
recommends a 0.15 sampling temperature; every arm here is greedy, which is
near that but not identical, and the other two models have no strong low
temperature guidance. Consult each publisher's model card for license terms.

## Native-thinking models

Forge can preserve a model's template-native reasoning path. The model
configuration key `enable_thinking` and CLI switches `--enable-thinking` /
`--disable-thinking` are passed to llama.cpp's Jinja chat-template renderer;
an explicit value fails rather than being ignored when the template does not
support the control. `--thought-native` combines enabled template thinking with
the cue-free lazy action grammar. Appending `--disable-thinking` leaves that
same grammar in place, providing a thinking-safe matched baseline instead of
amputating native reasoning with an eager token-0 action grammar.

The official Qwen3-4B Q8_0 smoke in
`benchmark/results/2026-08-31-reasoning-gated/native-qwen3-4b-smoke/` verifies
the path: native thinking passed `ceil_div` in 3 turns with 1,608 recorded
reasoning tokens; its thinking-disabled control reached the run limit in 9
turns. This single fixture establishes mechanism operation, not a general
accuracy benefit.

## Use a local model

```sh
forge run "Inspect this project and explain its entry points" \
  --workspace ./project --model /models/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf \
  --gpu-layers -1
```

Read-only operation is the default. Enable `--allow-write` and `--allow-exec`
only in a checkout where patches and unsandboxed commands are acceptable.

For smaller GPUs, use partial offload or a smaller model and measure it again.
Lowering the context saves KV memory but does not make the weight file smaller.
The model profile in `profiles/` is reference metadata, not an automatically
loaded configuration file. Model weights and session logs should stay outside
public Git history. Never put access tokens in command examples or commits.
