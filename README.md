# Forge

A native local coding-agent runtime, written in C17. Forge combines direct
llama.cpp inference with stable context ordering, reusable KV prefixes,
constrained tool calls, and compact repository intelligence.

**Status: implementation in progress; no performance claims or release yet.**

The runtime is C; llama.cpp and its GPU backends contain C++ and other native
code. Model weights are never downloaded automatically.

See the forthcoming build guide, security model, and milestone status in `docs/`.
