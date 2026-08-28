#ifndef FORGE_WATCH_H
#define FORGE_WATCH_H
#include "forge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct forge_watch forge_watch;

typedef enum {
    FORGE_WATCH_CREATED = 1u,
    FORGE_WATCH_MODIFIED = 2u,
    FORGE_WATCH_DELETED = 4u,
    FORGE_WATCH_RENAMED_FROM = 8u,
    FORGE_WATCH_RENAMED_TO = 16u,
    FORGE_WATCH_RENAMED = 32u,
    FORGE_WATCH_DIRECTORY = 64u,
    FORGE_WATCH_METADATA = 128u,
    FORGE_WATCH_SYMLINK = 256u
} forge_watch_flag;

typedef enum {
    FORGE_WATCH_RESCAN_INITIAL = 1u,
    FORGE_WATCH_RESCAN_NATIVE_OVERFLOW = 2u,
    FORGE_WATCH_RESCAN_EVENT_LIMIT = 4u,
    FORGE_WATCH_RESCAN_BYTE_LIMIT = 8u,
    FORGE_WATCH_RESCAN_PATH_LIMIT = 16u,
    FORGE_WATCH_RESCAN_DIRECTORY_LIMIT = 32u,
    FORGE_WATCH_RESCAN_DEPTH_LIMIT = 64u,
    FORGE_WATCH_RESCAN_IO = 128u,
    FORGE_WATCH_RESCAN_ROOT_CHANGED = 256u,
    FORGE_WATCH_RESCAN_PATH_ENCODING = 512u,
    FORGE_WATCH_RESCAN_SCAN_LIMIT = 1024u,
    FORGE_WATCH_RESCAN_SUBTREE = 2048u,
    FORGE_WATCH_RESCAN_TOPOLOGY = 4096u,
    FORGE_WATCH_RESCAN_CALLER = 8192u,
    FORGE_WATCH_RESCAN_DEADLINE = 16384u,
    FORGE_WATCH_RESCAN_MEMORY = 32768u,
    FORGE_WATCH_RESCAN_NATIVE_WORK_LIMIT = 65536u
} forge_watch_rescan_reason;

typedef struct {
    size_t max_events;        /* Distinct paths per returned batch: 1..65536. */
    size_t max_bytes;         /* Returned JSON bytes (excluding NUL): 1024..16 MiB. */
    size_t max_path_bytes;    /* Relative UTF-8 path bytes: 1..4095. */
    size_t max_directories;   /* Enrolled directories including root: 1..65536. */
    size_t max_depth;         /* Directory depth relative to root: 0..64. */
    size_t max_scan_entries;  /* Entries examined per create/poll: 1..10000000. */
    size_t max_native_events; /* Raw notifications consumed per poll: 1..1000000. */
} forge_watch_limits;

forge_watch_limits forge_default_watch_limits(void);

/* Creates native watches before recursively enrolling real directories. No
 * source contents are read. Root/ancestor symlinks and reparse points are
 * rejected; links inside the tree are reported as entries but never traversed.
 * Only actual .git/ and .forge/ directories directly under root are excluded.
 * Native events are queued while the caller is not polling.
 *
 * First consume the initial batch (initial_scan_required=true), then perform
 * the initial full index, then drain the events queued during that index. The
 * watcher itself does NOT establish an index baseline or a stable snapshot.
 * Creation timeout_ms=0 disables its cooperative deadline; nonzero timeouts
 * include enrollment and fail with LIMIT. All limits remain enforced.
 * NULL limits selects defaults. Root must fit 4095 UTF-8 bytes. */
forge_watch *forge_watch_create(const char *root, const forge_watch_limits *limits,
                                forge_cancel_fn cancelled, void *user, uint64_t timeout_ms,
                                forge_error *error);

/* Returns forge_free-owned compact JSON, or NULL on argument, allocation or
 * cancellation error. Native loss/errors are represented in the JSON as
 * rescan_required, never a clean empty batch. Cancellation keeps buffered
 * normalized events; any native loss during cancellation is marked for rescan.
 *
 * timeout_ms=0 is nonblocking. Positive timeouts bound waiting/processing with
 * cooperative checks; OS filesystem calls themselves may block. timed_out and
 * more_pending are explicit. Events are sorted by relative UTF-8 path and flags
 * are OR-coalesced. This preserves change signals, not an ordered audit log.
 * Rename direction is available on Windows/Linux; macOS may provide only the
 * RENAMED flag. Paths use '/', with "." reserved for the root itself.
 *
 * rescan_required means a full index is required. If reopen_required is also
 * true, it is sticky: destroy/create BEFORE rebuilding, then drain queued
 * changes. Otherwise it is a one-batch initial/new-subtree rescan request.
 * Empty events never proves unchanged contents or successful validation.
 *
 * No concurrent or reentrant operations on one watcher. On macOS create,
 * poll and destroy must occur on the same thread (a private run-loop mode). */
char *forge_watch_poll(forge_watch *, uint64_t timeout_ms, forge_cancel_fn cancelled, void *user,
                       forge_error *error);

/* Host-reported continuity loss (e.g. suspend or external state invalidation).
 * Shares the sticky overflow recovery path; NULL is harmless. Also allows
 * deterministic recovery tests without inducing an OS queue overflow. */
void forge_watch_invalidate(forge_watch *);

/* Releases all native watches and queued paths. On Windows outstanding async
 * reads are cancelled and completed before their storage is freed. NULL-safe. */
void forge_watch_destroy(forge_watch *);

#ifdef __cplusplus
}
#endif
#endif
