#ifndef FORGE_MEMORY_H
#define FORGE_MEMORY_H
#include "forge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Borrowed, read-only bytes, not necessarily text or NUL-terminated. The owner
 * must keep the backing storage alive. {NULL, 0} is a valid empty slice;
 * {NULL, nonzero} is invalid. The caller remains responsible for the extent. */
typedef struct {
    const char *ptr;
    size_t len;
} forge_slice;
typedef forge_slice forge_slice_t; /* Name used by the original design. */

/* Bounds are checked without offset+length overflow. Failure leaves *result
 * unchanged. Equal returns false for an invalid slice, including two invalid
 * slices. Empty valid slices compare equal without reading their pointers. */
forge_status forge_slice_subslice(forge_slice source, size_t offset, size_t length,
                                  forge_slice *result, forge_error *);
bool forge_slice_equal(forge_slice left, forge_slice right);

typedef struct forge_arena forge_arena;

/* Optional hooks are copied into the arena. Both functions must be provided.
 * alloc must return NULL or storage aligned for max_align_t, exactly as malloc.
 * free must accept every non-NULL result from alloc. user is borrowed and must
 * outlive the arena. Hooks must not reenter that arena or raise exceptions. */
typedef struct {
    void *(*alloc)(size_t bytes, void *user);
    void (*free)(void *allocation, void *user);
    void *user;
} forge_allocator;

typedef struct {
    size_t max_committed_bytes;
    size_t committed_bytes;      /* Control object + complete allocated blocks. */
    size_t peak_committed_bytes; /* Lifetime high-water mark. */
    size_t used_bytes;           /* Requested payload bytes since last reset. */
    size_t padding_bytes;        /* Consumed alignment padding since last reset. */
    size_t peak_used_bytes;      /* Lifetime payload high-water mark. */
    size_t allocation_count;     /* Successful nonempty allocations since reset. */
    size_t block_count;
} forge_arena_stats;

/* max_committed_bytes covers every byte requested from the allocator: arena
 * control, block headers, payload and spare/alignment space. Allocator-internal
 * bookkeeping is unknowable and excluded. The control object is allocated at
 * creation; blocks are allocated lazily. Insufficient control budget is LIMIT.
 * NULL hooks use malloc/free. Arena operations are not concurrently safe. */
forge_arena *forge_arena_create(size_t max_committed_bytes, forge_error *);
forge_arena *forge_arena_create_with_allocator(size_t max_committed_bytes, const forge_allocator *,
                                               forge_error *);

/* Returned storage is uninitialized, nonmoving and owned by the arena. Never
 * free it separately. alloc uses max_align_t alignment (equivalent fundamental
 * C alignment on MSVC, whose C headers omit that typedef). aligned_alloc accepts
 * any nonzero power-of-two alignment representable by uintptr_t; size need not
 * be an alignment multiple. Padding counts against the committed limit.
 *
 * With valid arena/alignment, size=0 returns NULL with FORGE_OK and changes
 * nothing. Invalid alignment/arena is ARGUMENT; overflow/budget is LIMIT;
 * allocator failure is MEMORY. Every failed request leaves all existing
 * allocations, block ownership, offsets, and statistics unchanged. */
void *forge_arena_alloc(forge_arena *, size_t size, forge_error *);
void *forge_arena_aligned_alloc(forge_arena *, size_t alignment, size_t size, forge_error *);

/* Reset invalidates ALL allocation pointers, retains blocks for reuse, and
 * clears live payload/padding/allocation counts. Peak values are retained.
 * Reset and destroy accept NULL. Neither operation securely erases old bytes. */
void forge_arena_reset(forge_arena *);
void forge_arena_destroy(forge_arena *);
/* Returns a value copy. A NULL arena yields all-zero statistics. */
forge_arena_stats forge_arena_get_stats(const forge_arena *);

typedef struct forge_file_view forge_file_view;
typedef enum {
    FORGE_FILE_VIEW_MAP = 1, /* Native read-only mapping; externally mutable. */
    FORGE_FILE_VIEW_READ = 2 /* Immutable owned copy after a completed read. */
} forge_file_view_mode;

#define FORGE_FILE_VIEW_MAX_PATH_BYTES 4095u

/* Open an explicitly named regular file. The opened object is checked: final
 * symlinks/reparse points, directories and special files are rejected. Path
 * containment and ancestor-symlink policy remain the caller's responsibility;
 * this API neither expands a workspace policy nor authorizes filesystem access.
 * Windows paths are UTF-8. POSIX paths retain their native bytes.
 *
 * The file must fit max_bytes and PTRDIFF_MAX. max_bytes=0 accepts only empty
 * files. Empty files return a valid view with {NULL, 0}; no zero-length mapping
 * is attempted. Neither mode adds a NUL terminator. A returned slice remains
 * valid only until close. MAP is not stable snapshot evidence: external writes
 * may be visible and POSIX truncation can fault on access. Use READ when the
 * caller cannot control the mapped file's lifetime. READ detects common changes
 * during copying, but neither mode is an atomic filesystem snapshot. */
forge_file_view *forge_file_view_open(const char *path, size_t max_bytes, forge_file_view_mode mode,
                                      forge_error *);
forge_slice forge_file_view_slice(const forge_file_view *);
void forge_file_view_close(forge_file_view *);

#ifdef __cplusplus
}
#endif
#endif
