# Linux + NVIDIA evidence — 2026-08-28 (WSL 2)

Platform evidence for the v0.1 gate rows `Linux` and `NVIDIA` in
`docs/DESIGN_CHECKLIST.md:438-439`.

**This is WSL 2, not native Linux.** The guest is Ubuntu 24.04.4 on a
Microsoft WSL2 kernel with NVIDIA GPU para-virtualisation. It is a real Linux
userland and a real Linux kernel running direct GGUF inference through
llama.cpp, but it is not a bare-metal Linux deployment. Treating this record as
native Linux evidence would overstate it.

## Environment

| Item | Value |
|---|---|
| Host OS | Windows 11 25H2, build 10.0.26200.9168 |
| WSL version | 2.7.12.0 |
| Guest kernel | 6.18.33.2-microsoft-standard-WSL2 |
| Guest distro | Ubuntu 24.04.4 LTS |
| GPU | NVIDIA GeForce RTX 5090 Laptop, 24,463 MiB |
| Host driver (KMD) | 610.88 |
| Guest `nvidia-smi` | 610.57.01, CUDA UMD 13.3 |
| CUDA toolkit (guest) | 13.3.73 (`cuda-toolkit-13-3`, WSL-Ubuntu repo) |
| GPU offload | `-1` (49/49 layers), Flash Attention, CUDA graphs active |
| CPU / RAM allocated | 8 vCPU / 4 GiB (capped in `.wslconfig`) |
| Model | `Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf`, 18,556,689,568 bytes |
| Model SHA-256 | `fadc3e5f8d42bf7e894a785b05082e47daee4df26680389817e2093056f088ad` |
| Forge binary SHA-256 | `64c0b24969f8531ef4ccf93ccc7c7da50f609f5926865e20a7a24e0a846adec0` |
| Forge commit | `bb8e0e73d59ccffc6e504011ce253a24c7e197cf` |
| Go (guest) | go1.27.0 linux/amd64 (matches CI; 1.22.2 was too old — see below) |
| Git (guest) | 2.55.0 (2.43.0 from Ubuntu 24.04 is too old — see below) |

Driver note: no Linux NVIDIA driver was installed in the guest. Per NVIDIA's
CUDA-on-WSL guide the Windows host driver is mapped in as `libcuda.so`; only
`cuda-toolkit-13-3` was installed, never `cuda`, `cuda-13-3` or `cuda-drivers`.
`libcuda.so.1` in `/usr/lib/wsl/lib/` was verified unmodified after install.

## Results

| Check | Result |
|---|---|
| CTest suite (Release, `-DGGML_CUDA=ON`) | **100% passed, 0 failed of 24** (34.73 s); `checkpoint_model` skipped (needs model args) |
| Real-model KV checkpoint, 4 cases | **4/4 `matched: true`** |
| `forge bench`, full 10-task suite | **9/10 passed** (Go 1.27.0) — matches the Windows baseline |

### Real-model checkpoint parity with Windows

| Case | Prompt tokens | Restored | Cached | Recomputed | State bytes |
|---|---|---|---|---|---|
| short A | 15 | 15 | 14 | 1 | 1,475,916 |
| short B | 15 | 15 | 14 | 1 | 1,475,916 |
| source A | 287 | 287 | 286 | 1 | 28,217,868 |
| source B | 287 | 287 | 286 | 1 | 28,217,868 |

Host-state sizes are **byte-identical** to the Windows figures recorded in
`RESUME.md:358` (1,475,916 / 28,217,868), and the token accounting matches the
Windows pattern (one token re-evaluated per restore).

### Coding run: 9/10, matching the Windows baseline

| Task | Passed | Turns | Validation failures | Tests unchanged |
|---|---|---|---|---|
| add | yes | 6 | 0 | yes |
| average | yes | 7 | 0 | yes |
| ceil_div | yes | 6 | 0 | yes |
| clamp | yes | 8 | 0 | yes |
| contains | yes | 6 | 0 | yes |
| last_index | yes | 8 | 0 | yes |
| prefix | **no** | 13 | 0 | yes |
| range_sum | yes | 13 | 0 | yes |
| reverse | yes | 7 | 0 | yes |
| unique | yes | 7 | 0 | yes |

This reproduces the Windows baseline of 9/10 recorded in
`docs/DESIGN_CHECKLIST.md:452`. `prefix` fails with `loop_warnings: 3` and
`Repeated-action loop detected` — the loop detector stopping a stuck agent,
which is intended behaviour, not a crash.

Two earlier claims in this file were wrong and are corrected here:

