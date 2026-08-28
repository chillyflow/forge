#include "internal.h"
#include "forge/validation.h"
#include "forge/verification.h"
#include "input_snapshot.h"

#define VALIDATION_MAX_FILES 100000u
#define VALIDATION_MAX_BYTES (UINT64_C(2) * 1024u * 1024u * 1024u)

static bool cancelled(fg_tool_context *c) {
    return (c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
           fg_now_ms() >= c->deadline;
}

/* Keep the execution verdict separate from successful delivery of its evidence.
 * A recording failure must never leave the returned report claiming success. */
static bool report_verdict(yyjson_mut_doc *doc, yyjson_mut_val *report,
                           fg_validation_result *result, forge_status status, bool checks_passed,
                           bool evidence_complete, forge_error *e) {
    result->passed = checks_passed && status == FORGE_OK && evidence_complete;
    if (status != FORGE_OK && !evidence_complete) {
        fg_buf message = {0};
        fg_buf_printf(&message, "Validation evidence could not be fully recorded: %s\n",
                      e && e->message[0] ? e->message : forge_status_string(status));
        if (result->summary)
            fg_buf_puts(&message, result->summary);
        free(result->summary);
        result->summary = fg_buf_take(&message);
    }
    const char *status_name =
        status == FORGE_OK && !result->applicable ? "not_applicable" : forge_status_string(status);
    return result->summary &&
           yyjson_mut_obj_put(report, yyjson_mut_str(doc, "passed"),
                              yyjson_mut_bool(doc, result->passed)) &&
           yyjson_mut_obj_put(report, yyjson_mut_str(doc, "checks_passed"),
                              yyjson_mut_bool(doc, checks_passed)) &&
           yyjson_mut_obj_put(report, yyjson_mut_str(doc, "evidence_complete"),
                              yyjson_mut_bool(doc, evidence_complete)) &&
           yyjson_mut_obj_put(report, yyjson_mut_str(doc, "status"),
                              yyjson_mut_str(doc, status_name)) &&
           yyjson_mut_obj_put(report, yyjson_mut_str(doc, "summary"),
                              yyjson_mut_strcpy(doc, result->summary));
}

static bool emit_value(fg_session *session, const char *type, yyjson_mut_val *value,
                       forge_error *e) {
    char *json = yyjson_mut_val_write(value, 0, NULL);
    if (!json) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot encode validation event");
        return false;
    }
    bool ok = fg_session_emit(session, type, json, e);
    free(json);
    return ok;
}

void fg_validation_result_free(fg_validation_result *result) {
    if (result) {
        free(result->json);
        free(result->summary);
        memset(result, 0, sizeof(*result));
    }
}

