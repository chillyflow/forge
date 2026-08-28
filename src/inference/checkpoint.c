#include "internal.h"
#include "forge/checkpoint.h"

struct forge_checkpoint {
    forge_checkpoint_info info;
    char instance_nonce[33];
    const fg_checkpoint_backend *backend;
    int32_t *tokens;
    uint8_t *state;
};

forge_checkpoint_options forge_default_checkpoint_options(void) {
    forge_checkpoint_options options = {0};
    options.max_state_bytes = (size_t)256 * 1024 * 1024;
    options.timeout_ms = 120000;
    return options;
}

static bool interrupted(const forge_checkpoint_options *options, uint64_t deadline) {
    return (options->cancelled && options->cancelled(options->userdata)) ||
           (deadline && fg_now_ms() >= deadline);
}

static forge_status begin(forge_model *model, const forge_checkpoint_options *options,
                          uint64_t start, uint64_t *deadline, forge_error *error) {
    if (!model || !options->max_state_bytes ||
        options->max_state_bytes > FORGE_CHECKPOINT_MAX_STATE_BYTES || !options->timeout_ms ||
        options->timeout_ms > FORGE_CHECKPOINT_MAX_TIMEOUT_MS)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Invalid checkpoint model or limits");
    if (model->operation_active)
        return fg_error(error, FORGE_ERR_CONFLICT, "Another operation is active on this model");
    const fg_checkpoint_backend *backend = model->checkpoint;
    if (!backend || !backend->supported || !backend->prefill || !backend->state_size ||
        !backend->state_get || !backend->state_set || !backend->accept_tokens || !backend->clear)
        return fg_error(error, FORGE_ERR_UNSUPPORTED,
                        "Physical checkpoints are unsupported by this backend; scripted output "
                        "does not contain model state");
    if (!model->instance_nonce[0])
        return fg_error(error, FORGE_ERR_CONFLICT, "Model instance identity is not initialized");
    if (start > UINT64_MAX - options->timeout_ms)
        return fg_error(error, FORGE_ERR_LIMIT, "Checkpoint deadline overflow");
    *deadline = start + options->timeout_ms;
    model->operation_active = true;
    forge_status status = backend->supported(model, error);
    if (status == FORGE_OK && interrupted(options, *deadline))
        status = fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint cancelled or deadline reached");
    if (status != FORGE_OK)
        model->operation_active = false;
    return status;
}

static bool same_instance(const forge_model *model, const forge_checkpoint *checkpoint) {
    return checkpoint && checkpoint->info.valid && checkpoint->tokens && checkpoint->state &&
           checkpoint->backend == model->checkpoint &&
           !memcmp(checkpoint->instance_nonce, model->instance_nonce,
                   sizeof(checkpoint->instance_nonce));
}

static void finish(forge_model *model, forge_checkpoint_stats *stats, uint64_t start) {
    model->operation_active = false;
    if (stats)
        stats->duration_ms = (double)(fg_now_ms() - start);
}

