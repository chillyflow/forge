# Native filesystem change feed

`forge/watch.h` implements the notification component of design phase 30. It supplies bounded change hints for a host to hash, reparse, update dependencies, invalidate summaries, and advance repository generation. It does not perform those index operations itself. It reads directory metadata, never source contents.

The module has native Windows, Linux, and macOS implementations and no worker thread of its own. Ordinary polls consume native notifications and check the root identity; they do not enumerate the complete workspace. Creation inventories directories once. A newly observed directory inventories only its subtree.

## Ownership and API

The public functions are:

```c
forge_watch_limits forge_default_watch_limits(void);
forge_watch *forge_watch_create(const char *root,
    const forge_watch_limits *limits, forge_cancel_fn cancelled,
    void *user, uint64_t timeout_ms, forge_error *error);
char *forge_watch_poll(forge_watch *watch, uint64_t timeout_ms,
    forge_cancel_fn cancelled, void *user, forge_error *error);
void forge_watch_invalidate(forge_watch *watch);
void forge_watch_destroy(forge_watch *watch);
```

The watcher owns its native resources, directory records, and pending events. Each successful poll returns a separate compact JSON string, owned by the caller and released with `forge_free`. Destroy and invalidate accept `NULL`.

Limits are copied during creation; `NULL` selects defaults. Callback and user pointers are borrowed only for their call. Operations on one watcher must not be concurrent or reentrant. On macOS, creation, polling, and destruction must use the same thread.

Creation returns `NULL` on incomplete enrollment: invalid arguments, unavailable native support, unsafe root paths, directory/read errors, exhausted limits, cancellation, deadline expiry, or allocation failure. It never returns a partially enrolled watcher as complete. Unsupported operating systems return `FORGE_ERR_UNSUPPORTED`; Linux also reports this if the descriptor-based `/proc/self/fd` enrollment mechanism is unavailable.

## Initial ordering and recovery

Use this order:

1. Create the watcher, which starts native monitoring before directory enrollment finishes.
2. Consume the initial batch. `initial_scan_required` and `rescan_required` are true. If `reopen_required` is also true, reopen first and repeat this step.
3. Build the full index. Notifications continue queuing while the index runs.
4. Drain subsequent batches, applying their changes or honoring their rescan requests.
5. Continue draining while `more_pending` is true, subject to the host's own work/deadline budget.

The initial marker is cleared only after successful serialization and delivery. An empty first batch is not an established index baseline. The initial inventory is not an atomic filesystem snapshot.

`rescan_required=true` requires a full index. A first batch or successfully enrolled new subtree can request a rescan without reopening. This covers changes that happened inside a new directory before its inotify watch was enrolled, and gives all backends the same conservative contract.

`reopen_required=true` is sticky until destruction. Destroy the watcher, create its replacement, consume the replacement's initial batch, and then rebuild the index. Rebuilding first would leave a notification gap. The replacement can fail; callers must expose that failure or explicitly switch to a separate scan mode, never treat unavailable monitoring as a clean workspace.

Reopening is required after native overflow, local loss, directory rename/deletion, root replacement, enrollment failure, or host invalidation. Directory rename pairs can be split across native batches or moved outside the workspace. The implementation does not guess how to relabel an incomplete descendant-watch map.

`forge_watch_invalidate` lets the host signal loss of continuity, such as suspension or a change outside the host's monitoring assumptions. It uses the same sticky recovery machinery as native overflow. It does not simulate successful native delivery.

## JSON contract

A sample batch, formatted here for readability:

```json
{
  "schema_version": 1,
  "backend": "ReadDirectoryChangesW",
  "events": [
    {"path": "src/example.go", "flags": 3},
    {"path": "testdata/old.csv", "flags": 44}
  ],
  "rescan_required": false,
  "initial_scan_required": false,
  "reopen_required": false,
  "timed_out": false,
  "more_pending": false,
  "reason_flags": 0,
  "dropped_events": 0,
  "dropped_events_unknown": false,
  "overflow_count": 0,
  "directories": 12,
  "path_encoding": "utf-8"
}
```

All keys are present. `backend` is `ReadDirectoryChangesW`, `inotify`, or `FSEvents`. Events are sorted by the byte order of their UTF-8 relative paths. Paths use `/`; `.` identifies the root itself. A path appears at most once per batch, with all observed flags combined using bitwise OR.

