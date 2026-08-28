#include "internal.h"
bool fg_session_start(fg_session *s, const char *root, forge_event_fn cb, void *user,
                      forge_error *e) {
    memset(s, 0, sizeof(*s));
    s->callback = cb;
    s->userdata = user;
    s->start_ms = fg_now_ms();
    s->edit_bytes_limit = 256u * 1024u * 1024u;
    char base[FG_PATH_MAX], sessions[FG_PATH_MAX], id[33], file[FG_PATH_MAX];
    if (!fg_path_join(base, root, ".forge") || !fg_mkdir(base, e) ||
        !fg_path_join(sessions, base, "sessions") || !fg_mkdir(sessions, e))
        return false;
    if (!fg_random_hex(id, 16) || !fg_path_join(s->dir, sessions, id) || !fg_mkdir(s->dir, e))
        return false;
    const char *folders[] = {"tool", "context", "validation"};
    for (size_t i = 0; i < sizeof(folders) / sizeof(*folders); i++)
        if (!fg_path_join(file, s->dir, folders[i]) || !fg_mkdir(file, e))
            return false;
    if (!fg_path_join(file, s->dir, "events.jsonl"))
        return false;
    s->events = fopen(file, "wb");
    if (!s->events) {
        fg_error(e, FORGE_ERR_IO, "Cannot create event log");
        return false;
    }
    return true;
}
bool fg_session_emit(fg_session *s, const char *type, const char *payload, forge_error *e) {
    if (!s || !s->events)
        return false;
    yyjson_doc *value = yyjson_read(payload, strlen(payload), 0);
    if (!value) {
        fg_error(e, FORGE_ERR_PARSE, "Invalid event payload");
        return false;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        yyjson_doc_free(value);
        return false;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, obj);
    yyjson_mut_obj_add_uint(doc, obj, "schema_version", 1);
    yyjson_mut_obj_add_str(doc, obj, "type", type);
    yyjson_mut_obj_add_uint(doc, obj, "sequence", ++s->sequence);
    yyjson_mut_obj_add_uint(doc, obj, "elapsed_ms", fg_now_ms() - s->start_ms);
    yyjson_mut_obj_add_val(doc, obj, "data", yyjson_val_mut_copy(doc, yyjson_doc_get_root(value)));
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_doc_free(value);
    yyjson_mut_doc_free(doc);
    if (!json) {
        fg_error(e, FORGE_ERR_MEMORY, "Event allocation failed");
        return false;
    }
    bool ok =
        fputs(json, s->events) >= 0 && fputc('\n', s->events) != EOF && fflush(s->events) == 0;
    if (ok && s->callback) {
        forge_event event = {type, s->sequence, json};
        s->callback(&event, s->userdata);
    }
    free(json);
    if (!ok)
        fg_error(e, FORGE_ERR_IO, "Event log write failed");
    return ok;
}
bool fg_session_artifact(fg_session *s, const char *name, const char *text, forge_error *e) {
    return fg_session_artifact_bytes(s, name, text, strlen(text), e);
}
bool fg_session_artifact_bytes(fg_session *s, const char *name, const char *bytes, size_t length,
                               forge_error *e) {
    char path[FG_PATH_MAX];
    if (!fg_path_join(path, s->dir, name)) {
        fg_error(e, FORGE_ERR_LIMIT, "Artifact path too long");
        return false;
    }
    return fg_write_file(path, bytes, length, e);
}
char *fg_metrics_json(const forge_metrics *m, forge_status status) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_uint(d, o, "schema_version", 1);
    yyjson_mut_obj_add_str(d, o, "status", forge_status_string(status));
    yyjson_mut_obj_add_bool(d, o, "simulated", m->simulated);
