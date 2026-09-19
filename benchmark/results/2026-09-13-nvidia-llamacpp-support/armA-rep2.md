| model                          |       size |     params | backend    | ngl |            test |                  t/s |
| ------------------------------ | ---------: | ---------: | ---------- | --: | --------------: | -------------------: |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           pp512 |     5164.75 ± 299.97 |
| qwen3moe 30B.A3B Q4_K - Medium |  17.28 GiB |    30.53 B | CUDA       |  99 |           tg128 |        230.18 ± 5.76 |

build: bb4caa754 (10566)
