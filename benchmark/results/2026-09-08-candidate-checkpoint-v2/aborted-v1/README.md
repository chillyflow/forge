# Aborted checkpoint integration attempt

This attempted 36-run diagnostic is invalid and incomplete. The first candidate
run failed before inference: the native template wrapper's schema validator
allowed only the original full/minimal registries and rejected the new candidate
registry. Prompt counting surfaced this as a context-limit error. Scripted agent
tests bypass that wrapper. The first candidate generated zero tokens; the next
minimal run was interrupted when the controller was stopped. Neither is retried
in this directory; 34 scheduled cells were never started.

The source, model, runtime and task identities were unchanged at shutdown, as
recorded in `audit.json`. `audit-initial-environment-mismatch.json` retains an
initial audit run without the original Go PATH; repeating the identity check
with the original environment resolved that audit discrepancy. It did not run
the model or modify any primary outcome.

Retain the protocol, source snapshot, failed output and interrupted cell here.
The fix adds the exact candidate registry and validation-only schema to the
native wrapper, with template-render/parser regressions. A new version must be
frozen and checked with real inference before starting another full matrix.
