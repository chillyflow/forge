# Scoped memory and file views

`include/forge/memory.h` implements the primitives specified by source-design
phases 28–29: scoped arenas and pointer/length views. Applications can give a
model, session, generation, or tool call its own arena and make the end of that
lifetime explicit. Source, prompt, patch, token, or output bytes can be passed
as slices without repeatedly constructing NUL-terminated copies.

These are allocation and ownership APIs. Their presence alone does not establish
that every Forge caller uses scoped storage or that a benchmark improved.

## Arena ownership

```c
forge_error error = {0};
forge_arena *tool = forge_arena_create(4u * 1024u * 1024u, &error);
if (!tool) {
    /* Handle the explicit limit/allocation error. */
    return;
}
char *payload = forge_arena_alloc(tool, 200, &error);
void *aligned = forge_arena_aligned_alloc(tool, 64, 129, &error);
/* Check both results before use. Storage is uninitialized. */
forge_arena_reset(tool);   /* All payload/aligned pointers are now invalid. */
forge_arena_destroy(tool); /* Releases control object and all retained blocks. */
```

Returned allocations do not move. Never call `free` or `forge_free` on them.
They remain valid until arena reset or destruction. Arenas do not own other
resources merely because their handles were stored in an allocation: close file
views, release model handles, and perform other required cleanup separately.
Arena operations and custom allocator hooks are not safe for concurrent access
to the same arena without caller synchronization.

### Limits, alignment, and failure

The caller supplies `max_committed_bytes`. It covers **every byte requested from
the allocator**, including the arena control object, complete block headers,
alignment slack, unused block capacity, and payload. Allocator-internal metadata
and OS accounting outside those requests cannot be counted by this API.

Creation allocates only the control object. A limit too small even for that
object returns `FORGE_ERR_LIMIT`. Blocks are acquired lazily, normally in 64 KiB
chunks; a larger request gets a sufficient block and a smaller remaining budget
caps a normal chunk. An allocation never silently raises the caller's limit.
Allocation searches the current block and later retained blocks; earlier skipped
tails may remain unused until reset. This is a monotonic scoped allocator, not
a general-purpose allocator with individual frees or compaction.

`forge_arena_alloc` aligns for `max_align_t` (or the equivalent fundamental C
alignment on MSVC, whose C headers omit that typedef). `forge_arena_aligned_alloc` accepts
a nonzero power-of-two alignment representable by `uintptr_t`; its size does
**not** need to be a multiple of that alignment. Alignment padding/slack consumes
budget. Huge size or alignment arithmetic is checked before allocation.

| Request | Result |
| --- | --- |
| Valid arena/alignment, size zero | `NULL`, `FORGE_OK`, no changes |
| Missing arena or invalid alignment | `NULL`, `FORGE_ERR_ARGUMENT` |
| Overflow or insufficient committed budget | `NULL`, `FORGE_ERR_LIMIT` |
| Allocator returns `NULL` | `NULL`, `FORGE_ERR_MEMORY` |
| Allocator violates its alignment contract | `NULL`, `FORGE_ERR_ARGUMENT`; returned storage is released |

Every failed allocation leaves the arena's blocks, offsets, existing data,
ownership, and statistics unchanged. A failed large allocation does not consume
space that prevents a subsequent smaller request. Success clears the supplied
error object, including the explicit zero-size no-op.

`forge_arena_reset` retains committed blocks for reuse and clears live payload,
alignment padding, and allocation counts. It does not allocate. Peak statistics
remain available; destroy releases everything. Neither reset nor destruction
securely erases old bytes.

### Statistics and allocator hooks

`forge_arena_get_stats` returns a value copy with the configured maximum,
committed/peak committed bytes, live/peak payload bytes, consumed alignment
padding, nonempty allocations since reset, and retained block count. Live payload
and padding do not include unused tails or block headers; committed bytes do.
A `NULL` arena returns zero statistics.

`forge_arena_create_with_allocator` optionally accepts an `alloc`, `free`, and
borrowed user pointer. The function pair is copied into the arena and is used
for both its control object and blocks. The user context must outlive it. Hooks
have the usual `malloc` alignment contract, must not reenter their arena, and
must not throw exceptions. Both hooks are required when supplied. This supports
embedding allocators and deterministic failure-injection tests without global
allocation state.

## Slices

```c
forge_slice bytes = {source, source_length};
forge_slice part;
if (forge_slice_subslice(bytes, offset, length, &part, &error) == FORGE_OK) {
    /* part.ptr aliases source; part.len is exact, including binary NUL bytes. */
}
```

`forge_slice_t` aliases `forge_slice`, preserving the name shown in the original
design. Its shape is `const char *ptr; size_t len;`. A slice owns nothing and does
not make writable backing memory immutable; it exposes a read-only borrow.
The backing owner supplies a valid extent and must outlive every derived slice.

