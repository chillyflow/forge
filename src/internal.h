#ifndef FORGE_INTERNAL_H
#define FORGE_INTERNAL_H
#include "forge/forge.h"
#include "forge/context.h"
#include "forge/checkpoint.h"
#include "yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <time.h>
#define FG_PATH_MAX 4096
#define FG_MAX_JSON (16u * 1024u * 1024u)
#define FG_MIN(a, b) ((a) < (b) ? (a) : (b))
#define FG_MAX(a, b) ((a) > (b) ? (a) : (b))
typedef struct {
    char *data;
    size_t len, cap;
    bool failed;
} fg_buf;
bool fg_buf_add(fg_buf *, const char *, size_t);
bool fg_buf_puts(fg_buf *, const char *);
bool fg_buf_printf(fg_buf *, const char *, ...);
char *fg_buf_take(fg_buf *);
void fg_buf_clear(fg_buf *);
char *fg_strdup(const char *);
uint64_t fg_hash(const void *, size_t);
uint64_t fg_now_ms(void);
forge_status fg_error(forge_error *, forge_status, const char *, ...);
char *fg_read_file(const char *, size_t, size_t *, forge_error *);
bool fg_write_file(const char *, const char *, size_t, forge_error *);
bool fg_mkdir(const char *, forge_error *);
bool fg_regular_target(const char *, forge_error *);
bool fg_workspace(const char *, char[FG_PATH_MAX], forge_error *);
bool fg_relative_path(const char *, char[FG_PATH_MAX], forge_error *);
bool fg_safe_path(const char *, const char *, bool, char[FG_PATH_MAX], forge_error *);
bool fg_path_join(char[FG_PATH_MAX], const char *, const char *);
bool fg_random_hex(char *, size_t);
bool fg_utf8_valid(const char *, size_t);
size_t fg_utf8_prefix(const char *, size_t length, size_t maximum);
size_t fg_utf8_forward(const char *, size_t length, size_t offset);
typedef bool (*fg_walk_fn)(const char *, void *);
bool fg_walk(const char *, const char *, fg_walk_fn, void *, forge_error *);
char *fg_json_string(const char *);
const char *fg_json_str(yyjson_val *, const char *);
bool fg_json_uint(yyjson_val *, const char *, size_t *, size_t);

typedef struct {
    char *out, *err;
    size_t out_len, err_len;
    int exit_code;
    bool timed_out, truncated, cancelled;
    bool started; /* True only after successful OS process creation/fork. */
    double duration_ms;
} fg_process_result;
forge_status fg_process(const char *root, const char *const *argv, uint64_t timeout,
                        size_t max_bytes, forge_cancel_fn, void *, fg_process_result *,
                        forge_error *);
void fg_process_free(fg_process_result *);
char *fg_process_render(const fg_process_result *);
char *fg_render_bytes(const char *, size_t);
forge_status fg_process_at(const char *workspace_root, const char *cwd, const char *const *argv,
                           uint64_t timeout, size_t max_bytes, forge_cancel_fn, void *,
                           fg_process_result *, forge_error *);

typedef struct {
    char dir[FG_PATH_MAX];
    FILE *events;
    uint64_t sequence, start_ms;
    forge_event_fn callback;
    void *userdata;
    size_t edit_bytes_reserved, edit_bytes_limit;
} fg_session;
bool fg_session_start(fg_session *, const char *, forge_event_fn, void *, forge_error *);
bool fg_session_emit(fg_session *, const char *, const char *, forge_error *);
bool fg_session_artifact(fg_session *, const char *, const char *, forge_error *);
bool fg_session_artifact_bytes(fg_session *, const char *, const char *, size_t, forge_error *);
bool fg_session_finish(fg_session *, const forge_metrics *, forge_status, forge_error *);
char *fg_metrics_json(const forge_metrics *, forge_status);
char *fg_compress_output(const char *, size_t, size_t *, forge_error *);
uint64_t fg_diagnostic_hash(const char *);

/* Internal checkpoint I/O seam. Real backends provide physical sequence state;
 * unit fixtures may substitute deterministic bytes, never model evidence. */
