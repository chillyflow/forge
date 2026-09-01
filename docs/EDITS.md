# Native edit evidence

Every successful `apply_patch` or `apply_hunk` preparation records exact content
evidence in the private session directory. Recording it launches no process and
needs only the normal WRITE approval for the requested edit. It does not grant
permission to run Git or any other executable. `read_file` emits a SHA-256 of the
entire current file; `apply_hunk` requires that anchor and replaces only its
inclusive 1-based line range.

For tool call `N`, `tool/NNNNNN.before` and `.after` contain the exact old and
proposed bytes. `tool/NNNNNN.patch` is a full-file unified text diff, and
`tool/NNNNNN.edit.json` identifies the path, byte counts, prior existence,
artifact names, and `prepared` state. Paths in the diff use Git's octal byte
escaping; text retains UTF-8, CRLF, and missing final newlines. A new empty file
has a mode-only new-file diff. These diffs are deliberately not minimal hunks.

## Ordering and failure

The tool stages the proposed file, writes the four exclusive artifacts, and
emits `edit_prepared` before replacing the target. For a Go path, the staged
bytes are parsed in-process first; syntax errors produce an `aborted` outcome
and leave the target untouched. It then rechecks the original bytes, path
policy, cancellation, and deadline. Conflicts or cancellation abort
replacement. After that decision, it writes `tool/NNNNNN.edit-result.json` and
emits `edit_result` with `applied` or `aborted` and the tool status.

The content recheck is not an operating-system compare-and-swap. An unrelated
process that writes the same path in the narrow interval between the final
recheck and atomic replacement can still lose its write; callers should treat a
live concurrently edited workspace as requiring external coordination.

**Preparation alone is not proof that an edit was applied.** Interrupted or
incomplete outcomes require inspecting the target and the recorded intent.
Outcome recording is attempted even after cancellation. If it fails, the agent
stops with an error and cannot accept another final response. If replacement
already occurred, the tool still reports the mutation to host change tracking.
An aborted edit does not count as an agent mutation.
Failure to emit the preparation event also stops the agent: an untouched target
does not make a partially written session event stream safe to continue.

Artifacts are created exclusively and never replace existing evidence. A
preparation failure leaves the target untouched; partial artifacts remain for
inspection. Denied edits, no-ops, stale hunk anchors, and conflicts detected
before preparation do not produce prepared-edit records.

## Bounds and limits

Each input is limited to 16 MiB of NUL-free UTF-8 text, in addition to the tool's
configured file limit. Session edit artifacts have a 256 MiB reservation budget,
including a reserved outcome allowance. Failed/partial attempts retain their
reservations. These bounds do not limit all session files or whole-process RSS.

The journal covers individual agent text edits, not arbitrary command changes,
Git HEAD, permissions, an aggregate final diff, or semantic impact. It is not a
crash-atomic filesystem transaction or an OS sandbox. Existing filesystem
check/replace race limitations and Windows narrow-path API limitations still
apply. See [security limits](SECURITY.md). Session evidence contains source
contents and must be reviewed before sharing.

The automatic aggregate `patch.diff` remains unimplemented. Explicit approved
`git_diff` still follows PROCESS authorization and normal fresh validation;
finalization never launches Git.

## Verification

The existing core and agent-change suites cover diff encoding, empty files,
CRLF, missing final newlines, invalid text, byte caps, reservation retention,
permission denial, no-ops, conflicts, cancellation, concurrent source edits,
preparation failures, and outcome-artifact/event failures. Scripted-agent tests
exercise the real tools, index, context, state, and journal with a watcher
double; they are not model or native-notification measurements.

The CLI integration suite also covers staged Go syntax rejection, successful
narrow line replacement, valid and invalid Go escape handling, and stale
SHA-256 hunk rejection.

The CLI integration suite independently applies emitted patches using an
explicitly authorized Git process in a fresh private fixture, then compares the
result bytes. This covers CRLF and space-containing paths, missing newlines,
truncation to empty, new empty files, and UTF-8 content. Pure unit tests cover
octal UTF-8 path encoding; this does not establish full Windows Unicode-path
portability.
