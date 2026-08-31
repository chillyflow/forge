#ifndef FORGE_CHAT_TEMPLATE_H
#define FORGE_CHAT_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>

struct llama_model;
typedef struct fg_chat_templates fg_chat_templates;

#ifdef __cplusplus
extern "C" {
#endif

/* C boundary around the pinned llama.cpp Jinja chat-template engine. */
fg_chat_templates *fg_chat_templates_create(const struct llama_model *model,
                                            const char *template_override,
                                            char *error, size_t error_size);
void fg_chat_templates_destroy(fg_chat_templates *templates);
bool fg_chat_templates_support_thinking(const fg_chat_templates *templates);
char *fg_chat_templates_apply(const fg_chat_templates *templates, const char *prompt,
                              bool enable_thinking, size_t *length,
                              char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
