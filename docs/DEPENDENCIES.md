# Native dependencies

| Dependency | Pin | License | Use |
| --- | --- | --- | --- |
| llama.cpp | v0.2.0, `bb4caa7540188872173c44d161602d9271386413` | MIT | Optional direct inference backend |
| yyjson | 0.12.0, `8b4a38dc994a110abaec8a400615567bd996105f` | MIT | JSON parsing/serialization |
| tomlc17 | `64a063b8636a4b48d142f978270f5e53e605e240` | MIT | Native TOML configuration |
| SQLite | 3.53.4 amalgamation | Public domain | Persistent index and FTS5 storage |
| Tree-sitter | v0.25.10, `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406` | MIT | C parser runtime |
| tree-sitter-go | v0.25.0, `1547678a9da59885853f5f5cc8a99cc203fa2e2c` | MIT | Go grammar |

Dependencies are fetched from upstream publishers; none are copied into Forge's
Git history. Preserve their license files when redistributing binaries. CUDA
libraries have NVIDIA's separate distribution terms. Upstream llama.cpp is
C/C++; the Forge source under `src/` and `include/` is C17.

Build tools, benchmark drivers and tests may use Python. They are not required by
the native CLI/library at runtime. Model weights have separate licensing.