forge_status fg_validation_run(fg_tool_context *c, const char *const *paths, size_t count,
                               forge_metrics *metrics, fg_validation_result *result,
                               forge_error *e) {
    memset(result, 0, sizeof(*result));
    uint64_t start = fg_now_ms();
    fg_input_snapshot *before = NULL, *after = NULL;
    double snapshot_ms = 0;
    size_t attempted = 0;
    bool evidence_complete = true;
    result->generation = forge_repo_generation(c->repo);
    size_t attempt = ++c->validation_id;
    char *plan = forge_repo_validation_plan(c->repo, paths, count, e);
    if (!plan)
        return e && e->code ? e->code : FORGE_ERR_MEMORY;
    yyjson_doc *pd = yyjson_read(plan, strlen(plan), 0);
    yyjson_val *po = pd ? yyjson_doc_get_root(pd) : NULL;
    yyjson_val *stages = yyjson_obj_get(po, "stages");
    if (!yyjson_is_arr(stages) || !yyjson_is_bool(yyjson_obj_get(po, "applicable"))) {
        free(plan);
        yyjson_doc_free(pd);
        return fg_error(e, FORGE_ERR_PARSE, "Invalid validation plan");
    }
    result->applicable = yyjson_get_bool(yyjson_obj_get(po, "applicable"));
    yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
    if (!rd) {
        free(plan);
        yyjson_doc_free(pd);
        return fg_error(e, FORGE_ERR_MEMORY, "Validation report allocation failed");
    }
    yyjson_mut_val *report = yyjson_mut_obj(rd), *commands = yyjson_mut_arr(rd);
    yyjson_mut_doc_set_root(rd, report);
    yyjson_mut_obj_add_uint(rd, report, "schema_version", 1);
    yyjson_mut_obj_add_uint(rd, report, "attempt", attempt);
    yyjson_mut_obj_add_uint(rd, report, "generation", result->generation);
    yyjson_mut_obj_add_bool(rd, report, "applicable", result->applicable);
    yyjson_mut_obj_add_strcpy(rd, report, "session", c->session->dir);
    yyjson_mut_obj_add_val(rd, report, "plan", yyjson_val_mut_copy(rd, po));
    yyjson_mut_obj_add_val(rd, report, "commands", commands);
    forge_status status = FORGE_OK;
    char plan_artifact[96];
    snprintf(plan_artifact, sizeof(plan_artifact), "validation/%04zu.plan.json", attempt);
    if (!fg_session_artifact(c->session, plan_artifact, plan, e) ||
        !fg_session_emit(c->session, "validation_plan", plan, e)) {
        evidence_complete = false;
        status = e && e->code ? e->code : FORGE_ERR_IO;
        goto finish;
    }
    if (cancelled(c)) {
        status = fg_error(e, FORGE_ERR_CANCELLED, "Validation cancelled or deadline reached");
        goto finish;
    }
    if (result->applicable) {
        uint64_t snapshot_start = fg_now_ms();
        before = fg_input_snapshot_take(c->root, VALIDATION_MAX_FILES, VALIDATION_MAX_BYTES,
                                        c->config.cancelled, c->config.userdata, c->deadline, e);
        snapshot_ms += (double)(fg_now_ms() - snapshot_start);
        if (!before) {
            status = e && e->code ? e->code : FORGE_ERR_IO;
            goto finish;
        }
    }
    size_t si, stage_count;
    yyjson_val *stage;
    yyjson_arr_foreach(stages, si, stage_count, stage) {
        if (!result->applicable)
            break;
        const char *name = fg_json_str(stage, "name");
        yyjson_val *list = yyjson_obj_get(stage, "commands");
        if (!name || !yyjson_is_arr(list)) {
            status = fg_error(e, FORGE_ERR_PARSE, "Invalid validation stage");
            break;
        }
        size_t ci, command_count;
        yyjson_val *command;
        yyjson_arr_foreach(list, ci, command_count, command) {
            if (cancelled(c)) {
                status =
                    fg_error(e, FORGE_ERR_CANCELLED, "Validation cancelled or deadline reached");
                break;
            }
            const char *relative = fg_json_str(command, "cwd");
            yyjson_val *av = yyjson_obj_get(command, "argv");
            bool require_empty = yyjson_get_bool(yyjson_obj_get(command, "require_empty_stdout"));
            if (!relative || !yyjson_is_arr(av) || !yyjson_arr_size(av) ||
                yyjson_arr_size(av) > 64) {
                status = fg_error(e, FORGE_ERR_PARSE, "Invalid validation command");
                break;
            }
            const char *argv[65] = {0};
            size_t ai, argc;
            yyjson_val *argument;
            yyjson_arr_foreach(av, ai, argc, argument) {
                if (!yyjson_is_str(argument) ||
                    yyjson_get_len(argument) != strlen(yyjson_get_str(argument))) {
                    status = fg_error(e, FORGE_ERR_PARSE, "Invalid validation argument");
                    break;
                }
                argv[ai] = yyjson_get_str(argument);
            }
            if (status != FORGE_OK)
                break;
            char cwd[FG_PATH_MAX];
            if (!strcmp(relative, "."))
                strcpy(cwd, c->root);
            else if (!fg_safe_path(c->root, relative, false, cwd, e)) {
                status = e && e->code ? e->code : FORGE_ERR_POLICY;
                break;
            }
            yyjson_mut_val *record = yyjson_mut_obj(rd);
            size_t command_id = ++attempted;
            yyjson_mut_obj_add_uint(rd, record, "id", command_id);
            yyjson_mut_obj_add_strcpy(rd, record, "stage", name);
            yyjson_mut_obj_add_strcpy(rd, record, "cwd", relative);
            yyjson_mut_obj_add_val(rd, record, "argv", yyjson_val_mut_copy(rd, av));
            yyjson_mut_obj_add_bool(rd, record, "require_empty_stdout", require_empty);
            yyjson_mut_obj_add_bool(rd, record, "started", false);
            yyjson_mut_arr_add_val(commands, record);
            char *policy_json = yyjson_mut_val_write(record, 0, NULL);
            if (!policy_json) {
                status = fg_error(e, FORGE_ERR_MEMORY, "Validation policy allocation failed");
                break;
            }
            bool allowed = c->config.allow_exec;
            if (c->config.policy)
                allowed = c->config.policy("run_command", FORGE_CAP_PROCESS, policy_json,
                                           c->config.userdata);
            free(policy_json);
            if (!allowed) {
                yyjson_mut_obj_add_str(rd, record, "status", "denied");
                yyjson_mut_obj_add_sint(rd, record, "exit_code", -1);
                if (!emit_value(c->session, "validation_command", record, e)) {
                    evidence_complete = false;
                    status = e && e->code ? e->code : FORGE_ERR_IO;
                } else
                    status = fg_error(
                        e, FORGE_ERR_POLICY,
                        "Automatic validation requires explicit unsandboxed process approval");
                break;
            }
            if (!emit_value(c->session, "validation_command_start", record, e)) {
                evidence_complete = false;
                status = e && e->code ? e->code : FORGE_ERR_IO;
                break;
            }
            fg_process_result r = {0};
            r.exit_code = -1;
            /* Policy and event callbacks can consume the remaining wall budget.
             * Re-read both cancellation and the clock immediately before spawn. */
            if (cancelled(c)) {
                r.cancelled = true;
                status =
                    fg_error(e, FORGE_ERR_CANCELLED, "Validation cancelled before command launch");
            } else {
                uint64_t now = fg_now_ms();
                if (now >= c->deadline) {
                    r.cancelled = true;
                    status = fg_error(e, FORGE_ERR_CANCELLED,
                                      "Validation deadline reached before command launch");
                } else
                    status = fg_process_at(
                        c->root, cwd, argv,
                        FG_MIN(c->deadline - now, c->config.limits.command_timeout_ms),
                        c->config.limits.max_tool_bytes, c->config.cancelled, c->config.userdata,
                        &r, e);
            }
            if (r.started) {
                result->commands++;
                metrics->validation_commands++;
            }
            yyjson_mut_set_bool(yyjson_mut_obj_get(record, "started"), r.started);
            yyjson_mut_obj_add_sint(rd, record, "exit_code", r.exit_code);
            yyjson_mut_obj_add_bool(rd, record, "timeout", r.timed_out);
            yyjson_mut_obj_add_bool(rd, record, "cancelled", r.cancelled);
            yyjson_mut_obj_add_bool(rd, record, "truncated", r.truncated);
            yyjson_mut_obj_add_real(rd, record, "duration_ms", r.duration_ms);
            yyjson_mut_obj_add_uint(rd, record, "stdout_bytes", r.out_len);
            yyjson_mut_obj_add_uint(rd, record, "stderr_bytes", r.err_len);
            char artifact[96];
            snprintf(artifact, sizeof(artifact), "validation/%04zu-%04zu.stdout", attempt,
                     command_id);
            yyjson_mut_obj_add_strcpy(rd, record, "stdout_artifact", artifact);
            bool saved =
                fg_session_artifact_bytes(c->session, artifact, r.out ? r.out : "", r.out_len, e);
            snprintf(artifact, sizeof(artifact), "validation/%04zu-%04zu.stderr", attempt,
                     command_id);
            yyjson_mut_obj_add_strcpy(rd, record, "stderr_artifact", artifact);
            saved =
                fg_session_artifact_bytes(c->session, artifact, r.err ? r.err : "", r.err_len, e) &&
                saved;
            if (!saved) {
                evidence_complete = false;
                status = e && e->code ? e->code : FORGE_ERR_IO;
            }
            if (status == FORGE_OK && r.cancelled)
                status = fg_error(e, FORGE_ERR_CANCELLED, "Validation command cancelled");
            else if (status == FORGE_OK && r.timed_out)
                status = fg_error(e, FORGE_ERR_LIMIT, "Validation command timed out");
            else if (status == FORGE_OK && r.truncated)
                status =
                    fg_error(e, FORGE_ERR_LIMIT, "Validation output exceeded its capture budget");
            else if (status == FORGE_OK && (r.exit_code != 0 || (require_empty && r.out_len)))
                status = fg_error(e, FORGE_ERR_CONFLICT,
                                  "Validation stage %s failed (exit_code=%d%s)", name, r.exit_code,
                                  require_empty && r.out_len ? ", files need formatting" : "");
            yyjson_mut_obj_add_str(rd, record, "status", forge_status_string(status));
            if (status != FORGE_OK) {
                char *raw = fg_process_render(&r);
                char *visible = raw ? fg_compress_output(raw, 4096, NULL, NULL) : NULL;
                fg_buf message = {0};
                fg_buf_printf(&message, "Automatic validation stopped at %s: %s\n", name,
                              e && e->message[0] ? e->message : forge_status_string(status));
                if (visible)
                    fg_buf_puts(&message, visible);
                if (require_empty && r.exit_code == 0 && r.out_len)
                    fg_buf_puts(&message,
                                "\nThe listed files need gofmt formatting. Format only files "
                                "you are authorized to edit (for example with gofmt -w), then "
                                "try validation again. A zero formatter exit code alone does "
                                "not pass this check.\n");
                result->summary = fg_buf_take(&message);
                free(raw);
                free(visible);
            }
            if (!emit_value(c->session, "validation_command", record, e)) {
                evidence_complete = false;
                status = e && e->code ? e->code : FORGE_ERR_IO;
            }
            fg_process_free(&r);
            if (status != FORGE_OK)
                break;
        }
        if (status != FORGE_OK)
            break;
        if (yyjson_arr_size(list))
            result->stages++;
    }
finish:
    if (before && result->commands) {
        uint64_t snapshot_start = fg_now_ms();
        forge_error snapshot_error = {0};
        after = fg_input_snapshot_take(c->root, VALIDATION_MAX_FILES, VALIDATION_MAX_BYTES,
                                       c->config.cancelled, c->config.userdata, c->deadline,
                                       &snapshot_error);
        snapshot_ms += (double)(fg_now_ms() - snapshot_start);
        if (!after) {
            result->inputs_changed = true; /* Unknown is never accepted as unchanged. */
            status = snapshot_error.code ? snapshot_error.code : FORGE_ERR_IO;
            if (e)
                *e = snapshot_error;
        } else if (!fg_input_snapshot_equal(before, after)) {
            result->inputs_changed = true;
            status = fg_error(e, FORGE_ERR_CONFLICT,
                              "Workspace inputs changed during validation; verify them again");
            if (result->summary) {
                fg_buf message = {0};
                fg_buf_puts(&message, result->summary);
                fg_buf_puts(&message, "\nWorkspace inputs changed during validation.");
                free(result->summary);
                result->summary = fg_buf_take(&message);
            }
        }
    }
    if (result->commands) {
        forge_error index_error = {0};
        forge_status indexed =
            fg_repo_index_until(c->repo, NULL, 0, true, c->deadline, c->config.cancelled,
                                c->config.userdata, &index_error);
        if (status == FORGE_OK && indexed != FORGE_OK) {
            status = indexed;
            if (e)
                *e = index_error;
        } else if (status == FORGE_OK && forge_repo_generation(c->repo) != result->generation) {
            status = fg_error(e, FORGE_ERR_CONFLICT,
                              "Repository changed during validation; verify the new generation");
        }
    }
    if (status == FORGE_OK && cancelled(c))
        status = fg_error(e, FORGE_ERR_CANCELLED, "Validation cancelled or deadline reached");
    if (status == FORGE_OK && result->applicable && !result->commands)
        status = fg_error(e, FORGE_ERR_NOT_FOUND, "No applicable validation command was planned");
    bool checks_passed = status == FORGE_OK && result->applicable && result->commands > 0 &&
                         before && after && !result->inputs_changed;
    result->passed = checks_passed;
    if (!result->summary) {
        fg_buf message = {0};
        if (status == FORGE_OK && !result->applicable)
            fg_buf_puts(&message, "Automatic validation not applicable: no indexed Go target. "
                                  "No test success has been established.");
        else if (result->passed)
            fg_buf_printf(&message,
                          "Automatic Go validation passed: %zu commands across %zu "
                          "stages, repository generation=%llu.",
                          result->commands, result->stages, (unsigned long long)result->generation);
        else
            fg_buf_printf(&message, "Automatic validation did not pass: %s",
                          e && e->message[0] ? e->message : forge_status_string(status));
        result->summary = fg_buf_take(&message);
    }
    double elapsed = (double)(fg_now_ms() - start);
    yyjson_mut_obj_add_uint(rd, report, "generation_after", forge_repo_generation(c->repo));
    yyjson_mut_obj_add_uint(rd, report, "commands_attempted", attempted);
    yyjson_mut_obj_add_uint(rd, report, "commands_run", result->commands);
    yyjson_mut_obj_add_uint(rd, report, "stages_passed", result->stages);
    yyjson_mut_obj_add_bool(rd, report, "inputs_changed", result->inputs_changed);
    yyjson_mut_obj_add_bool(rd, report, "inputs_checked", before && after);
    yyjson_mut_obj_add_uint(rd, report, "input_max_files", VALIDATION_MAX_FILES);
    yyjson_mut_obj_add_uint(rd, report, "input_max_bytes", VALIDATION_MAX_BYTES);
    char hash[17];
    if (before) {
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)fg_input_snapshot_hash(before));
        yyjson_mut_obj_add_strcpy(rd, report, "input_hash_before", hash);
    }
    if (after) {
        snprintf(hash, sizeof(hash), "%016llx", (unsigned long long)fg_input_snapshot_hash(after));
        yyjson_mut_obj_add_strcpy(rd, report, "input_hash_after", hash);
    }
    yyjson_mut_obj_add_real(rd, report, "snapshot_ms", snapshot_ms);
    yyjson_mut_obj_add_real(rd, report, "duration_ms", elapsed);
    if (report_verdict(rd, report, result, status, checks_passed, evidence_complete, e))
        result->json = yyjson_mut_write(rd, YYJSON_WRITE_PRETTY, NULL);
    if (!result->json || !result->summary) {
        result->passed = false;
        status = fg_error(e, FORGE_ERR_MEMORY, "Validation report allocation failed");
    } else {
        char artifact[96];
        snprintf(artifact, sizeof(artifact), "validation/%04zu.json", attempt);
        forge_error save_error = {0};
        bool saved =
            fg_session_artifact(c->session, artifact, result->json, &save_error) &&
            fg_session_artifact(c->session, "validation/latest.json", result->json, &save_error) &&
            fg_session_emit(c->session, "validation_result", result->json, &save_error);
        if (!saved) {
            status = save_error.code ? save_error.code : FORGE_ERR_IO;
            if (e)
                *e = save_error;
            result->passed = false;
            free(result->json);
            result->json = NULL;
            if (report_verdict(rd, report, result, status, checks_passed, false, e))
                result->json = yyjson_mut_write(rd, YYJSON_WRITE_PRETTY, NULL);
            if (result->json) {
                /* Best effort reconciliation of earlier writes. The API status
                 * remains a failure even if these repairs succeed. */
                fg_session_artifact(c->session, artifact, result->json, NULL);
                fg_session_artifact(c->session, "validation/latest.json", result->json, NULL);
                fg_session_emit(c->session, "validation_recording_error", result->json, NULL);
            }
        }
    }
    metrics->validation_ms += (double)(fg_now_ms() - start);
    if (status != FORGE_OK)
        metrics->validation_failures++;
    fg_input_snapshot_destroy(before);
    fg_input_snapshot_destroy(after);
    free(plan);
    yyjson_doc_free(pd);
    yyjson_mut_doc_free(rd);
    return status;
}

