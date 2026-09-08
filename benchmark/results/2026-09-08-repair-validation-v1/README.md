# Native repair-validation continuation — September 8, 2026

The interrupted completion-guidance fix is verified. This development run
passed **7/10**, compared with **6/10** for `stability-native-v1`. The
paraphrased retraction case recovered, and all six previous passes remained
passes. Three retraction repairs still exhausted the 16-turn limit. No crash,
timeout, or protected-file mutation occurred.

This is one repetition of ten variants from two task families, using previously
examined development inputs. It does not establish generalization, statistical
superiority, a latency improvement, or a promotion-gate result.

## Completed change

The source changes and regression tests were already present when this session
resumed. Verification had been interrupted. This continuation reviewed the
change, rebuilt it, ran the full suite and real-model probe, and completed the
development matrix without changing the measured source.

- `src/core/agent.c` keeps repair tools available on the penultimate turn even
  after a successful subprocess. Only the last turn exposes the final-only
  registry.
- `src/tools/tools.c` explains that a zero exit code does not establish test
  execution, and directs Python unittest work through the test runner.
- `tests/integration/test_cli.py` covers a definition-only unittest file that
  exits zero with empty output, followed by a real runner that executes and
  reports the intentionally failing test. Separate coverage verifies that
  arbitrary successful output does not mark validation as verified, and that
  the last turn still exposes only `final`.

Thinking defaults, native forcing, runtime libraries, model settings, turn
limits, fixture-hint removals, and protected inputs were retained.

## Verification

| Check | Result |
| --- | --- |
| GPU Release build | Passed |
| Full CTest suite with Go available | 24 passed; opt-in `checkpoint_model` skipped; 76.97 s |
| Qwen3-Coder GPU chat-template probe | Passed |
| Fixture preflight | All 10 broken baselines fail and all 10 supplied oracles pass |
| Development matrix | 7/10; all scheduled runs retained |
| Protected files | Unchanged in all 10 runs |
| Fixture identity and matched configuration | Passed comparison checks |
| Source, executable, and runtime library hashes after execution | Unchanged |
| Python syntax, scoped C formatting, whitespace checks | Passed |

The real-model probe covers forced opening, cancellation, generation-budget
exhaustion, recovery, cached tool-call equivalence, and generation without a
callback. It does not require byte-identical cached free-form prose.

| Family / variant | Baseline | Candidate |
| --- | --- | --- |
| Retractions / original | Fail | Fail |
| Retractions / renamed | Fail | Fail |
| Retractions / paraphrased | Fail | Pass |
| Retractions / distractor | Fail | Fail |
| Retractions / contrast | Pass | Pass |
| Window / original | Pass | Pass |
| Window / renamed | Pass | Pass |
| Window / paraphrased | Pass | Pass |
| Window / distractor | Pass | Pass |
| Window / contrast | Pass | Pass |

## What the traces establish

All five retraction runs first attempted direct execution of their definition-only
test file, then switched to `python -m unittest ... -v`. The generic guidance
therefore corrected runner selection after the initial mistake, but did not
prevent that first wasted command. The paraphrased variant completed on turn 15.

The other three failures reached turn 16 with incorrect repairs and one loop
warning each. The original variant returned to two previously applied source
hashes, including a return through `apply_hunk` after a repeated `apply_patch`
was rejected. The distractor and renamed variants attempted byte-identical
replacements. Actual failing unittest output was available during these loops.

The next bounded repair-quality investigation is recovery from ineffective
edits: preserve the failing evidence and recognize returns to failed source
states across edit tools. Any proposed fix needs coverage that still permits
legitimate reverts and changed validation inputs. These traces do not justify
changing thinking defaults, increasing budgets, or restoring fixture-specific
repair hints.

## Identity and evidence

- Baseline executable SHA-256:
  `5c5242b54af26811526ccc4f19cdd4c947d623e39064abbf635166f1fb1f8605`.
- Candidate executable SHA-256:
  `c09ead6fee7ef141fdd0699a3db4c1e38d83b54631849ceeee8452ddb5d8a27b`.
- Candidate source is the uncommitted tree on `1d1906c`, recorded in
  [source-identity.json](source-identity.json) and [source.patch](source.patch).
  The generalization generator is also retained alongside these records.
- Both runs used Qwen3-Coder-30B-A3B-Instruct-Q4_K_M, GPU layers `-1`, native
  protocol, embedded template, context 16384, output reserve 2048, temperature
  0, seed 42, 16 turns, one repetition, cold lifecycle, randomized order seed
  20260831, and a 600-second task limit.
- The GPU/driver (`RTX 5090 Laptop`, `616.56`), platform, Python, Go, Node,
  model hash, and all adjacent runtime DLL hashes matched the stable baseline.

See [comparison.json](comparison.json), [results.json](results.json),
[repair-analysis.json](repair-analysis.json), [environment.json](environment.json),
and [preflight.json](preflight.json). The `tasks/` directory retains all inputs;
each run directory retains its trace, session, verification output and diff,
plus the failed workspace when applicable. Local `ctest.log` and
`model-probe.log` retain verification output; `.log` files are Git-ignored.

The matrix runner and comparison command both exited 1 because three candidate
cases failed. The comparison report was successfully generated; its false
`all_candidate_checks_passed` value remains visible.

No commit, push, untouched holdout, or promotion claim was made.

## Reproduce the development comparison

Use the Go path and CMake `PYTHONPATH` setup documented in `AGENTS.md`. Select a
new output directory for another execution; do not overwrite these results.

```powershell
python benchmark/preflight.py --task-dir benchmark/results/2026-09-08-repair-validation-v1/tasks --suite generalization-v1 --output NEW_OUTPUT/preflight.json
python benchmark/run.py --forge build-gpu/Release/forge.exe --model C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf --task-dir benchmark/results/2026-09-08-repair-validation-v1/tasks --suite generalization-v1 --variants optimized --prompt-protocol native --gpu-layers=-1 --context 16384 --output-reserve 2048 --temperature 0 --seed 42 --max-turns 16 --repetitions 1 --order-seed 20260831 --timeout 600 --output NEW_OUTPUT
python benchmark/generalization.py compare --task-dir benchmark/results/2026-09-08-repair-validation-v1/tasks --baseline benchmark/results/stability-native-v1 --candidate NEW_OUTPUT --output NEW_OUTPUT/comparison.json
```
