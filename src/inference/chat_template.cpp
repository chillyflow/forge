#include "chat_template.h"
#include "chat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

struct fg_chat_templates {
    common_chat_templates_ptr value;
    bool supports_thinking;
};

static void set_error(char *error, size_t size, const char *message) {
    if (error && size)
        std::snprintf(error, size, "%s", message ? message : "Chat template failed");
}

extern "C" fg_chat_templates *fg_chat_templates_create(const struct llama_model *model,
                                                         const char *template_override,
                                                         char *error, size_t error_size) {
    try {
        auto *result = new fg_chat_templates();
        result->value = common_chat_templates_init(
            model, template_override ? std::string(template_override) : std::string());
        const std::string source = common_chat_templates_source(result->value.get());
        result->supports_thinking = source.find("enable_thinking") != std::string::npos ||
                                    common_chat_templates_support_enable_thinking(result->value.get());
        return result;
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
                                           bool enable_thinking, size_t *length,
                                           char *error, size_t error_size) {
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