typedef struct {
    void *userdata;
    void *(*allocate)(void *, size_t, forge_error *);
    void (*release)(void *, void *, size_t);
} fg_checkpoint_allocator;
typedef struct {
    forge_status (*supported)(forge_model *, forge_error *);
    forge_status (*prefill)(forge_model *, const char *, int32_t **, size_t *, forge_metrics *,
                            forge_cancel_fn, void *, uint64_t, forge_error *);
    size_t (*state_size)(forge_model *);
    size_t (*state_get)(forge_model *, uint8_t *, size_t);
    size_t (*state_set)(forge_model *, const uint8_t *, size_t);
    bool (*accept_tokens)(forge_model *, const int32_t *, size_t);
    void (*clear)(forge_model *);
    /* Optional automatic-cache hooks. probe_prefix performs only template/
     * token work, allocates all extra buffers through the supplied allocator,
     * and reports LCP(full tokens, separately templated raw prefix tokens).
     * live_prefix includes a matching final token; callers subtract it when
     * logits need recomputation. live_matches requires EXACT sequence coverage. */
    forge_status (*probe_prefix)(forge_model *, const char *, size_t, const int32_t *, size_t,
                                 size_t *, const fg_checkpoint_allocator *, forge_cancel_fn, void *,
                                 uint64_t, forge_error *);
    size_t (*live_prefix)(forge_model *, const int32_t *, size_t);
    bool (*live_matches)(forge_model *, const int32_t *, size_t);
} fg_checkpoint_backend;

typedef struct fg_checkpoint_cache fg_checkpoint_cache;
typedef struct {
    fg_checkpoint_cache *cache;
    const int32_t *tokens;
    size_t token_count, anchors[FORGE_CHECKPOINT_CACHE_MAX_ANCHORS], anchor_count;
    size_t capture_attempts, restored_tokens, previous_live_prefix;
    uint64_t context_hash, deadline;
    forge_cancel_fn cancelled;
    void *userdata;
    bool active, reuse_recorded;
} fg_checkpoint_cache_operation;

struct forge_model {
    forge_model_config config;
    void *backend;
    yyjson_doc *script;
    size_t script_cursor;
    char *previous_prompt;
    char instance_nonce[33];
    uint64_t next_checkpoint_id;
    bool operation_active;
    const fg_checkpoint_backend *checkpoint;
    fg_checkpoint_cache *cache;
    const forge_checkpoint_cache_request *cache_request;
    size_t (*count)(forge_model *, const char *);
    forge_status (*generate)(forge_model *, const char *, const char *, const char *, size_t,
                             forge_token_fn, void *, char **, forge_metrics *, forge_cancel_fn,
                             void *, uint64_t, forge_error *);
    void (*destroy)(forge_model *);
};
bool fg_model_instance_init(forge_model *, forge_error *);
size_t fg_model_count(const char *, void *);
bool fg_llama_init(forge_model *, forge_error *);
forge_status fg_model_generate(forge_model *, const char *, const char *, size_t, forge_token_fn,
                               void *, char **, forge_metrics *, forge_cancel_fn, void *, uint64_t,
                               forge_error *);
forge_status fg_model_generate_with_cache(forge_model *, const char *, const char *, size_t,
                                          forge_token_fn, void *, char **, forge_metrics *,
                                          forge_cancel_fn, void *, uint64_t,
                                          const forge_checkpoint_cache_request *, forge_error *);
forge_status fg_model_generate_routed_with_cache(forge_model *, const char *, const char *,
                                                 const char *, size_t, forge_token_fn, void *,
                                                 char **, forge_metrics *, forge_cancel_fn, void *,
                                                 uint64_t, const forge_checkpoint_cache_request *,
                                                 forge_error *);
/* For compound operations that already own operation_active. Does not release
 * the guard. No automatic checkpoint request is nominated. */
forge_status fg_model_generate_active(forge_model *, const char *, size_t, forge_token_fn, void *,
                                      char **, forge_metrics *, forge_cancel_fn, void *, uint64_t,
                                      forge_error *);

/* Internal cache operations run under the generation operation guard. */
forge_status fg_checkpoint_cache_validate_request(const char *,
                                                  const forge_checkpoint_cache_request *,
                                                  forge_error *);
forge_status fg_checkpoint_cache_begin(forge_model *, const char *, const int32_t *, size_t,
                                       forge_cancel_fn, void *, uint64_t,
                                       fg_checkpoint_cache_operation *, forge_error *);
size_t fg_checkpoint_cache_next(const fg_checkpoint_cache_operation *, size_t, size_t);
void fg_checkpoint_cache_note_reuse(fg_checkpoint_cache_operation *, size_t);
forge_status fg_checkpoint_cache_capture(forge_model *, fg_checkpoint_cache_operation *, size_t,
                                         forge_error *);
void fg_checkpoint_cache_destroy(fg_checkpoint_cache *);
size_t fg_checkpoint_allocation_bytes(size_t tokens, size_t state_bytes);
bool fg_checkpoint_matches_prefix(const forge_checkpoint *, const int32_t *, size_t);
forge_checkpoint *fg_checkpoint_capture_live(forge_model *, const int32_t *, size_t, size_t,
                                             uint64_t repo_generation, uint64_t context_hash,
                                             forge_cancel_fn, void *, uint64_t, forge_error *);
forge_status fg_checkpoint_restore_active(forge_model *, const forge_checkpoint *, uint64_t, size_t,
                                          forge_cancel_fn, void *, uint64_t,
                                          forge_checkpoint_stats *, forge_error *);

