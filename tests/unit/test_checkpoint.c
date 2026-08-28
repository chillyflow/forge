#include "internal.h"
#include "forge/checkpoint.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

/* This fixture tests the host protocol and failure handling only. These bytes
 * are not a KV cache, and passing this suite is not real-model restore proof. */
typedef struct {
    forge_model model;
    uint8_t bytes[128];
    size_t size;
    int32_t tokens[96];
    size_t count;
    size_t prefill_calls, get_calls, set_calls, clear_calls, generate_calls;
    size_t size_override;
    int get_result, set_result, cancel_stage, bad_tokens;
    bool unsupported, fail_prefill, fail_accept, cancelled, expire_prefill, expire_set, reenter;
} fixture;

static forge_status supported(forge_model *model, forge_error *error) {
    fixture *f = model->backend;
    return f->unsupported
               ? fg_error(error, FORGE_ERR_UNSUPPORTED, "Fixture unsupported architecture")
               : FORGE_OK;
}
static forge_status prefill(forge_model *model, const char *prompt, int32_t **tokens, size_t *count,
                            forge_metrics *stats, forge_cancel_fn cancel, void *userdata,
                            uint64_t deadline, forge_error *error) {
    (void)cancel;
    (void)userdata;
    fixture *f = model->backend;
    f->prefill_calls++;
    size_t n = strlen(prompt);
    assert(n && n < sizeof(f->tokens) / sizeof(*f->tokens));
    *tokens = malloc(n * sizeof(**tokens));
    assert(*tokens);
    for (size_t i = 0; i < n; i++)
        (*tokens)[i] = (unsigned char)prompt[i] + 1;
    *count = n;
    size_t prefix = 0;
    while (prefix < n && prefix < f->count && (*tokens)[prefix] == f->tokens[prefix])
        prefix++;
    if (prefix == n)
        prefix--;
    stats->prompt_tokens = n;
    stats->cached_tokens = prefix;
    stats->prefill_tokens = n - prefix;
    f->bytes[0] = 0xc5;
    f->bytes[1] = (uint8_t)n;
    memcpy(f->bytes + 2, prompt, n);
    f->bytes[n + 2] = 0; /* State is binary, including an embedded NUL. */
    f->size = n + 3;
    memcpy(f->tokens, *tokens, n * sizeof(**tokens));
    f->count = n;
    if (f->bad_tokens == 1)
        (*tokens)[0] = -1;
    if (f->bad_tokens == 2)
        *count = 1048577;
    if (f->bad_tokens == 3)
        *count = 0;
    if (f->cancel_stage == 1)
        f->cancelled = true;
    /* Advance to the supplied absolute deadline, without relying on scheduler
     * sleep granularity. Only used with a 2 ms budget in one test. */
    if (f->expire_prefill)
        while (fg_now_ms() < deadline) {
        }
    return f->fail_prefill ? fg_error(error, FORGE_ERR_MODEL, "Injected prefill failure")
                           : FORGE_OK;
}
static size_t state_size(forge_model *model) {
    fixture *f = model->backend;
    if (f->cancel_stage == 2)
        f->cancelled = true;
    return f->size_override == SIZE_MAX ? 0 : f->size_override ? f->size_override : f->size;
}
static size_t state_get(forge_model *model, uint8_t *bytes, size_t size) {
    fixture *f = model->backend;
    f->get_calls++;
    assert(size == f->size);
    memcpy(bytes, f->bytes, size);
    if (f->cancel_stage == 3)
        f->cancelled = true;
    return f->get_result < 0    ? 0
           : f->get_result == 1 ? size - 1
           : f->get_result == 2 ? size + 1
                                : size;
}
static size_t state_set(forge_model *model, const uint8_t *bytes, size_t size) {
    fixture *f = model->backend;
    f->set_calls++;
    assert(size <= sizeof(f->bytes));
    memcpy(f->bytes, bytes, size);
    f->size = size;
    f->count = 0;
    if (f->cancel_stage == 4)
        f->cancelled = true;
    if (f->expire_set) {
        uint64_t stop = fg_now_ms() + 55;
        while (fg_now_ms() < stop) {
        }
    }
    return f->set_result < 0    ? 0
           : f->set_result == 1 ? size - 1
           : f->set_result == 2 ? size + 1
                                : size;
}
static bool accept_tokens(forge_model *model, const int32_t *tokens, size_t count) {
    fixture *f = model->backend;
    if (f->fail_accept)
        return false;
    assert(f->bytes[0] == 0xc5 && f->bytes[1] == count && f->size == count + 3 &&
           !f->bytes[f->size - 1]);
    for (size_t i = 0; i < count; i++)
        assert(tokens[i] == f->bytes[i + 2] + 1);
    memcpy(f->tokens, tokens, count * sizeof(*tokens));
    f->count = count;
    if (f->cancel_stage == 5)
        f->cancelled = true;
    return true;
}
static void clear(forge_model *model) {
    fixture *f = model->backend;
    memset(f->bytes, 0, sizeof(f->bytes));
    f->size = f->count = 0;
    f->clear_calls++;
}
static const fg_checkpoint_backend backend = {supported, prefill,       state_size, state_get,
                                              state_set, accept_tokens, clear};

