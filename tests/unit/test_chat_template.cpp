#include "inference/chat_template.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

int main() {
    const char *source =
        "{% for message in messages %}{{ message['content'] }}{% endfor %}"
        "{% if add_generation_prompt %}{% if enable_thinking %}<think>"
        "{% else %}<answer>{% endif %}{% endif %}";
    char error[256] = {};
    fg_chat_templates *templates = fg_chat_templates_create(nullptr, source, error, sizeof(error));
    assert(templates && fg_chat_templates_support_thinking(templates));
    size_t length = 0;
    char *enabled = fg_chat_templates_apply(templates, "task", true, &length, error, sizeof(error));
    assert(enabled && length == std::strlen(enabled) && std::strstr(enabled, "task<think>"));
    std::free(enabled);
    char *disabled =
        fg_chat_templates_apply(templates, "task", false, &length, error, sizeof(error));
    assert(disabled && std::strstr(disabled, "task<answer>"));
    std::free(disabled);
    fg_chat_templates_destroy(templates);
    return 0;
}
