#include "forge/forge.h"
#include <stdio.h>
static void event(const forge_event *e, void *user) {
    (void)user;
    puts(e->json);
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: forge_embed MODEL.gguf WORKSPACE TASK\n");
        return 2;
    }
    forge_error error = {0};
    forge_model_config model_config = forge_default_model_config();
    model_config.model_path = argv[1];
    forge_model *model = forge_model_load(&model_config, &error);
    if (!model) {
        fprintf(stderr, "%s\n", error.message);
        return 1;
    }
    forge_agent_config config = {0};
    config.workspace = argv[2];
    config.model = model;
    config.limits = forge_default_limits();
    config.semantic_output = true;
    config.compact_context = true;
    config.thought = true;
    config.thought_in_history = false; /* Decode-side only; see docs/ARCHITECTURE.md. */
    /* Read-only by default. Supply a policy callback to approve writes/processes. */
    forge_agent *agent = forge_agent_create(&config, &error);
    forge_status status = agent ? forge_agent_run(agent, argv[3], event, NULL, &error) : error.code;
    if (status != FORGE_OK)
        fprintf(stderr, "%s\n", error.message);
    forge_agent_destroy(agent);
    forge_model_destroy(model);
    return status == FORGE_OK ? 0 : 1;
}
