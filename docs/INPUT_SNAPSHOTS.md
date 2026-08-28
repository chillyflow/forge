# Workspace input snapshots

The internal `src/core/input_snapshot.h` API records file inputs independently
of Forge's language index. Go tests can consume text fixtures, CSV, binary files,
hidden files, or vendor data that the source index does not contain. An unchanged
index generation alone therefore cannot establish unchanged verification inputs.

```c
fg_input_snapshot *before = fg_input_snapshot_take(
    root, 100000, UINT64_C(2) * 1024 * 1024 * 1024,
    cancelled, userdata, absolute_deadline, &error);
/* Run authorized checks only if before is non-NULL. */
fg_input_snapshot *after = fg_input_snapshot_take(
    root, 100000, UINT64_C(2) * 1024 * 1024 * 1024,
    cancelled, userdata, absolute_deadline, &error);
bool unchanged = fg_input_snapshot_equal(before, after);
fg_input_snapshot_destroy(before);
fg_input_snapshot_destroy(after);
```

A caller must treat either failed snapshot as missing evidence. Two `NULL`
pointers are **not equal**. `fg_input_snapshot_hash(NULL)` returns zero; a hash
alone must not replace `fg_input_snapshot_equal` or the success checks.

## What is recorded

Every regular file is included regardless of extension, size, hidden status,
Git tracking, `.gitignore`, language support, or directory names such as `vendor`
and `testdata`. Each record contains a relative path, exact byte length, and a
streamed 64-bit FNV-1a content hash. NUL and invalid UTF-8 bytes inside files are
ordinary bytes. Files are never decoded as text.

Records are sorted by relative path bytes. Paths use `/` between directory
components. Windows names are obtained through Unicode APIs and encoded as
UTF-8 without code-page substitution; POSIX filename bytes are retained as-is.
The aggregate hash uses explicit length framing and little-endian integers,
and does not contain the absolute workspace path or filesystem traversal order.
Identical file trees in different directories consequently compare equal.
Equality checks every path, length, and content hash in addition to the aggregate.
Creation, deletion, renaming, size changes, and same-length content edits change
the evidence. Timestamps, ownership, permissions, inode identities, and empty
directories are not part of its persisted identity. Hard-linked regular files
are included once per pathname and consume the byte budget once per pathname.

### Exact exclusions

Only actual directories named `.git` and `.forge` **immediately under the
workspace root** are excluded with their entire contents. Matching is exact on
POSIX and case-insensitive on Windows. Their files do not consume the content
or file-count budget. No excluded directory is traversed.

Nested `testdata/.forge/` or `vendor/.git/` directories remain inputs. A root
`.git` **file**, such as a worktree pointer, is included. `.gitignore` and files
it ignores are included. A symlink/reparse point named `.git` or `.forge` is
rejected before exclusion; links within an already excluded metadata subtree
are never visited or followed.

## Bounds and failure handling

Both caller limits must be nonzero. `max_files` limits regular-file records;
`max_bytes` limits the total bytes of file contents read, including repeated
content under distinct hard-link paths. Individual files may exceed a read
buffer: hashing streams in 64 KiB blocks. Allocations consist of bounded path
records, one streaming buffer, and directory traversal state, not whole files.

The hard traversal depth is 64 directories below the workspace root. Root and
relative paths, including separators and native enumeration suffixes, must fit
within the implementation's 4096-byte path buffers. Windows also bounds held
workspace ancestor handles to 65. An oversized file, too many files, excessive
depth/path, allocation failure, unreadable entry, enumeration error, or detected
concurrent change fails the entire operation. Previously accumulated records
and native handles are released; no partial snapshot is returned as valid.

| Condition | Error |
| --- | --- |
| Invalid arguments or zero limits | `FORGE_ERR_ARGUMENT` |
| Content/file/path/depth bound or deadline | `FORGE_ERR_LIMIT` |
| Cancellation callback returns true | `FORGE_ERR_CANCELLED` |
| Encountered link, reparse point, or special file | `FORGE_ERR_POLICY` |
| File/directory changed during its inspection | `FORGE_ERR_CONFLICT` |
| Read, open, enumeration, or close error | `FORGE_ERR_IO` |
| Allocation failure | `FORGE_ERR_MEMORY` |

An absolute deadline uses the same monotonic millisecond clock as `fg_now_ms`.
Zero disables that deadline. Cancellation and the deadline are checked before
filesystem work, while traversing entries, between streaming reads, and before
returning the sorted evidence. Checks are cooperative: a blocked kernel I/O
operation cannot be preempted by this synchronous API.

POSIX traversal opens each directory/file relative to an already-open directory
descriptor with `O_NOFOLLOW`; regular-file opens also use `O_NONBLOCK` so a
replacement FIFO cannot hang before type checking. Supplied workspace ancestors
are checked without resolving away symlinks. File identity, size, and change
timestamps are checked around reads; directories are checked around enumeration.

Windows uses Unicode native file APIs, extended-length absolute paths, explicit
reparse-point inspection, and handles without write/delete sharing while reading
files and traversing directories. Workspace ancestors remain held while full
native paths are used. Invalid Unicode names and device namespace roots fail
explicitly. Only normal file data streams are hashed; alternate data streams
and external operating-system inputs are outside this API's file-input model.

## Scope of the evidence

This component supplies input integrity evidence. It does not execute commands,
authorize tests, provide a filesystem sandbox, or establish that tests passed.
The caller decides when snapshots surround applicable validation, how to report
their failures, and whether another verification attempt is permitted.

FNV-1a is not cryptographic. The snapshot does not defend against deliberate hash
collisions, hostile kernel/filesystem behavior, or an adversary rewriting files
after they have already been read. The scan is not an atomic filesystem snapshot.
Detected races fail, but callers must arrange appropriate ownership/isolation
when simultaneous untrusted writers are possible. Inputs outside the workspace,
network responses, environment, toolchains, clocks, and excluded metadata are
not represented by this evidence.

## Tests

`tests/unit/test_snapshot.c` uses temporary fixtures and no subprocess, model,
GPU, or language index. It covers non-source and binary changes, streaming reads,
creation/deletion/rename, Unicode paths, stable ordering across roots, exact
metadata exclusions, file/content/depth/path bounds, cancellation including a
transient callback, deadlines, recovery after failure, invalid arguments, and
read errors. POSIX also checks FIFO denial. Windows additionally creates a native
junction and verifies reparse-point rejection without administrative privileges
or a subprocess. Symbolic-link checks explicitly report a skip when Windows does
not grant symlink-creation permission; the junction check still runs separately.
