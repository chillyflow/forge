#include "inference/chat_template.h"
#include "forge/forge.h"
#include "forge/context.h"
#include "forge/checkpoint.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

extern "C" char *fg_tool_minimal_native_schema(void);
extern "C" char *fg_tool_candidate_schema(bool validation_only);
extern "C" char *fg_tool_native_schema(void);
extern "C" char *fg_tool_native_extensions(const char *, bool ask_user, bool reflection_only);
extern "C" forge_status fg_native_action_normalize(const char *, bool, char **, forge_error *);

static void check_candidate_native_schema(const fg_chat_templates *templates);

static void require(bool condition, const char *expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "chat-template check failed at line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#undef assert
#define assert(condition) require(!!(condition), #condition, __LINE__)

static std::string read_template(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

static std::string read_native_template() {
    return read_template(FORGE_NATIVE_TEMPLATE_FIXTURE);
}

static void check_minimal_native_schema(const fg_chat_templates *templates) {
    char *schema = fg_tool_minimal_native_schema();
    assert(schema);
    std::string schema_text(schema);
    std::string request = "{\"protocol\":\"forge-native-v1\",\"tools\":" + schema_text +
                          ",\"anchor_message_count\":0,\"messages\":["
                          "{\"role\":\"system\",\"content\":\"Use the supplied tools.\"},"
                          "{\"role\":\"user\",\"content\":\"Inspect service.py.\"}]}";
    std::free(schema);
    char error[256] = {};
    fg_chat_render *render =
        fg_chat_templates_apply_native(templates, request.c_str(), false, error, sizeof(error));
    if (!render)
        std::fprintf(stderr, "minimal native template failed: %s\n", error);
    assert(render && fg_chat_render_force_prefix(render));
    const char *read_call = "<tool_call>\n<function=read_file>\n<parameter=path>\nservice.py\n"
                            "</parameter>\n<parameter=start>\n1\n</parameter>\n"
                            "<parameter=end>\n20\n</parameter>\n</function>\n</tool_call>";
    char *parsed = fg_chat_render_parse(render, read_call, error, sizeof(error));
    assert(parsed && std::strstr(parsed, "\"name\":\"read_file\"") &&
           std::strstr(parsed, "service.py"));
    std::free(parsed);
    std::string preamble(3000, 'x');
    std::string reasoned = preamble + "::reasoning_tail\n" + read_call;
    parsed = fg_chat_render_parse(render, reasoned.c_str(), error, sizeof(error));
    std::string retained = "\"reasoning_content\":\"" + preamble + "::reasoning_tail\\n\"";
    assert(parsed && std::strstr(parsed, retained.c_str()));
    std::free(parsed);
    const char *memory_call = "<tool_call>\n<function=memory>\n</function>\n</tool_call>";
    assert(!fg_chat_render_parse(render, memory_call, error, sizeof(error)));
    fg_chat_render_destroy(render);

    /* Check the physical next-turn prompt too: Qwen renders assistant content,
     * so merely retaining reasoning_content in the logical JSON is insufficient. */
    forge_context *history = forge_context_create(
        16384, 2048, [](const char *text, void *) { return std::strlen(text); }, nullptr);
    assert(history);
    assert(forge_context_set_prompt_protocol(history, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(
        forge_context_add(history, FORGE_SEG_SYSTEM, "Use the supplied tools.", 100, true, 0, 0));
    assert(forge_context_add(history, FORGE_SEG_TOOLS, schema_text.c_str(), 100, true, 0, 0));
    assert(forge_context_add(history, FORGE_SEG_TASK, "Inspect service.py.", 100, true, 0, 0));
    std::string action = "{\"assistant_content\":\"" + preamble +
                         "::reasoning_tail\\n\",\"tool\":\"read_file\",\"args\":{"
                         "\"path\":\"service.py\",\"start\":1,\"end\":20}}";
    uint64_t action_id =
        forge_context_add(history, FORGE_SEG_ACTION, action.c_str(), 100, true, 0, 0);
    assert(action_id);
    assert(forge_context_add(history, FORGE_SEG_RESULT, "1: pass", 100, true, action_id, 0));
    forge_error context_error = {};
    size_t tokens = 0, evicted = 0;
    char *next_request = forge_context_plan(history, &tokens, &evicted, &context_error);
    assert(next_request && !evicted);
    render = fg_chat_templates_apply_native(templates, next_request, false, error, sizeof(error));
    std::free(next_request);
    assert(render);
    size_t prompt_length = 0;
    const char *physical = fg_chat_render_prompt(render, &prompt_length);
    const char *assistant = physical ? std::strstr(physical, "<|im_start|>assistant\n") : nullptr;
    std::string full_preamble = preamble + "::reasoning_tail\n";
    assert(assistant && std::strstr(assistant, full_preamble.c_str()));
    assert(std::strstr(assistant, "<function=read_file>") &&
           std::strstr(assistant, "<tool_response>\n1: pass"));
    fg_chat_render_destroy(render);
    forge_context_destroy(history);

    /* An arbitrary five-tool registry must not bypass the memory contract. */
    std::string unsupported = request;
    size_t tool = unsupported.find("\"name\":\"run_command\"");
    assert(tool != std::string::npos);
    unsupported.replace(tool, std::strlen("\"name\":\"run_command\""), "\"name\":\"search_text\"");
    assert(!fg_chat_templates_apply_native(templates, unsupported.c_str(), false, error,
                                           sizeof(error)));
    assert(std::strstr(error, "Native function schemas must include"));
    unsupported = request;
    tool = unsupported.find("\"name\":\"final\"");
    assert(tool != std::string::npos);
    unsupported.replace(tool, std::strlen("\"name\":\"final\""), "\"name\":\"search_text\"");
    assert(!fg_chat_templates_apply_native(templates, unsupported.c_str(), false, error,
                                           sizeof(error)));
    assert(std::strstr(error, "Native function schemas must include"));
}

static void check_candidate_native_schema(const fg_chat_templates *templates) {
    for (bool validation_only : {false, true}) {
        char *schema = fg_tool_candidate_schema(validation_only);
        assert(schema);
        std::string request = "{\"protocol\":\"forge-native-v1\",\"tools\":" + std::string(schema) +
                              ",\"anchor_message_count\":0,\"messages\":["
                              "{\"role\":\"system\",\"content\":\"Use the supplied tools.\"},"
                              "{\"role\":\"user\",\"content\":\"Validate the repair.\"}]}";
        std::free(schema);
        char error[256] = {};
        fg_chat_render *render =
            fg_chat_templates_apply_native(templates, request.c_str(), false, error, sizeof(error));
        if (!render)
            std::fprintf(stderr, "candidate native template failed: %s\n", error);
        assert(render && fg_chat_render_force_prefix(render));
        char *parsed = fg_chat_render_parse(
            render, "<tool_call>\n<function=validate_candidate>\n</function>\n</tool_call>", error,
            sizeof(error));
        assert(parsed && std::strstr(parsed, "\"name\":\"validate_candidate\""));
        std::free(parsed);
        if (validation_only)
            assert(!fg_chat_render_parse(
                render, "<tool_call>\n<function=list_directory>\n</function>\n</tool_call>", error,
                sizeof(error)));
        fg_chat_render_destroy(render);
    }
}

static fg_chat_render *render_extension(const fg_chat_templates *templates, const char *schema) {
    std::string request = "{\"protocol\":\"forge-native-v1\",\"tools\":" + std::string(schema) +
                          ",\"anchor_message_count\":0,\"messages\":["
                          "{\"role\":\"system\",\"content\":\"Use the supplied tools.\"},"
                          "{\"role\":\"user\",\"content\":\"Complete the current task.\"}]}";
    char error[256] = {};
    fg_chat_render *render =
        fg_chat_templates_apply_native(templates, request.c_str(), false, error, sizeof(error));
    if (!render)
        std::fprintf(stderr, "extension native template failed: %s\n", error);
    assert(render && fg_chat_render_force_prefix(render));
    assert(fg_chat_render_grammar(render) && *fg_chat_render_grammar(render));
    return render;
}

static char *parse_extension_action(const fg_chat_render *render, const char *raw) {
    char error[256] = {};
    char *parsed = fg_chat_render_parse(render, raw, error, sizeof(error));
    if (!parsed)
        std::fprintf(stderr, "extension native parse failed: %s\n", error);
    assert(parsed);
    forge_error normalized_error = {};
    char *action = nullptr;
    forge_status status = fg_native_action_normalize(parsed, false, &action, &normalized_error);
    if (status != FORGE_OK)
        std::fprintf(stderr, "extension action normalization failed: %s\n",
                     normalized_error.message);
    std::free(parsed);
    assert(status == FORGE_OK && action);
    return action;
}

static void reject_extension_action(const fg_chat_render *render, const char *raw) {
    char error[256] = {};
    char *parsed = fg_chat_render_parse(render, raw, error, sizeof(error));
    if (!parsed)
        return; /* The actual native grammar/parser may reject before normalization. */
    forge_error normalized_error = {};
    char *action = nullptr;
    forge_status status = fg_native_action_normalize(parsed, false, &action, &normalized_error);
    std::free(parsed);
    std::free(action);
    assert(status != FORGE_OK);
}

static void check_partner_native_schemas(const fg_chat_templates *templates) {
    const char *question = "<tool_call>\n<function=ask_user>\n<parameter=question>\n"
                           "Which naming convention should be used?\n</parameter>\n"
                           "</function>\n</tool_call>";
    for (int registry = 0; registry < 3; registry++) {
        char *base = registry == 0   ? fg_tool_minimal_native_schema()
                     : registry == 1 ? fg_tool_candidate_schema(false)
                                     : fg_tool_native_schema();
        assert(base);
        char *schema = fg_tool_native_extensions(base, true, false);
        std::free(base);
        assert(schema);
        fg_chat_render *render = render_extension(templates, schema);
        char *action = parse_extension_action(render, question);
        assert(std::strstr(action, "\"tool\":\"ask_user\"") &&
               std::strstr(action, "Which naming convention should be used?"));

        reject_extension_action(render,
                                "<tool_call>\n<function=ask_user>\n</function>\n</tool_call>");
        reject_extension_action(render, "<tool_call>\n<function=ask_user>\n<parameter=question>\n"
                                        "What name?\n</parameter>\n<parameter=approval>\ntrue\n"
                                        "</parameter>\n</function>\n</tool_call>");
        reject_extension_action(render, "<tool_call>\n<function=ask_user>\n<parameter=question>\n"
                                        "\n</parameter>\n</function>\n</tool_call>");
        reject_extension_action(
            render, "<tool_call>\n<function=reflect_failure>\n<parameter=diagnosis>\n"
                    "No failed validation occurred.\n</parameter>\n</function>\n</tool_call>");
        std::string oversized = "<tool_call>\n<function=ask_user>\n<parameter=question>\n" +
                                std::string(4097, 'q') +
                                "\n</parameter>\n</function>\n</tool_call>";
        reject_extension_action(render, oversized.c_str());
        fg_chat_render_destroy(render);

        /* Exercise the next physical prompt after a real parsed call, not only
         * simulated JSON. Its answer remains paired with the asking assistant. */
        forge_context *history = forge_context_create(
            131072, 4096, [](const char *text, void *) { return std::strlen(text); }, nullptr);
        assert(history);
        assert(forge_context_set_prompt_protocol(history, FORGE_PROMPT_NATIVE) == FORGE_OK);
        assert(forge_context_add(history, FORGE_SEG_SYSTEM, "Use the supplied tools.", 100, true, 0,
                                 0));
        assert(forge_context_add(history, FORGE_SEG_TOOLS, schema, 100, true, 0, 0));
        assert(forge_context_add(history, FORGE_SEG_SOURCE, "Ask about naming.", 100, true, 0, 0));
        uint64_t action_id = forge_context_add(history, FORGE_SEG_ACTION, action, 100, true, 0, 0);
        assert(action_id);
        assert(forge_context_add(history, FORGE_SEG_RESULT,
                                 "{\"status\":\"answered\",\"answer\":\"snake_case\"}", 100, true,
                                 action_id, 0));
        assert(forge_context_add(history, FORGE_SEG_SOURCE, "Use the user's naming convention.",
                                 100, true, 0, 0));
        forge_error context_error = {};
        size_t tokens = 0, evicted = 0;
        char *request = forge_context_plan(history, &tokens, &evicted, &context_error);
        assert(request && !evicted);
        char error[256] = {};
        render = fg_chat_templates_apply_native(templates, request, false, error, sizeof(error));
        assert(render);
        size_t length = 0;
        const char *physical = fg_chat_render_prompt(render, &length);
        const char *assistant =
            physical ? std::strstr(physical, "<|im_start|>assistant\n") : nullptr;
        assert(assistant && std::strstr(assistant, "<function=ask_user>") &&
               std::strstr(assistant,
                           "<tool_response>\n{\"status\":\"answered\",\"answer\":\"snake_case\"}"));
        const char *answer = std::strstr(assistant, "snake_case");
        const char *continuation = std::strstr(assistant, "Use the user's naming convention.");
        assert(answer && continuation && answer < continuation);
        std::free(request);
        std::free(action);
        std::free(schema);
        fg_chat_render_destroy(render);
        forge_context_destroy(history);
    }
}

static void check_reflection_native_schema(const fg_chat_templates *templates) {
    char *base = fg_tool_candidate_schema(false);
    assert(base);
    /* A diagnostic checkpoint overrides the ordinary registry even when a
     * question callback exists. It cannot ask, edit, run a command or finish. */
    char *schema = fg_tool_native_extensions(base, true, true);
    std::free(base);
    assert(schema && std::strstr(schema, "reflect_failure") && !std::strstr(schema, "ask_user"));
    fg_chat_render *render = render_extension(templates, schema);
    const char *diagnosis = "<tool_call>\n<function=reflect_failure>\n<parameter=diagnosis>\n"
                            "The failed test requires two; replace the returned zero.\n"
                            "</parameter>\n</function>\n</tool_call>";
    char *action = parse_extension_action(render, diagnosis);
    assert(std::strstr(action, "\"tool\":\"reflect_failure\"") &&
           std::strstr(action, "replace the returned zero"));
    std::free(action);
    for (const char *rejected :
         {"<tool_call>\n<function=final>\n<parameter=answer>\nDone\n</parameter>\n</function>\n</"
          "tool_call>",
          "<tool_call>\n<function=validate_candidate>\n</function>\n</tool_call>",
          "<tool_call>\n<function=ask_user>\n<parameter=question>\nHelp?\n</parameter>\n</"
          "function>\n</tool_call>",
          "<tool_call>\n<function=reflect_failure>\n</function>\n</tool_call>",
          "<tool_call>\n<function=reflect_failure>\n<parameter=diagnosis>\n   "
          "\n</parameter>\n</function>\n</tool_call>",
          "<tool_call>\n<function=reflect_failure>\n<parameter=diagnosis>\nCause\n</parameter>\n"
          "<parameter=command>\ngo test\n</parameter>\n</function>\n</tool_call>"})
        reject_extension_action(render, rejected);
    std::string oversized = "<tool_call>\n<function=reflect_failure>\n<parameter=diagnosis>\n" +
                            std::string(8193, 'd') + "\n</parameter>\n</function>\n</tool_call>";
    reject_extension_action(render, oversized.c_str());
    std::free(schema);
    fg_chat_render_destroy(render);

    /* Public/native message inputs can encode malformed strings that cannot
     * occur as a raw NUL in the C renderer API. Reject rather than truncating. */
    for (const char *name : {"ask_user", "reflect_failure"}) {
        const char *field = !std::strcmp(name, "ask_user") ? "question" : "diagnosis";
        for (const char *value :
             {"123", "null", "[]", "{}", "\"\"", "\"   \"", "\"visible\\u0000hidden\""}) {
            std::string message = "{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{"
                                  "\"type\":\"function\",\"function\":{\"name\":\"" +
                                  std::string(name) + "\",\"arguments\":{\"" + field +
                                  "\":" + value + "}}}]}";
            forge_error error = {};
            char *normalized = nullptr;
            forge_status status =
                fg_native_action_normalize(message.c_str(), false, &normalized, &error);
            std::free(normalized);
            assert(status != FORGE_OK);
        }
    }
}

struct model_stream {
    std::string text;
    bool cancel_at_opener = false;
};

static bool collect_model(const char *bytes, size_t length, void *userdata) {
    auto &stream = *static_cast<model_stream *>(userdata);
    stream.text.append(bytes, length);
    return !stream.cancel_at_opener || stream.text.find("<tool_call>") == std::string::npos;
}

static void check_model(const char *path, int gpu_layers) {
    const char *request =
        "{\"protocol\":\"forge-native-v1\",\"anchor_message_count\":1,\"tools\":["
        "{\"type\":\"function\",\"function\":{\"name\":\"final\",\"description\":\"Finish with "
        "answer done\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},"
        "\"required\":[\"answer\"],\"additionalProperties\":false}}}],\"messages\":["
        "{\"role\":\"system\",\"content\":\"Explain in plain text before calling final with answer "
        "done.\"},"
        "{\"role\":\"user\",\"content\":\"Explain hash tables in at least 1000 words of plain "
        "text. "
        "Do not call any tools until the explanation is finished.\"}]}";
    forge_model_config config = forge_default_model_config();
    config.model_path = path;
    config.context_tokens = 4096;
    config.gpu_layers = gpu_layers;
    config.thinking = FORGE_THINKING_DISABLED;
    forge_error error = {};
    forge_model *model = forge_model_load(&config, &error);
    if (!model)
        std::fprintf(stderr, "native model load failed: %s\n", error.message);
    assert(model);
    /* A supported native backend must reject caller syntax as input error,
     * never report an unsupported checkpoint backend or silently skip a probe. */
    forge_metrics malformed_metrics = {};
    assert(forge_complete(model, "Return alpha", 16, nullptr, nullptr,
                          &malformed_metrics, &error) == FORGE_ERR_PARSE);
    assert(error.code == FORGE_ERR_PARSE && malformed_metrics.generated_tokens == 0);
    assert(forge_complete(model, "Return alpha", 16, nullptr, nullptr,
                          &malformed_metrics, nullptr) == FORGE_ERR_PARSE);
    forge_checkpoint_options checkpoint_options = forge_default_checkpoint_options();
    forge_checkpoint_stats malformed_checkpoint = {};
    error = {};
    assert(!forge_checkpoint_save(model, "Return alpha", &checkpoint_options,
                                  &malformed_checkpoint, &error));
    assert(error.code == FORGE_ERR_PARSE && malformed_checkpoint.prompt_tokens == 0);
    assert(!forge_checkpoint_save(model, "Return alpha", &checkpoint_options,
                                  &malformed_checkpoint, nullptr));
    error = {};
    model_stream cancelled;
    cancelled.cancel_at_opener = true;
    forge_metrics metrics = {};
    assert(forge_complete(model, request, 768, collect_model, &cancelled, &metrics, &error) ==
           FORGE_ERR_CANCELLED);
    assert(cancelled.text.find("<tool_call>") != std::string::npos);
    assert(metrics.generated_tokens > 256 && metrics.generated_tokens <= 272);
    assert(metrics.think_tokens == 256);
    model_stream limited;
    error = {};
    assert(forge_complete(model, request, 768, collect_model, &limited, &metrics, &error) ==
           FORGE_ERR_LIMIT);
    assert(metrics.generated_tokens == 768 && metrics.forced_actions == 1);
    assert(std::strstr(error.message, "before one complete native call"));
    const std::string arguments = "\"properties\":{\"answer\":{\"type\":\"string\"}},"
                                  "\"required\":[\"answer\"],";
    std::string bounded_request(request);
    size_t position = bounded_request.find(arguments);
    assert(position != std::string::npos);
    bounded_request.replace(position, arguments.size(), "\"properties\":{},");
    request = bounded_request.c_str();
    model_stream reference;
    error = {};
    forge_status status =
        forge_complete(model, request, 768, collect_model, &reference, &metrics, &error);
    if (status != FORGE_OK)
        std::fprintf(stderr, "native generation failed: %s\n", error.message);
    assert(status == FORGE_OK);
    assert(!metrics.simulated && metrics.forced_actions == 1 && metrics.generated_tokens <= 768);
    assert(metrics.think_tokens == 256);
    char detail[256] = {};
    std::string source = read_native_template();
    fg_chat_templates *templates =
        fg_chat_templates_create(nullptr, source.c_str(), detail, sizeof(detail));
    fg_chat_render *render =
        fg_chat_templates_apply_native(templates, request, false, detail, sizeof(detail));
    char *parsed = fg_chat_render_parse(render, reference.text.c_str(), detail, sizeof(detail));
    if (!parsed)
        std::fprintf(stderr, "native model parse failed: %s\n", detail);
    assert(parsed && std::strstr(parsed, "\"name\":\"final\""));
    const char *calls = std::strstr(parsed, "\"tool_calls\":");
    assert(calls);
    std::string expected_calls(calls);
    std::free(parsed);
    model_stream repeated;
    assert(forge_complete(model, request, 768, collect_model, &repeated, &metrics, &error) ==
           FORGE_OK);
    assert(metrics.generated_tokens > 256 && metrics.generated_tokens <= 768);
    assert(metrics.cached_tokens > 0 && metrics.forced_actions == 1);
    parsed = fg_chat_render_parse(render, repeated.text.c_str(), detail, sizeof(detail));
    assert(parsed);
    calls = std::strstr(parsed, "\"tool_calls\":");
    assert(calls && expected_calls == calls);
    std::free(parsed);
    assert(forge_complete(model, request, 768, nullptr, nullptr, &metrics, &error) == FORGE_OK);
    assert(metrics.generated_tokens > 256 && metrics.generated_tokens <= 768);
    assert(metrics.cached_tokens > 0 && metrics.forced_actions == 1);
    fg_chat_render_destroy(render);
    fg_chat_templates_destroy(templates);
    forge_model_destroy(model);
    std::puts("native model checks passed: malformed-input rejection, forced opener, cancellation, budget exhaustion, "
              "recovery, cached tool-call equivalence, no-callback generation");
}

int main(int argc, char **argv) {
    assert(argc == 1 || (argc == 3 && (!std::strcmp(argv[2], "0") || !std::strcmp(argv[2], "-1"))));
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const char *source = "{% for message in messages %}{{ message['content'] }}{% endfor %}"
                         "{% if add_generation_prompt %}{% if enable_thinking %}<think>"
                         "{% else %}<answer>{% endif %}{% endif %}";
    char error[256] = {};
    fg_chat_templates *templates = fg_chat_templates_create(nullptr, source, error, sizeof(error));
    assert(templates && fg_chat_templates_support_thinking(templates));
    size_t length = 0;
    char *enabled = fg_chat_templates_apply(templates, "task", true, &length, error, sizeof(error));
    assert(enabled && length == std::strlen(enabled) && !std::strcmp(enabled, "task<think>"));
    std::free(enabled);
    char *disabled =
        fg_chat_templates_apply(templates, "task", false, &length, error, sizeof(error));
    assert(disabled && !std::strcmp(disabled, "task<answer>"));
    std::free(disabled);

    const char *request = "{\"protocol\":\"forge-native-v1\",\"tools\":["
                          "{\"type\":\"function\",\"function\":{\"name\":\"final\","
                          "\"description\":\"Finish\",\"parameters\":{\"type\":\"object\","
                          "\"properties\":{\"answer\":{\"type\":\"string\"}},"
                          "\"required\":[\"answer\"],\"additionalProperties\":false}}},"
                          "{\"type\":\"function\",\"function\":{\"name\":\"memory\","
                          "\"description\":\"Remember\",\"parameters\":{\"type\":\"object\","
                          "\"properties\":{},\"additionalProperties\":false}}}],"
                          "\"anchor_message_count\":1,\"messages\":["
                          "{\"role\":\"system\",\"content\":\"Native system\"},"
                          "{\"role\":\"user\",\"content\":\"Do the work\"},"
                          "{\"role\":\"assistant\",\"reasoning_content\":\"too soon\","
                          "\"tool_calls\":[{\"id\":\"forge_1\",\"type\":\"function\","
                          "\"function\":{\"name\":\"final\",\"arguments\":{"
                          "\"answer\":\"premature\"}}}]},"
                          "{\"role\":\"tool\",\"name\":\"final\",\"tool_call_id\":\"forge_1\","
                          "\"content\":\"validation rejected\"},"
                          "{\"role\":\"user\",\"content\":\"Continue\"}]}";
    fg_chat_render *unsupported =
        fg_chat_templates_apply_native(templates, request, true, error, sizeof(error));
    assert(!unsupported && std::strstr(error, "does not support native"));
    fg_chat_templates_destroy(templates);

    std::string native_source = read_native_template();
    templates = fg_chat_templates_create(nullptr, native_source.c_str(), error, sizeof(error));
    assert(templates);
    check_minimal_native_schema(templates);
    check_candidate_native_schema(templates);
    check_partner_native_schemas(templates);
    check_reflection_native_schema(templates);
    fg_chat_render *render =
        fg_chat_templates_apply_native(templates, request, true, error, sizeof(error));
    assert(render);
    const char *prompt = fg_chat_render_prompt(render, &length);
    assert(prompt && length == std::strlen(prompt));
    assert(std::strstr(prompt, "<|im_start|>system\nNative system"));
    assert(std::strstr(prompt, "<|im_start|>user\nDo the work"));
    assert(std::strstr(prompt, "<function=final>"));
    assert(std::strstr(prompt, "<tool_response>\nvalidation rejected"));
    assert(std::strstr(prompt, "<|im_start|>assistant\n"));
    assert(fg_chat_render_cache_anchor(render) > 0 && fg_chat_render_cache_anchor(render) < length);
    assert(fg_chat_render_grammar(render) && *fg_chat_render_grammar(render));
    assert(fg_chat_render_generation_prompt(render) && *fg_chat_render_generation_prompt(render));
    assert(!fg_chat_render_force_prefix(render));
    assert(!fg_chat_render_force_prefix(nullptr));
    assert(!fg_chat_render_action_started(render, "reasoning before a call"));
    assert(!fg_chat_render_action_started(render, "#include <functional>"));
    assert(fg_chat_render_action_started(render, "inspect first\n<tool_call>"));
    assert(fg_chat_render_action_started(render, "inspect first\n<function=final>"));
    fg_chat_render *bounded =
        fg_chat_templates_apply_native(templates, request, false, error, sizeof(error));
    assert(bounded && fg_chat_render_force_prefix(bounded));
    assert(!std::strcmp(fg_chat_render_force_prefix(bounded), "<tool_call>"));
    assert(fg_chat_render_action_started(bounded, fg_chat_render_force_prefix(bounded)));
    assert(!fg_chat_render_action_started(bounded, "<tool_cal"));
    assert(!fg_chat_render_action_started(bounded, ""));
    assert(!fg_chat_render_action_started(bounded, "<function="));
    assert(!fg_chat_render_action_started(bounded, "<function=final"));
    assert(!fg_chat_render_action_started(bounded, "<function=unknown>"));
    assert(!fg_chat_render_action_started(bounded, "#include <functional>"));
    assert(!fg_chat_render_action_started(nullptr, "<tool_call>"));
    assert(!fg_chat_render_action_started(bounded, nullptr));
    assert(fg_chat_render_action_started(bounded, "<function=memory>"));
    fg_chat_render_destroy(bounded);

    /* Final-action pressure narrows the registry, but historical calls remain
     * valid. Exercise the real native template, not just the scripted backend. */
    std::string terminal_request(request);
    size_t memory_schema =
        terminal_request.find(",{\"type\":\"function\",\"function\":{\"name\":\"memory\"");
    size_t tools_end = terminal_request.find("],\"anchor_message_count\"", memory_schema);
    assert(memory_schema != std::string::npos && tools_end != std::string::npos);
    terminal_request.erase(memory_schema, tools_end - memory_schema);
    size_t historical_name =
        terminal_request.find("\"name\":\"final\"", terminal_request.find("\"messages\""));
    assert(historical_name != std::string::npos);
    terminal_request.replace(historical_name, std::strlen("\"name\":\"final\""),
                             "\"name\":\"memory\"");
    historical_name = terminal_request.find("\"name\":\"final\"", historical_name);
    assert(historical_name != std::string::npos);
    terminal_request.replace(historical_name, std::strlen("\"name\":\"final\""),
                             "\"name\":\"memory\"");
    fg_chat_render *terminal = fg_chat_templates_apply_native(templates, terminal_request.c_str(),
                                                              true, error, sizeof(error));
    if (!terminal)
        std::fprintf(stderr, "terminal native template failed: %s\n", error);
    assert(terminal && fg_chat_render_prompt(terminal, &length));
    const char *terminal_call = "<tool_call>\n<function=final>\n<parameter=answer>\ndone\n"
                                "</parameter>\n</function>\n</tool_call>";
    char *terminal_parsed = fg_chat_render_parse(terminal, terminal_call, error, sizeof(error));
    assert(terminal_parsed && std::strstr(terminal_parsed, "\"name\":\"final\""));
    std::free(terminal_parsed);
    const char *memory_call = "<tool_call>\n<function=memory>\n</function>\n</tool_call>";
    assert(!fg_chat_render_parse(terminal, memory_call, error, sizeof(error)));
    fg_chat_render_destroy(terminal);

    const char *raw = "<tool_call>\n<function=final>\n<parameter=answer>\ndone\n</parameter>\n"
                      "</function>\n</tool_call>";
    char *parsed = fg_chat_render_parse(render, raw, error, sizeof(error));
    assert(parsed && std::strstr(parsed, "\"name\":\"final\"") && std::strstr(parsed, "done"));
    std::free(parsed);
    std::string reasoned = std::string("inspect first\n") + raw;
    parsed = fg_chat_render_parse(render, reasoned.c_str(), error, sizeof(error));
    if (!parsed) {
        std::fprintf(stderr, "reasoned native parse failed: %s\n", error);
        return 3;
    }
    if (!std::strstr(parsed, "\"reasoning_content\":\"inspect first\\n\"") ||
        !std::strstr(parsed, "\"name\":\"final\"")) {
        std::fprintf(stderr, "unexpected reasoned native parse: %s\n", parsed);
        std::free(parsed);
        return 4;
    }
    std::free(parsed);
    parsed = fg_chat_render_parse(render, "plain assistant content", error, sizeof(error));
    assert(!parsed && std::strstr(error, "Native response"));
    std::string parallel = std::string(raw) + "\n" + raw;
    parsed = fg_chat_render_parse(render, parallel.c_str(), error, sizeof(error));
    if (parsed) {
        std::fprintf(stderr, "parallel native parse unexpectedly succeeded: %s\n", parsed);
        std::free(parsed);
        return 5;
    }
    fg_chat_render_destroy(render);

    std::string fixture_path = FORGE_NATIVE_TEMPLATE_FIXTURE;
    size_t separator = fixture_path.find_last_of("/\\");
    assert(separator != std::string::npos);
    native_source =
        read_template(fixture_path.substr(0, separator + 1) + "poolside-Laguna-S-2.1.jinja");
    fg_chat_templates_destroy(templates);
    templates = fg_chat_templates_create(nullptr, native_source.c_str(), error, sizeof(error));
    assert(templates);
    render = fg_chat_templates_apply_native(templates, request, true, error, sizeof(error));
    if (!render || fg_chat_render_stop_count(render) == 0) {
        std::fprintf(stderr, "native stop fixture failed: %s\n", error);
        return 2;
    }
    const char *stop = fg_chat_render_stop(render, 0);
    if (!stop || std::strlen(stop) <= 1) {
        std::fprintf(stderr, "native stop fixture returned an empty stop\n");
        return 2;
    }
    size_t split = std::strlen(stop) / 2;
    std::string streamed = std::string("ready") + std::string(stop, split);
    size_t safe_length = 0;
    bool stopped = false;
    fg_chat_render_scan_stop(render, streamed.data(), streamed.size(), 0, &safe_length, &stopped);
    assert(!stopped && safe_length == std::strlen("ready"));
    size_t emitted = safe_length;
    streamed.append(stop + split);
    fg_chat_render_scan_stop(render, streamed.data(), streamed.size(), emitted, &safe_length,
                             &stopped);
    assert(stopped && safe_length == emitted);

    streamed = std::string("done") + stop + "must-not-stream";
    fg_chat_render_scan_stop(render, streamed.data(), streamed.size(), 0, &safe_length, &stopped);
    assert(stopped && safe_length == std::strlen("done"));
    fg_chat_render_destroy(render);
    fg_chat_templates_destroy(templates);
    if (argc == 3)
        check_model(argv[1], std::atoi(argv[2]));
    return 0;
}
