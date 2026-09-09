#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "forge/config.h"
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#define TEST_PATH 4096
#define GIB UINT64_C(1073741824)
static char test_directory[TEST_PATH];

static void path_for(const char *relative, char path[TEST_PATH]) {
    int n = snprintf(path, TEST_PATH, "%s/%s", test_directory, relative);
    assert(n > 0 && n < TEST_PATH);
}

static void write_document(const char *relative, const char *document) {
    char path[TEST_PATH];
    path_for(relative, path);
    FILE *file = fopen(path, "wb");
    assert(file);
    size_t length = strlen(document);
    assert(fwrite(document, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void delete_document(const char *relative) {
    char path[TEST_PATH];
    path_for(relative, path);
    assert(remove(path) == 0);
}

static void create_test_directory(void) {
#ifdef _WIN32
    char temporary[TEST_PATH];
    DWORD n = GetTempPathA(sizeof(temporary), temporary);
    assert(n > 0 && n < sizeof(temporary));
    assert(GetTempFileNameA(temporary, "fgc", 0, test_directory));
    assert(DeleteFileA(test_directory));
    assert(_mkdir(test_directory) == 0);
#else
    strcpy(test_directory, "/tmp/forge-config-XXXXXX");
    assert(mkdtemp(test_directory));
#endif
    char profiles[TEST_PATH];
    path_for("profiles", profiles);
#ifdef _WIN32
    assert(_mkdir(profiles) == 0);
#else
    assert(mkdir(profiles, 0700) == 0);
#endif
}

static void remove_test_directory(void) {
    char profiles[TEST_PATH];
    path_for("profiles", profiles);
#ifdef _WIN32
    assert(_rmdir(profiles) == 0);
    assert(_rmdir(test_directory) == 0);
#else
    assert(rmdir(profiles) == 0);
    assert(rmdir(test_directory) == 0);
#endif
}

static bool paths_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
#ifdef _WIN32
        if (ca == '\\')
            ca = '/';
        if (cb == '\\')
            cb = '/';
        ca = (unsigned char)tolower(ca);
        cb = (unsigned char)tolower(cb);
#endif
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

static forge_status parse(forge_config *config, const char *text, forge_error *error) {
    char source[TEST_PATH];
    path_for("forge.toml", source);
    return forge_config_parse(config, text, strlen(text), source, error);
}

static void rejected(const char *text, const char *expected) {
    forge_config config;
    forge_config_init(&config);
    size_t old_context = config.model.context_tokens;
    forge_error error = {0};
    forge_status status = parse(&config, text, &error);
    if (status == FORGE_OK || !strstr(error.message, expected)) {
        fprintf(stderr, "Unexpected config result: %d / %s; wanted %s\n%s\n", (int)status,
                error.message, expected, text);
        abort();
    }
    assert(error.code == status);
    assert(config.model.context_tokens == old_context);
    assert(!config.model.model_path);
    forge_config_destroy(&config);
}

static void config_values(void) {
    forge_config config;
    forge_config_init(&config);
    forge_error error = {0};
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    const char *document =
        "# Quoted keys, inline tables, bases, underscores and comments are real TOML.\n"
        "model = {path = 'models/model # one.gguf', context = 0x4000, "
        "chat_template = \"\"\"chatml\"\"\", enable_thinking = false}\n"
        "[\"inference\"]\n"
        "gpu_layers = \"auto\"\n"
        "threads = 3\n"
        "seed = 4_294_967_295\n"
        "temperature = 0.25\n"
        "reuse_prefix = false\n"
        "grammar_fast_path = false\n"
        "speculative = false\n"
        "[agent]\n"
        "output_reserve = 1_024\n"
        "max_turns = 50\n"
        "max_tokens = 100_000\n"
        "max_input = 500_000\n"
        "max_tool_bytes = 16_384\n"
        "max_file_bytes = 32_768\n"
        "wall_timeout_ms = 900_000\n"
        "semantic_output = false\n"
        "compact_context = false\n"
        "[tools.shell]\n"
        "timeout = 7\n"
        "network = false\n"
        "[index]\n"
        "languages = [\"go\",]\n";
    assert(parse(&config, document, &error) == FORGE_OK);
    assert(error.code == FORGE_OK && !error.message[0]);
    char expected[TEST_PATH];
    path_for("models/model # one.gguf", expected);
    assert(paths_equal(config.model.model_path, expected));
    assert(!config.checkpoint_cache_enabled);
    assert(parse(&config,
                 "[inference.checkpoints]\nenabled=true\nmax_bytes=1_048_576\n"
                 "max_entries=3\nmin_prefix_tokens=48\nmax_captures_per_prompt=1\n",
                 &error) == FORGE_OK);
    assert(config.checkpoint_cache_enabled && config.checkpoint_cache.max_bytes == 1048576 &&
           config.checkpoint_cache.max_entries == 3 &&
           config.checkpoint_cache.min_prefix_tokens == 48 &&
           config.checkpoint_cache.max_captures_per_prompt == 1);
    assert(parse(&config, "inference.checkpoints.max_entries=0\n", &error) == FORGE_ERR_PARSE);
    assert(config.checkpoint_cache_enabled && config.checkpoint_cache.max_entries == 3);
    assert(parse(&config, "inference.checkpoints.enabled=false\n", &error) == FORGE_OK);
    assert(!config.checkpoint_cache_enabled && config.checkpoint_cache.max_entries == 3);
    assert(!strcmp(config.model.chat_template, "chatml"));
    assert(config.model.thinking == FORGE_THINKING_DISABLED);
    assert(config.model.context_tokens == 16384 && config.limits.context_tokens == 16384);
    assert(config.model.gpu_layers == FORGE_GPU_LAYERS_AUTO);
    assert(config.model.threads == 3 && config.model.seed == UINT32_MAX);
    assert(config.model.temperature == 0.25f);
    assert(!config.model.reuse_prefix && !config.model.grammar_fast_path);
    assert(config.limits.output_reserve == 1024 && config.limits.max_turns == 50);
    assert(config.limits.max_generated_tokens == 100000 &&
           config.limits.max_input_tokens == 500000);
    assert(config.limits.max_tool_bytes == 16384 && config.limits.max_file_bytes == 32768);
    assert(config.limits.command_timeout_ms == 7000 && config.limits.wall_timeout_ms == 900000);
    assert(!config.semantic_output && !config.compact_context);
    assert(forge_config_check_exec(&config, false, &error) == FORGE_OK);
    assert(forge_config_check_exec(&config, true, &error) == FORGE_ERR_POLICY);
    assert(strstr(error.message, "cannot enforce a network sandbox"));
    assert(parse(&config, "tools.shell.network = true\n", &error) == FORGE_OK);
    assert(forge_config_check_exec(&config, true, &error) == FORGE_OK);
    assert(parse(&config, "inference.temperature = 0\ninference.gpu_layers = -1\n", &error) ==
           FORGE_OK);
    assert(config.model.temperature == 0 && config.model.gpu_layers == -1);
    assert(parse(&config, "[model]\npath = \"models/a\\u0062.gguf\"\n", &error) == FORGE_OK);
    path_for("models/ab.gguf", expected);
    assert(paths_equal(config.model.model_path, expected));
    forge_config_destroy(&config);
    forge_config_destroy(&config); /* Destruction is idempotent. */
}

static void config_rejections(void) {
    static const struct {
        const char *text, *expected;
    } cases[] = {
        {"model.context = \"4096\"", "model.context"},
        {"model.context = 4096.0", "model.context"},
        {"model.context = true", "model.context"},
        {"model.context = 127", "model.context"},
        {"model.context = 1048577", "model.context"},
        {"model.context = 1024", "output_reserve"},
        {"model.context = 2026-08-28", "model.context"},
        {"model.path = 1", "model.path"},
        {"model.path = ''", "model.path"},
        {"model.path = \"foo\\u0000bar\"", "model.path"},
        {"model.path = \"foo\\nbar\"", "model.path"},
        {"model.chat_template = ''", "model.chat_template"},
        {"model.enable_thinking = 1", "model.enable_thinking"},
        {"model = []", "model"},
        {"inference.seed = -1", "inference.seed"},
        {"inference.seed = 4294967296", "inference.seed"},
        {"inference.threads = -1", "inference.threads"},
        {"inference.threads = 1025", "inference.threads"},
        {"inference.gpu_layers = -2", "inference.gpu_layers"},
        {"inference.gpu_layers = 65536", "inference.gpu_layers"},
        {"inference.gpu_layers = \"AUTO\"", "inference.gpu_layers"},
        {"inference.gpu_layers = \"all\"", "inference.gpu_layers"},
        {"inference.reuse_prefix = 1", "inference.reuse_prefix"},
        {"inference.checkpoints = true", "inference.checkpoints"},
        {"inference.checkpoints.enabled = 1", "inference.checkpoints.enabled"},
        {"inference.checkpoints.max_bytes = 4095", "inference.checkpoints.max_bytes"},
        {"inference.checkpoints.max_bytes = 1073741825", "inference.checkpoints.max_bytes"},
        {"inference.checkpoints.max_entries = 65", "inference.checkpoints.max_entries"},
        {"inference.checkpoints.min_prefix_tokens = 0", "inference.checkpoints.min_prefix_tokens"},
        {"inference.checkpoints.max_captures_per_prompt = 5",
         "inference.checkpoints.max_captures_per_prompt"},
        {"inference.grammar_fast_path = \"true\"", "inference.grammar_fast_path"},
        {"inference.temperature = nan", "inference.temperature"},
        {"inference.temperature = inf", "inference.temperature"},
        {"inference.temperature = -0.1", "inference.temperature"},
        {"inference.temperature = 2.01", "inference.temperature"},
        {"inference.speculative = true", "not implemented"},
        {"inference.speculative = 0", "boolean"},
        {"inference.draft_model = 'draft.gguf'", "unknown"},
        {"agent.max_turns = 0", "agent.max_turns"},
        {"agent.max_turns = 1001", "agent.max_turns"},
        {"agent.max_tokens = 0", "agent.max_tokens"},
        {"agent.max_input = 2147483648", "agent.max_input"},
        {"agent.output_reserve = 16384", "output_reserve"},
        {"agent.max_tool_bytes = 16777217", "agent.max_tool_bytes"},
        {"agent.max_file_bytes = 0", "agent.max_file_bytes"},
        {"agent.wall_timeout_ms = 604800001", "agent.wall_timeout_ms"},
        {"tools.shell.timeout = 0", "tools.shell.timeout"},
        {"tools.shell.timeout = 86401", "tools.shell.timeout"},
        {"tools.shell.timeout = 1.5", "tools.shell.timeout"},
        {"tools.shell.network = \"false\"", "tools.shell.network"},
        {"tools.shell.allow_exec = true", "unknown"},
        {"agent.allow_write = true", "unknown"},
        {"index.languages = ['go', 'c']", "only [\"go\"]"},
        {"index.languages = ['c']", "only [\"go\"]"},
        {"index.languages = []", "only [\"go\"]"},
        {"index.languages = ['go', 'go']", "only [\"go\"]"},
        {"index.languages = [true]", "only [\"go\"]"},
        {"index.languages = 'go'", "only [\"go\"]"},
        {"agent.max_truns = 10", "unknown"},
        {"[model.extra]\nvalue = 1", "unknown"},
        {"[extension]", "unknown"},
        {"'model.context' = 4096", "unknown"},
        {"[Model]\ncontext=4096", "unknown"},
        {"extends = []", "extends"},
        {"extends = ''", "extends"},
        {"model.context = 4096\nmodel.context = 8192", "forge.toml"},
        {"[model]\ncontext = 4_096_", "forge.toml"},
        {"[model]\npath = \"unterminated", "forge.toml"},
        {"\"model\\u0000\" = {}", "NUL"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        rejected(cases[i].text, cases[i].expected);
    forge_config config;
    forge_config_init(&config);
    char source[TEST_PATH];
    path_for("forge.toml", source);
    forge_error error = {0};
    const char invalid_utf8[] = {'#', (char)0xc0, (char)0xaf};
    assert(forge_config_parse(&config, invalid_utf8, sizeof(invalid_utf8), source, &error) ==
           FORGE_ERR_PARSE);
    const char raw_nul[] = {'#', 0, '\n'};
    assert(forge_config_parse(&config, raw_nul, sizeof(raw_nul), source, &error) ==
           FORGE_ERR_PARSE);
    assert(forge_config_parse(&config, "", (size_t)FORGE_CONFIG_MAX_BYTES + 1, source, &error) ==
           FORGE_ERR_LIMIT);
    assert(forge_config_parse(&config, NULL, 0, source, &error) == FORGE_ERR_ARGUMENT);
    assert(forge_config_parse(&config, "", 0, "", &error) == FORGE_ERR_ARGUMENT);
    assert(forge_config_load(&config, test_directory, &error) == FORGE_ERR_IO);
    assert(forge_config_load(&config, source, &error) == FORGE_ERR_IO);
    char oversized_path[TEST_PATH];
    path_for("oversized.toml", oversized_path);
    FILE *oversized = fopen(oversized_path, "wb");
    assert(oversized);
    assert(fputc('#', oversized) != EOF);
    char block[1024];
    memset(block, ' ', sizeof(block));
    for (size_t i = 0; i < FORGE_CONFIG_MAX_BYTES / sizeof(block); i++)
        assert(fwrite(block, 1, sizeof(block), oversized) == sizeof(block));
    assert(fclose(oversized) == 0);
    assert(forge_config_load(&config, oversized_path, &error) == FORGE_ERR_LIMIT);
    assert(strstr(error.message, "oversized.toml"));
    assert(forge_config_load(&config, oversized_path, NULL) == FORGE_ERR_LIMIT);
    delete_document("oversized.toml");
#ifdef _WIN32
    assert(forge_config_parse(&config, "", 0, "C:relative.toml", &error) == FORGE_ERR_ARGUMENT);
    rejected("model.path = '\\root-relative.gguf'", "root-relative");
#endif
    forge_config_destroy(&config);
}

static void inheritance_and_ownership(void) {
    write_document("profiles/base.toml",
                   "[model]\npath = '../models/base.gguf'\ncontext = 8192\n"
                   "chat_template = 'chatml'\n[inference]\nthreads = 2\ntemperature = 0.25\n");
    write_document("child.toml", "extends = 'profiles/base.toml'\nmodel.context = 32768\n"
                                 "agent.max_turns = 50\n");
    forge_config config;
    forge_config_init(&config);
    config.model.seed = 123; /* Existing overlay survives absent file fields. */
    forge_error error = {0};
    char path[TEST_PATH], expected[TEST_PATH];
    path_for("child.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_OK);
    path_for("models/base.gguf", expected);
    assert(paths_equal(config.model.model_path, expected));
    assert(config.model.context_tokens == 32768 && config.limits.context_tokens == 32768);
    assert(config.model.seed == 123 && config.model.threads == 2);
    assert(config.model.temperature == 0.25f && config.limits.max_turns == 50);

    /* A later project overlay wins; defaults are not reintroduced on each load. */
    write_document("project.toml", "model.context = 16384\ninference.threads = 4\n");
    path_for("project.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_OK);
    assert(config.model.context_tokens == 16384 && config.model.threads == 4);
    assert(config.model.seed == 123 && config.limits.max_turns == 50);
    const char *old_path = config.model.model_path;
    assert(parse(&config, "model.path='changed.gguf'\nagent.max_turns=0", &error) ==
           FORGE_ERR_PARSE);
    assert(config.model.model_path == old_path && paths_equal(old_path, expected));
    assert(config.limits.max_turns == 50);

    write_document("a.toml", "extends='b.toml'\n");
    write_document("b.toml", "extends='./a.toml'\n");
    path_for("a.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_ERR_PARSE);
    assert(strstr(error.message, "cycle"));
    assert(config.model.model_path == old_path);
    write_document("missing-parent.toml", "extends='does-not-exist.toml'\n");
    path_for("missing-parent.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_ERR_IO);
    assert(strstr(error.message, "does-not-exist.toml"));
    assert(config.model.model_path == old_path);

    for (unsigned i = 0; i <= FORGE_CONFIG_MAX_INHERITANCE; i++) {
        char name[64], text[128];
        snprintf(name, sizeof(name), "chain%u.toml", i);
        if (i < FORGE_CONFIG_MAX_INHERITANCE)
            snprintf(text, sizeof(text), "extends='chain%u.toml'\n", i + 1);
        else
            strcpy(text, "agent.max_turns=10\n");
        write_document(name, text);
    }
    path_for("chain0.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_ERR_LIMIT);
    assert(strstr(error.message, "inheritance exceeds"));
    assert(config.model.model_path == old_path);
    path_for("chain1.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_OK); /* Exactly eight files. */
    assert(config.limits.max_turns == 10);

    /* Cross-field checks occur after the whole inheritance chain, allowing the
     * child to replace a parent's context while retaining its reserve. */
    write_document("relations-base.toml", "model.context=512\nagent.output_reserve=1024\n");
    write_document("relations-child.toml", "extends='relations-base.toml'\nmodel.context=4096\n");
    path_for("relations-child.toml", path);
    assert(forge_config_load(&config, path, &error) == FORGE_OK);
    assert(config.model.context_tokens == 4096 && config.limits.output_reserve == 1024);

    /* Explicit CLI values are applied last, with caller-owned strings. */
    char cli_path[] = "cli-model.gguf";
    char cli_template[] = "chatml";
    config.model.model_path = cli_path;
    config.model.chat_template = cli_template;
    config.model.context_tokens = config.limits.context_tokens = 8192;
    config.model.threads = 1;
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    assert(config.model.model_path == cli_path && config.model.threads == 1);
    /* Even a subsequent overlay copies borrowed strings and owns its copies. */
    assert(parse(&config, "inference.seed=456\n", &error) == FORGE_OK);
    assert(config.model.model_path != cli_path && !strcmp(config.model.model_path, cli_path));
    config.model.model_path = cli_path;
    config.model.chat_template = cli_template;
    forge_config_destroy(&config); /* Must not free either stack string. */
    assert(!strcmp(cli_path, "cli-model.gguf"));

    const char *files[] = {
        "profiles/base.toml",  "child.toml",          "project.toml",        "a.toml", "b.toml",
        "missing-parent.toml", "relations-base.toml", "relations-child.toml"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        delete_document(files[i]);
    for (unsigned i = 0; i <= FORGE_CONFIG_MAX_INHERITANCE; i++) {
        char name[64];
        snprintf(name, sizeof(name), "chain%u.toml", i);
        delete_document(name);
    }
}

static void final_override_validation(void) {
    forge_config config;
    forge_config_init(&config);
    forge_error error = {0};
    config.model.context_tokens = 8192;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "agree"));
    config.limits.context_tokens = 8192;
    config.model.temperature = NAN;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    config.model.temperature = 0;
    config.model.thinking = (forge_thinking_mode)99;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    config.model.thinking = FORGE_THINKING_AUTO;
    config.model.model_path = "model.gguf";
    config.model.script_path = "fixture.json";
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    config.model.script_path = NULL;
    config.limits.command_timeout_ms = 1; /* CLI units need not be whole seconds. */
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    config.model.prompt_protocol = (forge_prompt_protocol)99;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "prompt protocol"));
    config.model.prompt_protocol = FORGE_PROMPT_NATIVE;
    config.thought_routed = true;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "cannot be combined"));
    config.thought_routed = false;
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    config.model.prompt_protocol = FORGE_PROMPT_FLATTENED;
    config.shell_network = (forge_shell_network)99;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(forge_config_check_exec(&config, true, &error) == FORGE_ERR_ARGUMENT);
    forge_config_destroy(&config);
    assert(forge_config_check_exec(NULL, true, &error) == FORGE_ERR_ARGUMENT);
}

static forge_model_requirements test_requirements(void) {
    forge_model_requirements requirements = {0};
    requirements.model_bytes = 4 * GIB;
    requirements.model_bytes_known = true;
    requirements.kv_bytes_per_token = 65536;
    requirements.kv_bytes_known = true;
    requirements.layer_count = 32;
    requirements.training_context = 32768;
    return requirements;
}

static forge_hardware test_hardware(void) {
    forge_hardware hardware = {0};
    hardware.logical_cpus = 16;
    hardware.ram_total_bytes = 16 * GIB;
    hardware.ram_available_bytes = 12 * GIB;
    hardware.ram_total_known = hardware.ram_available_known = true;
    return hardware;
}

static void planner_tests(void) {
    forge_hardware hardware = test_hardware();
    forge_model_requirements requirements = test_requirements();
    forge_hardware_plan_result plan;
    forge_error error = {0};
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_ESTIMATED && plan.context_tokens == 16384);
    assert(plan.gpu_layers == 0 && plan.gpu_index == -1 && plan.threads == 8);
    assert(plan.kv_estimate_available && plan.estimated_kv_bytes == GIB);
    assert(!plan.draft_enabled && !strcmp(plan.kv_format, "f16"));
    assert(plan.host_headroom_bytes > 0 && plan.host_reserve_bytes >= GIB);

    hardware.ram_available_bytes = 6 * GIB;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.context_reduced && plan.context_tokens == 4096);
    assert(plan.fit == FORGE_FIT_ESTIMATED);
    hardware.ram_available_bytes = 0;
    assert(forge_hardware_plan(&hardware, &requirements, 8192, 256, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_INSUFFICIENT && plan.context_tokens > 256);
    assert(plan.gpu_layers == 0 && plan.host_headroom_bytes == 0);

    hardware = test_hardware();
    hardware.gpu_detection_available = true;
    hardware.gpu_count = 1;
    hardware.gpus[0] = (forge_gpu_info){"test GPU", 24 * GIB, 20 * GIB, true, false, false};
    assert(forge_hardware_plan(&hardware, &requirements, 32768, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers == -1 && plan.gpu_index == 0 && plan.fit == FORGE_FIT_ESTIMATED);
    assert(plan.gpu_headroom_bytes > 0 && plan.context_tokens == 32768);

    hardware.gpus[0].available_bytes = 3 * GIB;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers > 0 && (size_t)plan.gpu_layers < requirements.layer_count);
    assert(plan.fit == FORGE_FIT_ESTIMATED && plan.host_headroom_bytes > 0);
    hardware.gpus[0].available_bytes = 0;
    hardware.gpu_count = 2;
    hardware.gpus[1] = (forge_gpu_info){"unused GPU", 80 * GIB, 80 * GIB, true, false, false};
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers == 0); /* No memory summing or unselectable GPU assumptions. */

    hardware.gpu_count = 1;
    hardware.gpus[0].available_bytes = 20 * GIB;
    hardware.gpus[0].memory_known = false;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers == 0);
    hardware.gpus[0].memory_known = true;
    hardware.gpu_detection_available = false;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers == 0);

    hardware.gpu_detection_available = true;
    hardware.gpus[0].unified_memory = true;
    hardware.ram_available_bytes = 4 * GIB;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_INSUFFICIENT && plan.gpu_layers == 0);
    hardware.ram_available_bytes = 8 * GIB;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.gpu_layers == -1 && plan.gpu_headroom_bytes <= 8 * GIB);
    hardware.gpus[0].unified_memory = false;
    hardware.gpus[0].memory_is_budget = true;
    hardware.ram_available_bytes = 4 * GIB;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_INSUFFICIENT && plan.gpu_layers == 0);
    hardware.ram_available_bytes = 8 * GIB;

    requirements.kv_bytes_known = false;
    assert(forge_hardware_plan(&hardware, &requirements, 32768, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_UNKNOWN && !plan.kv_estimate_available);
    assert(plan.gpu_layers == 0 && plan.context_tokens == 4096 && plan.context_reduced);
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 8000, &plan, &error) == FORGE_OK);
    assert(plan.context_tokens == 8001 && plan.context_tokens > 8000);

    requirements = test_requirements();
    hardware = (forge_hardware){0};
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_UNKNOWN && plan.threads == 1 && plan.gpu_layers == 0);
    hardware = test_hardware();
    requirements.training_context = 8192;
    assert(forge_hardware_plan(&hardware, &requirements, 32768, 2048, &plan, &error) == FORGE_OK);
    assert(plan.context_tokens == 8192 && plan.context_reduced);
    requirements.training_context = 1024;
    assert(forge_hardware_plan(&hardware, &requirements, 32768, 2048, &plan, &error) ==
           FORGE_ERR_LIMIT);

    requirements = test_requirements();
    requirements.model_bytes = UINT64_MAX;
    requirements.kv_bytes_per_token = UINT64_MAX;
    assert(forge_hardware_plan(&hardware, &requirements, 1048576, 2048, &plan, &error) == FORGE_OK);
    assert(plan.fit == FORGE_FIT_INSUFFICIENT && plan.gpu_layers == 0);
    assert(plan.estimated_kv_bytes == UINT64_MAX);
    requirements = test_requirements();
    requirements.kv_bytes_per_token = 0;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
    requirements = test_requirements();
    assert(forge_hardware_plan(&hardware, &requirements, 127, 16, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 16384, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
    hardware.ram_available_bytes = hardware.ram_total_bytes + 1;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
    hardware = test_hardware();
    hardware.gpu_count = FORGE_HARDWARE_MAX_GPUS + 1;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
    hardware.gpu_count = 1;
    hardware.gpus[0].memory_known = true;
    hardware.gpus[0].available_bytes = 1;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
}

static void metadata_errors(void) {
    char path[TEST_PATH];
    path_for("not-a-model.txt", path);
    write_document("not-a-model.txt", "this is not a GGUF");
    forge_model_requirements requirements;
    forge_error error = {0};
    assert(forge_hardware_model_file(path, &requirements, &error) == FORGE_ERR_MODEL);
    delete_document("not-a-model.txt");
    assert(forge_hardware_model_file(path, &requirements, &error) == FORGE_ERR_IO);
    assert(forge_hardware_model_file(test_directory, &requirements, &error) == FORGE_ERR_IO);
    assert(forge_hardware_model_file(NULL, &requirements, &error) == FORGE_ERR_ARGUMENT);
}

int main(int argc, char **argv) {
    create_test_directory();
    config_values();
    config_rejections();
    inheritance_and_ownership();
    final_override_validation();
    planner_tests();
    metadata_errors();
    remove_test_directory();
    /* Optional metadata-only smoke probe of an already-installed model. CTest
     * supplies no argument and never needs model files, downloads or inference. */
    if (argc == 2) {
        forge_model_requirements requirements;
        forge_error error = {0};
        forge_status status = forge_hardware_model_file(argv[1], &requirements, &error);
        if (status != FORGE_OK) {
            fprintf(stderr, "%s\n", error.message);
            return 1;
        }
        printf("metadata=%d architecture=%s layers=%zu context=%zu model_bytes=%" PRIu64
               " kv_known=%d kv_bytes_per_token=%" PRIu64 "\n%s\n",
               requirements.metadata_available, requirements.architecture, requirements.layer_count,
               requirements.training_context, requirements.model_bytes, requirements.kv_bytes_known,
               requirements.kv_bytes_per_token, requirements.note);
    }
    puts("configuration and hardware planner tests passed");
    return 0;
}