static void reconcile_finish_failure(fg_session *session, fg_validation_result *result,
                                     forge_status status, forge_error *e) {
    result->passed = false;
    if (!result->json)
        return;
    yyjson_doc *old = yyjson_read(result->json, strlen(result->json), 0);
    yyjson_mut_doc *doc = old ? yyjson_doc_mut_copy(old, NULL) : NULL;
    bool rewritten = false;
    if (doc) {
        yyjson_mut_val *report = yyjson_mut_doc_get_root(doc);
        bool checks_passed = yyjson_mut_get_bool(yyjson_mut_obj_get(report, "checks_passed"));
        if (report_verdict(doc, report, result, status, checks_passed, false, e)) {
            char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, NULL);
            if (json) {
                free(result->json);
                result->json = json;
                rewritten = true;
                char artifact[96];
                snprintf(
                    artifact, sizeof(artifact), "validation/%04llu.json",
                    (unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(report, "attempt")));
                fg_session_artifact(session, artifact, result->json, NULL);
                fg_session_artifact(session, "validation/latest.json", result->json, NULL);
            }
        }
    }
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(old);
    if (!rewritten) {
        free(result->json);
        result->json = NULL;
    }
}

forge_status forge_verify_workspace(const forge_agent_config *config, const char *const *paths,
                                    size_t count, forge_event_fn cb, void *user, char **report_json,
                                    forge_error *e) {
    if (report_json)
        *report_json = NULL;
    if (!config || !config->limits.command_timeout_ms ||
        config->limits.command_timeout_ms > UINT64_C(86400000) || !config->limits.wall_timeout_ms ||
        !config->limits.max_tool_bytes || config->limits.max_tool_bytes > 16u * 1024u * 1024u ||
        (count && !paths) || count > 1024)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid verification configuration");
    fg_tool_context tools = {0};
    tools.config = *config;
    uint64_t start = fg_now_ms();
    tools.deadline = config->limits.wall_timeout_ms > UINT64_MAX - start
                         ? UINT64_MAX
                         : start + config->limits.wall_timeout_ms;
    if (!fg_workspace(config->workspace, tools.root, e))
        return e && e->code ? e->code : FORGE_ERR_ARGUMENT;
    tools.repo = forge_repo_open(tools.root, e);
    if (!tools.repo)
        return e && e->code ? e->code : FORGE_ERR_IO;
    forge_status status = fg_repo_index_until(tools.repo, NULL, 0, true, tools.deadline,
                                              config->cancelled, config->userdata, e);
    if (status != FORGE_OK) {
        forge_repo_close(tools.repo);
        return status;
    }
    fg_session session = {0};
    forge_metrics metrics = {0};
    if (!fg_session_start(&session, tools.root, cb, user, e)) {
        forge_repo_close(tools.repo);
        return e && e->code ? e->code : FORGE_ERR_IO;
    }
    tools.session = &session;
    fg_validation_result result = {0};
    status = fg_validation_run(&tools, paths, count, &metrics, &result, e);
    metrics.duration_ms = (double)(fg_now_ms() - start);
    forge_error finish_error = {0};
    if (!fg_session_finish(&session, &metrics, status, &finish_error)) {
        if (status == FORGE_OK)
            metrics.validation_failures++;
        status = finish_error.code ? finish_error.code : FORGE_ERR_IO;
        if (e)
            *e = finish_error;
        reconcile_finish_failure(&session, &result, status, e);
        char *json = fg_metrics_json(&metrics, status);
        if (json) {
            fg_session_artifact(&session, "metrics.json", json, NULL);
            free(json);
        }
    }
    if (report_json) {
        *report_json = result.json;
        result.json = NULL;
    }
    fg_validation_result_free(&result);
    forge_repo_close(tools.repo);
    return status;
}
