#include "internal.h"
#include "forge/config.h"
#include "forge/validation.h"
#include "forge/verification.h"
#include "forge/index.h"
#include "forge/retrieval.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
static volatile sig_atomic_t stop_requested = 0;
static void signal_stop(int sig) {
    (void)sig;
    stop_requested = 1;
}
static bool cancelled(void *u) {
    (void)u;
    return stop_requested != 0;
}
static void usage(void) {
    puts("Forge " FORGE_VERSION " - native local coding agent\n\n"
         "  forge run TASK --model model.gguf [options]\n"
         "  forge complete PROMPT --model model.gguf [options]\n"
         "  forge index [CHANGED_PATH] | inspect SYMBOL | references SYMBOL | search TEXT\n"
         "  forge retrieve QUERY [--depth 0..3]    staged indexed evidence as JSON\n"
         "  forge watch [--wall-ms N]              update index from native file events\n"
         "  forge index-info PATH                 report indexed source/AST/symbol hashes\n"
         "  forge validation-plan [CHANGED_PATH]   print staged Go verification plan\n"
         "  forge validate [CHANGED_PATH] --allow-exec   execute staged verification\n"
         "  forge hardware-plan [--model model.gguf] [--json]\n"
         "  forge replay SESSION | stats SESSION | context SESSION\n"
         "  forge bench TASK.json --model model.gguf --allow-exec [options]\n"
         "  forge --model model.gguf                interactive task prompt\n\n"
         "Options:\n"
         "  --workspace PATH     existing repository (default .)\n"
         "  --profile FILE       explicit TOML base profile\n"
         "  --config FILE        TOML override (default workspace/forge.toml if present)\n"
         "  --no-config          disable workspace config discovery (profile still applies)\n"
         "  --model PATH         local GGUF; never downloaded by the runtime\n"
         "  --gpu-layers N|auto  offloaded layers, -1 for all (default 0); auto estimates fit\n"
         "  --context N          context capacity (default 16384)\n"
         "  --output-reserve N   per-turn generation budget (default 2048)\n"
         "  --max-turns N        hard agent turn limit (default 32)\n"
         "  --max-tokens N       total generated-token limit (default 32768)\n"
         "  --max-input N        total prompt-token limit (default 262144)\n"
         "  --timeout-ms N       command timeout (default 120000)\n"
         "  --wall-ms N          run deadline (default 1800000)\n"
         "  --max-tool-bytes N   captured bytes per output stream (default 65536)\n"
         "  --max-file-bytes N   file read/patch limit (default 2097152)\n"
         "  --threads N          inference threads\n"
         "  --temperature N      finite sampling temperature 0..2\n"
         "  --seed N             sampling seed 0..4294967295\n"
         "  --chat-template NAME override unsupported model chat template\n"
         "  --allow-write        permit repository patches\n"
         "  --allow-exec         permit UNSANDBOXED commands, including repository code\n"
         "  --json               JSON-lines events\n"
         "  --no-kv-reuse | --no-semantic | --no-compaction  ablations\n"
         "  --checkpoint-cache | --no-checkpoint-cache   opt-in bounded physical cache\n"
         "  --checkpoint-cache-bytes N        aggregate cache allocation cap\n"
         "  --checkpoint-cache-entries N      retained prefixes (1..64)\n"
         "  --checkpoint-cache-min-tokens N   minimum eligible prefix length\n"
         "  --checkpoint-cache-captures N     captures per prompt (1..4)\n"
         "  --grammar-first      disable greedy grammar fast path (ablation)\n"
         "  --no-auto-validation skip final Go validation (explicit ablation)\n"
         "  --script FILE        explicit simulated test backend (not inference)\n"
         "  --depth N            symbol expansion or retrieval graph hops, 0..3\n");
}
static bool number(const char *text, size_t *out) {
    if (!text || !*text || *text == '-')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno || *end || v > SIZE_MAX)
        return false;
    *out = (size_t)v;
    return true;
}

/* Parse option arity once for configuration discovery so an option-looking path
 * cannot accidentally become a new flag. The second pass applies CLI values
 * after every file, independent of argument order. */
static int option_arity(const char *option) {
    static const char *const flags[] = {"--help",
                                        "-h",
                                        "--version",
                                        "--allow-write",
                                        "--allow-exec",
                                        "--json",
                                        "--no-kv-reuse",
                                        "--checkpoint-cache",
                                        "--no-checkpoint-cache",
                                        "--grammar-first",
                                        "--no-semantic",
                                        "--no-compaction",
                                        "--no-auto-validation",
                                        "--no-config"};
    static const char *const values[] = {"--workspace",
                                         "--profile",
                                         "--config",
                                         "--model",
                                         "--script",
                                         "--chat-template",
                                         "--context",
                                         "--output-reserve",
                                         "--max-turns",
                                         "--max-tokens",
                                         "--max-input",
                                         "--timeout-ms",
                                         "--wall-ms",
                                         "--max-tool-bytes",
                                         "--max-file-bytes",
                                         "--gpu-layers",
                                         "--threads",
                                         "--depth",
                                         "--temperature",
                                         "--seed",
                                         "--checkpoint-cache-bytes",
                                         "--checkpoint-cache-entries",
                                         "--checkpoint-cache-min-tokens",
                                         "--checkpoint-cache-captures"};
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (!strcmp(option, flags[i]))
            return 0;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
        if (!strcmp(option, values[i]))
            return 1;
    return -1;
}