| Event flag | Value | Meaning |
| --- | ---: | --- |
| `FORGE_WATCH_CREATED` | 1 | Creation or rename destination |
| `FORGE_WATCH_MODIFIED` | 2 | Content/write notification |
| `FORGE_WATCH_DELETED` | 4 | Deletion or rename source |
| `FORGE_WATCH_RENAMED_FROM` | 8 | Rename source, when supplied by the backend |
| `FORGE_WATCH_RENAMED_TO` | 16 | Rename destination, when supplied by the backend |
| `FORGE_WATCH_RENAMED` | 32 | Rename signal; macOS may supply no direction |
| `FORGE_WATCH_DIRECTORY` | 64 | Directory type from a native flag, inventory, or current attributes |
| `FORGE_WATCH_METADATA` | 128 | Attribute/security/metadata notification |
| `FORGE_WATCH_SYMLINK` | 256 | Link/reparse type when available from native flags or attributes |

Type flags are hints, not proof of the current object's type. In particular, absence of `SYMLINK` does not authorize following a path. Windows reports a modification without distinguishing content from metadata, so both flags are set. Windows/Linux preserve old-name and new-name signals without pretending to provide a transactionally matched rename pair. Creation followed by deletion retains both flags. This is a change feed, not a chronological filesystem audit log.

`dropped_events` is a saturating lifetime lower bound on native records omitted after local capacity failures. It is not a count of distinct files or filesystem operations. Native systems can coalesce events themselves. `dropped_events_unknown` makes uncountable loss explicit. `overflow_count` counts native overflow/invalid-batch signals received by this watcher. These counters are not reset by a successful poll.

`more_pending` means a native buffer has unread records or the per-call native work allowance was exhausted. In the latter case it is deliberately conservative: another poll may find an empty native queue. It does not replace rescan/reopen flags.

`reason_flags` combines the following public masks:

| Reason suffix (`FORGE_WATCH_RESCAN_…`) | Value | Recovery |
| --- | ---: | --- |
| `INITIAL` | 1 | Full initial index |
| `NATIVE_OVERFLOW` | 2 | Reopen, then full index |
| `EVENT_LIMIT` | 4 | Reopen, then full index |
| `BYTE_LIMIT` | 8 | Reopen, then full index |
| `PATH_LIMIT` | 16 | Reopen with adequate limits, or report failure |
| `DIRECTORY_LIMIT` | 32 | Reopen with adequate limits, or report failure |
| `DEPTH_LIMIT` | 64 | Reopen with adequate limits, or report failure |
| `IO` | 128 | Reopen after resolving the native/enrollment error |
| `ROOT_CHANGED` | 256 | Reopen the intended root, then full index |
| `PATH_ENCODING` | 512 | Resolve the unrepresentable/invalid path before reopening |
| `SCAN_LIMIT` | 1024 | Reopen with adequate enrollment allowance |
| `SUBTREE` | 2048 | Full index; reopen only if another flag requires it |
| `TOPOLOGY` | 4096 | Reopen, then full index |
| `CALLER` | 8192 | Reopen, then full index |
| `DEADLINE` | 16384 | Incomplete enrollment/native callback processing: reopen |
| `MEMORY` | 32768 | Allocation caused loss: reopen after memory is available |
| `NATIVE_WORK_LIMIT` | 65536 | Indivisible FSEvents batch exceeded its allowance: reopen |

Native loss is returned as valid JSON with `FORGE_OK`; the batch itself says the index must be rebuilt. Argument, cancellation, or allocation failures can instead return `NULL`. The caller must inspect both the return status and the JSON recovery flags.

## Bounds and deadlines

| Limit | Default | Accepted range |
| --- | ---: | --- |
| Distinct batch paths (`max_events`) | 1,024 | 1–65,536 |
| Serialized batch bytes (`max_bytes`) | 1 MiB | 1,024–16 MiB |
| Relative UTF-8 path bytes (`max_path_bytes`) | 4,095 | 1–4,095 |
| Directory inventory, including root (`max_directories`) | 4,096 | 1–65,536 |
| Relative directory depth (`max_depth`) | 64 | 0–64 |
| Enrollment entries per create/poll (`max_scan_entries`) | 1,000,000 | 1–10,000,000 |
| Raw notifications per poll (`max_native_events`) | 8,192 | 1–1,000,000 |