forge_checkpoint *forge_checkpoint_save(forge_model *model, const char *prompt,
                                        const forge_checkpoint_options *requested,
                                        forge_checkpoint_stats *stats, forge_error *error) {
    uint64_t start = fg_now_ms();
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!prompt) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Missing checkpoint prompt");
        return NULL;
    }
    size_t prompt_bytes = 0;
    while (prompt_bytes <= FG_MAX_JSON && prompt[prompt_bytes])
        prompt_bytes++;
    if (prompt_bytes > FG_MAX_JSON || !fg_utf8_valid(prompt, prompt_bytes)) {
        fg_error(error, prompt_bytes > FG_MAX_JSON ? FORGE_ERR_LIMIT : FORGE_ERR_PARSE,
                 "Checkpoint prompt must be valid UTF-8 within 16 MiB");
        return NULL;
    }
    forge_checkpoint_options options = requested ? *requested : forge_default_checkpoint_options();
    uint64_t deadline = 0;
    if (begin(model, &options, start, &deadline, error) != FORGE_OK)
        return NULL;
    const fg_checkpoint_backend *backend = model->checkpoint;
    forge_checkpoint *checkpoint = NULL;
    int32_t *tokens = NULL;
    size_t token_count = 0;
    forge_metrics prefill = {0};
    uint64_t save_start = 0;
    if (!model->next_checkpoint_id) {
        fg_error(error, FORGE_ERR_LIMIT, "Checkpoint ID space exhausted for this model instance");
        goto failed;
    }
    if (options.parent && !same_instance(model, options.parent)) {
        fg_error(error, FORGE_ERR_CONFLICT, "Checkpoint parent belongs to another model instance");
        goto failed;
    }
    forge_status status = backend->prefill(model, prompt, &tokens, &token_count, &prefill,
                                           options.cancelled, options.userdata, deadline, error);
    if (stats) {
        stats->prompt_tokens = prefill.prompt_tokens;
        stats->cached_tokens = prefill.cached_tokens;
        stats->prefill_tokens = prefill.prefill_tokens;
        stats->prefill_ms = prefill.prefill_ms;
    }
    if (status != FORGE_OK) {
        backend->clear(model);
        goto failed;
    }
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint cancelled after prefill");
        goto failed;
    }
    if (!tokens || !token_count || token_count > 1048576) {
        backend->clear(model);
        fg_error(error, FORGE_ERR_MODEL, "Backend returned an invalid checkpoint token sequence");
        goto failed;
    }
    for (size_t i = 0; i < token_count; i++) {
        if (tokens[i] < 0) {
            backend->clear(model);
            fg_error(error, FORGE_ERR_MODEL, "Backend returned an invalid checkpoint token ID");
            goto failed;
        }
    }
    if (options.parent && (options.parent->info.token_end > token_count ||
                           memcmp(options.parent->tokens, tokens,
                                  options.parent->info.token_end * sizeof(*tokens)))) {
        fg_error(error, FORGE_ERR_CONFLICT,
                 "Checkpoint parent is not an exact prefix of the templated token sequence");
        goto failed;
    }
    save_start = fg_now_ms();
    size_t state_bytes = backend->state_size(model);
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint cancelled while sizing model state");
        goto failed;
    }
    if (!state_bytes) {
        fg_error(error, FORGE_ERR_MODEL, "Backend reported an empty checkpoint state");
        goto failed;
    }
    if (state_bytes > options.max_state_bytes) {
        fg_error(error, FORGE_ERR_LIMIT, "Checkpoint needs %zu state bytes; cap is %zu",
                 state_bytes, options.max_state_bytes);
        goto failed;
    }
    checkpoint = calloc(1, sizeof(*checkpoint));
    if (checkpoint)
        checkpoint->state = malloc(state_bytes);
    if (!checkpoint || !checkpoint->state) {
        fg_error(error, FORGE_ERR_MEMORY, "Checkpoint state allocation failed");
        goto failed;
    }
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint cancelled before state capture");
        goto failed;
    }
    size_t written = backend->state_get(model, checkpoint->state, state_bytes);
    if (written != state_bytes) {
        fg_error(error, FORGE_ERR_MODEL, "Incomplete checkpoint capture: %zu of %zu bytes", written,
                 state_bytes);
        goto failed;
    }
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint cancelled during state capture");
        goto failed;
    }
    checkpoint->tokens = tokens;
    tokens = NULL;
    checkpoint->backend = backend;
    memcpy(checkpoint->instance_nonce, model->instance_nonce, sizeof(checkpoint->instance_nonce));
    checkpoint->info.id = model->next_checkpoint_id;
    checkpoint->info.parent = options.parent ? options.parent->info.id : 0;
    checkpoint->info.repo_generation = options.repo_generation;
    checkpoint->info.context_hash = fg_hash(prompt, prompt_bytes);
    checkpoint->info.token_hash =
        fg_hash(checkpoint->tokens, token_count * sizeof(*checkpoint->tokens));
    checkpoint->info.token_end = token_count;
    checkpoint->info.state_bytes = state_bytes;
    checkpoint->info.valid = true;
    model->next_checkpoint_id =
        model->next_checkpoint_id == UINT64_MAX ? 0 : model->next_checkpoint_id + 1;
    if (stats) {
        stats->state_bytes = state_bytes;
        stats->save_ms = (double)(fg_now_ms() - save_start);
    }
    finish(model, stats, start);
    return checkpoint;

failed:
    if (stats && save_start)
        stats->save_ms = (double)(fg_now_ms() - save_start);
    free(tokens);
    forge_checkpoint_destroy(checkpoint);
    finish(model, stats, start);
    return NULL;
}