/* Optional free-text reasoning allowed on every model action. It is bounded
 * both at the parser boundary and because retained ACTION segments can re-enter
 * later prompts; the cap keeps that recurring prompt cost small. */
#define FG_THOUGHT_MAX_BYTES 2048u
/* std::regex pattern for llama.cpp's lazy grammar sampler. The first capture
 * starts at the action object's opening brace, so earlier reasoning remains
 * unconstrained while the complete action is replayed into the GBNF state. */
#define FG_ACTION_TRIGGER_PATTERN "(\\{[ \\t\\r\\n]*\"(tool|memory|final)\"[ \\t\\r\\n]*:)"
/* Routed elicitation. A lazy trigger alone never elicits a prefix, because
 * nothing stops the model from opening the action object on its very first
 * token (measured: 0 prefixes in 67 routed actions). Banning '{' alone is
 * worse: parked at the end of a long prompt with its best token excluded,
 * greedy decoding falls into prompt echo and burns the whole turn budget
 * without an action (measured: 10/10 benchmark limit deaths). So routed
 * generation force-decodes this cue first, steering the continuation into
 * prose; the cue is host scaffold, streamed and recorded in the raw response
 * but stripped before the thought is bounded, validated, or counted. */
#define FG_THOUGHT_CUE "Thought: "
/* After the cue, action-opening tokens stay excluded for this many sampled
 * tokens (bounded by a quarter of the turn's token budget) so the model must
 * produce some reasoning text, and end-of-generation tokens stay excluded
 * until the action actually begins, so a generation cannot end actionless;
 * the turn's token budget is the backstop. Floor on room to reason, not a
 * cap: FG_THOUGHT_MAX_BYTES still bounds the accepted prefix. */
#define FG_THOUGHT_MIN_PREFIX_TOKENS 32u
typedef struct {
    const char *name, *description, *fields, *grammar;
    forge_capability capability;
} fg_tool_def;
const fg_tool_def *fg_tools(size_t *);
/* `thought` selects whether reasoning is offered, `required` whether it may be
 * empty, and `routed` whether it is a plain-text prefix outside the action JSON.
 * Routed mode leaves the action constrained and uses a lazy grammar trigger. */
char *fg_tool_schema(bool thought, bool required, bool routed);
char *fg_tool_grammar(bool thought, bool required, bool routed);
bool fg_tool_validate(const char *, yyjson_val *, forge_error *);
uint64_t fg_tool_signature(const char *, yyjson_val *, uint64_t generation,
                           uint64_t diagnostic_hash);
typedef struct {
    forge_agent_config config;
    forge_repo *repo;
    fg_session *session;
    char root[FG_PATH_MAX];
    size_t call_id;
    size_t validation_id;
    uint64_t deadline;
    bool process_ran;
    bool evidence_failed;      /* An edit event or prepared outcome could not be recorded. */
    fg_process_result process; /* Metadata only; out/err pointers stay NULL. */
} fg_tool_context;
typedef struct {
    bool applicable, passed, inputs_changed;
    size_t commands, stages;
    uint64_t generation;
    char *json, *summary;
} fg_validation_result;
forge_status fg_validation_run(fg_tool_context *, const char *const *, size_t, forge_metrics *,
                               fg_validation_result *, forge_error *);
void fg_validation_result_free(fg_validation_result *);
char *fg_tool_execute(fg_tool_context *, const char *, yyjson_val *, bool *, forge_error *);
char *fg_repo_search(forge_repo *, const char *, size_t, forge_error *);
char *fg_repo_targets(forge_repo *, const char *, forge_error *);
forge_status fg_repo_note_change(forge_repo *, forge_error *);
forge_status fg_repo_note_change_until(forge_repo *, uint64_t deadline, forge_cancel_fn, void *,
                                       forge_error *);
forge_status fg_repo_index_until(forge_repo *, const char *const *, size_t, bool, uint64_t deadline,
                                 forge_cancel_fn, void *, forge_error *);
typedef struct fg_repo_monitor fg_repo_monitor;
typedef struct {
    char *json;
    bool changed, full_scan, delta_scan, reopened, native;
    size_t events;
    uint64_t generation;
    double duration_ms;
} fg_repo_change;
fg_repo_monitor *fg_repo_monitor_create(forge_repo *, const char *, forge_cancel_fn, void *,
                                        uint64_t deadline, bool require_native, fg_repo_change *,
                                        forge_error *);
forge_status fg_repo_monitor_poll(fg_repo_monitor *, uint64_t wait_ms, bool force_full,
                                  fg_repo_change *, forge_error *);
void fg_repo_change_free(fg_repo_change *);
void fg_repo_monitor_destroy(fg_repo_monitor *);
#endif