All absolute paths also fit 4,095 UTF-8 bytes. Windows root-plus-relative enrollment is additionally limited to 130 anchored components. Depth zero permits only the root, so encountering a real child directory fails enrollment instead of silently omitting it. Files, links, and special entries count toward enrollment-entry work, but their contents are not read. Excluded metadata directories count as entries; their descendants are not enumerated.

The serialized byte limit excludes the terminating NUL. Admission reserves 768 bytes for metadata plus `6 * path_bytes + 64` per retained event, covering worst-case JSON escaping. A batch may therefore request a rescan before an unescaped rendering would fill the limit. No path is silently truncated. Accepted paths remain available when a later path exceeds a limit.

Retained process memory is bounded by the configured directory/event counts and path lengths, plus a fixed 64 KiB native buffer on Windows/Linux. Hash-table capacities are bounded by those counts. Directory paths remain until destruction; a directory deletion requests reopening rather than accumulating unbounded retired-watch records. JSON serialization and one escaped path require additional bounded temporary storage. Windows directory-enrollment scratch is heap allocated per recursion level, avoiding exhaustion of the default thread stack at the supported depth. OS-owned notification queues and FSEvents callback storage are governed by the OS, not this module's JSON budget.

Creation timeout zero disables its cooperative deadline, while finite entry/directory/path bounds still apply. Poll timeout zero does not wait. A positive poll timeout includes waiting and processing; native waits use slices of at most 25 ms for cancellation checks. `timed_out` marks deadline exhaustion, or an empty nonblocking poll with no other pending work. OS filesystem calls, native stream startup, and cancellation completion are not forcibly interruptible and can overrun a cooperative deadline.

Normalized events survive cancellation and serialization failures. Windows/Linux also retain unprocessed native buffer tails across calls. FSEvents owns callback strings, so a callback tail that cannot be processed before a work/deadline/cancellation limit is explicitly counted as lost and requires reopening. A normal wait timeout does not invalidate the watcher; incomplete subtree enrollment does.

## Backends and path safety

