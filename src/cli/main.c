#include "internal.h"
#include <errno.h>
#include <signal.h>
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
         "  forge index | inspect SYMBOL | references SYMBOL | search TEXT\n"
         "  forge replay SESSION | stats SESSION | context SESSION\n"
         "  forge bench TASK.json --model model.gguf --allow-exec [options]\n"
         "  forge --model model.gguf                interactive task prompt\n\n"
         "Options:\n"
         "  --workspace PATH     existing repository (default .)\n"
         "  --model PATH         local GGUF; never downloaded by the runtime\n"
         "  --gpu-layers N       offloaded layers, -1 for all (default 0)\n"
         "  --context N          context capacity (default 16384)\n"
         "  --output-reserve N   per-turn generation budget (default 2048)\n"
         "  --max-turns N        hard agent turn limit (default 32)\n"
         "  --max-tokens N       total generated-token limit (default 32768)\n"
         "  --max-input N        total prompt-token limit (default 262144)\n"
         "  --timeout-ms N       command timeout (default 120000)\n"
         "  --wall-ms N          run deadline (default 1800000)\n"
         "  --max-tool-bytes N   captured bytes per output stream (default 65536)\n"
         "  --threads N          inference threads\n"
         "  --chat-template NAME override unsupported model chat template\n"
         "  --allow-write        permit repository patches\n"
         "  --allow-exec         permit UNSANDBOXED commands, including repository code\n"
         "  --json               JSON-lines events\n"
         "  --no-kv-reuse | --no-semantic | --no-compaction  ablations\n"
         "  --grammar-first      disable greedy grammar fast path (ablation)\n"
         "  --script FILE        explicit simulated test backend (not inference)\n"
         "  --depth N            symbol expansion level 0..3\n");
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
int main(int argc, char **argv) {
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    forge_error error = {0};
    forge_model_config mc = forge_default_model_config();
    forge_agent_config ac = {0};
    ac.workspace = ".";
    ac.limits = forge_default_limits();
    ac.semantic_output = true;
    ac.compact_context = true;
    ac.cancelled = cancelled;
    const char *command = NULL, *argument = NULL;
    bool json = false;
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
        if (!strcmp(a, "--workspace"))
            ac.workspace = value;
        else if (!strcmp(a, "--model"))
            mc.model_path = value;
        else if (!strcmp(a, "--script"))
            mc.script_path = value;
        else if (!strcmp(a, "--chat-template"))
            mc.chat_template = value;
        else {
            size_t n = 0;
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
            else if (!strcmp(a, "--gpu-layers") && n <= INT_MAX)
                mc.gpu_layers = (int)n;
            else if (!strcmp(a, "--threads") && n <= INT_MAX)
                mc.threads = (int)n;
            else if (!strcmp(a, "--depth") && n <= 3)
                depth = (int)n;
            else {
                fg_error(&error, FORGE_ERR_ARGUMENT, "Unknown option or out-of-range value: %s", a);
                return failed(&error);
            }
        }
    }
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
    if (!strcmp(command, "index") || !strcmp(command, "inspect") ||
        !strcmp(command, "references") || !strcmp(command, "search")) {
        if (strcmp(command, "index") && !argument) {
            usage();
            return 2;
        }
        forge_repo *r = forge_repo_open(ac.workspace, &error);
        if (!r)
            return failed(&error);
        if (forge_repo_index(r, &error) != FORGE_OK) {
            forge_repo_close(r);
            return failed(&error);
        }
        char *text = NULL;
        if (!strcmp(command, "index")) {
            printf("Indexed repository; generation=%llu\n",
                   (unsigned long long)forge_repo_generation(r));
        } else if (!strcmp(command, "inspect"))
            text = forge_repo_inspect(r, argument, depth, &error);
        else if (!strcmp(command, "references"))
            text = forge_repo_references(r, argument, &error);
        else
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
        yyjson_arr_foreach(verify, i, n, item) if (!yyjson_is_str(item)) {
            yyjson_doc_free(benchmark);
            fg_error(&error, FORGE_ERR_PARSE, "verify arguments must be strings");
            return failed(&error);
        }
    }
    ac.model = forge_model_load(&mc, &error);
    if (!ac.model) {
        yyjson_doc_free(benchmark);
        return failed(&error);
    }
    if (!strcmp(command, "complete")) {
        forge_metrics metrics;
        forge_status s = forge_complete(ac.model, argument, ac.limits.output_reserve, tokens, NULL,
                                        &metrics, &error);
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
