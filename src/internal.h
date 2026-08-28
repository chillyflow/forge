#ifndef FORGE_INTERNAL_H
#define FORGE_INTERNAL_H
#include "forge/forge.h"
#include "forge/context.h"
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
    forge_status (*supported)(forge_model *, forge_error *);
    forge_status (*prefill)(forge_model *, const char *, int32_t **, size_t *, forge_metrics *,
                            forge_cancel_fn, void *, uint64_t, forge_error *);
    size_t (*state_size)(forge_model *);
    size_t (*state_get)(forge_model *, uint8_t *, size_t);
    size_t (*state_set)(forge_model *, const uint8_t *, size_t);
    bool (*accept_tokens)(forge_model *, const int32_t *, size_t);
    void (*clear)(forge_model *);
} fg_checkpoint_backend;

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
    size_t (*count)(forge_model *, const char *);
    forge_status (*generate)(forge_model *, const char *, const char *, size_t, forge_token_fn,
                             void *, char **, forge_metrics *, forge_cancel_fn, void *, uint64_t,
                             forge_error *);
    void (*destroy)(forge_model *);
};
bool fg_model_instance_init(forge_model *, forge_error *);
size_t fg_model_count(const char *, void *);
bool fg_llama_init(forge_model *, forge_error *);
forge_status fg_model_generate(forge_model *, const char *, const char *, size_t, forge_token_fn,
                               void *, char **, forge_metrics *, forge_cancel_fn, void *, uint64_t,
                               forge_error *);

typedef struct {
    const char *name, *description, *fields, *grammar;
    forge_capability capability;
} fg_tool_def;
const fg_tool_def *fg_tools(size_t *);
char *fg_tool_schema(void);
char *fg_tool_grammar(void);
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
