# Frozen acceptance contract

The schedule was frozen at `2026-09-09T19:45:56.133396+00:00`, before runtime
implementation began. Its canonical SHA-256 is
`c04b088b0183d5ce68c0db6e4e6ea56d33232cf4832e80fa676cd63b684577b8`.
[contract.json](contract.json) contains every scheduled run, all settings, exact
membership, and hashes for 57 manifest paths, materialized fixture contents and
protected files. Fixture preparation uses the existing UTF-8/LF/language-format
protocol, including Go formatting. No task, fixture, prompt or oracle was edited.

The coordinator resolved the plan's profile mapping before the freeze:

| Population | Development | Confirmation | Profile |
| --- | ---: | ---: | --- |
| G1 current failures | 18 | 36 | 32 actions, temperature 0.6 |
| G2 variations | 30 | 30 | 16 actions, temperature 0 |
| G3 regressions | 12 | 12 | 16 actions, temperature 0 |
| G3 invariants | 60 | 60 | 16 actions, temperature 0 |
| G4 full development | 87 | 87 | 16 actions, temperature 0 |
| G5 examined former holdout | 36 | 36 | 16 actions, temperature 0 |
| Scheduled coding outcomes | 243 | 261 | Separate profiles |

G1 development uses three cold repetitions at base seed 42. G6 repeats G1 at
42 three times, then once each at 43, 44 and 45. Deterministic populations retain
three repetitions at seed 42. Order seed is 20260831 except G5, whose original
holdout protocol records 20260902. Task order is frozen explicitly in the JSON.
All profiles retain native embedded templating and thought history, 16384 context,
2048 output reserve, 32768 generated tokens, 262144 cumulative input tokens,
600 seconds of agent time and 120 seconds of independent verification. Startup
and end-to-end times remain separately available in the raw harness records.

The historical regression and invariant membership comes from the retained
`2026-09-08-repair-recovery/v3-gates/development-protocol.json`, cross-checked
against `2026-09-02-tranche2-repair/final-development/development-gates.json`.
It was not reconstructed by selecting favorable outcomes. Historical implicit
token and timeout limits were checked against revision `5222616`'s CLI and
benchmark runner. Protocol, source-snapshot, baseline and archive references are
retained with their hashes under `provenance` in the contract.

G0 consists of the complete GPU CTest suite, the new deterministic reporter
regressions, the native-template model probe, and both checkpoint model probes.
The frozen original CTest inventory is [ctest-inventory.json](ctest-inventory.json).
Additional maintained CTest entries are allowed and must also pass. CTest's
opt-in checkpoint skip is allowed only because the two explicit model checks
remain mandatory. Unsupported direct probes do not pass.

[initial-report.json](initial-report.json) records all five G0 obligations and
all 504 coding outcomes as missing. It is the initial scheduling report, not a
claim about subsequent candidates. Each candidate writes its own report.

## Execute a candidate

The adapter requires the local GGUF and an already-built GPU Release runtime. It
does not build Forge, download models, change budgets or select a configuration
from outcomes. `freeze` requires a new output directory; source, runtime, model,
selected policy and all G0 executables/DLLs are identified before checks begin.
The selected arm is `loop-repair` with one candidate in every profile. The original
minimal and checkpoint-only controls remain retained as diagnostic evidence.

```powershell
$env:PYTHONPATH="$PWD/.tools"
$env:PATH="$PWD/.tools/go/bin;$env:PATH"
python benchmark/agent_loop_campaign.py freeze --contract benchmark/results/2026-09-09-agent-loop-all-green/acceptance/contract.json --candidate-id candidate-001 --forge build-gpu/Release/forge.exe --model C:/Users/flowc/models/forge/Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf --output benchmark/results/2026-09-09-agent-loop-all-green/candidate-001
python benchmark/agent_loop_campaign.py run-g0 --candidate-dir benchmark/results/2026-09-09-agent-loop-all-green/candidate-001
python benchmark/agent_loop_campaign.py run-gate --candidate-dir benchmark/results/2026-09-09-agent-loop-all-green/candidate-001 --gate G1
```

