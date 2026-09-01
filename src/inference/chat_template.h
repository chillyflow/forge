#ifndef FORGE_CHAT_TEMPLATE_H
#define FORGE_CHAT_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct llama_model;
typedef struct fg_chat_templates fg_chat_templates;
typedef struct fg_chat_render fg_chat_render;

#ifdef __cplusplus
extern "C" {
#endif

/* C boundary around the pinned llama.cpp Jinja chat-template engine. */
fg_chat_templates *fg_chat_templates_create(const struct llama_model *model,
                                            const char *template_override, char *error,
                                            size_t error_size);
void fg_chat_templates_destroy(fg_chat_templates *templates);
bool fg_chat_templates_support_thinking(const fg_chat_templates *templates);
char *fg_chat_templates_apply(const fg_chat_templates *templates, const char *prompt,
                              bool enable_thinking, size_t *length, char *error, size_t error_size);

/* Structured native protocol. request_json is the bounded forge-native-v1
 * conversation produced by the context planner. The opaque render retains the
 * exact prompt, generated tool grammar, parser, triggers, and stop strings for
 * one generation. */
fg_chat_render *fg_chat_templates_apply_native(const fg_chat_templates *templates,
                                               const char *request_json, bool enable_thinking,
                                               char *error, size_t error_size);
void fg_chat_render_destroy(fg_chat_render *render);
const char *fg_chat_render_prompt(const fg_chat_render *render, size_t *length);
size_t fg_chat_render_cache_anchor(const fg_chat_render *render);
const char *fg_chat_render_grammar(const fg_chat_render *render);
bool fg_chat_render_grammar_lazy(const fg_chat_render *render);
const char *fg_chat_render_generation_prompt(const fg_chat_render *render);
size_t fg_chat_render_trigger_pattern_count(const fg_chat_render *render);
const char *fg_chat_render_trigger_pattern(const fg_chat_render *render, size_t index);
size_t fg_chat_render_trigger_token_count(const fg_chat_render *render);
int32_t fg_chat_render_trigger_token(const fg_chat_render *render, size_t index);
size_t fg_chat_render_preserved_count(const fg_chat_render *render);
const char *fg_chat_render_preserved(const fg_chat_render *render, size_t index);
size_t fg_chat_render_stop_count(const fg_chat_render *render);
const char *fg_chat_render_stop(const fg_chat_render *render, size_t index);
void fg_chat_render_scan_stop(const fg_chat_render *render, const char *response,
                              size_t response_length, size_t emitted_length, size_t *safe_length,
                              bool *stopped);
char *fg_chat_render_parse(const fg_chat_render *render, const char *response, char *error,
                           size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