static forge_status restore_state(forge_model *model, const forge_checkpoint *checkpoint,
                                  const forge_checkpoint_options *options, uint64_t deadline,
                                  forge_checkpoint_stats *stats, forge_error *error) {
    uint64_t restore_start = 0;
    forge_status status = FORGE_OK;
    const fg_checkpoint_backend *backend = model->checkpoint;
    if (!same_instance(model, checkpoint)) {
        status = fg_error(error, FORGE_ERR_CONFLICT,
                          "Checkpoint belongs to another or previously destroyed model instance");
        goto done;
    }
    if (checkpoint->info.repo_generation != options->repo_generation) {
        status = fg_error(error, FORGE_ERR_CONFLICT, "Checkpoint repository generation differs");
        goto done;
    }
    if (checkpoint->info.state_bytes > options->max_state_bytes) {
        status = fg_error(error, FORGE_ERR_LIMIT, "Checkpoint exceeds the restore state-byte cap");
        goto done;
    }
    if (interrupted(options, deadline)) {
        status = fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint restore cancelled before write");
        goto done;
    }
    restore_start = fg_now_ms();
    size_t read = backend->state_set(model, checkpoint->state, checkpoint->info.state_bytes);
    if (read != checkpoint->info.state_bytes) {
        status = fg_error(error, FORGE_ERR_MODEL, "Incomplete checkpoint restore: %zu of %zu bytes",
                          read, checkpoint->info.state_bytes);
        goto clear;
    }
    if (interrupted(options, deadline)) {
        status = fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint restore cancelled during write");
        goto clear;
    }
    if (!backend->accept_tokens(model, checkpoint->tokens, checkpoint->info.token_end)) {
        status = fg_error(error, FORGE_ERR_MODEL,
                          "Restored checkpoint does not cover the saved token sequence");
        goto clear;
    }
    if (interrupted(options, deadline)) {
        status = fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint restore cancelled after write");
        goto clear;
    }
    if (stats) {
        stats->state_bytes = checkpoint->info.state_bytes;
        stats->restored_tokens = checkpoint->info.token_end;
    }
    goto done;

clear:
    backend->clear(model);
done:
    if (stats && restore_start)
        stats->restore_ms = (double)(fg_now_ms() - restore_start);
    return status;
}

forge_status forge_checkpoint_restore(forge_model *model, const forge_checkpoint *checkpoint,
                                      const forge_checkpoint_options *requested,
                                      forge_checkpoint_stats *stats, forge_error *error) {
    uint64_t start = fg_now_ms();
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!checkpoint)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing checkpoint");
    forge_checkpoint_options options = requested ? *requested : forge_default_checkpoint_options();
    uint64_t deadline = 0;
    forge_status status = begin(model, &options, start, &deadline, error);
    if (status != FORGE_OK)
        return status;
    status = restore_state(model, checkpoint, &options, deadline, stats, error);
    finish(model, stats, start);
    return status;
}

size_t fg_checkpoint_allocation_bytes(size_t count, size_t state_bytes) {
    if (!count || count > 1048576 || !state_bytes ||
        state_bytes > FORGE_CHECKPOINT_MAX_STATE_BYTES ||
        count > (SIZE_MAX - sizeof(forge_checkpoint)) / sizeof(int32_t))
        return 0;
    size_t bytes = sizeof(forge_checkpoint) + count * sizeof(int32_t);
    return state_bytes > SIZE_MAX - bytes ? 0 : bytes + state_bytes;
}

bool fg_checkpoint_matches_prefix(const forge_checkpoint *checkpoint, const int32_t *tokens,
                                  size_t count) {
    return checkpoint && checkpoint->info.valid && checkpoint->tokens && tokens &&
           checkpoint->info.token_end && checkpoint->info.token_end <= 1048576 &&
           checkpoint->info.token_end <= count &&
           !memcmp(checkpoint->tokens, tokens, checkpoint->info.token_end * sizeof(*tokens));
}