**Windows:** one recursive `ReadDirectoryChangesW` request watches the verified root with a 64 KiB buffer and an I/O completion port. Only the root retains a native directory handle. Keeping handles on all descendants can prevent renaming their parents on Windows. During inventory, temporary ancestor handles reject reparse points and deny write/delete sharing; these handles are released after enrollment. Long-lived monitoring permits read/write/delete sharing. Destruction cancels outstanding I/O and drains its completion packets before freeing `OVERLAPPED` storage. Buffer overflow must be handled by enumerating/rebuilding, as documented by [Microsoft's notification API](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw).

**Linux:** one nonblocking inotify instance enrolls every real directory. Descriptor-relative `openat` with `O_NOFOLLOW` validates root components and descendants. `/proc/self/fd/<opened-directory>/.` enrolls the already-open inode without reopening an unchecked workspace path. Inotify descriptors map to owned relative paths. New subtrees are enrolled on demand; rename/deletion invalidates that map until reopening. Alias paths resolving to an already-watched inode fail explicitly. Native watch quotas still apply. Inotify is not inherently recursive and does not reliably report every form of memory-mapped modification; see the [Linux inotify manual](https://www.man7.org/linux/man-pages/man7/inotify.7.html).

**macOS:** an FSEvents file-event stream starts before the safe POSIX directory inventory. It uses a private run-loop mode serviced only by `forge_watch_poll`, so no caller-owned background thread is needed. Root-change, dropped-event, and recursive-rescan flags invalidate continuity. An FSEvents rename signal does not establish a source/destination pair. Link **CoreServices** and **CoreFoundation** when linking this module; it uses C framework APIs, not Objective-C. This follows Apple's [FSEvents lifecycle and loss handling](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html).

On all backends, explicitly supplied root symlinks/reparse points and linked ancestors are rejected. Use a real canonical root when the system's temporary-directory spelling itself passes through a symlink. Descendant links are entries, never directories to enroll or enumerate. Native Windows link notifications concern the linked entry, not a target outside the watched tree; see [Microsoft's symbolic-link notification behavior](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw). The tests exercise this with a real Windows junction.

Only actual directories named `.git` and `.forge` directly inside the root are excluded. Matching is case insensitive on Windows and exact on POSIX. Nested `package/.git` and `package/.forge` directories are included. A root `.git` regular file, as used by worktrees, is included. Hidden, vendor, binary, CSV, and test-data files are not otherwise filtered. Repository ignore policy belongs to the indexer, not the native feed.

The native root streams on Windows/macOS can still receive ignored metadata traffic before filtering. That traffic counts toward raw notification work and can contribute to OS queue overflow; loss is reported even if some preceding events were excluded. No backend claims that an empty event array proves unchanged inputs. Notifications are not cryptographic evidence, an OS sandbox, a stable content snapshot, or successful test validation. Keep the independent [input snapshot checks](INPUT_SNAPSHOTS.md) for validation integrity.

## Agent and CLI coordination

`forge watch --workspace ROOT --wall-ms 60000 --json` starts native coverage,
takes a full index and emits versioned batches with index/generation metadata.
It does not execute tools or load a model. Native coverage is required for this
command; it refuses incomplete enrollment instead of reporting a silent watch.

Agent runs use the private coordinator in `src/repo/monitor.c`. It consumes the
initial batch before indexing, drains after each full scan, and retains a dirty
flag when changes arrived during the scan. `followup_scan_required` and
`after_scan_events` expose that extra work. Ordinary file modifications can use
path-delta indexing. Directories, ignore-policy changes, missing paths,
deletion/rename flags, hardlinks, loss and pending batches require a full scan;
lost native continuity also requires reopening.

When native coverage is unavailable, agents fall back to bounded input snapshots
covering up to 100,000 files and 2 GiB of content, including binary/unindexed
fixtures. Snapshots before and after a full scan detect work that needs another
scan. Unsafe, unreadable or incomplete fallback inputs fail closed. The fallback
is more expensive and is recorded as `watch_warning`; it is not native watching.

The agent checks before and after inference and around final validation. An
observed change discards the generated action/final, invalidates source context
and requires fresh inspection. Discarded inference still consumes normal limits.
All source-bound context is conservatively invalidated, including after known
patches, because path spelling alone does not safely resolve filesystem aliases.
Known edits to unindexed files and launched commands with unknown effects also
invalidate the previous generation even if indexed source bytes did not change.
No empty poll or successful reindex replaces independent validation snapshots.
Notifications for the agent's own earlier edits can arrive late and trigger a
conservative extra generation. They are not silently ignored based on path/time
matching; distinguishing them safely needs stronger mutation identity tracking.

`index_ms` covers coordinator enrollment/poll/index work and direct post-tool
indexing. Validation has its own timing. Index-attempt/cache metrics come from
the repository handle; native event counts are observed signals, not unique
modified files, elapsed I/O time or peak memory.

## Verification status

`tests/unit/test_watch.c` performs real temporary-filesystem operations without subprocesses, repository indexing, models, or GPU use. Coverage includes binary/Unicode file changes, rename/deletion flags, deterministic ordering/coalescing, nested enrollment, initial ordering, timeout/cancellation, retained events, explicit invalidation, byte/event/native-work limits, creation/runtime enrollment limits, the full supported nesting depth, root moves, and repeated native-resource cleanup. Windows uses an actual junction for link exclusion/root-rejection coverage; POSIX tests use symbolic links.

An isolated Windows MSVC C17 Debug and Release harness passes these tests. Linux and macOS runtime behavior has **not** been verified in this local environment and needs platform CI. Host invalidation and local budget failures exercise the shared recovery machinery; the tests do not claim to have forced a real kernel queue overflow. There are no throughput, latency, or 100,000-file performance claims from these unit results.

`tests/unit/test_monitor.c` uses a watcher double with the real index and input
snapshots to force fallback, loss, cancellation, scan races and hardlink-alias
cases. It does not establish native notification delivery. The separate
`test_agent_watch.c` uses real native events with a scripted model to exercise
stale-response rejection; scripted output is not an inference correctness test.
`test_agent_changes.c` supplies a silent watcher double and verifies immediate
post-patch invalidation, including unindexed files and slash/backslash aliases,
without relying on eventual native delivery.
