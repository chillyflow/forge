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
        "repetition_penalty = 1.05\n"
        "repetition_last_n = 64\n"
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
    assert(config.model.repetition_penalty == 1.05f);
    assert(config.model.repetition_last_n == 64);
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
        {"inference.repetition_penalty = 0", "inference.repetition_penalty"},
        {"inference.repetition_penalty = -0.5", "inference.repetition_penalty"},
        {"inference.repetition_penalty = 2.01", "inference.repetition_penalty"},
        {"inference.repetition_penalty = nan", "inference.repetition_penalty"},
        {"inference.repetition_last_n = -1", "inference.repetition_last_n"},
        {"inference.repetition_last_n = 1025", "inference.repetition_last_n"},
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
    config.model.repetition_penalty = 0;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    config.model.repetition_penalty = 1.0f;
    config.model.repetition_last_n = -1;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    config.model.repetition_last_n = 0;
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
    /* The interval search finds the largest fitting 128-token step (7,168);
     * the previous halving walk stopped at the first fit, 4,096. */
    assert(plan.context_reduced && plan.context_tokens == 7168);
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

static void planner_kv_tests(void) {
    forge_error error = {0};
    forge_hardware_plan_result plan;
    /* Retained campaign measurements for qwen3moe (the 2026-09-11-capability
     * environment.json records): 17.28 GiB of tensors, 98,304 f16 KV bytes per
     * token, 22.61 GiB free VRAM, 21.89 GiB free RAM, 31.43 GiB total RAM. */
    forge_hardware hardware = {0};
    hardware.logical_cpus = 24;
    hardware.ram_total_bytes = 31 * GIB + GIB * 43 / 100;
    hardware.ram_available_bytes = 21 * GIB + GIB * 89 / 100;
    hardware.ram_total_known = hardware.ram_available_known = true;
    hardware.gpu_detection_available = true;
    hardware.gpu_count = 1;
    hardware.gpus[0] = (forge_gpu_info){"campaign GPU", 23 * GIB + GIB * 89 / 100,
                                        22 * GIB + GIB * 61 / 100, true, false, false};
    forge_model_requirements requirements = {0};
    requirements.model_bytes = 17 * GIB + GIB * 28 / 100;
    requirements.model_bytes_known = true;
    requirements.kv_bytes_per_token = 98304; /* measured f16 K+V payload per token */
    requirements.kv_bytes_known = true;
    requirements.layer_count = 48;
    requirements.training_context = 262144;

    /* f16: the largest fitting 128-token step below the requested context. The
     * old halving walk stopped at 8,192; the interval search raises it to 8,704
     * because the GPU budget covers it. */
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(plan.context_tokens == 8704 && plan.context_tokens > 8192);
    assert(plan.context_reduced && plan.fit == FORGE_FIT_ESTIMATED);
    assert(plan.gpu_layers == -1 && plan.gpu_index == 0 && !strcmp(plan.kv_format, "f16"));
    assert(plan.estimated_kv_bytes == UINT64_C(98304) * 8704);
    assert(plan.gpu_headroom_bytes > 0 && plan.gpu_headroom_bytes < 256 * 1024 * 1024);

    /* q8_0 packs 32 elements into 34 bytes (2-byte f16 scale + 32 int8 quants),
     * so the measured f16 payload scales by 34/64 - derived from the pinned ggml
     * block layout, never assumed to be half. */
    uint32_t numerator = 0, denominator = 0;
    assert(forge_kv_type_bytes_ratio(FORGE_KV_Q8_0, &numerator, &denominator));
    assert(numerator == 34 && denominator == 32);
    uint64_t q8_bytes_per_token = ((uint64_t)98304 / 2) * numerator / denominator;
    assert(q8_bytes_per_token == 52224);
    requirements.kv_type = FORGE_KV_Q8_0;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(!plan.context_reduced && plan.context_tokens == 16384);
    assert(!strcmp(plan.kv_format, "q8_0"));
    assert(plan.estimated_kv_bytes == q8_bytes_per_token * 16384); /* 855,638,016 */

    /* q4_0 must fit at least as much context as q8_0 on the same host. */
    requirements.kv_type = FORGE_KV_Q4_0;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) == FORGE_OK);
    assert(!plan.context_reduced && plan.context_tokens == 16384);
    assert(!strcmp(plan.kv_format, "q4_0"));

    /* When K and V differ, the planner sizes against the larger-cost type. */
    assert(forge_kv_type_max(FORGE_KV_Q4_0, FORGE_KV_Q8_0) == FORGE_KV_Q8_0);
    assert(forge_kv_type_max(FORGE_KV_Q8_0, FORGE_KV_Q4_0) == FORGE_KV_Q8_0);
    assert(forge_kv_type_max(FORGE_KV_F16, FORGE_KV_Q5_0) == FORGE_KV_F16);

    /* An out-of-range type is refused, not silently planned as f16. */
    requirements.kv_type = (forge_kv_type)99;
    assert(forge_hardware_plan(&hardware, &requirements, 16384, 2048, &plan, &error) ==
           FORGE_ERR_ARGUMENT);
}