static forge_status prepare_config(int argc, char **argv, forge_config *config,
                                   const char **workspace, int *early_exit, forge_error *e) {
    const char *profile = NULL, *explicit_config = NULL;
    bool no_config = false;
    *workspace = ".";
    *early_exit = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-')
            continue;
        int arity = option_arity(a);
        if (arity < 0)
            return fg_error(e, FORGE_ERR_ARGUMENT, "Unknown option: %s", a);
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            *early_exit = 1;
            return FORGE_OK;
        }
        if (!strcmp(a, "--version")) {
            *early_exit = 2;
            return FORGE_OK;
        }
        if (!strcmp(a, "--no-config"))
            no_config = true;
        if (!arity)
            continue;
        if (i + 1 >= argc)
            return fg_error(e, FORGE_ERR_ARGUMENT, "Missing value for %s", a);
        const char *value = argv[++i];
        if (!strcmp(a, "--workspace"))
            *workspace = value;
        else if (!strcmp(a, "--profile")) {
            if (profile)
                return fg_error(e, FORGE_ERR_ARGUMENT, "Choose only one --profile file");
            profile = value;
        } else if (!strcmp(a, "--config")) {
            if (explicit_config)
                return fg_error(e, FORGE_ERR_ARGUMENT, "Choose only one --config file");
            explicit_config = value;
        }
    }
    if (no_config && explicit_config)
        return fg_error(e, FORGE_ERR_ARGUMENT, "--no-config cannot be combined with --config");
    if (profile) {
        forge_status status = forge_config_load(config, profile, e);
        if (status != FORGE_OK)
            return status;
    }
    if (explicit_config)
        return forge_config_load(config, explicit_config, e);
    if (no_config)
        return FORGE_OK;
    char root[FG_PATH_MAX], path[FG_PATH_MAX];
    if (!fg_workspace(*workspace, root, e))
        return e && e->code ? e->code : FORGE_ERR_IO;
    if (!fg_path_join(path, root, "forge.toml"))
        return fg_error(e, FORGE_ERR_LIMIT, "Workspace configuration path is too long");
#ifdef _WIN32
    struct _stat64 st;
    int exists = _stat64(path, &st);
#else
    struct stat st;
    int exists = stat(path, &st);
#endif
    if (exists != 0) {
        if (errno == ENOENT)
            return FORGE_OK;
        return fg_error(e, FORGE_ERR_IO, "Cannot inspect workspace configuration: %s", path);
    }
    return forge_config_load(config, path, e);
}

static const char *fit_name(forge_memory_fit fit) {
    switch (fit) {
    case FORGE_FIT_ESTIMATED:
        return "estimated";
    case FORGE_FIT_INSUFFICIENT:
        return "insufficient";
    default:
        return "unknown";
    }
}

static void nullable_uint(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, bool known,
                          uint64_t value) {
    if (known)
        yyjson_mut_obj_add_uint(doc, object, key, value);
    else
        yyjson_mut_obj_add_null(doc, object, key);
}

