#include "chat_template.h"
#include "chat.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct fg_chat_templates {
    common_chat_templates_ptr value;
    bool supports_thinking;
};

struct fg_chat_render {
    common_chat_params params;
    size_t cache_anchor = 0;
    std::vector<std::string> trigger_patterns;
    std::vector<llama_token> trigger_tokens;
};

static void set_error(char *error, size_t size, const char *message) {
    if (error && size)
        std::snprintf(error, size, "%s", message ? message : "Chat template failed");
}

static char *copy_string(const std::string &value) {
    char *copy = static_cast<char *>(std::malloc(value.size() + 1));
    if (!copy)
        return nullptr;
    std::memcpy(copy, value.data(), value.size());
    copy[value.size()] = 0;
    return copy;
}

static std::string native_regex_escape(const std::string &word) {
    static const std::string metacharacters = R"(\.^$|()[]{}*+?)";
    std::string escaped;
    escaped.reserve(word.size() * 2);
    for (char c : word) {
        if (metacharacters.find(c) != std::string::npos)
            escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

static void validate_native_history(const json &messages, size_t anchor_count) {
    if (!messages.is_array() || messages.empty())
        throw std::invalid_argument("Native prompt requires a nonempty messages array");
    if (anchor_count > messages.size())
        throw std::invalid_argument("Native cache anchor exceeds the message count");
    std::string pending_id, pending_name;
    for (size_t i = 0; i < messages.size(); i++) {
        const auto &message = messages.at(i);
        if (!message.is_object() || !message.contains("role") || !message.at("role").is_string())
            throw std::invalid_argument("Native message requires a string role");
        const std::string role = message.at("role");
        if (i < anchor_count && role != "system")
            throw std::invalid_argument("Native cache anchor may contain only system messages");
        if (!pending_id.empty()) {
            if (role != "tool" || !message.contains("tool_call_id") ||
                !message.at("tool_call_id").is_string() ||
                message.at("tool_call_id").get<std::string>() != pending_id ||
                !message.contains("name") || !message.at("name").is_string() ||
                message.at("name").get<std::string>() != pending_name)
                throw std::invalid_argument(
                    "Native assistant tool call is not followed by its matching tool result");
            pending_id.clear();
            pending_name.clear();
            continue;
        }
        if (role == "tool")
            throw std::invalid_argument("Native tool result has no preceding assistant call");
        if (role != "system" && role != "user" && role != "assistant")
            throw std::invalid_argument("Native prompt contains an unsupported message role");
        if (role != "assistant") {
            if (message.contains("tool_calls"))
                throw std::invalid_argument(
                    "Only native assistant messages may contain tool calls");
            continue;
        }
        if (!message.contains("tool_calls") || !message.at("tool_calls").is_array() ||
            message.at("tool_calls").size() != 1)
            throw std::invalid_argument(
                "Native assistant history requires exactly one non-parallel tool call");
        const auto &call = message.at("tool_calls").at(0);
        if (!call.is_object() || call.value("type", std::string()) != "function" ||
            !call.contains("id") || !call.at("id").is_string() ||
            call.at("id").get<std::string>().empty() || !call.contains("function") ||
            !call.at("function").is_object() || !call.at("function").contains("name") ||
            !call.at("function").at("name").is_string())
            throw std::invalid_argument("Malformed native assistant tool call in history");
        pending_id = call.at("id").get<std::string>();
        pending_name = call.at("function").at("name").get<std::string>();
    }
    if (!pending_id.empty())
        throw std::invalid_argument("Native assistant history ends with an unmatched tool call");
}

static void validate_native_tools(const json &tools) {
    if (!tools.is_array() || tools.empty())
        throw std::invalid_argument("Native prompt requires function tool schemas");
    bool has_final = false, has_memory = false;
    std::vector<std::string> names;
    for (const auto &tool : tools) {
        if (!tool.is_object() || tool.value("type", std::string()) != "function" ||
            !tool.contains("function") || !tool.at("function").is_object())
            throw std::invalid_argument("Malformed native function schema");
        const auto &function = tool.at("function");
        if (!function.contains("name") || !function.at("name").is_string() ||
            !function.contains("description") || !function.at("description").is_string() ||
            !function.contains("parameters") || !function.at("parameters").is_object())
            throw std::invalid_argument("Native function schema is incomplete");
        std::string name = function.at("name");
        if (name.empty() || std::find(names.begin(), names.end(), name) != names.end())
            throw std::invalid_argument("Native function names must be nonempty and unique");
        names.push_back(name);
        has_final |= name == "final";
        has_memory |= name == "memory";
    }
    if (!has_final || !has_memory)
        throw std::invalid_argument("Native function schemas must include final and memory");
}

extern "C" fg_chat_templates *fg_chat_templates_create(const struct llama_model *model,
                                                       const char *template_override, char *error,
                                                       size_t error_size) {
    try {
        auto result = std::make_unique<fg_chat_templates>();
        result->value = common_chat_templates_init(
            model, template_override ? std::string(template_override) : std::string());
        const std::string source = common_chat_templates_source(result->value.get());
        result->supports_thinking =
            source.find("enable_thinking") != std::string::npos ||
            common_chat_templates_support_enable_thinking(result->value.get());
        return result.release();
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
    } catch (...) {
        set_error(error, error_size, "Unknown chat-template exception");
    }
    return nullptr;
}

extern "C" void fg_chat_templates_destroy(fg_chat_templates *templates) {
    delete templates;
}

extern "C" bool fg_chat_templates_support_thinking(const fg_chat_templates *templates) {
    return templates && templates->supports_thinking;
}

extern "C" char *fg_chat_templates_apply(const fg_chat_templates *templates, const char *prompt,
                                         bool enable_thinking, size_t *length, char *error,
                                         size_t error_size) {
    if (!templates || !prompt || !length) {
        set_error(error, error_size, "Invalid chat-template request");
        return nullptr;
    }
    try {
        common_chat_templates_inputs inputs;
        common_chat_msg message;
        message.role = "user";
        message.content = prompt;
        inputs.messages.push_back(std::move(message));
        inputs.add_generation_prompt = true;
        inputs.use_jinja = true;
        inputs.enable_thinking = enable_thinking;
        inputs.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        common_chat_params rendered = common_chat_templates_apply(templates->value.get(), inputs);
        if (rendered.prompt.size() > 16u * 1024u * 1024u) {
            set_error(error, error_size, "Templated prompt exceeds the 16 MiB bound");
            return nullptr;
        }
        char *copy = static_cast<char *>(std::malloc(rendered.prompt.size() + 1));
        if (!copy) {
            set_error(error, error_size, "Cannot allocate templated prompt");
            return nullptr;
        }
        std::memcpy(copy, rendered.prompt.data(), rendered.prompt.size());
        copy[rendered.prompt.size()] = 0;
        *length = rendered.prompt.size();
        return copy;
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
    } catch (...) {
        set_error(error, error_size, "Unknown chat-template exception");
    }
    return nullptr;
}

extern "C" fg_chat_render *fg_chat_templates_apply_native(const fg_chat_templates *templates,
                                                          const char *request_json,
                                                          bool enable_thinking, char *error,
                                                          size_t error_size) {
    if (!templates || !request_json) {
        set_error(error, error_size, "Invalid native chat-template request");
        return nullptr;
    }
    try {
        size_t request_length = 0;
        while (request_length <= 16u * 1024u * 1024u && request_json[request_length])
            request_length++;
        if (request_length > 16u * 1024u * 1024u)
            throw std::invalid_argument("Native prompt exceeds the 16 MiB bound");
        json request = json::parse(request_json, request_json + request_length);
        if (!request.is_object() || request.value("protocol", std::string()) != "forge-native-v1" ||
            !request.contains("messages") || !request.contains("tools") ||
            !request.contains("anchor_message_count") ||
            !request.at("anchor_message_count").is_number_unsigned())
            throw std::invalid_argument("Malformed forge-native-v1 prompt envelope");
        size_t anchor_count = request.at("anchor_message_count").get<size_t>();
        validate_native_history(request.at("messages"), anchor_count);
        validate_native_tools(request.at("tools"));

        const auto capabilities = common_chat_templates_get_caps(templates->value.get());
        auto supported = [&](const char *name) {
            auto found = capabilities.find(name);
            return found != capabilities.end() && found->second;
        };
        if (!supported("supports_system_role") || !supported("supports_tools") ||
            !supported("supports_tool_calls"))
            throw std::invalid_argument(
                "Selected chat template does not support native system roles and tool calls");

        common_chat_templates_inputs inputs;
        inputs.messages = common_chat_msgs_parse_oaicompat(request.at("messages"));
        inputs.tools = common_chat_tools_parse_oaicompat(request.at("tools"));
        inputs.add_generation_prompt = true;
        inputs.use_jinja = true;
        inputs.tool_choice = COMMON_CHAT_TOOL_CHOICE_REQUIRED;
        inputs.parallel_tool_calls = false;
        inputs.enable_thinking = enable_thinking;
        inputs.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;

        common_chat_params params = common_chat_templates_apply(templates->value.get(), inputs);
        if (params.prompt.empty() || params.prompt.size() > 16u * 1024u * 1024u)
            throw std::invalid_argument("Rendered native prompt is empty or exceeds 16 MiB");
        if (params.grammar.empty() || params.grammar.size() > 16u * 1024u * 1024u ||
            params.parser.empty() || params.format == COMMON_CHAT_FORMAT_CONTENT_ONLY)
            throw std::invalid_argument(
                "Selected chat template has no enforceable native tool grammar/parser");

        auto render = std::make_unique<fg_chat_render>();
        render->params = std::move(params);
        for (const auto &trigger : render->params.grammar_triggers) {
            switch (trigger.type) {
            case COMMON_GRAMMAR_TRIGGER_TYPE_WORD:
                render->trigger_patterns.push_back(native_regex_escape(trigger.value));
                break;
            case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN:
                render->trigger_patterns.push_back(trigger.value);
                break;
            case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL: {
                std::string pattern = trigger.value.empty() ? "^$" : trigger.value;
                if (!trigger.value.empty() && trigger.value.front() != '^')
                    pattern.insert(pattern.begin(), '^');
                if (!trigger.value.empty() && trigger.value.back() != '$')
                    pattern.push_back('$');
                render->trigger_patterns.push_back(std::move(pattern));
                break;
            }
            case COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN:
                render->trigger_tokens.push_back(trigger.token);
                break;
            default:
                throw std::invalid_argument("Native template returned an unknown grammar trigger");
            }
        }
        if (render->params.grammar_lazy && render->trigger_patterns.empty() &&
            render->trigger_tokens.empty())
            throw std::invalid_argument("Native lazy grammar has no trigger");

        if (anchor_count) {
            common_chat_templates_inputs anchor_inputs = inputs;
            anchor_inputs.messages.resize(anchor_count);
            common_chat_msg sentinel;
            sentinel.role = "user";
            sentinel.content = "__FORGE_NATIVE_CACHE_ANCHOR__";
            anchor_inputs.messages.push_back(std::move(sentinel));
            common_chat_params anchor =
                common_chat_templates_apply(templates->value.get(), anchor_inputs);
            size_t common = 0, limit = std::min(render->params.prompt.size(), anchor.prompt.size());
            while (common < limit && render->params.prompt[common] == anchor.prompt[common])
                common++;
            while (common && common < render->params.prompt.size() &&
                   (static_cast<unsigned char>(render->params.prompt[common]) & 0xc0u) == 0x80u)
                common--;
            render->cache_anchor = common;
        }
        return render.release();
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
    } catch (...) {
        set_error(error, error_size, "Unknown native chat-template exception");
    }
    return nullptr;
}

extern "C" void fg_chat_render_destroy(fg_chat_render *render) {
    delete render;
}

extern "C" const char *fg_chat_render_prompt(const fg_chat_render *render, size_t *length) {
    if (!render)
        return nullptr;
    if (length)
        *length = render->params.prompt.size();
    return render->params.prompt.c_str();
}

extern "C" size_t fg_chat_render_cache_anchor(const fg_chat_render *render) {
    return render ? render->cache_anchor : 0;
}

extern "C" const char *fg_chat_render_grammar(const fg_chat_render *render) {
    return render ? render->params.grammar.c_str() : nullptr;
}

extern "C" bool fg_chat_render_grammar_lazy(const fg_chat_render *render) {
    return render && render->params.grammar_lazy;
}

extern "C" const char *fg_chat_render_generation_prompt(const fg_chat_render *render) {
    return render ? render->params.generation_prompt.c_str() : nullptr;
}

extern "C" size_t fg_chat_render_trigger_pattern_count(const fg_chat_render *render) {
    return render ? render->trigger_patterns.size() : 0;
}

extern "C" const char *fg_chat_render_trigger_pattern(const fg_chat_render *render, size_t index) {
    return render && index < render->trigger_patterns.size()
               ? render->trigger_patterns[index].c_str()
               : nullptr;
}

extern "C" size_t fg_chat_render_trigger_token_count(const fg_chat_render *render) {
    return render ? render->trigger_tokens.size() : 0;
}

extern "C" int32_t fg_chat_render_trigger_token(const fg_chat_render *render, size_t index) {
    return render && index < render->trigger_tokens.size() ? render->trigger_tokens[index]
                                                           : LLAMA_TOKEN_NULL;
}

extern "C" size_t fg_chat_render_preserved_count(const fg_chat_render *render) {
    return render ? render->params.preserved_tokens.size() : 0;
}

extern "C" const char *fg_chat_render_preserved(const fg_chat_render *render, size_t index) {
    return render && index < render->params.preserved_tokens.size()
               ? render->params.preserved_tokens[index].c_str()
               : nullptr;
}

extern "C" size_t fg_chat_render_stop_count(const fg_chat_render *render) {
    return render ? render->params.additional_stops.size() : 0;
}

extern "C" const char *fg_chat_render_stop(const fg_chat_render *render, size_t index) {
    return render && index < render->params.additional_stops.size()
               ? render->params.additional_stops[index].c_str()
               : nullptr;
}

extern "C" void fg_chat_render_scan_stop(const fg_chat_render *render, const char *response,
                                         size_t response_length, size_t emitted_length,
                                         size_t *safe_length, bool *stopped) {
    if (safe_length)
        *safe_length = response_length;
    if (stopped)
        *stopped = false;
    if (!render || !response || emitted_length > response_length)
        return;

    const char *begin = response + emitted_length;
    const char *end = response + response_length;
    const char *earliest = end;
    for (const std::string &stop : render->params.additional_stops) {
        if (stop.empty())
            continue;
        const char *found = std::search(begin, end, stop.begin(), stop.end());
        if (found < earliest)
            earliest = found;
    }
    if (earliest != end) {
        if (safe_length)
            *safe_length = static_cast<size_t>(earliest - response);
        if (stopped)
            *stopped = true;
        return;
    }

    size_t pending = 0;
    size_t available = response_length - emitted_length;
    for (const std::string &stop : render->params.additional_stops) {
        if (stop.size() < 2)
            continue;
        size_t candidate = std::min(available, stop.size() - 1);
        while (candidate > pending) {
            if (!std::memcmp(response + response_length - candidate, stop.data(), candidate)) {
                pending = candidate;
                break;
            }
            candidate--;
        }
    }
    if (safe_length)
        *safe_length = response_length - pending;
}

extern "C" char *fg_chat_render_parse(const fg_chat_render *render, const char *response,
                                      char *error, size_t error_size) {
    if (!render || !response) {
        set_error(error, error_size, "Invalid native response parse request");
        return nullptr;
    }
    try {
        size_t length = 0;
        while (length <= 16u * 1024u * 1024u && response[length])
            length++;
        if (length > 16u * 1024u * 1024u)
            throw std::invalid_argument("Native response exceeds the 16 MiB bound");
        common_chat_parser_params parser(render->params);
        parser.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
        parser.parse_tool_calls = true;
        parser.parser.load(render->params.parser);
        const std::string effective =
            parser.generation_prompt.empty()
                ? std::string(response, length)
                : parser.generation_prompt + std::string(response, length);
        common_peg_parse_context strict_context(effective, COMMON_PEG_PARSE_FLAG_NONE);
        common_peg_parse_result strict_result = parser.parser.parse(strict_context);
        if (!strict_result.success() || strict_result.end != effective.size())
            throw std::invalid_argument(
                "Native response does not exactly match the selected template parser");
        common_chat_msg message = common_chat_parse(std::string(response, length), false, parser);
        if (message.role != "assistant" || message.tool_calls.size() != 1)
            throw std::invalid_argument(
                "Native response must parse to exactly one assistant tool call");
        /* Several native templates (including Qwen3-Coder) explicitly permit
         * natural-language reasoning before the one tool-call block. llama.cpp
         * parses that prefix as content rather than reasoning_content. Treat it
         * as Forge's non-executable thought channel; a response carrying both
         * representations is ambiguous and fails closed. */
        if (!message.content.empty()) {
            if (!message.reasoning_content.empty())
                throw std::invalid_argument(
                    "Native response contains ambiguous content and reasoning");
            message.reasoning_content = std::move(message.content);
            message.content.clear();
        }
        std::string encoded = message.to_json_oaicompat(false).dump();
        char *copy = copy_string(encoded);
        if (!copy)
            set_error(error, error_size, "Cannot allocate parsed native response");
        return copy;
    } catch (const std::exception &exception) {
        set_error(error, error_size, exception.what());
    } catch (...) {
        set_error(error, error_size, "Unknown native response parse exception");
    }
    return nullptr;
}