After G1 passes, invoke the same `run-gate` command successively for G2 through
G6. Each gate refuses execution until its predecessor is accepted. G6 records a
separate confirmation freeze before execution, tied to the same candidate hash.
Every scheduled repetition in a launched gate is retained. Failed runs are not
replaced. A partial/crashed gate retains its start markers and missing outcomes;
the adapter will not overwrite an attempted run. A new candidate requires a new
directory and a complete gate sequence.

`run-g0 --check-id NAME` can execute one of the five named obligations. The
`attach-g0` command accepts an external log, exit code, start/end UTC timestamps,
and (for CTest) its JUnit XML. It checks that the frozen binaries/source still
match and refuses duplicate evidence. Checks executed before the candidate freeze
cannot satisfy its G0. Prefer `run-g0` to capture execution directly.

The adapter's `run-g0` command returns zero only when G0 is accepted, or when its
explicitly selected individual check passed. A following coding gate independently
requires the entire G0 to be accepted. The strict standalone reporter returns 1
while any gate remains open:

```powershell
python benchmark/agent_loop_acceptance.py --contract CANDIDATE/contract.json --candidate CANDIDATE/candidate.json --outcomes CANDIDATE/outcomes.json --g0 CANDIDATE/g0.json --output CANDIDATE/acceptance-report.json
```

## Evidence envelope

Each outcome binds its scheduled run ID and unique execution ID to the frozen
candidate, source/runtime/model/configuration hashes before and after execution,
manifest and normalized fixture hashes, exact profile/seed, and timestamps.
The adapter invokes the existing runner with `--retain-terminal`, retains the
exact Forge/harness commands and environment, and archives all successful and
failed terminal inputs, prompts, outputs, session events, tool journals,
validation records and token/time measurements.

The archive has an exact file inventory. Every entry's size and hash are checked;
missing, altered or duplicate entries are rejected. Envelope artifact references
are also checked against the archive. Protected terminal bytes must match the
frozen fixture, and the runner's protected snapshots must match before and after
independent verification. Any changed verification input makes verification
incomplete. Passing independent output must show the same positive test count as
the fixture oracle preflight under the unchanged manifest command. Python test
skips, absent Go package verdicts and skipped Go tests are non-passing.

Primary success additionally requires a completed agent process and a changed
terminal implementation within all limits. A passing terminal workspace without
agent completion is reported separately. Missing measurements, malformed or
incomplete evidence, no-op terminal workspaces, budget excess, crashes and
timeouts remain non-passing; they are never removed from the denominator.
G3 also requires syntax-valid terminal workspaces and no terminal loop. A loop
warning followed by successful recovery is not itself classified as a terminal
loop. All warnings remain in the raw metrics.

The reporter rejects duplicate scheduled runs, reused execution identities,
outcomes from another candidate, changed runtime/configuration, altered protected
files, removed schedule entries and confirmation lacking a freeze. Overlapping
populations are not independent observations, and no cross-gate execution reuse
is scheduled. The final result cannot pool wins from different candidates.

## Deterministic verification

`python tests/unit/test_agent_loop_acceptance.py` passed 22 tests. The tests cover
the exact real frozen schedule, denominator preservation, changed/duplicate
evidence, candidate and runtime substitution, protected mutation, missing/partial
test execution, unsupported G0 probes, actual Python fixture preflight, JUnit
skip/failure preservation, exact adapter limits/seeds, verifier-input mutation,
full synthetic aggregation requiring a separate confirmation freeze, and G0
command exit codes for the complete gate versus an individual check.
Synthetic records are confined to temporary test directories and never populate
campaign outcomes. Python compilation and scoped whitespace checks passed.

No model run was executed by the acceptance-contract subtask. These deterministic
checks do not establish any coding gate result.