1. An initial run reported the coding run as failing with "Windows task-success
   is not reproduced". The cause was environmental: the box had Go 1.22.2 while
   every fixture's `go.mod` declares `go 1.24`, so each `go` invocation tried to
   download a toolchain instead of running the fixture
   (`go: download go1.24 for linux/amd64: toolchain not available`). The agent
   had in fact applied the correct fix — `return a - b` to `return a + b`,
   recorded `state: applied` — and was blocked only at verification.
2. A reported "greedy self-copy loop" came from an invalid run in which
   `--workspace` was omitted, so the agent targeted the Forge repository itself.
   Discarded.

Worth noting: when verification failed, the agent attempted to finalise and
Forge **refused**, because validation had not passed. That is the
verify-before-finishing rule working as specified. The failing run is retained
as `add-optimized-go1.22.2-FAILED.json`.

## Memory footprint (measured, not estimated)

| Activity | Peak guest usage |
|---|---|
| CUDA build of llama.cpp + Forge (`-j 8`) | ~2.4 GiB |
| Real model inference + KV checkpoint | ~0.8 GiB |
| Agent coding run | ~0.7 GiB |
| 17.28 GB model copy (page cache) | host `vmmemWSL` 5,025 MB, reclaimed |

`memory=4GB` was verified sufficient: full suite and all four model cases pass.
This is a ceiling, not a reservation.

### Idle behaviour

With `vmIdleTimeout` at the 60 s default and `autoMemoryReclaim=dropCache`,
`vmmemWSL` terminates entirely after idle — verified absent from the host
process list after 75 s. Idle cost is zero, not merely low.

## Undocumented toolchain requirements on Linux

Two environment failures here traced to undocumented version requirements, not
to product defects. Both are now documented in-tree.

### Git 2.45+ (fallback is intended, not a bug)

`docs/INDEX.md:8-11` specifies that `--no-lazy-fetch` needs Git 2.45+, that
unsupported Git "uses the documented native fallback on a full scan", and that a
failing delta must "never" retry with fewer restrictions. The fallback is
therefore **intended behaviour**, and an earlier reading of it as a defect was
wrong. What was wrong was that the requirement was undocumented and that two
tests asserted gitignore-exclusion that only holds on newer Git.

Ubuntu 24.04 ships Git 2.43.0, which rejects the flag Forge hardcodes at
`src/repo/repo.c:1184`:

```
$ git --no-lazy-fetch -c core.fsmonitor=false ls-files -z --cached --others --exclude-standard
unknown option: --no-lazy-fetch
```

`forge_repo_index` then takes the fallback at `repo.c:1209`, setting
`filesystem_scan = true` and walking the tree — **which ignores `.gitignore`**.
Excluded files get indexed. Nothing is logged or surfaced.

This produced both Linux failures seen before the upgrade:

- `tests/unit/test_index.c:704` — `test_git_delta_eligibility` assert failed
- `tests/integration/test_cli.py:188` — `IgnoreMe` found in `symbols`

Upgrading to Git 2.55.0 made the command succeed and both tests pass, which
confirms the cause. Resolution in-tree: the two tests now probe for
`--no-lazy-fetch` support and skip rather than fail, and `README.md` states
Git 2.45+. Product behaviour was deliberately left unchanged, since the spec
blesses the fallback.

Residual consequence, unchanged: on Git older than 2.45 the full scan does not
exclude `.gitignore` paths, so excluded files (commonly `.env` and credentials)
are indexed. Fixing that means teaching the native walker to honour gitignore,
which is a spec amendment rather than a bug fix.

### Go 1.24+ for the benchmark fixtures

Every fixture's `go.mod` declares `go 1.24`, but `benchmark/README.md` said only
"Go and gofmt must be on PATH". With Go 1.22.2 installed, each `go` invocation
attempted a toolchain download and failed, which surfaced as an apparent agent
failure rather than an unusable environment. Installing Go 1.27.0 (matching CI)
resolved it; `benchmark/README.md` now states the requirement.

## Caveats

- WSL 2, not native Linux (see above).
- NVML under WSL does not report GPU utilisation or active compute processes, so
  no GPU-utilisation metric is recorded. The final `environment.json` does record
  the GPU name/driver/VRAM; an earlier run recorded `gpu: null`.
- One suite run at one context size. No performance claim is made here; the
  token-reduction comparison in `benchmark/results/2026-08-28-normalized/` is a
  separate Windows measurement and must not be combined with these numbers.
- `prefix` fails on loop detection, as it does on Windows. Not investigated
  further.
