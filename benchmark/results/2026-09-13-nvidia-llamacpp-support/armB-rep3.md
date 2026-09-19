| model                          |       size |     params | backend    | ngl |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | --: | --------------: | -------------------: |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           pp512 |     5285.09 ± 277.53 |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           tg128 |        230.06 ± 9.17 |

build: 5f436dddb (10948)