static void kv_control_tests(void) {
    /* Defaults are unchanged: f16 K and V, flash attention AUTO, KQV offload on,
     * and an f16 plan when no key or flag is given. */
    forge_config config;
    forge_config_init(&config);
    forge_error error = {0};
    assert(config.model.cache_type_k == FORGE_KV_F16 && config.model.cache_type_v == FORGE_KV_F16);
    assert(config.model.flash_attn == FORGE_FLASH_ATTN_AUTO && config.model.offload_kqv);
    assert(forge_config_validate(&config, &error) == FORGE_OK);

    forge_kv_type type = (forge_kv_type)99;
    assert(forge_kv_type_from_name("f16", &type) && type == FORGE_KV_F16);
    assert(forge_kv_type_from_name("q8_0", &type) && type == FORGE_KV_Q8_0);
    assert(forge_kv_type_from_name("q4_0", &type) && type == FORGE_KV_Q4_0);
    assert(forge_kv_type_from_name("q5_0", &type) && type == FORGE_KV_Q5_0);
    assert(!forge_kv_type_from_name("q6_k", &type) && type == FORGE_KV_Q5_0);
    assert(!forge_kv_type_from_name("", &type));
    assert(!forge_kv_type_from_name("F16", &type));
    assert(!strcmp(forge_kv_type_name(FORGE_KV_Q8_0), "q8_0"));
    assert(!strcmp(forge_kv_type_name(FORGE_KV_F16), "f16"));
    uint32_t numerator = 0, denominator = 0;
    assert(forge_kv_type_bytes_ratio(FORGE_KV_F16, &numerator, &denominator));
    assert(numerator == 2 && denominator == 1);
    assert(forge_kv_type_bytes_ratio(FORGE_KV_Q4_0, &numerator, &denominator));
    assert(numerator == 18 && denominator == 32);
    assert(forge_kv_type_bytes_ratio(FORGE_KV_Q5_0, &numerator, &denominator));
    assert(numerator == 22 && denominator == 32);
    assert(!forge_kv_type_bytes_ratio((forge_kv_type)99, &numerator, &denominator));
    forge_flash_attn mode = (forge_flash_attn)99;
    assert(forge_flash_attn_from_name("auto", &mode) && mode == FORGE_FLASH_ATTN_AUTO);
    assert(forge_flash_attn_from_name("on", &mode) && mode == FORGE_FLASH_ATTN_ENABLED);
    assert(forge_flash_attn_from_name("off", &mode) && mode == FORGE_FLASH_ATTN_DISABLED);
    assert(!forge_flash_attn_from_name("enabled", &mode) && mode == FORGE_FLASH_ATTN_DISABLED);

    /* The keys parse, validate and round-trip through a profile document. */
    assert(parse(&config,
                 "[inference]\ncache_type_k = \"q8_0\"\ncache_type_v = \"q4_0\"\n"
                 "flash_attn = \"on\"\noffload_kqv = false\n",
                 &error) == FORGE_OK);
    assert(config.model.cache_type_k == FORGE_KV_Q8_0 &&
           config.model.cache_type_v == FORGE_KV_Q4_0);
    assert(config.model.flash_attn == FORGE_FLASH_ATTN_ENABLED && !config.model.offload_kqv);
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    assert(parse(&config, "inference.flash_attn = true\n", &error) == FORGE_OK);
    assert(config.model.flash_attn == FORGE_FLASH_ATTN_ENABLED);
    /* Dropping back to AUTO while a quantized type is selected is refused: the
     * dependency holds on every path, including a later overlay. */
    assert(parse(&config, "inference.flash_attn = \"auto\"\n", &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "flash_attn"));
    assert(config.model.flash_attn == FORGE_FLASH_ATTN_ENABLED);
    assert(parse(&config, "inference.cache_type_k = \"f16\"\n", &error) == FORGE_OK);
    assert(parse(&config, "inference.cache_type_v = \"f16\"\n", &error) == FORGE_OK);
    assert(parse(&config, "inference.flash_attn = \"auto\"\n", &error) == FORGE_OK);
    assert(config.model.flash_attn == FORGE_FLASH_ATTN_AUTO);
    assert(parse(&config, "inference.offload_kqv = true\n", &error) == FORGE_OK);
    assert(config.model.offload_kqv);
    forge_config_destroy(&config);

    /* Non-f16 KV without explicit flash attention is refused, with the reason
     * named. A silently ignored flag would be measured as "no effect". */
    rejected("inference.cache_type_k = \"q8_0\"", "flash_attn");
    rejected("inference.cache_type_v = \"q4_0\"", "flash_attn");
    rejected("inference.cache_type_k = \"q5_0\"\ninference.flash_attn = false", "flash_attn");
    rejected("inference.cache_type_k = \"q8_0\"\ninference.flash_attn = \"off\"", "flash_attn");
    rejected("inference.cache_type_k = \"q7_0\"", "cache_type_k");
    rejected("inference.cache_type_k = \"F16\"", "cache_type_k");
    rejected("inference.cache_type_k = 8", "cache_type_k");
    rejected("inference.cache_type_k = \"\"", "cache_type_k");
    rejected("inference.cache_type_v = \"f32\"", "cache_type_v");
    rejected("inference.flash_attn = \"maybe\"", "flash_attn");
    rejected("inference.flash_attn = 1", "flash_attn");
    rejected("inference.offload_kqv = \"true\"", "offload_kqv");

    /* The refusal also holds when values are set directly (library callers). */
    forge_config_init(&config);
    config.model.cache_type_k = FORGE_KV_Q8_0;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "flash_attn"));
    config.model.flash_attn = FORGE_FLASH_ATTN_ENABLED;
    assert(forge_config_validate(&config, &error) == FORGE_OK);
    config.model.cache_type_k = FORGE_KV_F16;
    config.model.cache_type_v = (forge_kv_type)99;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    assert(strstr(error.message, "cache_type"));
    config.model.cache_type_v = FORGE_KV_F16;
    config.model.flash_attn = (forge_flash_attn)99;
    assert(forge_config_validate(&config, &error) == FORGE_ERR_ARGUMENT);
    forge_config_destroy(&config);
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
    kv_control_tests();
    planner_tests();
    planner_kv_tests();
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
