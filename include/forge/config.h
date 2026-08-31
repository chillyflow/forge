#ifndef FORGE_CONFIG_H
#define FORGE_CONFIG_H

#include "forge/forge.h"
#include "forge/checkpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A configuration/planner sentinel, never pass this to forge_model_load(). */
#define FORGE_GPU_LAYERS_AUTO (-2)
#define FORGE_CONFIG_MAX_BYTES (256u * 1024u)
#define FORGE_CONFIG_MAX_INHERITANCE 8u
#define FORGE_HARDWARE_MAX_GPUS 16u

typedef enum {
    FORGE_SHELL_NETWORK_UNSPECIFIED = 0,
    FORGE_SHELL_NETWORK_DENY,
    FORGE_SHELL_NETWORK_ALLOW
} forge_shell_network;

typedef struct {
    forge_model_config model;
    forge_limits limits;
    forge_checkpoint_cache_options checkpoint_cache;
    bool checkpoint_cache_enabled; /* Opt-in; physical cache support is checked at model load. */
    bool semantic_output, compact_context;
    bool thought, thought_required, thought_in_history, thought_routed;
    /* Routed-mode §32 controls; CLI-only, no TOML keys. The cue string is
     * borrowed (CLI argument), never owned by this object. */
    const char *thought_cue;
    size_t thought_budget;
    bool thought_budget_unbounded;
    forge_shell_network shell_network;
    /* Private storage. Do not free, assign, or copy these pointers. The public
     * model strings may be replaced by borrowed CLI strings; destroy only frees
     * the storage allocated by this configuration object. */
    char *_owned_model_path, *_owned_script_path, *_owned_chat_template;
} forge_config;

/* Initialize before use. Config objects are not shallow-copyable. No permissions
 * to write files or execute commands are ever granted by a configuration file. */
void forge_config_init(forge_config *);
void forge_config_destroy(forge_config *);

/* Transactional overlays: on failure the previous configuration is unchanged.
 * Missing keys inherit their current values. Each file may extend one other file;
 * its parent is applied first. Relative paths are based on the file defining them,
 * not the working directory. No environment or home-directory expansion occurs.
 * Load profiles first, project config second, and explicit CLI overrides last.
 * Files must exist; automatic discovery/missing-file policy belongs to the caller. */
forge_status forge_config_load(forge_config *, const char *path, forge_error *);

/* Parse an in-memory document. source_path identifies the file and supplies the
 * base directory, but need not exist. A nonempty source path is required. */
forge_status forge_config_parse(forge_config *, const char *toml, size_t length,
                                const char *source_path, forge_error *);

/* Validate again after applying CLI overrides. No model path is required, allowing
 * model-free commands such as index. Context fields must agree. */
forge_status forge_config_validate(const forge_config *, forge_error *);

/* There is currently no network sandbox. An explicit network=false therefore
 * forbids subprocess execution. network=true does NOT authorize execution. */
forge_status forge_config_check_exec(const forge_config *, bool allow_exec, forge_error *);

enum {
    FORGE_CPU_SSE2 = 1u << 0,
    FORGE_CPU_AVX = 1u << 1,
    FORGE_CPU_AVX2 = 1u << 2,
    FORGE_CPU_NEON = 1u << 3
};

typedef struct {
    char name[128];
    uint64_t total_bytes, available_bytes;
    bool memory_known, unified_memory;
    /* Some APIs (Metal) expose a process working-set budget, not globally free
     * dedicated VRAM. Budget these conservatively against host RAM as well. */
    bool memory_is_budget;
} forge_gpu_info;

typedef struct {
    char cpu_arch[32], cpu_name[128];
    unsigned logical_cpus, cpu_features;
    uint64_t ram_total_bytes, ram_available_bytes;
    bool ram_total_known, ram_available_known;
    /* false means this build cannot enumerate usable inference GPUs. It does not
     * mean the host has no physical GPU. Only local ggml devices are enumerated. */
    bool gpu_detection_available, gpu_list_truncated;
    size_t gpu_count;
    forge_gpu_info gpus[FORGE_HARDWARE_MAX_GPUS];
} forge_hardware;

typedef struct {
    uint64_t file_bytes, model_bytes, kv_bytes_per_token;
    size_t layer_count, training_context;
    bool model_bytes_known, tensor_bytes_known, kv_bytes_known, metadata_available;
    char architecture[64], note[256];
} forge_model_requirements;

typedef enum {
    FORGE_FIT_UNKNOWN = 0,
    FORGE_FIT_ESTIMATED,
    FORGE_FIT_INSUFFICIENT
} forge_memory_fit;

typedef struct {
    size_t context_tokens;
    int gpu_layers, threads, gpu_index; /* gpu_index=-1 means CPU. */
    forge_memory_fit fit;
    bool context_reduced, kv_estimate_available, draft_enabled;
    char kv_format[8];
    uint64_t estimated_model_bytes, estimated_kv_bytes;
    uint64_t host_reserve_bytes, gpu_reserve_bytes;
    uint64_t host_headroom_bytes, gpu_headroom_bytes;
    char assumptions[512];
} forge_hardware_plan_result;

/* Startup measurement only; no subprocesses, model inference, or downloads.
 * Without llama.cpp GPU detection is unavailable. No backend is unloaded. */
forge_status forge_hardware_detect(forge_hardware *, forge_error *);

/* A regular local GGUF is required. With llama.cpp this reads vocabulary and
 * metadata only (empty device list, zero GPU layers); no tensor data or inference
 * context is allocated. Unsupported attention geometry keeps KV explicitly
 * unknown. Without llama.cpp only a file-size proxy is available. */
forge_status forge_hardware_model_file(const char *, forge_model_requirements *, forge_error *);

/* Pure, deterministic recommendations; injected measurements make tests portable.
 * f16 KV applies to one sequence and scalar, conventional attention geometry.
 * Margins are heuristics, not guarantees. Only GPU 0 is budgeted; device memories
 * are never added together. Unknown KV geometry never enables automatic offload.
 * Callers decide whether to apply recommendations or retain explicit settings. */
forge_status forge_hardware_plan(const forge_hardware *, const forge_model_requirements *,
                                 size_t requested_context, size_t output_reserve,
                                 forge_hardware_plan_result *, forge_error *);

#ifdef __cplusplus
}
#endif
#endif