static forge_status generate(forge_model *model, const char *prompt, const char *grammar,
                             size_t max_tokens, forge_token_fn callback, void *userdata, char **out,
                             forge_metrics *stats, forge_cancel_fn cancel, void *cancel_data,
                             uint64_t deadline, forge_error *error) {
    (void)prompt;
    (void)grammar;
    (void)max_tokens;
    (void)callback;
    (void)userdata;
    (void)stats;
    (void)cancel;
    (void)cancel_data;
    (void)deadline;
    (void)error;
    fixture *f = model->backend;
    f->generate_calls++;
    *out = fg_strdup("fixture generation");
    return FORGE_OK;
}
static void init(fixture *f) {
    memset(f, 0, sizeof(*f));
    forge_error error = {0};
    assert(fg_model_instance_init(&f->model, &error));
    f->model.backend = f;
    f->model.checkpoint = &backend;
    f->model.generate = generate;
    f->model.config = forge_default_model_config();
}
static bool cancelled(void *userdata) {
    fixture *f = userdata;
    if (f->reenter) {
        forge_error error = {0};
        forge_metrics stats = {0};
        f->reenter = false;
        assert(forge_complete(&f->model, "nested", 1, NULL, NULL, &stats, &error) ==
               FORGE_ERR_CONFLICT);
        assert(!forge_checkpoint_save(&f->model, "nested", NULL, NULL, &error));
        assert(error.code == FORGE_ERR_CONFLICT && !f->generate_calls && f->model.operation_active);
    }
    return f->cancelled;
}
static forge_checkpoint_options options_for(fixture *f) {
    forge_checkpoint_options options = forge_default_checkpoint_options();
    options.max_state_bytes = sizeof(f->bytes);
    options.repo_generation = 7;
    options.cancelled = cancelled;
    options.userdata = f;
    return options;
}

