# Security model

Forge is an experimental agent runtime. **It is not an operating-system sandbox.**

## Default policy

Reads through Forge tools are restricted to an explicitly selected workspace.
Absolute paths, traversal, reserved Windows paths, `.git`, `.forge`, symlinks and
Windows reparse points are rejected. Patches additionally reject hard-linked
targets. Internal index files are checked before SQLite opens them.

Repository writes require `--allow-write` or an embedding application's policy
callback. Arbitrary processes require `--allow-exec` or that callback. A callback
replaces the CLI flag decision, so embedders can permit or deny each operation.
There is no hidden fallback that runs denied commands through another tool.

Automatic validation and `forge validate` use this same PROCESS decision for every
stage. The Go compile check can execute `init` and `TestMain`; formatting checks
and tests are not implicitly trusted. TOML configuration never grants writes or
execution. An explicit `tools.shell.network=false` prevents CLI execution even
with `--allow-exec`, since network isolation is not implemented.

`git_diff` and `git_status` require PROCESS approval, just like `run_command`.
Git can execute configured clean/process filters while inspecting a worktree;
disabling external diff and textconv does not disable these filters. Approved
Git tools therefore run with the same host privileges as other approved code.

Session finalization does not launch Git, even with `--allow-exec`: a filter could
modify files after final validation. The `patch_snapshot` event records
`not_collected` with reason `explicit_git_diff_required`. Use the explicitly
approved `git_diff` tool during the agent loop; its stdout/stderr and tool result
are recorded, and the command invalidates prior validation like other processes.
A native edit journal for automatic diff artifacts remains future work.

Repository indexing still uses a fixed `git ls-files` invocation, with fsmonitor
disabled and `--no-lazy-fetch`, to enumerate eligible paths. Git 2.45 or newer is
needed for that restriction; unsupported Git falls back to native enumeration on
a full scan, never an unrestricted Git retry. This does not request worktree
content conversion, but it is not an OS isolation boundary or a general promise
that untrusted Git metadata is safe. Git is resolved from absolute PATH entries
outside the workspace. Treat repository Git configuration and the installed
executable as trusted until the remaining implicit-process policy is closed.

## What process approval grants

`--allow-exec` authorizes code with your user account's privileges. Even
`go test`, `pytest`, `make`, and build scripts can read outside the workspace,
modify files, spawn processes, or access the network. A small inherited
environment reduces accidental credential exposure but does not prevent a
process reading credentials from disk. Command approval does not imply isolation.
Windows child processes inherit only their three standard handles. POSIX children
close other descriptors before execution; environment allocation happens before
forking so model worker threads cannot leave allocator locks held in the child.

Use a container, VM, dedicated account, or external sandbox for untrusted repos.
Never run Forge elevated. Network blocking, seccomp/Landlock, namespaces, and
platform filesystem isolation are **not implemented**. The CLI does not claim
that `--allow-exec` restricts commands to a repository.

Timeouts and process groups/Windows Job Objects bound ordinary subprocess trees.
They do not prevent a deliberately adversarial POSIX child from escaping its
process group. CPU and memory quotas are future work.

## Filesystem races and portability

File validation checks each existing path component before file access, and
patching rechecks source content before atomic replacement. This protects normal
use, accidental escapes, and static symlinks. It is **not race-free against a
hostile process concurrently replacing directories or links**. Run in an isolated
checkout with no adversarial concurrent writers. Multi-file patches are not
transactional. Native Windows paths currently use narrow OS APIs; complete
Unicode-path portability is not promised.

## Logs and data

`.forge` is ignored in this repository. Session prompts, source snippets, tool
results, and command output can contain secrets. Review them before uploading.
No telemetry, cloud inference, model download, or automatic sharing is built into
the runtime. Review repository files and use a clean environment before indexing
sensitive source. A model can still be influenced by malicious repository text;
prompt instructions are not a security boundary.

## Dependencies

Build dependencies are pinned. Model and CUDA downloads used for development
are outside the tracked source and verified against publisher SHA-256 digests.
The library supports untrusted JSON only within its configured limits; it relies
on native upstream libraries for GGUF and source parsing. Do not load unknown
binary models or replace dependency sources without reviewing their origin.