`{NULL, 0}` is valid and empty. `{NULL, nonzero}` is invalid. Checked subslicing
uses subtraction-based bounds checks, leaves its output unchanged on failure,
and permits an empty slice at the end. Equality compares explicit lengths and
bytes, never `strlen`; valid empty slices compare equal. Invalid slices compare
unequal even to each other. The API cannot prove that a non-NULL pointer actually
has the caller-declared extent.

## Read-only file views

```c
forge_file_view *source = forge_file_view_open(
    path, 2u * 1024u * 1024u, FORGE_FILE_VIEW_MAP, &error);
if (source) {
    forge_slice bytes = forge_file_view_slice(source);
    /* Pass bytes.ptr and bytes.len to a parser accepting an explicit length. */
    forge_file_view_close(source); /* Every borrowed slice is now invalid. */
}
```

Choose storage explicitly:

| Mode | Storage and lifetime |
| --- | --- |
| `FORGE_FILE_VIEW_MAP` | POSIX `mmap(PROT_READ, MAP_PRIVATE)` or Windows `CreateFileMapping`/`MapViewOfFile` with read-only protection. No source copy is constructed. |
| `FORGE_FILE_VIEW_READ` | An owned, read-only API view of a binary-safe copy. Reads are streamed into the bounded allocation; later filesystem writes cannot alter the returned bytes. |

The file must be regular, fit the caller's content cap, and fit `PTRDIFF_MAX`.
The cap concerns file bytes, not the small view object or operating-system
mapping bookkeeping. Zero cap permits an empty file only. Empty files return a
valid owner with `{NULL, 0}`; no zero-length mapping is attempted. Neither mode
adds a NUL terminator. Use explicit lengths; do not call C string functions on a
view unless a separate validated contract supplies termination.

The opened object's type is checked, including POSIX `O_NOFOLLOW`/`fstat` and
Windows `FILE_FLAG_OPEN_REPARSE_POINT`/handle metadata. Final symlinks, reparse
points, directories, and special files are rejected. POSIX nonblocking opens
prevent a raced FIFO from hanging before its opened type can be checked. Common
file identity/size/time changes during opening or copying are rejected. Errors
release allocated buffers, mappings, and file/mapping handles.

The path is explicitly supplied by the caller. Workspace containment and
ancestor-symlink policy must be enforced by that caller before opening a view;
this generic API does not establish a workspace boundary. Windows uses UTF-8
input and native Unicode APIs without code-page substitution. Native device
namespace roots are rejected. Paths are capped at 4095 bytes (including absolute
resolution on Windows); POSIX filename bytes need not be UTF-8.

### Mapping is not snapshot evidence

**Mapped files are externally mutable.** Other writers may change visible bytes.
On POSIX, truncating a mapped file can cause `SIGBUS` when the caller accesses
pages beyond its new end. A private mapping does not remove this risk or freeze
the file's content. Windows mappings have their own write/resize semantics and
are likewise not a promise of a stable filesystem version.

Use mappings only when the caller controls the file's required lifetime; close
them before edits where appropriate. Use `FORGE_FILE_VIEW_READ` when that cannot
be arranged. A read copy is stable after it returns, but obtaining it is not an
atomic filesystem snapshot against concurrent adversarial writers.

Neither storage mode establishes unchanged validation inputs or test success.
Keep [workspace input snapshot checks](INPUT_SNAPSHOTS.md) around applicable
verification. Source views and arenas must not replace those integrity checks.

## Runtime callers

The agent uses a 64 MiB committed-byte arena for constrained action JSON. The
arena is reset between generations; context, state, and session data copy any
values that outlive the turn. `generation_arena_peak_bytes` reports this arena's
committed high-water mark, not process RAM or all runtime allocations.

`read_file` uses an owned `FORGE_FILE_VIEW_READ` snapshot and explicit slices
for bounded line selection, including empty files and an unterminated last
line. Its output is copied before closing the view. It does not memory-map
mutable repository files or treat a source view as validation evidence.
Other runtime paths still use their existing allocation lifetimes.

## Verification

`tests/unit/test_memory.c` exercises aligned and nonmoving allocations, exact
allocator-byte accounting, reset reuse, custom hooks, injected allocation
failure/rollback, zero-size/overflow/limit handling, binary slice bounds,
read/map byte equality, Unicode filenames, empty files, size caps, streaming
copies, stable read ownership after source rewrites, invalid inputs, and cleanup.
Windows checks handle counts across repeated success/failure and sharing errors;
POSIX includes FIFO denial and permission failure where applicable. Symbolic-link
creation tests explicitly skip when the host does not grant creation permission.
These checks establish module behavior, not task-time performance gains or
whole-application adoption.
