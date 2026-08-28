# Build details

## Source builds

Linux: install a C/C++ compiler, CMake 3.24+, Git and Python 3.10+. Set
`-DGGML_CUDA=ON` for an installed NVIDIA CUDA toolkit, or use llama.cpp's platform
backend options. Apple Silicon uses the upstream Metal configuration. Windows
uses Visual Studio 2022 with the C++ desktop workload and a Windows SDK.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`-DFORGE_WITH_LLAMA=OFF` removes the inference dependency for core tests.
`-DFORGE_SANITIZE=ON` enables ASan/UBSan on GCC/Clang. Production inference is never
silently replaced by the test backend.

Dependencies are cached below `build/_deps`. For offline configuration, populate
those caches first or use CMake's `FETCHCONTENT_SOURCE_DIR_YYJSON`,
`FETCHCONTENT_SOURCE_DIR_SQLITE`, `FETCHCONTENT_SOURCE_DIR_TREESITTER`,
`FETCHCONTENT_SOURCE_DIR_TSGO`, `FETCHCONTENT_SOURCE_DIR_TOMLC17`, and
`FETCHCONTENT_SOURCE_DIR_LLAMA` overrides.

## Windows with prebuilt CUDA libraries

For development without a CUDA toolkit, download the upstream **b10566** x64
CUDA archives. That nightly corresponds to the pinned llama.cpp **v0.2.0** source
commit `bb4caa7540188872173c44d161602d9271386413`.

| Archive | SHA-256 |
| --- | --- |
| `llama-b10566-bin-win-cuda-13.3-x64.zip` | `c3e2336c1427e8bd7b5beb3c8618d2f7a268bc5fb6ec3f28c1e06cdb78d2e80a` |
| `cudart-llama-bin-win-cuda-13.3-x64.zip` | `1462a050eb4c684921ba51dcc4cc488a036674c3e73e9945ee705b854808d03e` |

Obtain them from the [official release](https://github.com/ggml-org/llama.cpp/releases/tag/b10566),
verify hashes, and extract both into one folder. Do not mix DLL releases.

```powershell
cmake -S . -B build-gpu -G "Visual Studio 17 2022" -A x64 `
  -DFORGE_LLAMA_PREBUILT=C:/path/to/verified/llama-cuda
cmake --build build-gpu --config Release --parallel
ctest --test-dir build-gpu -C Release --output-on-failure
```

CMake uses `dumpbin`/`lib` to create import libraries and copies the DLLs next to
the executables. Python is needed for this development-only conversion. Model
weights remain separate. Use a driver compatible with the selected CUDA runtime.

## Model choice

The initial development target is Qwen3-Coder-30B-A3B-Instruct Q4_K_M with a
16,384-token context and 2,048-token output reserve. The JSON file
`profiles/qwen3-coder.json` records provenance; `--profile profiles/qwen3-coder.toml`
loads runtime settings. Supply an existing model with `--model` or a project config.

GGUF source: [Unsloth](https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF)
at revision `b17cb02dd882d5b6ab62fc777ad2995f19668350`.

File: `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`, 18,556,689,568 bytes.
SHA-256: `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad`.

Model weights are Apache-2.0 according to the publisher. Download them separately
and verify the hash. Smaller models or CPU execution are also supported; no
specific performance is promised. If a chat template is unsupported by the C
API, use an appropriate `--chat-template` override (e.g. `chatml` for Qwen).
