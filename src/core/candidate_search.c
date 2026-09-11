#include "candidate_store.h"
#include "conversation.h"
#include "forge/verification.h"

typedef struct {
    fg_session *parent;
    char *final;
    bool failed;
} candidate_events;
static void child_event(const forge_event *event, void *user) {
    candidate_events *c = user;
    if (!strcmp(event->type, "final")) {
        free(c->final);
        yyjson_doc *doc = yyjson_read(event->json, strlen(event->json), 0);
        yyjson_val *data = doc ? yyjson_obj_get(yyjson_doc_get_root(doc), "data") : NULL;
        c->final = yyjson_is_str(data) ? yyjson_val_write(data, 0, NULL) : NULL;
        yyjson_doc_free(doc);
        if (!c->final)
            c->failed = true;
        return;
    }
    if (!strcmp(event->type, "done"))
        return;
    /* Child logs retain exact native events. Parent uses a separate envelope,
     * so discarded trajectories cannot masquerade as the selected conversation. */
    char *type = fg_json_string(event->type);
    fg_buf wrapped = {0};
    if (type)
        fg_buf_printf(&wrapped, "{\"type\":%s,\"data\":%s}", type, event->json);
    free(type);
    if (!wrapped.data || !fg_session_emit(c->parent, "candidate_event", wrapped.data, NULL))
        c->failed = true;
    fg_buf_clear(&wrapped);
}
static void add_metrics(forge_metrics *out, const forge_metrics *m) {
#define SUM(field) out->field += m->field
    SUM(prompt_tokens);
    SUM(generated_tokens);
    SUM(cached_tokens);
    SUM(prefill_tokens);
    SUM(turns);
    SUM(tool_calls);
    SUM(raw_tool_bytes);
    SUM(visible_tool_bytes);
    SUM(files_modified);
    SUM(context_evictions);
    SUM(loop_warnings);
    SUM(grammar_fast_tokens);
    SUM(grammar_fallback_tokens);
    SUM(prefill_ms);
    SUM(decode_ms);
    SUM(sampling_ms);
    SUM(raw_tool_tokens);
    SUM(visible_tool_tokens);
    SUM(files_opened);
    SUM(validation_commands);
    SUM(validation_failures);
    SUM(tool_ms);
    SUM(validation_ms);
    SUM(repo_full_scans);
    SUM(repo_delta_scans);
    SUM(filesystem_events);
    SUM(index_ms);
    SUM(stale_generations);
    SUM(think_tokens);
    SUM(forced_actions);
    SUM(action_stops);
    SUM(action_select_tokens);
    SUM(action_argument_tokens);
    SUM(patch_tokens);
    SUM(final_tokens);
    SUM(memory_tokens);
    SUM(forced_action_progress_tokens);
#undef SUM
    out->load_ms = FG_MAX(out->load_ms, m->load_ms);
    out->generation_arena_peak_bytes =
        FG_MAX(out->generation_arena_peak_bytes, m->generation_arena_peak_bytes);
    out->simulated = m->simulated;
}
static bool real_validation(fg_tool_context *tools, forge_metrics *metrics, forge_error *e) {
    if (!tools->repo)
        tools->repo = forge_repo_open(tools->root, e);
    if (!tools->repo ||
        fg_repo_index_until(tools->repo, NULL, 0, true, tools->deadline, tools->config.cancelled,
                            tools->config.userdata, e) != FORGE_OK)
        return false;
    metrics->repo_full_scans++;
    fg_validation_result result = {0};
    forge_status status = fg_validation_run(tools, NULL, 0, metrics, &result, e);
    bool passed = status == FORGE_OK && result.applicable && result.passed && result.commands;
    fg_validation_result_free(&result);
    return passed;
}
static bool restore_baseline(const fg_candidate_store *candidate,
                             const fg_candidate_store *baseline, fg_tool_context *selection,
                             uint64_t hard_deadline, forge_error *e) {
    fg_tool_context cleanup = *selection;
    /* Leave time inside the original wall limit to restore known contents after
     * a cancelled/timed-out check. Unexpected input changes still refuse restore. */
    cleanup.config.cancelled = NULL;
    cleanup.deadline = hard_deadline;
    bool ok = fg_candidate_store_apply(candidate, baseline, &cleanup, e);
    selection->call_id = cleanup.call_id;
    return ok;
}
forge_status fg_candidate_search(const forge_agent_config *config, const char *request,
                                 fg_session *session, forge_metrics *metrics, forge_event_fn cb,
                                 void *user, forge_error *e) {
    if (!fg_session_start(session, config->workspace, cb, user, e))
        return e ? e->code : FORGE_ERR_IO;
    uint64_t start = fg_now_ms();
    uint64_t hard_deadline = config->limits.wall_timeout_ms > UINT64_MAX - start
                                 ? UINT64_MAX
                                 : start + config->limits.wall_timeout_ms;
    uint64_t cleanup_reserve = FG_MIN(UINT64_C(5000), config->limits.wall_timeout_ms / 10);
    uint64_t deadline = hard_deadline - cleanup_reserve;
    forge_status status = FORGE_OK;
    fg_candidate_store *baseline = NULL, *best = NULL;
    size_t best_index = 0, best_cost = SIZE_MAX;
    char *best_final = NULL;
    forge_conversation *best_conversation = NULL;
    uint32_t seed = config->model->config.seed;
    fg_tool_context selection = {0};
    selection.config = *config;
    selection.session = session;
    selection.deadline = deadline;
    if (!fg_workspace(config->workspace, selection.root, e)) {
        status = e ? e->code : FORGE_ERR_IO;
        goto finish;
    }
    baseline =
        fg_candidate_store_take(selection.root, config->cancelled, config->userdata, deadline, e);
    if (!baseline) {
        status = e ? e->code : FORGE_ERR_IO;
        goto finish;
    }
    for (size_t i = 0; i < config->candidate_count; ++i) {
        if (metrics->turns > config->limits.max_turns ||
            metrics->generated_tokens > config->limits.max_generated_tokens ||
            metrics->prompt_tokens > config->limits.max_input_tokens) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Candidate exceeded its shared budget");
            goto finish;
        }
        size_t left = config->candidate_count - i;
        size_t turns = (config->limits.max_turns - metrics->turns) / left;
        size_t tokens = (config->limits.max_generated_tokens - metrics->generated_tokens) / left;
        size_t inputs = (config->limits.max_input_tokens - metrics->prompt_tokens) / left;
        if (fg_now_ms() >= deadline || (config->cancelled && config->cancelled(config->userdata)) ||
            turns < 3 || !tokens || !inputs) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Shared candidate budget exhausted");
            goto finish;
        }
        char relative[64], root[FG_PATH_MAX], event[256];
        snprintf(relative, sizeof(relative), "trial-%02zu", i + 1);
        if (!fg_path_join(root, session->dir, relative) ||
            !fg_candidate_store_materialize_until(baseline, root, config->cancelled,
                                                  config->userdata, deadline, e)) {
            status = e && e->code ? e->code : FORGE_ERR_IO;
            goto finish;
        }
        if (fg_now_ms() >= deadline) {
            status = fg_error(e, FORGE_ERR_LIMIT, "Candidate copy exhausted the shared deadline");
            goto finish;
        }
        uint32_t trial_seed = seed + (uint32_t)i * UINT32_C(0x9e3779b9);
        snprintf(event, sizeof(event),
                 "{\"candidate\":%zu,\"count\":%zu,\"seed\":%u,\"max_turns\":%zu,\"independent_"
                 "baseline\":true}",
                 i + 1, config->candidate_count, trial_seed, turns);
        if (!fg_session_emit(session, "candidate_start", event, e)) {
            status = FORGE_ERR_IO;
            goto finish;
        }
        forge_agent_config trial = *config;
        trial.workspace = root;
        trial.candidate_count = 0;
        trial.limits.max_turns = turns;
        trial.limits.max_generated_tokens = tokens;
        trial.limits.max_input_tokens = inputs;
        trial.conversation =
            config->conversation ? forge_conversation_clone(config->conversation, e) : NULL;
        if (config->conversation && !trial.conversation) {
            status = FORGE_ERR_MEMORY;
            goto finish;
        }
        /* Event callbacks and cloning may consume the remaining time. Read
         * the clock once after both, before unsigned subtraction; a zero child
         * allocation must not accidentally become an unlimited/default budget. */
        uint64_t now = fg_now_ms();
        if (now >= deadline || (deadline - now) / (left + 1) == 0 ||
            (config->cancelled && config->cancelled(config->userdata))) {
            forge_conversation_destroy(trial.conversation);
            status = fg_error(e,
                              now >= deadline || (deadline - now) / (left + 1) == 0
                                  ? FORGE_ERR_LIMIT
                                  : FORGE_ERR_CANCELLED,
                              "Candidate preparation exhausted its shared budget");
            goto finish;
        }
        trial.limits.wall_timeout_ms = (deadline - now) / (left + 1);
        config->model->config.seed = trial_seed;
        forge_agent *agent = forge_agent_create(&trial, e);
        fg_agent_mark_independent_workspace(agent);
        candidate_events events = {session, NULL, false};
        forge_status run = agent ? forge_agent_run(agent, request, child_event, &events, e)
                                 : (e ? e->code : FORGE_ERR_MEMORY);
        if (agent)
            add_metrics(metrics, forge_agent_metrics(agent));
        char *child_session =
            agent ? fg_json_string(forge_agent_session(agent)) : fg_strdup("null");
        fg_buf outcome = {0};
        if (child_session)
            fg_buf_printf(&outcome, "{\"candidate\":%zu,\"status\":\"%s\",\"session\":%s}", i + 1,
                          forge_status_string(run), child_session);
        free(child_session);
        forge_agent_destroy(agent);
        bool recorded =
            outcome.data && fg_session_emit(session, "candidate_generated", outcome.data, e);
        fg_buf_clear(&outcome);
        if (!recorded || events.failed) {
            free(events.final);
            forge_conversation_destroy(trial.conversation);
            status = FORGE_ERR_IO;
            goto finish;
        }
        fg_candidate_store *candidate =
            run == FORGE_OK && events.final
                ? fg_candidate_store_take(root, config->cancelled, config->userdata, deadline, e)
                : NULL;
        bool eligible = candidate && !fg_candidate_store_equal(candidate, baseline);
        if (eligible) {
            if (!fg_candidate_store_apply_reserving_restore(baseline, candidate, &selection, e)) {
                fg_candidate_store_recover(baseline, candidate, &selection, hard_deadline, e);
                fg_candidate_store_destroy(candidate);
                free(events.final);
                forge_conversation_destroy(trial.conversation);
                status = e && e->code ? e->code : FORGE_ERR_IO;
                goto finish;
            }
            forge_error validation_error = {0};
            bool passed = real_validation(&selection, metrics, &validation_error);
            size_t cost = fg_candidate_store_cost(baseline, candidate);
            snprintf(event, sizeof(event),
                     "{\"candidate\":%zu,\"real_workspace\":true,\"passed\":%s,\"changed_content_"
                     "cost\":%zu}",
                     i + 1, passed ? "true" : "false", cost);
            bool evidence = fg_session_emit(session, "candidate_selection", event, e);
            /* Refuse to overwrite unexpected mutation by validation or an
             * external writer. Initial content remains in trial workspaces and
             * the before/after selection journal for explicit recovery. */
            if (!restore_baseline(candidate, baseline, &selection, hard_deadline, e)) {
                fg_candidate_store_destroy(candidate);
                free(events.final);
                forge_conversation_destroy(trial.conversation);
                status = e && e->code ? e->code : FORGE_ERR_IO;
                goto finish;
            }
            if (!evidence) {
                fg_candidate_store_destroy(candidate);
                free(events.final);
                forge_conversation_destroy(trial.conversation);
                status = FORGE_ERR_IO;
                goto finish;
            }
            if (passed && cost < best_cost) {
                fg_candidate_store_destroy(best);
                best = candidate;
                candidate = NULL;
                free(best_final);
                best_final = events.final;
                events.final = NULL;
                forge_conversation_destroy(best_conversation);
                best_conversation = trial.conversation;
                trial.conversation = NULL;
                best_cost = cost;
                best_index = i + 1;
            }
        }
        fg_candidate_store_destroy(candidate);
        free(events.final);
        forge_conversation_destroy(trial.conversation);
        if (run == FORGE_ERR_CANCELLED ||
            (config->cancelled && config->cancelled(config->userdata))) {
            status = fg_error(e, FORGE_ERR_CANCELLED, "Candidate search cancelled");
            goto finish;
        }
        if (e)
            memset(e, 0, sizeof(*e));
    }
    if (!best) {
        status = fg_error(e, FORGE_ERR_CONFLICT,
                          "No changed candidate completed and passed real-workspace validation");
        goto finish;
    }
    if (!fg_candidate_store_apply_reserving_restore(baseline, best, &selection, e)) {
        fg_candidate_store_recover(baseline, best, &selection, hard_deadline, e);
        status = e && e->code ? e->code : FORGE_ERR_IO;
        goto finish;
    }
    if (!real_validation(&selection, metrics, e)) {
        forge_error restore = {0};
        if (!restore_baseline(best, baseline, &selection, hard_deadline, &restore)) {
            if (e)
                *e = restore;
        }
        status = e && e->code ? e->code : FORGE_ERR_CONFLICT;
        goto finish;
    }
    if (config->conversation && best_conversation) {
        /* Replace shared history only after the winner passed in the actual
         * workspace. The helper swaps owned content without reallocating. */
        fg_conversation_swap(config->conversation, best_conversation);
    }
    char winner[128];
    snprintf(winner, sizeof(winner),
             "{\"candidate\":%zu,\"count\":%zu,\"selection\":\"passing_then_smallest_content_"
             "change_then_first\"}",
             best_index, config->candidate_count);
    if (!fg_session_emit(session, "candidate_selected", winner, e) ||
        !fg_session_emit(session, "final", best_final, e))
        status = FORGE_ERR_IO;
finish:
    config->model->config.seed = seed;
    metrics->duration_ms = (double)(fg_now_ms() - start);
    if (!fg_session_finish(session, metrics, status, status == FORGE_OK ? e : NULL) &&
        status == FORGE_OK)
        status = FORGE_ERR_IO;
    forge_repo_close(selection.repo);
    fg_candidate_store_destroy(baseline);
    fg_candidate_store_destroy(best);
    free(best_final);
    forge_conversation_destroy(best_conversation);
    return status;
}