#define U(field) yyjson_mut_obj_add_uint(d, o, #field, m->field)
#define R(field) yyjson_mut_obj_add_real(d, o, #field, m->field)
    U(prompt_tokens);
    U(generated_tokens);
    U(cached_tokens);
    U(prefill_tokens);
    U(turns);
    U(tool_calls);
    U(raw_tool_bytes);
    U(visible_tool_bytes);
    U(files_modified);
    U(context_evictions);
    U(loop_warnings);
    U(grammar_fast_tokens);
    U(grammar_fallback_tokens);
    U(raw_tool_tokens);
    U(visible_tool_tokens);
    U(files_opened);
    U(validation_commands);
    U(validation_failures);
    U(generation_arena_peak_bytes);
    U(repo_full_scans);
    U(repo_delta_scans);
    U(filesystem_events);
    U(watch_reopens);
    U(stale_generations);
    U(index_cold_parses);
    U(index_incremental_parses);
    U(index_cache_hits);
    U(index_cache_evictions);
    U(peak_index_source_bytes);
    U(peak_index_nodes);
    U(checkpoint_lookups);
    U(checkpoint_hits);
    U(checkpoint_misses);
    U(checkpoint_captures);
    U(checkpoint_evictions);
    U(checkpoint_restored_tokens);
    U(checkpoint_reused_tokens);
    U(checkpoint_additional_tokens);
    U(checkpoint_peak_bytes);
    U(summary_lookups);
    U(summary_hits);
    U(summary_generations);
    U(summary_failures);
    R(load_ms);
    R(prefill_ms);
    R(decode_ms);
    R(sampling_ms);
    R(duration_ms);
    R(tool_ms);
    R(validation_ms);
    R(index_ms);
    R(checkpoint_probe_ms);
    R(checkpoint_capture_ms);
    R(checkpoint_restore_ms);
    R(summary_ms);
#undef U
#undef R
    char *json = yyjson_mut_write(d, YYJSON_WRITE_PRETTY, NULL);
    yyjson_mut_doc_free(d);
    return json;
}
bool fg_session_finish(fg_session *s, const forge_metrics *m, forge_status status, forge_error *e) {
    char *json = fg_metrics_json(m, status);
    bool ok = json != NULL;
    if (!json)
        fg_error(e, FORGE_ERR_MEMORY, "Metrics allocation failed");
    if (ok)
        ok = fg_session_artifact(s, "metrics.json", json, e);
    if (ok)
        ok = fg_session_emit(s, status == FORGE_OK ? "done" : "error", json, e);
    free(json);
    if (s->events) {
        if (fclose(s->events) != 0) {
            fg_error(e, FORGE_ERR_IO, "Event log close failed");
            ok = false;
        }
        s->events = NULL;
    }
    return ok;
}
forge_status forge_replay(const char *session, forge_event_fn cb, void *user, forge_error *e) {
    char path[FG_PATH_MAX];
    if (!session || !fg_path_join(path, session, "events.jsonl"))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid session path");
    size_t len = 0;
    char *text = fg_read_file(path, 64u * 1024u * 1024u, &len, e);
    if (!text)
        return e ? e->code : FORGE_ERR_IO;
    char *p = text;
    uint64_t expected = 1;
    while (*p) {
        char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n) {
            yyjson_doc *d = yyjson_read(p, n, 0);
            yyjson_val *o = d ? yyjson_doc_get_root(d) : NULL;
            const char *type = fg_json_str(o, "type");
            yyjson_val *seq = yyjson_obj_get(o, "sequence");
            if (!type || !yyjson_is_uint(seq) || yyjson_get_uint(seq) != expected ||
                yyjson_get_uint(yyjson_obj_get(o, "schema_version")) != 1) {
                yyjson_doc_free(d);
                free(text);
                return fg_error(e, FORGE_ERR_PARSE, "Invalid event at sequence %llu",
                                (unsigned long long)expected);
            }
            char *json = yyjson_write(d, 0, NULL);
            if (!json) {
                yyjson_doc_free(d);
                free(text);
                return fg_error(e, FORGE_ERR_MEMORY, "Replay allocation failed");
            }
            forge_event event = {type, expected++, json};
            if (cb)
                cb(&event, user);
            free(json);
            yyjson_doc_free(d);
        }
        if (!end)
            break;
        p = end + 1;
    }
    free(text);
    return FORGE_OK;
}