static forge_status hardware_report(const forge_config *config, bool json, forge_error *e) {
    if (config->model.script_path)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "hardware-plan accepts a real local GGUF, "
                        "not a simulated --script fixture");
    forge_hardware hardware;
    forge_model_requirements requirements = {0};
    forge_hardware_plan_result plan = {0};
    forge_status status = forge_hardware_detect(&hardware, e);
    if (status != FORGE_OK)
        return status;
    if (config->model.model_path) {
        status = forge_hardware_model_file(config->model.model_path, &requirements, e);
        if (status != FORGE_OK)
            return status;
    } else {
        snprintf(requirements.note, sizeof(requirements.note),
                 "No model was selected; "
                 "model and KV memory requirements are unknown.");
    }
    status = forge_hardware_plan(&hardware, &requirements, config->model.context_tokens,
                                 config->limits.output_reserve, &plan, e);
    if (status != FORGE_OK)
        return status;
    if (!json) {
        printf("CPU: %s (%s); logical CPUs=%u; feature mask=%u\n", hardware.cpu_name,
               hardware.cpu_arch, hardware.logical_cpus, hardware.cpu_features);
        if (hardware.ram_available_known && hardware.ram_total_known)
            printf("RAM: %.2f GiB available / %.2f GiB total\n",
                   (double)hardware.ram_available_bytes / 1073741824.0,
                   (double)hardware.ram_total_bytes / 1073741824.0);
        else
            puts("RAM: measurement unavailable or incomplete");
        if (!hardware.gpu_detection_available)
            puts("GPU: detection unavailable in this build");
        else if (!hardware.gpu_count)
            puts("GPU: no usable local inference device detected");
        for (size_t i = 0; i < hardware.gpu_count; i++) {
            const forge_gpu_info *gpu = &hardware.gpus[i];
            printf("GPU %zu: %s%s\n", i, gpu->name,
                   gpu->unified_memory
                       ? " (shared RAM)"
                       : (gpu->memory_is_budget ? " (process working-set budget)" : ""));
            if (gpu->memory_known)
                printf("  %.2f GiB available / %.2f GiB total\n",
                       (double)gpu->available_bytes / 1073741824.0,
                       (double)gpu->total_bytes / 1073741824.0);
            else
                puts("  memory measurement unavailable");
        }
        if (requirements.model_bytes_known)
            printf("Model: %.2f GiB %s\n", (double)requirements.model_bytes / 1073741824.0,
                   requirements.tensor_bytes_known ? "tensor bytes" : "file-size proxy");
        printf("%s\n", requirements.note);
        printf("Recommendation: context=%zu gpu_layers=%d threads=%d fit=%s\n", plan.context_tokens,
               plan.gpu_layers, plan.threads, fit_name(plan.fit));
        if (plan.kv_estimate_available)
            printf("Estimated f16 KV payload: %.2f GiB\n",
                   (double)plan.estimated_kv_bytes / 1073741824.0);
        else
            puts("KV payload: unknown");
        printf("%s\n", plan.assumptions);
        return FORGE_OK;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc)
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate hardware report");
    yyjson_mut_val *root = yyjson_mut_obj(doc), *host = yyjson_mut_obj(doc);
    yyjson_mut_val *gpus = yyjson_mut_arr(doc), *model = yyjson_mut_obj(doc);
    yyjson_mut_val *recommendation = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_uint(doc, root, "schema_version", 1);
    yyjson_mut_obj_add_str(doc, root, "type", "hardware_plan");
    yyjson_mut_obj_add_val(doc, root, "hardware", host);
    yyjson_mut_obj_add_strcpy(doc, host, "cpu_arch", hardware.cpu_arch);
    yyjson_mut_obj_add_strcpy(doc, host, "cpu_name", hardware.cpu_name);
    yyjson_mut_obj_add_uint(doc, host, "logical_cpus", hardware.logical_cpus);
    yyjson_mut_obj_add_uint(doc, host, "cpu_features", hardware.cpu_features);
    yyjson_mut_obj_add_bool(doc, host, "ram_total_known", hardware.ram_total_known);
    yyjson_mut_obj_add_bool(doc, host, "ram_available_known", hardware.ram_available_known);
    nullable_uint(doc, host, "ram_total_bytes", hardware.ram_total_known, hardware.ram_total_bytes);
    nullable_uint(doc, host, "ram_available_bytes", hardware.ram_available_known,
                  hardware.ram_available_bytes);
    yyjson_mut_obj_add_bool(doc, host, "gpu_detection_available", hardware.gpu_detection_available);
    yyjson_mut_obj_add_bool(doc, host, "gpu_list_truncated", hardware.gpu_list_truncated);
    yyjson_mut_obj_add_val(doc, host, "gpus", gpus);
    for (size_t i = 0; i < hardware.gpu_count; i++) {
        const forge_gpu_info *gpu = &hardware.gpus[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_arr_add_val(gpus, item);
        yyjson_mut_obj_add_uint(doc, item, "index", i);
        yyjson_mut_obj_add_strcpy(doc, item, "name", gpu->name);
        yyjson_mut_obj_add_bool(doc, item, "unified_memory", gpu->unified_memory);
        yyjson_mut_obj_add_bool(doc, item, "memory_is_budget", gpu->memory_is_budget);
        yyjson_mut_obj_add_bool(doc, item, "memory_known", gpu->memory_known);
        nullable_uint(doc, item, "total_bytes", gpu->memory_known, gpu->total_bytes);
        nullable_uint(doc, item, "available_bytes", gpu->memory_known, gpu->available_bytes);
    }
    yyjson_mut_obj_add_val(doc, root, "model", model);
    if (config->model.model_path)
        yyjson_mut_obj_add_strcpy(doc, model, "path", config->model.model_path);
    else
        yyjson_mut_obj_add_null(doc, model, "path");
    yyjson_mut_obj_add_bool(doc, model, "metadata_available", requirements.metadata_available);
    yyjson_mut_obj_add_bool(doc, model, "model_bytes_known", requirements.model_bytes_known);
    yyjson_mut_obj_add_bool(doc, model, "tensor_bytes_known", requirements.tensor_bytes_known);
    yyjson_mut_obj_add_bool(doc, model, "kv_bytes_known", requirements.kv_bytes_known);
    yyjson_mut_obj_add_uint(doc, model, "file_bytes", requirements.file_bytes);
    nullable_uint(doc, model, "model_bytes", requirements.model_bytes_known,
                  requirements.model_bytes);
    nullable_uint(doc, model, "kv_bytes_per_token", requirements.kv_bytes_known,
                  requirements.kv_bytes_per_token);
    yyjson_mut_obj_add_strcpy(doc, model, "architecture", requirements.architecture);
    yyjson_mut_obj_add_uint(doc, model, "layer_count", requirements.layer_count);
    yyjson_mut_obj_add_uint(doc, model, "training_context", requirements.training_context);
    yyjson_mut_obj_add_strcpy(doc, model, "note", requirements.note);
    yyjson_mut_obj_add_val(doc, root, "plan", recommendation);
    yyjson_mut_obj_add_str(doc, recommendation, "fit", fit_name(plan.fit));
    yyjson_mut_obj_add_uint(doc, recommendation, "context_tokens", plan.context_tokens);
    yyjson_mut_obj_add_int(doc, recommendation, "gpu_layers", plan.gpu_layers);
    yyjson_mut_obj_add_int(doc, recommendation, "gpu_index", plan.gpu_index);
    yyjson_mut_obj_add_int(doc, recommendation, "threads", plan.threads);
    yyjson_mut_obj_add_bool(doc, recommendation, "context_reduced", plan.context_reduced);
    yyjson_mut_obj_add_bool(doc, recommendation, "kv_estimate_available",
                            plan.kv_estimate_available);
    yyjson_mut_obj_add_bool(doc, recommendation, "draft_enabled", plan.draft_enabled);
    yyjson_mut_obj_add_strcpy(doc, recommendation, "kv_format", plan.kv_format);
    nullable_uint(doc, recommendation, "estimated_model_bytes", requirements.model_bytes_known,
                  plan.estimated_model_bytes);
    nullable_uint(doc, recommendation, "estimated_kv_bytes", plan.kv_estimate_available,
                  plan.estimated_kv_bytes);
    yyjson_mut_obj_add_uint(doc, recommendation, "host_reserve_bytes", plan.host_reserve_bytes);
    yyjson_mut_obj_add_uint(doc, recommendation, "gpu_reserve_bytes", plan.gpu_reserve_bytes);
    yyjson_mut_obj_add_uint(doc, recommendation, "host_headroom_bytes", plan.host_headroom_bytes);
    yyjson_mut_obj_add_uint(doc, recommendation, "gpu_headroom_bytes", plan.gpu_headroom_bytes);
    yyjson_mut_obj_add_strcpy(doc, recommendation, "assumptions", plan.assumptions);
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!text)
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot encode hardware report");
    puts(text);
    free(text);
    return FORGE_OK;
}

