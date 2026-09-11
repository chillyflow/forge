# Candidate closure helper

Run this only after all 18 G1 outcomes and the final acceptance report have been
written, while the candidate source/runtime remain unchanged and no model/build
work is active. The helper refuses missing, duplicated, unscheduled or extra-gate
records, and compares the stored report to a fresh read-only report. It has not
been run against candidate-02 by the implementation task.

Proposed command from the repository root:

```powershell
python -B benchmark/results/2026-09-09-agent-loop-all-green/close_candidate.py `
  --candidate benchmark/results/2026-09-09-agent-loop-all-green/candidate-02 `
  --output benchmark/results/2026-09-09-agent-loop-all-green/candidate-02-closure `
  --through-gate G1 `
  --include docs/plans/agent-loop-all-green.md `
  --include benchmark/results/2026-09-09-agent-loop-all-green/README.md `
  --include benchmark/results/2026-09-09-agent-loop-all-green/starting-state
```

The output must be new and outside the original candidate directory. Add explicit
`--include` paths for any additional build/provenance logs stored outside the
candidate. The helper includes all files inside the candidate, plus named source,
runtime, G0, per-run archive/inventory and evidence references even if external.
Unreferenced external files are outside this declared scope.

The archive **includes frozen runtime bytes**, all retained G0 executables/DLLs,
source archives and diff, original logs, raw sessions, caches, hidden files,
preflights, original per-run ZIPs and the final report. ZIP entries are stored
without recompression and verified in streaming chunks. The existing model is
hashed before and after closure but never copied; any included GGUF/model path is
rejected. Source revision/diff, full source identity, inference runtime, G0 runtime,
model and candidate configuration must remain frozen. The final evidence hashes
and membership are rechecked after the last identity audit.

Original candidate files are never rewritten. Generated `closure-plan.json`, the
report copy and helper copy are included under `closure/`. Only the generated ZIP
and its own inventory are excluded. `candidate-evidence-inventory.json` acts as
the final receipt and records the archive hash, complete member hashes/sizes,
final identity and model/runtime inclusion policy.

Exit zero means the declared measured population was fully sealed and verified.
It does **not** mean the candidate passed: `closure_complete` and
`candidate_accepted` are distinct fields. Missing/unrun G2-G6 remain missing in the
unchanged report. A legacy archive preserves only originally retained bytes and
cannot recover cache bytes omitted by a legacy runner. The source/diff, binaries
and build/check logs are retained provenance, not a reproducible-build claim.

For a later failed candidate, choose its own directory and a new closure output.
If it measured through another gate, declare that gate with `--through-gate`; all
scheduled populations through that gate must be present. Do not overwrite a
partial closure attempt or alter original evidence to satisfy a check.

Model-free tests:

```powershell
python -B benchmark/results/2026-09-09-agent-loop-all-green/test_close_candidate.py -v
```

The tests use temporary synthetic evidence, check the fixed 18-outcome prerequisite,
reject duplicate/borrowed/extra outcomes and archive mutations, retain runtime,
hidden/cache/nested session bytes, and run a full synthetic closure with failed
acceptance and an external model. No actual candidate closure or model run occurs.
