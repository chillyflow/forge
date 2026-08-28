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
