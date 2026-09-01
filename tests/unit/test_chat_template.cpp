#include "inference/chat_template.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

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

int main() {
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
    return 0;
}
