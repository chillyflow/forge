# candidate-03 `elide-noop-03` — aborted G0, retained

This candidate is **retained and abandoned**. Its G0 was interrupted by a shell
environment defect before the third check could execute. No model run was ever
performed against it, and no coding gate was attempted.

It is kept immutable because the campaign tooling refuses to re-attempt a G0
check whose directory already exists — *"G0 already attempted; retain the result
and use a new candidate"* — and that rule is not weakened here just because the
interruption was mechanical rather than a result.

The identical source was re-frozen as `candidate-04-elide-noop`, whose
`identity` block matches this one exactly. That equality is the evidence that
nothing changed between the two freezes.

## What happened

`run-g0` completed `full-gpu-ctest` (passed) and
`new-deterministic-regressions` (passed), then raised `FileNotFoundError
[WinError 2]` launching `native-template-model`. Two checks are recorded in
`g0.json`; `g0/native-template-model/` holds a `started.json` and a zero-byte
`check.log` because the process never started.

## Cause

The contract runs the three model probes by a **bare forward-slash relative
path**, e.g. `build-gpu/Release/forge_chat_template_unit.exe`, resolved against
the current directory. The shell that invoked the campaign had
`NoDefaultCurrentDirectoryInExePath=1` set, which removes the current directory
from Windows' executable search, so `CreateProcess` could not resolve it.

Demonstrated directly, from the repository root:

| Executable form | Result |
| --- | --- |
| `build-gpu/Release/forge_chat_template_unit.exe` | `FileNotFoundError` |
| `./build-gpu/Release/forge_chat_template_unit.exe` | launches |
| `build-gpu\Release\forge_chat_template_unit.exe` | launches |
| absolute path | launches |

Removing the variable from the **child's** environment does not help:
`NeedCurrentDirectoryForExePathW` is consulted in the *calling* process, so the
variable must be cleared in the invoking shell. With it cleared, the bare
relative path launches and returns 0.

This is why candidates 01 and 02 recorded the same command successfully — they
were launched from a shell without that variable. The contract is not defective;
the invoking environment was.

## Operational note

Clear `NoDefaultCurrentDirectoryInExePath` before invoking `run-g0`, or the
three model probes cannot start. `benchmark/agent_loop_campaign.py`'s
`environment()` builds the child environment but cannot fix this, because the
variable is read by the parent.