static forge_status auto_hardware(forge_config *config, forge_error *e) {
    if (config->model.gpu_layers != FORGE_GPU_LAYERS_AUTO)
        return FORGE_OK;
    if (config->model.script_path) {
        config->model.gpu_layers = 0;
        fprintf(stderr, "forge: auto placement is disabled for the explicit simulated fixture\n");
        return FORGE_OK;
    }
    if (!config->model.model_path)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Automatic hardware planning requires a model path");
    forge_hardware hardware;
    forge_model_requirements requirements;
    forge_hardware_plan_result plan = {0};
    forge_status status = forge_hardware_detect(&hardware, e);
    if (status == FORGE_OK)
        status = forge_hardware_model_file(config->model.model_path, &requirements, e);
    if (status == FORGE_OK)
        status = forge_hardware_plan(&hardware, &requirements, config->model.context_tokens,
                                     config->limits.output_reserve, &plan, e);
    if (status != FORGE_OK)
        return status;
    fprintf(stderr, "forge: hardware auto: context=%zu gpu_layers=%d threads=%d fit=%s\n%s\n%s\n",
            plan.context_tokens, plan.gpu_layers,
            config->model.threads ? config->model.threads : plan.threads, fit_name(plan.fit),
            requirements.note, plan.assumptions);
    if (plan.context_reduced)
        fprintf(stderr, "forge: hardware planner reduced context from %zu to %zu tokens\n",
                config->model.context_tokens, plan.context_tokens);
    if (plan.fit == FORGE_FIT_UNKNOWN)
        fprintf(stderr,
                "forge: warning: model/context fit is unknown; using conservative CPU settings\n");
    if (plan.fit == FORGE_FIT_INSUFFICIENT)
        return fg_error(e, FORGE_ERR_LIMIT,
                        "Automatic planning found insufficient memory "
                        "even at the reduced context. Choose a smaller model; explicit numeric "
                        "--gpu-layers bypasses this heuristic if you have measured a valid fit");
    config->model.gpu_layers = plan.gpu_layers;
    config->model.context_tokens = config->limits.context_tokens = plan.context_tokens;
    if (!config->model.threads)
        config->model.threads = plan.threads;
    return forge_config_validate(config, e);
}
static void events(const forge_event *event, void *u) {
    bool json = *(bool *)u;
    if (json) {
        puts(event->json);
        fflush(stdout);
        return;
    }
    yyjson_doc *d = yyjson_read(event->json, strlen(event->json), 0);
    yyjson_val *data = yyjson_obj_get(yyjson_doc_get_root(d), "data");
    if (!strcmp(event->type, "message"))
        printf("%s\n", yyjson_get_str(data));
    else if (!strcmp(event->type, "tool_call"))
        fprintf(stderr, "  tool: %s\n", fg_json_str(data, "tool"));
    else if (!strcmp(event->type, "tool_result"))
        fprintf(stderr, "  %s\n", fg_json_str(data, "output"));
    else if (!strcmp(event->type, "validation_command_start")) {
        const char *stage = fg_json_str(data, "stage");
        fprintf(stderr, "  validation: %s\n", stage ? stage : "command");
    } else if (!strcmp(event->type, "validation_result")) {
        const char *summary = fg_json_str(data, "summary");
        if (summary)
            fprintf(stderr, "  validation result: %s\n", summary);
    } else if (!strcmp(event->type, "watch_warning")) {
        const char *reason = fg_json_str(data, "watch_fallback");
        fprintf(stderr, "  watch unavailable; using source scans: %s\n", reason ? reason : "");
    }
    yyjson_doc_free(d);
}
static bool tokens(const char *p, size_t n, void *u) {
    (void)u;
    fwrite(p, 1, n, stdout);
    fflush(stdout);
    return !stop_requested;
}
static int failed(const forge_error *e) {
    fprintf(stderr, "forge: %s: %s\n", forge_status_string(e->code), e->message);
    return e->code == FORGE_ERR_ARGUMENT ? 2 : 1;
}
static int watch_repository(const char *workspace, uint64_t wall_ms, bool json) {
    forge_error error = {0};
    uint64_t start = fg_now_ms();
    uint64_t deadline = wall_ms > UINT64_MAX - start ? UINT64_MAX : start + wall_ms;
    forge_repo *repo = forge_repo_open(workspace, &error);
    if (!repo)
        return failed(&error);
    fg_repo_change change = {0};
    fg_repo_monitor *monitor =
        fg_repo_monitor_create(repo, workspace, cancelled, NULL, deadline, true, &change, &error);
    if (!monitor) {
        forge_repo_close(repo);
        return failed(&error);
    }
    forge_status status = FORGE_OK;
    int written =
        json ? puts(change.json)
             : printf("Watch ready; generation=%llu\n", (unsigned long long)change.generation);
    if (written < 0 || fflush(stdout) != 0) {
        status = fg_error(&error, FORGE_ERR_IO, "Cannot write initial watch report");
        fg_repo_change_free(&change);
        goto finish_watch;
    }
    fg_repo_change_free(&change);
    while (!stop_requested && fg_now_ms() < deadline) {
        uint64_t now = fg_now_ms();
        if (now >= deadline)
            break;
        status = fg_repo_monitor_poll(monitor, FG_MIN(UINT64_C(250), deadline - now), false,
                                      &change, &error);
        if (status != FORGE_OK) {
            if (status == FORGE_ERR_CANCELLED && (stop_requested || fg_now_ms() >= deadline))
                status = FORGE_OK;
            fg_repo_change_free(&change);
            break;
        }
        if (change.changed || change.full_scan || change.delta_scan || change.reopened) {
            if (json)
                written = puts(change.json);
            else
                written = printf("%zu file signals; index=%s generation=%llu%s\n", change.events,
                                 change.full_scan    ? "full"
                                 : change.delta_scan ? "delta"
                                                     : "none",
                                 (unsigned long long)change.generation,
                                 change.reopened ? " (watch reopened)" : "");
            if (written < 0 || fflush(stdout) != 0) {
                status = fg_error(&error, FORGE_ERR_IO, "Cannot write watch report");
                fg_repo_change_free(&change);
                break;
            }
        }
        fg_repo_change_free(&change);
    }
finish_watch:
    fg_repo_monitor_destroy(monitor);
    forge_repo_close(repo);
    return status == FORGE_OK ? 0 : failed(&error);
}
static int cli_main(int argc, char **argv, forge_config *config) {
    forge_error error = {0};
    forge_agent_config ac = {0};
    int early_exit = 0;
    if (prepare_config(argc, argv, config, &ac.workspace, &early_exit, &error) != FORGE_OK)
        return failed(&error);
    if (early_exit == 1) {
        usage();
        return 0;
    }
    if (early_exit == 2) {
        puts(FORGE_VERSION);
        return 0;
    }
    forge_model_config mc = config->model;
    ac.limits = config->limits;
    ac.semantic_output = config->semantic_output;
    ac.compact_context = config->compact_context;
    ac.cancelled = cancelled;
    const char *command = NULL, *argument = NULL;
    bool json = false;
    bool explicit_model = false, explicit_script = false;
    int depth = 1;
    char input[8192];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "--version")) {
            puts(FORGE_VERSION);
            return 0;
        }
        if (a[0] != '-') {
            if (!command)
                command = a;
            else if (!argument)
                argument = a;
            else {
                fg_error(&error, FORGE_ERR_ARGUMENT, "Quote the task as a single argument");
                return failed(&error);
            }
            continue;
        }
        if (!strcmp(a, "--allow-write")) {
            ac.allow_write = true;
            continue;
        }
        if (!strcmp(a, "--no-config"))
            continue;
        if (!strcmp(a, "--no-auto-validation")) {
            ac.skip_validation = true;
            continue;
        }
        if (!strcmp(a, "--allow-exec")) {
            ac.allow_exec = true;
            continue;
        }
        if (!strcmp(a, "--json")) {
            json = true;
            continue;
        }
        if (!strcmp(a, "--no-kv-reuse")) {
            mc.reuse_prefix = false;
            continue;
        }
        if (!strcmp(a, "--checkpoint-cache") || !strcmp(a, "--no-checkpoint-cache")) {
            config->checkpoint_cache_enabled = !strcmp(a, "--checkpoint-cache");
            continue;
        }
        if (!strcmp(a, "--grammar-first")) {
            mc.grammar_fast_path = false;
            continue;
        }
        if (!strcmp(a, "--no-semantic")) {
            ac.semantic_output = false;
            continue;
        }
        if (!strcmp(a, "--no-compaction")) {
            ac.compact_context = false;
            continue;
        }
        if (i + 1 >= argc) {
            fg_error(&error, FORGE_ERR_ARGUMENT, "Missing value for %s", a);
            return failed(&error);
        }
        const char *value = argv[++i];
        if (!strcmp(a, "--profile") || !strcmp(a, "--config"))
            continue; /* Already overlaid, before any CLI settings. */
        if (!strcmp(a, "--workspace"))
            ac.workspace = value;
        else if (!strcmp(a, "--model")) {
            mc.model_path = value;
            explicit_model = true;
        } else if (!strcmp(a, "--script")) {
            mc.script_path = value;
            explicit_script = true;
        } else if (!strcmp(a, "--chat-template"))
            mc.chat_template = value;
        else if (!strcmp(a, "--temperature")) {
            char *end = NULL;
            errno = 0;
            float temperature = strtof(value, &end);
            if (errno || end == value || *end || !isfinite(temperature)) {
                fg_error(&error, FORGE_ERR_ARGUMENT, "Invalid numeric value for %s", a);
                return failed(&error);
            }
            mc.temperature = temperature;
        } else {
            size_t n = 0;
            if (!strcmp(a, "--gpu-layers") && !strcmp(value, "auto")) {
                mc.gpu_layers = FORGE_GPU_LAYERS_AUTO;
                continue;
            }
            if (!strcmp(a, "--gpu-layers") && !strcmp(value, "-1")) {
                mc.gpu_layers = -1;
                continue;
            }
            if (!number(value, &n)) {
                fg_error(&error, FORGE_ERR_ARGUMENT, "Invalid numeric value for %s", a);
                return failed(&error);
            }
            if (!strcmp(a, "--context"))
                mc.context_tokens = ac.limits.context_tokens = n;
            else if (!strcmp(a, "--output-reserve"))
                ac.limits.output_reserve = n;
            else if (!strcmp(a, "--max-turns"))
                ac.limits.max_turns = n;
            else if (!strcmp(a, "--max-tokens"))
                ac.limits.max_generated_tokens = n;
            else if (!strcmp(a, "--max-input"))
                ac.limits.max_input_tokens = n;
            else if (!strcmp(a, "--timeout-ms"))
                ac.limits.command_timeout_ms = n;
            else if (!strcmp(a, "--wall-ms"))
                ac.limits.wall_timeout_ms = n;
            else if (!strcmp(a, "--max-tool-bytes"))
                ac.limits.max_tool_bytes = n;
            else if (!strcmp(a, "--max-file-bytes"))
                ac.limits.max_file_bytes = n;
            else if (!strcmp(a, "--checkpoint-cache-bytes"))
                config->checkpoint_cache.max_bytes = n;
            else if (!strcmp(a, "--checkpoint-cache-entries"))
                config->checkpoint_cache.max_entries = n;
            else if (!strcmp(a, "--checkpoint-cache-min-tokens"))
                config->checkpoint_cache.min_prefix_tokens = n;
            else if (!strcmp(a, "--checkpoint-cache-captures"))
                config->checkpoint_cache.max_captures_per_prompt = n;
            else if (!strcmp(a, "--gpu-layers") && n <= INT_MAX)
                mc.gpu_layers = (int)n;
            else if (!strcmp(a, "--threads") && n <= INT_MAX)
                mc.threads = (int)n;
            else if (!strcmp(a, "--seed") && n <= UINT32_MAX)
                mc.seed = (uint32_t)n;
            else if (!strcmp(a, "--depth") && n <= 3)
                depth = (int)n;
            else {
                fg_error(&error, FORGE_ERR_ARGUMENT, "Unknown option or out-of-range value: %s", a);
                return failed(&error);
            }
        }
    }
    if (explicit_script && !explicit_model)
        mc.model_path = NULL; /* Explicit test selection overrides a configured real model. */
    if (explicit_model && !explicit_script)
        mc.script_path = NULL;
    config->model = mc;
    config->limits = ac.limits;
    config->semantic_output = ac.semantic_output;
    config->compact_context = ac.compact_context;
    if (forge_config_validate(config, &error) != FORGE_OK ||
        forge_config_check_exec(config, ac.allow_exec, &error) != FORGE_OK)
        return failed(&error);
    if (!command) {
        if (!mc.model_path && !mc.script_path) {
            usage();
            return 0;
        }
        fprintf(stderr, "Task> ");
        if (!fgets(input, sizeof(input), stdin))
            return 0;
        input[strcspn(input, "\r\n")] = 0;
        command = "run";
        argument = input;
    }
    if (!strcmp(command, "replay")) {
        if (!argument) {
            usage();
            return 2;
        }
        forge_status s = forge_replay(argument, events, &json, &error);
        return s == FORGE_OK ? 0 : failed(&error);
    }
    if (!strcmp(command, "hardware-plan")) {
        if (argument) {
            fg_error(&error, FORGE_ERR_ARGUMENT,
                     "hardware-plan takes --model PATH, "
                     "not a positional argument");
            return failed(&error);
        }
        forge_status s = hardware_report(config, json, &error);
        return s == FORGE_OK ? 0 : failed(&error);
    }
    if (!strcmp(command, "validate")) {
        const char *paths[] = {argument};
        char *report = NULL;
        forge_status s = forge_verify_workspace(&ac, argument ? paths : NULL, argument ? 1 : 0,
                                                events, &json, &report, &error);
        if (report && !json) {
            yyjson_doc *doc = yyjson_read(report, strlen(report), 0);
            const char *session = fg_json_str(doc ? yyjson_doc_get_root(doc) : NULL, "session");
            if (session)
                fprintf(stderr, "Session: %s\n", session);
            yyjson_doc_free(doc);
        }
        forge_free(report);
        return s == FORGE_OK ? 0 : failed(&error);
    }
    if (!strcmp(command, "stats") || !strcmp(command, "context")) {
        if (!argument) {
            usage();
            return 2;
        }
        char path[FG_PATH_MAX];
        if (!fg_path_join(path, argument,
                          !strcmp(command, "stats") ? "metrics.json" : "context/latest.json"))
            return 2;
        char *s = fg_read_file(path, FG_MAX_JSON, NULL, &error);
        if (!s)
            return failed(&error);
        puts(s);
        free(s);
        return 0;
    }
    if (!strcmp(command, "watch")) {
        if (argument) {
            fg_error(&error, FORGE_ERR_ARGUMENT, "watch does not take a positional argument");
            return failed(&error);
        }
        return watch_repository(ac.workspace, ac.limits.wall_timeout_ms, json);
    }
    if (!strcmp(command, "index") || !strcmp(command, "index-info") ||
        !strcmp(command, "validation-plan") || !strcmp(command, "inspect") ||
        !strcmp(command, "references") || !strcmp(command, "search") ||
        !strcmp(command, "retrieve")) {
        if (strcmp(command, "index") && strcmp(command, "validation-plan") && !argument) {
            usage();
            return 2;
        }
        bool retrieving = !strcmp(command, "retrieve");
        uint64_t retrieval_deadline = 0;
        if (retrieving) {
            uint64_t now = fg_now_ms();
            retrieval_deadline = ac.limits.wall_timeout_ms > UINT64_MAX - now
                                     ? UINT64_MAX
                                     : now + ac.limits.wall_timeout_ms;
        }
        forge_repo *r = forge_repo_open(ac.workspace, &error);
        if (!r)
            return failed(&error);
        bool delta = !strcmp(command, "index") && argument;
        const char *index_paths[] = {argument};
        forge_status indexed =
            retrieving
                ? fg_repo_index_until(r, NULL, 0, true, retrieval_deadline, cancelled, NULL, &error)
            : delta ? forge_repo_index_paths(r, index_paths, 1, &error)
                    : forge_repo_index(r, &error);
        if (indexed != FORGE_OK) {
            forge_repo_close(r);
            return failed(&error);
        }
        char *text = NULL;
        if (!strcmp(command, "index")) {
            if (json)
                printf("{\"index_mode\":\"%s\",\"generation\":%llu,\"paths\":%u}\n",
                       delta ? "delta" : "full", (unsigned long long)forge_repo_generation(r),
                       delta ? 1u : 0u);
            else
                printf("Indexed repository (%s); generation=%llu\n", delta ? "delta" : "full",
                       (unsigned long long)forge_repo_generation(r));
        } else if (!strcmp(command, "index-info"))
            text = forge_repo_index_describe(r, argument, &error);
        else if (!strcmp(command, "validation-plan")) {
            const char *paths[] = {argument};
            text = forge_repo_validation_plan(r, argument ? paths : NULL, argument ? 1 : 0, &error);
        } else if (!strcmp(command, "inspect"))
            text = forge_repo_inspect(r, argument, depth, &error);
        else if (!strcmp(command, "references"))
            text = forge_repo_references(r, argument, &error);
        else if (!strcmp(command, "retrieve")) {
            forge_retrieval_options options = forge_default_retrieval_options();
            options.graph_depth = (size_t)depth;
            options.max_output_bytes = FG_MIN(options.max_output_bytes, ac.limits.max_tool_bytes);
            options.cancelled = cancelled;
            options.deadline_ms = retrieval_deadline;
            text = forge_repo_retrieve(r, argument, &options, NULL, &error);
        } else
            text = fg_repo_search(r, argument, 50, &error);
        if (text) {
            puts(text);
            free(text);
        }
        forge_repo_close(r);
        return error.code ? failed(&error) : 0;
    }
    if (strcmp(command, "run") && strcmp(command, "complete") && strcmp(command, "bench")) {
        fg_error(&error, FORGE_ERR_ARGUMENT, "Unknown command: %s", command);
        return failed(&error);
    }
    if (!argument) {
        usage();
        return 2;
    }
    yyjson_doc *benchmark = NULL;
    yyjson_val *verify = NULL;
    if (!strcmp(command, "bench")) {
        if (!ac.allow_exec) {
            fg_error(&error, FORGE_ERR_POLICY,
                     "Bench verification executes code; --allow-exec is required");
            return failed(&error);
        }
        char *text = fg_read_file(argument, 1024 * 1024, NULL, &error);
        if (!text)
            return failed(&error);
        benchmark = yyjson_read(text, strlen(text), 0);
        free(text);
        yyjson_val *o = benchmark ? yyjson_doc_get_root(benchmark) : NULL;
        argument = fg_json_str(o, "prompt");
        verify = yyjson_obj_get(o, "verify");
        if (!argument || !yyjson_is_arr(verify) || yyjson_arr_size(verify) < 1 ||
            yyjson_arr_size(verify) > 64) {
            yyjson_doc_free(benchmark);
            fg_error(&error, FORGE_ERR_PARSE, "Task manifest requires prompt and verify argv");
            return failed(&error);
        }
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(verify, i, n,
                           item) if (!yyjson_is_str(item) ||
                                     yyjson_get_len(item) != strlen(yyjson_get_str(item))) {
            yyjson_doc_free(benchmark);
            fg_error(&error, FORGE_ERR_PARSE, "verify arguments must be strings");
            return failed(&error);
        }
    }
    if (auto_hardware(config, &error) != FORGE_OK) {
        yyjson_doc_free(benchmark);
        return failed(&error);
    }
    mc = config->model;
    ac.limits = config->limits;
    ac.model = forge_model_load(&mc, &error);
    if (!ac.model) {
        yyjson_doc_free(benchmark);
        return failed(&error);
    }
    if (config->checkpoint_cache_enabled &&
        forge_checkpoint_cache_configure(ac.model, &config->checkpoint_cache, &error) != FORGE_OK) {
        forge_model_destroy(ac.model);
        yyjson_doc_free(benchmark);
        return failed(&error);
    }
    if (!strcmp(command, "complete")) {
        forge_metrics metrics;
        forge_status s;
        if (config->checkpoint_cache_enabled) {
            char workspace[FG_PATH_MAX];
            if (!fg_workspace(ac.workspace, workspace, &error)) {
                forge_model_destroy(ac.model);
                return failed(&error);
            }
            size_t anchor = strlen(argument);
            forge_checkpoint_cache_request request = {workspace, "cli.complete.v1", 0, &anchor, 1};
            s = forge_complete_with_cache(ac.model, argument, &request, ac.limits.output_reserve,
                                          tokens, NULL, &metrics, &error);
        } else
            s = forge_complete(ac.model, argument, ac.limits.output_reserve, tokens, NULL, &metrics,
                               &error);
        puts("");
        char *m = fg_metrics_json(&metrics, s);
        if (m) {
            fprintf(stderr, "%s\n", m);
            free(m);
        }
        forge_model_destroy(ac.model);
        return s == FORGE_OK ? 0 : failed(&error);
    }
    forge_agent *agent = forge_agent_create(&ac, &error);
    if (!agent) {
        forge_model_destroy(ac.model);
        yyjson_doc_free(benchmark);
        return failed(&error);
    }
    forge_status s = forge_agent_run(agent, argument, events, &json, &error);
    if (s == FORGE_OK && benchmark) {
        const char *args[65] = {0};
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(verify, i, n, item) args[i] = yyjson_get_str(item);
        fg_process_result result = {0};
        s = fg_process(ac.workspace, args, ac.limits.command_timeout_ms, ac.limits.max_tool_bytes,
                       cancelled, NULL, &result, &error);
        bool passed =
            s == FORGE_OK && result.exit_code == 0 && !result.timed_out && !result.cancelled;
        char report[256];
        snprintf(report, sizeof(report),
                 "{\"type\":\"benchmark\",\"passed\":%s,\"exit_code\":%d,\"simulated\":%s}",
                 passed ? "true" : "false", result.exit_code,
                 forge_agent_metrics(agent)->simulated ? "true" : "false");
        puts(report);
        char path[FG_PATH_MAX];
        fg_path_join(path, forge_agent_session(agent), "benchmark.json");
        fg_write_file(path, report, strlen(report), NULL);
        fg_path_join(path, forge_agent_session(agent), "verification.stdout");
        if (result.out)
            fg_write_file(path, result.out, result.out_len, NULL);
        fg_path_join(path, forge_agent_session(agent), "verification.stderr");
        if (result.err)
            fg_write_file(path, result.err, result.err_len, NULL);
        if (!passed)
            s = fg_error(&error, FORGE_ERR_CONFLICT, "Benchmark verification failed");
        fg_process_free(&result);
    }
    fprintf(stderr, "Session: %s\n", forge_agent_session(agent));
    const forge_metrics *m = forge_agent_metrics(agent);
    fprintf(stderr, "%s turns=%zu prompt=%zu generated=%zu cached=%zu elapsed=%.0fms\n",
            m->simulated ? "SIMULATED" : "INFERENCE", m->turns, m->prompt_tokens,
            m->generated_tokens, m->cached_tokens, m->duration_ms);
    forge_agent_destroy(agent);
    forge_model_destroy(ac.model);
    yyjson_doc_free(benchmark);
    return s == FORGE_OK ? 0 : failed(&error);
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    forge_config config;
    forge_config_init(&config);
    int status = cli_main(argc, argv, &config);
    forge_config_destroy(&config);
    return status;
}
