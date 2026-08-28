#include "internal.h"
#include "forge/summary.h"

static size_t plus(size_t left, size_t right) {
    return right > SIZE_MAX - left ? SIZE_MAX : left + right;
}
static void account(forge_metrics *total, const forge_summary_generation_stats *stats) {
    const forge_metrics *work = &stats->inference;
    total->summary_lookups = plus(total->summary_lookups, 1);
    total->summary_hits = plus(total->summary_hits, stats->cache_hit ? 1u : 0u);
    total->summary_generations = plus(total->summary_generations, stats->model_calls);
    total->summary_ms += stats->duration_ms;
    total->simulated = total->simulated || work->simulated;
#define ADD(field) total->field = plus(total->field, work->field)
    ADD(prompt_tokens);
    ADD(generated_tokens);
    ADD(cached_tokens);
    ADD(prefill_tokens);
    ADD(grammar_fast_tokens);
    ADD(grammar_fallback_tokens);
#undef ADD
    total->load_ms = FG_MAX(total->load_ms, work->load_ms);
    total->prefill_ms += work->prefill_ms;
    total->decode_ms += work->decode_ms;
    total->sampling_ms += work->sampling_ms;
}
static char *context_json(const forge_summary_view *view, const char *scope, bool hit,
                          forge_error *error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!root) {
        yyjson_mut_doc_free(doc);
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate summary context JSON");
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    bool ok =
        yyjson_mut_obj_add_uint(doc, root, "schema_version", 1) &&
        yyjson_mut_obj_add_bool(doc, root, "summary_unverified", true) &&
        yyjson_mut_obj_add_bool(doc, root, "indexed_snapshot_only", true) &&
        yyjson_mut_obj_add_bool(doc, root, "cache_hit", hit) &&
        yyjson_mut_obj_add_str(doc, root, "scope", scope) &&
        yyjson_mut_obj_add_str(doc, root, "path", view->path) &&
        yyjson_mut_obj_add_str(doc, root, "symbol", view->symbol ? view->symbol : "") &&
        yyjson_mut_obj_add_str(doc, root, "evidence",
                               view->evidence == FORGE_SUMMARY_FULL_SOURCE ? "full_source"
                                                                           : "syntactic_outline") &&
        yyjson_mut_obj_add_uint(doc, root, "generation", view->generation) &&
        yyjson_mut_obj_add_str(doc, root, "cache_key", view->cache_key) &&
        yyjson_mut_obj_add_str(doc, root, "dependency_hash", view->dependency_hash) &&
        yyjson_mut_obj_add_bool(doc, root, "go_index_incomplete", view->go_index_incomplete) &&
        yyjson_mut_obj_add_bool(doc, root, "filesystem_scan", view->filesystem_scan) &&
        yyjson_mut_obj_add_str(doc, root, "text", view->text);
    char *json = ok ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    if (!json)
        fg_error(error, FORGE_ERR_MEMORY, "Cannot serialize summary context");
    return json;
}
static bool record(fg_tool_context *context, const forge_summary_input *input,
                   const forge_summary_generation_stats *stats, forge_status status,
                   forge_error *error) {
    char artifact[64] = {0};
    if (input) {
        char *evidence = forge_summary_input_json(input, error);
        if (!evidence)
            return false;
        snprintf(artifact, sizeof(artifact), "tool/%06zu.summary.json", context->call_id);
        bool saved = fg_session_artifact(context->session, artifact, evidence, error);
        free(evidence);
        if (!saved)
            return false;
    }
    char *metrics = fg_metrics_json(&stats->inference, status);
    fg_buf event = {0};
    bool ok = metrics &&
              fg_buf_printf(&event,
                            "{\"tool_call\":%zu,\"artifact\":\"%s\",\"cache_hit\":%s,"
                            "\"model_calls\":%zu,\"published\":%s,\"reused_competing_writer\":%s,"
                            "\"repaired_corruption\":%s,\"duration_ms\":%.0f,"
                            "\"inference\":%s}",
                            context->call_id, artifact, stats->cache_hit ? "true" : "false",
                            stats->model_calls, stats->published ? "true" : "false",
                            stats->store.reused ? "true" : "false",
                            stats->store.repaired_corruption ? "true" : "false", stats->duration_ms,
                            metrics);
    free(metrics);
    if (ok)
        ok = fg_session_emit(context->session, "summary", event.data, error);
    else
        fg_error(error, FORGE_ERR_MEMORY, "Cannot serialize summary generation event");
    fg_buf_clear(&event);
    return ok;
}
char *fg_tool_summary(fg_tool_context *context, yyjson_val *args, forge_error *error) {
    if (!context->config.summary_producer_id || !context->config.model || !context->metrics ||
        !context->session) {
        fg_error(error, FORGE_ERR_UNSUPPORTED,
                 "Summary context requires an enabled host producer identity");
        return NULL;
    }
    const char *scope = fg_json_str(args, "scope"), *path = fg_json_str(args, "path"),
               *symbol = fg_json_str(args, "symbol");
    const char *const names[] = {"repository", "module", "package", "file", "symbol"};
    size_t selected = 0;
    while (scope && selected < sizeof(names) / sizeof(*names) && strcmp(scope, names[selected]))
        selected++;
    if (!scope || !path || !symbol || selected == sizeof(names) / sizeof(*names) ||
        ((selected == FORGE_SUMMARY_SYMBOL) != (*symbol != 0))) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid summary scope, path or symbol");
        return NULL;
    }
    const forge_limits *limits = &context->config.limits;
    forge_metrics *metrics = context->metrics;
    if (metrics->prompt_tokens >= limits->max_input_tokens ||
        metrics->generated_tokens >= limits->max_generated_tokens ||
        limits->max_tool_bytes < 1024) {
        fg_error(error, FORGE_ERR_LIMIT,
                 "Insufficient remaining summary input, output or tool budget");
        return NULL;
    }
    size_t reserve =
        FG_MIN((size_t)512, FG_MIN(limits->output_reserve,
                                   limits->max_generated_tokens - metrics->generated_tokens));
    if (!reserve || reserve >= limits->context_tokens) {
        fg_error(error, FORGE_ERR_LIMIT, "Insufficient summary context reserve");
        return NULL;
    }
    forge_summary_options options = forge_default_summary_options();
    options.producer_id = context->config.summary_producer_id;
    options.max_input_tokens =
        FG_MIN(limits->max_input_tokens - metrics->prompt_tokens, limits->context_tokens - reserve);
    options.max_input_bytes = FG_MIN(options.max_input_bytes, limits->max_file_bytes);
    options.max_summary_bytes = FG_MIN(options.max_summary_bytes, limits->max_tool_bytes / 2);
    options.max_summary_tokens = limits->context_tokens / 8;
    uint64_t now = fg_now_ms();
    uint64_t deadline = now > UINT64_MAX - 30000 ? UINT64_MAX : now + 30000;
    if (context->deadline && context->deadline < deadline)
        deadline = context->deadline;
    options.deadline_ms = deadline;
    options.timeout_ms = 0;
    options.cancelled = context->config.cancelled;
    options.userdata = context->config.userdata;
    forge_summary_target target = {0};
    target.scope = (forge_summary_scope)selected;
    target.path = path;
    target.symbol = *symbol ? symbol : NULL;
    forge_summary_generation_stats stats;
    forge_summary_input *input = forge_repo_summary_generate(
        context->repo, context->config.model, &target, &options, reserve, &stats, error);
    account(metrics, &stats);
    forge_status status = input ? FORGE_OK : error->code;
    char *json = NULL;
    if (input) {
        forge_summary_view view;
        forge_summary_input_get(input, &view);
        json = context_json(&view, scope, stats.cache_hit, error);
        if (json && (strlen(json) > limits->max_tool_bytes ||
                     fg_model_count(json, context->config.model) > limits->context_tokens / 4)) {
            free(json);
            json = NULL;
            fg_error(error, FORGE_ERR_LIMIT,
                     "Complete summary context JSON exceeds its output budget");
        }
        if (!json)
            status = error->code;
    }
    bool cancelled = json && options.cancelled && options.cancelled(options.userdata);
    if (json && (cancelled || fg_now_ms() >= deadline)) {
        free(json);
        json = NULL;
        status = fg_error(error, cancelled ? FORGE_ERR_CANCELLED : FORGE_ERR_LIMIT,
                          "Summary context cancelled or deadline reached");
    }
    forge_error record_error = {0};
    if (!record(context, input, &stats, status, &record_error)) {
        free(json);
        json = NULL;
        *error = record_error;
    }
    if (!json)
        metrics->summary_failures = plus(metrics->summary_failures, 1);
    forge_summary_input_destroy(input);
    return json;
}
