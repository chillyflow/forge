#ifndef FORGE_FORGE_H
#define FORGE_FORGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FORGE_VERSION "0.1.0-dev"
#define FORGE_ABI_VERSION 1
typedef enum {
    FORGE_OK=0, FORGE_ERR_ARGUMENT, FORGE_ERR_MEMORY, FORGE_ERR_IO,
    FORGE_ERR_POLICY, FORGE_ERR_LIMIT, FORGE_ERR_MODEL, FORGE_ERR_PARSE,
    FORGE_ERR_CANCELLED, FORGE_ERR_NOT_FOUND, FORGE_ERR_CONFLICT
} forge_status;
typedef struct { forge_status code; char message[512]; } forge_error;
typedef enum {
    FORGE_AGENT_INIT, FORGE_AGENT_PREFILL, FORGE_AGENT_GENERATING,
    FORGE_AGENT_TOOL_REQUEST, FORGE_AGENT_TOOL_RUNNING, FORGE_AGENT_TOOL_RESULT,
    FORGE_AGENT_RECONTEXTUALIZE, FORGE_AGENT_DONE, FORGE_AGENT_ERROR
} forge_agent_state;
typedef enum { FORGE_CAP_READ=1, FORGE_CAP_WRITE=2, FORGE_CAP_PROCESS=4 } forge_capability;
typedef struct {
    const char *type;
    uint64_t sequence;
    const char *json; /* Borrowed UTF-8 JSON, valid only during callback. */
} forge_event;
typedef void (*forge_event_fn)(const forge_event *, void *);
typedef bool (*forge_policy_fn)(const char *tool, forge_capability, const char *arguments_json, void *);
typedef bool (*forge_cancel_fn)(void *);
typedef bool (*forge_token_fn)(const char *utf8, size_t length, void *);
typedef struct {
    size_t context_tokens, output_reserve, max_turns, max_generated_tokens;
    size_t max_input_tokens, max_tool_bytes, max_file_bytes;
    uint64_t command_timeout_ms, wall_timeout_ms;
} forge_limits;
typedef struct {
    size_t prompt_tokens, generated_tokens, cached_tokens, prefill_tokens;
    size_t turns, tool_calls, raw_tool_bytes, visible_tool_bytes, files_modified;
    size_t context_evictions, loop_warnings;
    double load_ms, prefill_ms, decode_ms, duration_ms;
    bool simulated;
} forge_metrics;
typedef struct {
    const char *model_path;
    const char *script_path; /* Explicit deterministic test fixture; never auto-selected. */
    const char *chat_template; /* NULL: model metadata, or supported llama template name. */
    size_t context_tokens;
    int gpu_layers, threads;
    uint32_t seed;
    float temperature;
    bool reuse_prefix;
} forge_model_config;
typedef struct forge_model forge_model;
typedef struct forge_agent forge_agent;
typedef struct forge_repo forge_repo;
typedef struct {
    const char *workspace;
    forge_model *model; /* Borrowed; model must outlive agent. One active run per model. */
    forge_limits limits;
    bool allow_write, allow_exec, semantic_output, compact_context;
    forge_policy_fn policy;
    forge_cancel_fn cancelled;
    void *userdata;
} forge_agent_config;
forge_limits forge_default_limits(void);
forge_model_config forge_default_model_config(void);
forge_model *forge_model_load(const forge_model_config *, forge_error *);
void forge_model_destroy(forge_model *);
forge_status forge_complete(forge_model *, const char *prompt, size_t max_tokens,
                            forge_token_fn, void *, forge_metrics *, forge_error *);
forge_agent *forge_agent_create(const forge_agent_config *, forge_error *);
forge_status forge_agent_run(forge_agent *, const char *request, forge_event_fn, void *, forge_error *);
const forge_metrics *forge_agent_metrics(const forge_agent *);
const char *forge_agent_session(const forge_agent *);
void forge_agent_destroy(forge_agent *);
forge_repo *forge_repo_open(const char *workspace, forge_error *);
forge_status forge_repo_index(forge_repo *, forge_error *);
/* Returned strings are caller-owned; release with forge_free. */
char *forge_repo_inspect(forge_repo *, const char *name, int depth, forge_error *);
char *forge_repo_references(forge_repo *, const char *name, forge_error *);
char *forge_repo_summary(forge_repo *, forge_error *);
uint64_t forge_repo_generation(const forge_repo *);
void forge_repo_close(forge_repo *);
forge_status forge_replay(const char *session_dir, forge_event_fn, void *, forge_error *);
void forge_free(void *);
const char *forge_status_string(forge_status);
#ifdef __cplusplus
}
#endif
#endif
