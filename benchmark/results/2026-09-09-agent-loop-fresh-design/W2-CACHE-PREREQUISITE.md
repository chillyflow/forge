# W2 blocking prerequisite: the verification bytecode-cache hole

Investigated September 9 2026 while W0 was executing. No code changed. The
findings below are demonstrated, not argued.

## The hole is real, and fresh workspaces do not close it

I had initially guessed that per-run fresh temp workspaces
(`benchmark/run.py:179`) already prevented this. **That guess is wrong**, and the
reason matters: fresh workspaces prevent *cross-run* carry-over only. The stale
bytecode is created *within the same run*, before verification:

1. The agent is explicitly told to run `python -m unittest discover -v`
   (`src/tools/tools.c:206`, and again at `src/tools/tools.c:1576`).
2. Its child process inherits no bytecode policy: the environment allowlist in
   `src/core/process.c:296` is `PATH, SystemRoot, TEMP, TMP, USERPROFILE,
   LOCALAPPDATA, APPDATA` on Windows (`:430` on POSIX) — no
   `PYTHONDONTWRITEBYTECODE`, no `PYTHONPYCACHEPREFIX`. So it writes
   `__pycache__` into the workspace.
3. Verification then runs in that same root (`benchmark/common.py:446`), and the
   host's own stages run there too (`src/repo/validation.c:665`, `:699`).

242 retained `.pyc` files under `benchmark/results/**/failed-workspace/**/`
confirm step 2 empirically.

Neither integrity check can see it. The host's snapshot excludes `.git`/`.forge`
only at the root (`src/core/input_snapshot.c:80`), so a `.pyc` present in both
the before and after snapshot reads as *stable*; the benchmark deliberately
drops `__pycache__` from its maps (`benchmark/run.py:212`, `:220`), so
`verification_inputs_unchanged` is blind to it by construction. A stable hash
proves which bytes were present, not which bytes executed.

An end-to-end demonstration against the frozen binary already exists in-tree at
`candidate-02/diagnosis/verification-cache-audit.md`.

## Demonstrated: `-B` controls writing, never loading

Run on this machine against CPython 3.11.9, with a module whose source was
changed while size and mtime were preserved byte-for-byte:

| Invocation | Value loaded | Value in source |
| --- | --- | --- |
| `python -B` | **1** (stale bytecode) | 2 |
| `PYTHONDONTWRITEBYTECODE=1` | **1** (stale bytecode) | 2 |
| `python -B -X pycache_prefix=<fresh>` | 2 | 2 |

This is consistent with the interpreter source: `sys.dont_write_bytecode` is
consulted only in the write branch of `importlib._bootstrap_external`, after the
code object already exists, whereas `sys.pycache_prefix` is read inside
`cache_from_source`, which determines where bytecode is *read from*.

Consequence: every one of this tree's Python validation stages relies on `-B`
(`src/repo/validation.c:665`, `:667`, `:699`, `:703`) and therefore does not
close the hole. The syntax stage (`validation.c:615`) is genuinely immune — it
calls `compile()` on bytes without importing.

The self-reported guarantees at `src/repo/validation.c:846` and the limitations
string at `:925` are each literally true and both invite the wrong reading.

## Why `cache-fix.patch` is the wrong fix, quantified

The patch sets `sys.pycache_prefix` to a **fresh** temporary directory *and*
retains `-B`, so nothing is ever written into the prefix and the full cold
compile is paid by every command, inside an untimed 120 s cap
(`src/core/verification.c:365`, `benchmark/run.py:109`). Measured here for a
bare `import unittest`:

| Approach | First call | Steady state |
| --- | --- | --- |
| warm, no prefix | 91 ms | 91 ms |
| **fresh prefix + `-B`** (the patch) | **503 ms** | **503 ms, every command** |
| fresh prefix, writes allowed | 212 ms | **84 ms** (50 `.pyc` retained) |

A real discovery run imports far more than `unittest`, so the 5.5x is a floor,
not a ceiling. The patch also asserts in user-visible text that the bypass works
while its pytest half was never executed
(`cache-wrapper-checks.json`: `"pytest": "not run: package unavailable"`).

## The minimal correct fix

Do not disable writes; **redirect reads to a prefix that persists**. Append
`-X pycache_prefix=<dir under the workspace's .forge/>` to the existing argv at
`src/repo/validation.c:665`, `:667`, `:699`, `:703`, and drop `-B` from those
stages. Then:

- workspace `__pycache__` can never satisfy an import, because lookups resolve
  under the prefix instead — the load path is closed;
- the standard library compiles once per session and is reused, so steady-state
  cost is nil (84 ms versus a 91 ms warm baseline);
- `.forge` is already excluded from the input snapshot at the root
  (`src/core/input_snapshot.c:80`), so candidate input hashes are not perturbed;
- no wrapper script is needed, so the patch's import-ordering defect cannot recur;
- no change to `src/core/process.c`'s environment allowlist is needed.

For the independent verifier (`benchmark/common.py:446`), set
`PYTHONPYCACHEPREFIX` once per `run.py` invocation rather than per verification,
and gate on the verifier's interpreter (`task['verify'][0]`, matching
`benchmark/agent_loop_acceptance.py:282`) rather than on `task['language']` —
those are different fields, and the patch gates on the wrong one.

`benchmark/replay_loop_pressure.py:126` is a third verifier path that the patch
does not touch and that needs the same treatment.

**Not yet done, and required before shipping:** time a full `unittest discover`
and a pytest command under the prefix and assert a per-command budget well under
the 120 s cap. The numbers above are for `import unittest` alone.
