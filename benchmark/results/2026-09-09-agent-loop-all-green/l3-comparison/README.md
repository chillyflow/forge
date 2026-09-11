# Deferred L3 diagnostic driver

`driver.py` is a results-side diagnostic, outside the candidate source inventory.
Only its `run` command starts model work. No comparison has been frozen or run by
this implementation task. All paths below are relative to the repository root.

The minimum comparison references all 18 completed G1 `loop-repair` executions
from one supplied candidate and executes 18 new `loop-repair-best-of-2` roots.
The fixed six-task, seed-42, three-repetition schedule is inherited from the
unchanged acceptance contract. There are 18 paired observations and 36 planned B
children. All children share the original root's 32-action, 32768-generated-token,
262144-input-token and 600-second limits. No acceptance gate is produced.

The supplied baseline must have passed G0 and have all 18 auditable G1 outcomes,
including failures. Source, local runtime, model and fixtures must still match
that baseline. Freeze requires the corrected verifier's metadata
`python_cache_policy="fresh-external-prefix-no-write"` for Python and
`"not-applicable"` otherwise, plus
`terminal_retention_policy="complete-except-git-forge"`. Both
`pre-verification-workspace` and `terminal-workspace` must retain every input byte,
including caches and nested `.git`/`.forge`; only root `.git`/`.forge` directories are excluded; regular files with those names remain inputs.
The frozen candidate-02's legacy retention is unsupported for this future
comparison. This restriction does not alter its recorded A outcomes.

After the repaired single-candidate measurement completes and model/build work is
idle, substitute that baseline's directory and a **new, absent** output directory:

```powershell
$driver = 'benchmark/results/2026-09-09-agent-loop-all-green/l3-comparison/driver.py'
$baseline = 'benchmark/results/2026-09-09-agent-loop-all-green/candidate-03'
$comparison = 'benchmark/results/2026-09-09-agent-loop-all-green/l3-comparison/measurement-03'
python -B $driver freeze --baseline $baseline --output $comparison
python -B $driver run --directory $comparison
python -B $driver report --directory $comparison
python -B $driver close --directory $comparison
```

`candidate-03` is an illustrative future path, not an assertion that it exists or
is ready. Invoke the original `driver.py`; the copied `frozen-driver.py` is retained
evidence. Freeze hashes the driver, protocol, both arm configurations, exact A
execution references, candidate source/runtime/model and full schedule. Changes
after freeze are rejected. The driver uses a fresh external Python import-cache
prefix; `-B` alone would not bind executed imports to fresh source.

Each B root retains its command/start/outcome, complete harness tree, both child
workspaces and nested prompt/session/final streams, before/after edit artifacts,
selection-validation output, and pre/post-verifier workspaces. Secondary root and
child verification uses copies with Python bytecode removed and the frozen common
verifier. Those diagnostic checks never replace a missing primary completion.
Original cache bytes remain in the raw evidence archive.

The audit enforces independent child seeds, remaining-budget allocations, summed
root consumption, child/selection deadlines, complete native finals, real-workspace
selection and restoration, deterministic passing-first winner choice, protected
inputs, full independent test counts and unchanged verifier inputs. A valid failed
search may omit `done`; a successful root or child must have matching final and
end-stage evidence. Missing children, altered evidence, stale verifier policy or
borrowed completions remain non-passing and make the comparison incomplete.

Every root has an inventory-verified archive. `close` seals top-level reports and
all raw/per-root evidence, records a final identity audit, and indexes externally
referenced A/source archives by their original hashes. Closure also preserves an
incomplete diagnostic and returns nonzero. Closed diagnostics cannot be extended.
An interrupted start cannot be overwritten or silently retried; missing records
remain visible in the fixed denominator. Keep the supplied baseline and its
external artifacts available for later audits.

`report` and `run` return zero only for a fully auditable 18-pair diagnostic; that
means evidence completeness, not model perfection or acceptance. A qualifying
development signal additionally requires preserving every A pass and gaining at
least one B pass. The output always keeps best-of-N experimental and requires
further preregistered evaluation. A followed by B is a sequential-block comparison,
so the report makes no causal latency or broad superiority claim.

Run the model-free checks with:

```powershell
python -B benchmark/results/2026-09-09-agent-loop-all-green/l3-comparison/selftest.py -v
```

The tests audit all six retained historical best-of-two roots (12 children) and
mutate only temporary copies to check rejection of missing children/prompts,
protected edits, wrong seeds, overspending, completion borrowing, incomplete
validation, changed archives, stale policy, omitted cache bytes, duplicate roots,
borrowed execution IDs and changed source/runtime identity. These checks establish
driver behavior and retained-data compatibility; they are not new model outcomes.
