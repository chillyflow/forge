| model                          |       size |     params | backend    | ngl |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | --: | --------------: | -------------------: |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           pp512 |     5187.92 ± 390.69 |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           tg128 |        224.77 ± 3.35 |

build: 5f436dddb (10948)
