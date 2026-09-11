#ifndef FORGE_CONVERSATION_INTERNAL_H
#define FORGE_CONVERSATION_INTERNAL_H
#include "forge/forge.h"
#include "forge/context.h"

/* Call after system/tool setup and before adding this run's TASK. Historical
 * evidence is retained verbatim with remapped tool-result dependencies. The new
 * request must be SOURCE in conversation mode to preserve chronological order;
 * retained TASK entries are seeded as SOURCE for the same reason. */
forge_status fg_conversation_seed(const forge_conversation *, forge_context *, size_t *start_index,
                                  forge_error *);
/* Capture the new run only, including an accepted final ACTION/RESULT pair.
 * An incomplete or oversized exchange blocks continuation until explicit reset;
 * this never silently substitutes a summary for assistant/tool evidence. */
forge_status fg_conversation_capture(forge_conversation *, const forge_context *,
                                     size_t start_index, forge_error *);
/* Commit only a selected trial's history by exchanging owned contents. Both
 * objects must be valid, inactive conversation instances. No allocation. */
void fg_conversation_swap(forge_conversation *, forge_conversation *);
/* Host callback answer, refusal and cancellation are returned as bounded JSON
 * tool output. Caller owns *output even when status is non-OK. No capability or
 * execution permission is granted by a question or its answer. */
forge_status fg_conversation_ask(const forge_agent_config *, const char *question,
                                 uint64_t deadline, char **output, forge_error *);
#endif