forge_checkpoint *fg_checkpoint_capture_live(forge_model *model, const int32_t *tokens,
                                             size_t count, size_t state_bytes,
                                             uint64_t repo_generation, uint64_t context_hash,
                                             forge_cancel_fn cancelled, void *userdata,
                                             uint64_t deadline, forge_error *error) {
    const fg_checkpoint_backend *backend = model ? model->checkpoint : NULL;
    if (!model || !model->operation_active || !tokens ||
        !fg_checkpoint_allocation_bytes(count, state_bytes)) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid active checkpoint capture");
        return NULL;
    }
    if (!backend || !backend->state_get || !backend->live_matches) {
        fg_error(error, FORGE_ERR_UNSUPPORTED, "Backend has no exact live-state capture hook");
        return NULL;
    }
    if (!model->instance_nonce[0] || !model->next_checkpoint_id) {
        fg_error(error, FORGE_ERR_LIMIT, "Checkpoint instance/ID is unavailable");
        return NULL;
    }
    forge_checkpoint_options options = {0};
    options.cancelled = cancelled;
    options.userdata = userdata;
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint capture cancelled before allocation");
        return NULL;
    }
    if (!backend->live_matches(model, tokens, count)) {
        fg_error(error, FORGE_ERR_MODEL, "Live sequence does not exactly match capture tokens");
        return NULL;
    }
    for (size_t i = 0; i < count; i++)
        if (tokens[i] < 0) {
            fg_error(error, FORGE_ERR_MODEL, "Negative token in live checkpoint capture");
            return NULL;
        }
    forge_checkpoint *checkpoint = calloc(1, sizeof(*checkpoint));
    if (checkpoint) {
        checkpoint->tokens = malloc(count * sizeof(*tokens));
        checkpoint->state = malloc(state_bytes);
    }
    if (!checkpoint || !checkpoint->tokens || !checkpoint->state) {
        fg_error(error, FORGE_ERR_MEMORY, "Live checkpoint allocation failed");
        goto failed;
    }
    memcpy(checkpoint->tokens, tokens, count * sizeof(*tokens));
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint capture cancelled before state copy");
        goto failed;
    }
    size_t written = backend->state_get(model, checkpoint->state, state_bytes);
    if (written != state_bytes) {
        fg_error(error, FORGE_ERR_MODEL, "Incomplete checkpoint capture: %zu of %zu bytes", written,
                 state_bytes);
        goto failed;
    }
    if (interrupted(&options, deadline)) {
        fg_error(error, FORGE_ERR_CANCELLED, "Checkpoint capture cancelled during state copy");
        goto failed;
    }
    checkpoint->backend = backend;
    memcpy(checkpoint->instance_nonce, model->instance_nonce, sizeof(checkpoint->instance_nonce));
    checkpoint->info = (forge_checkpoint_info){0};
    checkpoint->info.id = model->next_checkpoint_id;
    checkpoint->info.repo_generation = repo_generation;
    checkpoint->info.context_hash = context_hash;
    checkpoint->info.token_hash = fg_hash(tokens, count * sizeof(*tokens));
    checkpoint->info.token_end = count;
    checkpoint->info.state_bytes = state_bytes;
    checkpoint->info.valid = true;
    model->next_checkpoint_id =
        model->next_checkpoint_id == UINT64_MAX ? 0 : model->next_checkpoint_id + 1;
    return checkpoint;
failed:
    forge_checkpoint_destroy(checkpoint);
    return NULL;
}

forge_status fg_checkpoint_restore_active(forge_model *model, const forge_checkpoint *checkpoint,
                                          uint64_t repo_generation, size_t max_state_bytes,
                                          forge_cancel_fn cancelled, void *userdata,
                                          uint64_t deadline, forge_checkpoint_stats *stats,
                                          forge_error *error) {
    uint64_t start = fg_now_ms();
    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!model || !model->operation_active || !checkpoint || !max_state_bytes ||
        max_state_bytes > FORGE_CHECKPOINT_MAX_STATE_BYTES)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Invalid active checkpoint restore");
    const fg_checkpoint_backend *backend = model->checkpoint;
    if (!backend || !backend->state_set || !backend->accept_tokens || !backend->clear)
        return fg_error(error, FORGE_ERR_UNSUPPORTED, "Backend cannot restore checkpoints");
    forge_checkpoint_options options = {0};
    options.max_state_bytes = max_state_bytes;
    options.repo_generation = repo_generation;
    options.cancelled = cancelled;
    options.userdata = userdata;
    forge_status status = restore_state(model, checkpoint, &options, deadline, stats, error);
    if (stats)
        stats->duration_ms = (double)(fg_now_ms() - start);
    return status;
}

bool forge_checkpoint_get_info(const forge_checkpoint *checkpoint, forge_checkpoint_info *info) {
    if (!checkpoint || !info)
        return false;
    *info = checkpoint->info;
    return true;
}

void forge_checkpoint_destroy(forge_checkpoint *checkpoint) {
    if (checkpoint) {
        free(checkpoint->tokens);
        free(checkpoint->state);
        free(checkpoint);
    }
}
