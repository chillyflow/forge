#ifndef FG_SEMANTIC_STATE_H
#define FG_SEMANTIC_STATE_H
#include "forge/forge.h"

/* Private repair-loop evidence, never validation or a proof of program
 * equivalence. Compare only non-NULL, complete states. A matching canonical
 * state can trigger bounded diagnosis; it must not authorize final or skip a
 * required test. Paths and non-source inputs participate in identity.
 *
 * Ordinary Go/Python comments and horizontal formatting are normalized while
 * literals, Python indentation, and significant newlines remain. C/C++, Rust,
 * and TypeScript use more conservative lexical normalization. Directives,
 * doc comments, ambiguous lexing, unsupported syntax, invalid UTF-8 and source
 * files over 4 MiB retain their exact bytes. No external parser is invoked. */
typedef struct fg_semantic_state fg_semantic_state;
typedef struct {
    size_t files, canonical_files, raw_files;
    bool complete;
} fg_semantic_state_info;

/* Limits must be nonzero; deadline is an absolute fg_now_ms(), zero disables
 * it. Two secure input scans reject incomplete/unsafe reads and observed
 * concurrent mutation. Only root .git and .forge directories are excluded.
 * The result owns bounded per-file fingerprints, not complete source text. */
fg_semantic_state *fg_semantic_state_take(const char *root, size_t max_files, uint64_t max_bytes,
                                          forge_cancel_fn cancel, void *user,
                                          uint64_t absolute_deadline, forge_error *error);
bool fg_semantic_state_equal(const fg_semantic_state *, const fg_semantic_state *);
uint64_t fg_semantic_state_hash(const fg_semantic_state *);
fg_semantic_state_info fg_semantic_state_describe(const fg_semantic_state *);
void fg_semantic_state_destroy(fg_semantic_state *);

/* Host diagnostic fields, not model-authored descriptions. `identity` is an
 * adapter-supplied full stable identity (including meaningful operands) when
 * available. A code alone is insufficient to ignore message wording.
 * Severity, code, path, symbol and identity all remain significant. Locations
 * are deliberately omitted: harmless comment edits shift line numbers. With
 * no identity, message bytes are retained apart from whitespace normalization
 * in unquoted prose; quoted payloads preserve all whitespace exactly.
 * Null fields mean empty. Incomplete/truncated diagnostic sets return false.
 * Ordering is ignored, duplicate diagnostics remain significant. */
typedef struct {
    const char *tool, *severity, *path, *code, *symbol, *identity, *message;
} fg_semantic_diagnostic;
bool fg_semantic_diagnostic_fingerprint(const fg_semantic_diagnostic *diagnostics, size_t count,
                                        bool complete, uint64_t *hash, forge_error *error);
#endif
