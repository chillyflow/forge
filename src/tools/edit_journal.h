#ifndef FORGE_EDIT_JOURNAL_H
#define FORGE_EDIT_JOURNAL_H
#include "internal.h"
#include "forge/memory.h"

/* Private content-only edit evidence. Files are exclusive, never overwritten.
 * A prepared record is not evidence that the target replacement completed.
 * The caller must finish every successful preparation exactly once. */
typedef struct {
    size_t call_id, before_bytes, after_bytes;
    char path[FG_PATH_MAX];
    char before[64], after[64], diff[64], intent[64], outcome[64];
    bool before_exists, prepared, finished;
} fg_edit_record;

/* Full-file unified text diff, not a minimal or semantic diff. Accepts at most
 * 16 MiB of valid UTF-8, NUL-free data on either side. Caller owns the result.
 * The quoted Git path uses octal UTF-8 byte escapes, never JSON \u escapes. */
char *fg_edit_diff(const char *path, bool before_exists, forge_slice before, forge_slice after,
                   size_t *length, forge_cancel_fn cancelled, void *user, uint64_t deadline,
                   forge_error *error);
bool fg_edit_prepare(fg_tool_context *context, const char *path, bool before_exists,
                     forge_slice before, forge_slice after, fg_edit_record *record,
                     forge_error *error);
bool fg_edit_finish(fg_tool_context *context, fg_edit_record *record, bool applied,
                    forge_status status, forge_error *error);
#endif