static void test_independent_states_and_parent(void) {
    fixture f;
    init(&f);
    forge_checkpoint_options options = options_for(&f);
    forge_checkpoint_stats stats = {0};
    forge_error error = {0};
    forge_checkpoint *a = forge_checkpoint_save(&f.model, "alpha", &options, &stats, &error);
    assert(a && stats.prompt_tokens == 5 && stats.prefill_tokens == 5 && stats.state_bytes == 8);
    assert(!stats.restored_tokens && !f.generate_calls);
    forge_checkpoint_info ai = {0}, bi = {0}, ci = {0};
    assert(forge_checkpoint_get_info(a, &ai));
    assert(ai.valid && ai.id == 1 && ai.parent == 0 && ai.token_start == 0 && ai.token_end == 5);
    assert(ai.repo_generation == 7 && ai.context_hash == fg_hash("alpha", 5));
    forge_checkpoint *b = forge_checkpoint_save(&f.model, "branch", &options, NULL, &error);
    assert(b && forge_checkpoint_get_info(b, &bi) && bi.id == 2 && bi.token_hash != ai.token_hash);
    assert(forge_checkpoint_restore(&f.model, a, &options, &stats, &error) == FORGE_OK);
    assert(stats.restored_tokens == 5 && stats.state_bytes == 8 && stats.prefill_tokens == 0);
    assert(f.count == 5 && !memcmp(f.bytes + 2, "alpha", 5));
    assert(forge_checkpoint_restore(&f.model, b, &options, NULL, &error) == FORGE_OK);
    assert(f.count == 6 && !memcmp(f.bytes + 2, "branch", 6));
    options.parent = a;
    forge_checkpoint *child = forge_checkpoint_save(&f.model, "alphabet", &options, NULL, &error);
    assert(child && forge_checkpoint_get_info(child, &ci));
    assert(ci.parent == ai.id && ci.token_end == 8);
    size_t captures = f.get_calls;
    assert(!forge_checkpoint_save(&f.model, "other", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CONFLICT && f.get_calls == captures &&
           !f.model.operation_active);
    assert(!forge_checkpoint_save(&f.model, "al", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CONFLICT && f.get_calls == captures);
    forge_checkpoint *same = forge_checkpoint_save(&f.model, "alpha", &options, NULL, &error);
    assert(same);
    forge_checkpoint_destroy(a);
    options.parent = NULL;
    assert(forge_checkpoint_restore(&f.model, child, &options, NULL, &error) == FORGE_OK);
    assert(f.count == 8 && !memcmp(f.bytes + 2, "alphabet", 8));
    size_t writes = f.set_calls;
    f.model.operation_active = true;
    assert(forge_checkpoint_restore(&f.model, same, &options, NULL, &error) == FORGE_ERR_CONFLICT);
    assert(f.model.operation_active && f.set_calls == writes && f.count == 8);
    f.model.operation_active = false;
    assert(forge_checkpoint_restore(&f.model, same, &options, NULL, &error) == FORGE_OK);
    assert(f.count == 5);
    forge_checkpoint_destroy(b);
    forge_checkpoint_destroy(child);
    forge_checkpoint_destroy(same);
    assert(!forge_checkpoint_get_info(NULL, &ai) && !forge_checkpoint_get_info(NULL, NULL));
    forge_checkpoint_destroy(NULL);
}

static void test_preflight_and_instance_identity(void) {
    fixture f, other;
    init(&f);
    init(&other);
    forge_error error = {0};
    forge_checkpoint_options options = options_for(&f);
    forge_checkpoint *checkpoint = forge_checkpoint_save(&f.model, "live", &options, NULL, &error);
    assert(checkpoint);
    size_t before = f.set_calls;
    options.repo_generation++;
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_CONFLICT);
    options.repo_generation--;
    options.max_state_bytes = 1;
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_LIMIT);
    assert(f.set_calls == before && f.count == 4);
    options = options_for(&other);
    assert(forge_checkpoint_restore(&other.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_CONFLICT);
    options.parent = checkpoint;
    assert(!forge_checkpoint_save(&other.model, "live plus", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CONFLICT && !other.prefill_calls && !other.set_calls);

    /* The address is unchanged, but a destroyed/reloaded model must not match. */
    char old_nonce[33];
    memcpy(old_nonce, f.model.instance_nonce, sizeof(old_nonce));
    assert(fg_model_instance_init(&f.model, &error));
    assert(memcmp(old_nonce, f.model.instance_nonce, sizeof(old_nonce)));
    options = options_for(&f);
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_CONFLICT);
    assert(f.set_calls == before && f.count == 4);
    forge_checkpoint_destroy(checkpoint);

    f.model.checkpoint = NULL;
    assert(!forge_checkpoint_save(&f.model, "scripted", &options, NULL, &error));
    assert(error.code == FORGE_ERR_UNSUPPORTED);
    f.model.checkpoint = &backend;
    f.unsupported = true;
    assert(!forge_checkpoint_save(&f.model, "architecture", &options, NULL, &error));
    assert(error.code == FORGE_ERR_UNSUPPORTED && !f.model.operation_active);
    f.unsupported = false;
    f.model.operation_active = true;
    assert(!forge_checkpoint_save(&f.model, "busy", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CONFLICT);
    f.model.operation_active = false;
    f.reenter = true;
    checkpoint = forge_checkpoint_save(&f.model, "guard", &options, NULL, &error);
    assert(checkpoint && !f.generate_calls && !f.model.operation_active);
    forge_checkpoint_destroy(checkpoint);
}

static void test_limits_and_capture_failures(void) {
    fixture f;
    init(&f);
    forge_error error = {0};
    forge_checkpoint_options options = options_for(&f);
    assert(!forge_checkpoint_save(NULL, "x", &options, NULL, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_checkpoint_save(&f.model, NULL, &options, NULL, &error));
    assert(!forge_checkpoint_save(&f.model, "\xff", &options, NULL, &error));
    assert(error.code == FORGE_ERR_PARSE);
    char *oversized = malloc((size_t)FG_MAX_JSON + 2);
    assert(oversized);
    memset(oversized, 'x', (size_t)FG_MAX_JSON + 1);
    oversized[FG_MAX_JSON + 1] = 0;
    assert(!forge_checkpoint_save(&f.model, oversized, &options, NULL, &error));
    assert(error.code == FORGE_ERR_LIMIT && !f.prefill_calls);
    free(oversized);
    options.max_state_bytes = 0;
    assert(!forge_checkpoint_save(&f.model, "x", &options, NULL, &error));
    options.max_state_bytes = FORGE_CHECKPOINT_MAX_STATE_BYTES + 1;
    assert(!forge_checkpoint_save(&f.model, "x", &options, NULL, &error));
    options = options_for(&f);
    options.timeout_ms = 0;
    assert(!forge_checkpoint_save(&f.model, "x", &options, NULL, &error));
    options.timeout_ms = FORGE_CHECKPOINT_MAX_TIMEOUT_MS + 1;
    assert(!forge_checkpoint_save(&f.model, "x", &options, NULL, &error));
    assert(!f.prefill_calls);
    options = options_for(&f);
    f.size_override = options.max_state_bytes + 1;
    assert(!forge_checkpoint_save(&f.model, "limit", &options, NULL, &error));
    assert(error.code == FORGE_ERR_LIMIT && !f.get_calls && f.model.next_checkpoint_id == 1);
    f.size_override = SIZE_MAX;
    assert(!forge_checkpoint_save(&f.model, "empty state", &options, NULL, &error));
    assert(error.code == FORGE_ERR_MODEL && !f.get_calls);
    f.size_override = 0;
    for (int mode = -1; mode <= 2; mode++) {
        if (!mode)
            continue;
        f.get_result = mode;
        assert(!forge_checkpoint_save(&f.model, "short state", &options, NULL, &error));
        assert(error.code == FORGE_ERR_MODEL && !f.model.operation_active);
    }
    f.get_result = 0;
    for (int mode = 1; mode <= 3; mode++) {
        f.bad_tokens = mode;
        assert(!forge_checkpoint_save(&f.model, "bad tokens", &options, NULL, &error));
        assert(error.code == FORGE_ERR_MODEL && !f.count);
    }
    f.bad_tokens = 0;
    f.fail_prefill = true;
    assert(!forge_checkpoint_save(&f.model, "failed prefill", &options, NULL, &error));
    assert(error.code == FORGE_ERR_MODEL && !f.count && !f.size);
    f.fail_prefill = false;
    f.model.next_checkpoint_id = UINT64_MAX;
    forge_checkpoint *last = forge_checkpoint_save(&f.model, "last", &options, NULL, &error);
    forge_checkpoint_info info;
    assert(last && forge_checkpoint_get_info(last, &info) && info.id == UINT64_MAX);
    assert(!forge_checkpoint_save(&f.model, "overflow", &options, NULL, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    forge_checkpoint_destroy(last);
}

static void test_failed_restore_clears_and_cancellation(void) {
    fixture f;
    init(&f);
    forge_error error = {0};
    forge_checkpoint_options options = options_for(&f);
    forge_checkpoint_stats stats;
    forge_checkpoint *checkpoint = forge_checkpoint_save(&f.model, "saved", &options, NULL, &error);
    assert(checkpoint);
    for (int mode = -1; mode <= 2; mode++) {
        if (!mode)
            continue;
        f.set_result = mode;
        size_t clears = f.clear_calls;
        assert(forge_checkpoint_restore(&f.model, checkpoint, &options, &stats, &error) ==
               FORGE_ERR_MODEL);
        assert(f.clear_calls == clears + 1 && !f.count && !f.size);
        assert(!stats.restored_tokens && !stats.state_bytes && !f.model.operation_active);
    }
    f.set_result = 0;
    f.fail_accept = true;
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_MODEL);
    assert(!f.count && !f.size);
    f.fail_accept = false;
    for (int stage = 4; stage <= 5; stage++) {
        f.cancel_stage = stage;
        f.cancelled = false;
        assert(forge_checkpoint_restore(&f.model, checkpoint, &options, &stats, &error) ==
               FORGE_ERR_CANCELLED);
        assert(!f.count && !f.size && !stats.restored_tokens && !f.model.operation_active);
    }
    f.cancel_stage = 0;
    f.cancelled = false;
    f.expire_set = true;
    options.timeout_ms = 50;
    size_t clears = f.clear_calls;
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, &stats, &error) ==
           FORGE_ERR_CANCELLED);
    assert(f.clear_calls == clears + 1 && !f.count && !f.size && !stats.restored_tokens);
    f.expire_set = false;
    options = options_for(&f);
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) == FORGE_OK);
    size_t writes = f.set_calls;
    f.cancelled = true;
    assert(forge_checkpoint_restore(&f.model, checkpoint, &options, NULL, &error) ==
           FORGE_ERR_CANCELLED);
    assert(f.count == 5 && f.set_calls == writes);
    assert(!forge_checkpoint_save(&f.model, "cancel before", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CANCELLED);
    for (int stage = 1; stage <= 3; stage++) {
        f.cancelled = false;
        f.cancel_stage = stage;
        assert(!forge_checkpoint_save(&f.model, "cancel after", &options, &stats, &error));
        assert(error.code == FORGE_ERR_CANCELLED && !stats.state_bytes &&
               !f.model.operation_active);
    }
    f.cancelled = false;
    f.cancel_stage = 0;
    f.expire_prefill = true;
    options.timeout_ms = 2;
    assert(!forge_checkpoint_save(&f.model, "timeout", &options, NULL, &error));
    assert(error.code == FORGE_ERR_CANCELLED && !f.model.operation_active);
    forge_checkpoint_destroy(checkpoint);
}

int main(void) {
    test_independent_states_and_parent();
    test_preflight_and_instance_identity();
    test_limits_and_capture_failures();
    test_failed_restore_clears_and_cancellation();
    puts("checkpoint host-protocol tests passed (not real-model KV evidence)");
    return 0;
}
