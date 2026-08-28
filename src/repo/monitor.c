#include "internal.h"
#include "forge/watch.h"
#include "core/input_snapshot.h"

struct fg_repo_monitor {
    forge_repo *repo;
    forge_watch *watch;
    char root[FG_PATH_MAX], fallback[512];
    forge_cancel_fn cancelled;
    void *user;
    uint64_t deadline;
    bool require_native;
    bool dirty;
    size_t after_scan_events;
    fg_input_snapshot *fallback_inputs;
};

static bool monitor_cancelled(void *user) {
    fg_repo_monitor *monitor = user;
    return (monitor->cancelled && monitor->cancelled(monitor->user)) ||
           fg_now_ms() >= monitor->deadline;
}
static forge_status check(fg_repo_monitor *monitor, forge_error *error) {
    if (monitor_cancelled(monitor))
        return fg_error(error, FORGE_ERR_CANCELLED,
                        "Repository monitoring cancelled or run deadline reached");
    return FORGE_OK;
}
static const char *empty_batch =
    "{\"schema_version\":1,\"backend\":\"scan\",\"events\":[],"
    "\"rescan_required\":false,\"initial_scan_required\":false,\"reopen_required\":false,"
    "\"timed_out\":false,\"more_pending\":false,\"reason_flags\":0,"
    "\"dropped_events\":0,\"dropped_events_unknown\":false,\"overflow_count\":0,"
    "\"directories\":0,\"path_encoding\":\"utf-8\"}";

static bool open_watch(fg_repo_monitor *monitor, forge_error *error) {
    if (check(monitor, error) != FORGE_OK)
        return false;
    uint64_t now = fg_now_ms();
    if (now >= monitor->deadline)
        return check(monitor, error) == FORGE_OK;
    forge_error failure = {0};
    monitor->watch = forge_watch_create(monitor->root, NULL, monitor_cancelled, monitor,
                                        FG_MIN(UINT64_C(30000), monitor->deadline - now), &failure);
    if (monitor->watch)
        return true;
    if (check(monitor, error) != FORGE_OK)
        return false;
    if (monitor->require_native) {
        if (error)
            *error = failure;
        return false;
    }
    snprintf(monitor->fallback, sizeof(monitor->fallback), "%s",
             failure.message[0] ? failure.message : "Native watcher is unavailable");
    return true;
}
static yyjson_doc *read_batch(fg_repo_monitor *monitor, uint64_t wait_ms, forge_error *error) {
    if (check(monitor, error) != FORGE_OK)
        return NULL;
    uint64_t now = fg_now_ms();
    if (now >= monitor->deadline) {
        check(monitor, error);
        return NULL;
    }
    wait_ms = FG_MIN(wait_ms, monitor->deadline - now);
    char *json = monitor->watch
                     ? forge_watch_poll(monitor->watch, wait_ms, monitor_cancelled, monitor, error)
                     : fg_strdup(empty_batch);
    if (!json)
        return NULL;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    free(json);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!doc || !yyjson_is_obj(root) || !yyjson_is_arr(yyjson_obj_get(root, "events"))) {
        yyjson_doc_free(doc);
        fg_error(error, FORGE_ERR_PARSE, "Invalid native watch batch");
        return NULL;
    }
    return doc;
}
static bool bool_field(yyjson_val *value, const char *name) {
    return yyjson_get_bool(yyjson_obj_get(value, name));
}
static bool write_change(fg_repo_monitor *monitor, yyjson_doc *doc, fg_repo_change *change,
                         forge_error *error) {
    yyjson_mut_doc *out = yyjson_doc_mut_copy(doc, NULL);
    if (!out) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot copy repository change batch");
        return false;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(out);
    bool ok = yyjson_mut_obj_add_str(out, root, "index_mode",
                                     change->full_scan    ? "full"
                                     : change->delta_scan ? "delta"
                                                          : "none") &&
              yyjson_mut_obj_add_uint(out, root, "generation", change->generation) &&
              yyjson_mut_obj_add_bool(out, root, "changed", change->changed) &&
              yyjson_mut_obj_add_bool(out, root, "watch_available", monitor->watch != NULL) &&
              yyjson_mut_obj_add_bool(out, root, "watch_reopened", change->reopened) &&
              yyjson_mut_obj_add_bool(out, root, "followup_scan_required", monitor->dirty) &&
              yyjson_mut_obj_add_uint(out, root, "after_scan_events", monitor->after_scan_events) &&
              yyjson_mut_obj_add_str(out, root, "watch_fallback", monitor->fallback);
    change->json = ok ? yyjson_mut_write(out, 0, NULL) : NULL;
    yyjson_mut_doc_free(out);
    if (!change->json)
        fg_error(error, FORGE_ERR_MEMORY, "Cannot encode repository change batch");
    return change->json != NULL;
}
/* Consume the new watcher's initial batch BEFORE taking the index baseline.
 * Discarding an initial batch after indexing would lose changes from that scan. */
static bool consume_initial(fg_repo_monitor *monitor, forge_error *error) {
    if (!monitor->watch)
        return true;
    yyjson_doc *doc = read_batch(monitor, 0, error);
    if (!doc)
        return false;
    bool unusable = bool_field(yyjson_doc_get_root(doc), "reopen_required");
    yyjson_doc_free(doc);
    if (unusable) {
        forge_watch_destroy(monitor->watch);
        monitor->watch = NULL;
        if (monitor->require_native) {
            fg_error(error, FORGE_ERR_LIMIT,
                     "Native watcher could not establish complete bounded coverage");
            return false;
        }
        snprintf(monitor->fallback, sizeof(monitor->fallback),
                 "Native watcher could not establish complete bounded coverage");
    }
    return true;
}
static fg_input_snapshot *take_inputs(fg_repo_monitor *monitor, forge_error *error) {
    return fg_input_snapshot_take(monitor->root, 100000, UINT64_C(2147483648), monitor_cancelled,
                                  monitor, monitor->deadline, error);
}
fg_repo_monitor *fg_repo_monitor_create(forge_repo *repo, const char *root,
                                        forge_cancel_fn cancelled, void *user, uint64_t deadline,
                                        bool require_native, fg_repo_change *initial,
                                        forge_error *error) {
    if (initial)
        memset(initial, 0, sizeof(*initial));
    if (!repo || !root || !initial || !deadline) {
        fg_error(error, FORGE_ERR_ARGUMENT, "Invalid repository monitor");
        return NULL;
    }
    uint64_t start = fg_now_ms();
    fg_repo_monitor *monitor = calloc(1, sizeof(*monitor));
    if (!monitor) {
        fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate repository monitor");
        return NULL;
    }
    monitor->repo = repo;
    monitor->cancelled = cancelled;
    monitor->user = user;
    monitor->deadline = deadline;
    monitor->require_native = require_native;
    if (!fg_workspace(root, monitor->root, error) || !open_watch(monitor, error) ||
        !consume_initial(monitor, error) || check(monitor, error) != FORGE_OK) {
        fg_repo_monitor_destroy(monitor);
        return NULL;
    }
    if (!monitor->watch && !(monitor->fallback_inputs = take_inputs(monitor, error))) {
        fg_repo_monitor_destroy(monitor);
        return NULL;
    }
    if (fg_repo_index_until(repo, NULL, 0, true, deadline, cancelled, user, error) != FORGE_OK ||
        check(monitor, error) != FORGE_OK) {
        fg_repo_monitor_destroy(monitor);
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(empty_batch, strlen(empty_batch), 0);
    if (!doc)
        fg_error(error, FORGE_ERR_MEMORY, "Cannot encode initial repository scan");
    initial->full_scan = true;
    initial->native = monitor->watch != NULL;
    initial->generation = forge_repo_generation(repo);
    initial->duration_ms = (double)(fg_now_ms() - start);
    bool ok = doc && write_change(monitor, doc, initial, error);
    yyjson_doc_free(doc);
    if (!ok) {
        fg_repo_monitor_destroy(monitor);
        return NULL;
    }
    return monitor;
}
static bool needs_full_path(fg_repo_monitor *monitor, yyjson_val *event) {
    const char *path = fg_json_str(event, "path");
    unsigned flags = (unsigned)yyjson_get_uint(yyjson_obj_get(event, "flags"));
    if (!path || !*path || !strcmp(path, ".") ||
        (flags & (FORGE_WATCH_DIRECTORY | FORGE_WATCH_SYMLINK | FORGE_WATCH_DELETED |
                  FORGE_WATCH_RENAMED_FROM | FORGE_WATCH_RENAMED_TO | FORGE_WATCH_RENAMED)))
        return true;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!strcmp(base, ".gitignore") || !strcmp(base, ".gitattributes") ||
        !strcmp(base, ".gitmodules"))
        return true;
    char full[FG_PATH_MAX];
    if (!fg_safe_path(monitor->root, path, false, full, NULL))
        return true;
    /* A write through one hard-link name also changes other indexed names,
     * which may not receive a native event of their own. Missing/renamed paths
     * above also require a full scan: a removed alias can no longer expose its
     * old link count, and a replacement may already have a different identity. */
    return !fg_regular_target(full, NULL);
}
forge_status fg_repo_monitor_poll(fg_repo_monitor *monitor, uint64_t wait_ms, bool force_full,
                                  fg_repo_change *change, forge_error *error) {
    if (!monitor || !change)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing repository monitor/change output");
    memset(change, 0, sizeof(*change));
    uint64_t start = fg_now_ms();
    yyjson_doc *doc = read_batch(monitor, wait_ms, error);
    if (!doc)
        return error && error->code ? error->code : FORGE_ERR_IO;
    yyjson_val *root = yyjson_doc_get_root(doc), *events = yyjson_obj_get(root, "events");
    change->events = yyjson_arr_size(events);
    bool rescan = bool_field(root, "rescan_required"), reopen = bool_field(root, "reopen_required");
    bool more = bool_field(root, "more_pending");
    bool signal = change->events || rescan || more || monitor->dirty;
    bool full =
        force_full || !monitor->watch || rescan || more || monitor->dirty || change->events > 4096;
    const char **paths = NULL;
    fg_input_snapshot *inputs = NULL;
    forge_status status = FORGE_OK;
    monitor->after_scan_events = 0;
    uint64_t before = forge_repo_generation(monitor->repo);
    if (reopen) {
        forge_watch_destroy(monitor->watch);
        monitor->watch = NULL;
        change->reopened = true;
        if (!open_watch(monitor, error) || !consume_initial(monitor, error)) {
            status = error && error->code ? error->code : FORGE_ERR_IO;
            goto finish;
        }
        full = true;
    }
    if (!monitor->watch) {
        inputs = take_inputs(monitor, error);
        if (!inputs) {
            status = error && error->code ? error->code : FORGE_ERR_IO;
            goto finish;
        }
        signal |= !fg_input_snapshot_equal(inputs, monitor->fallback_inputs);
    }
    if (!full && change->events) {
        paths = calloc(change->events, sizeof(*paths));
        if (!paths) {
            status = fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate changed path list");
            goto finish;
        }
        for (size_t i = 0; i < change->events; i++) {
            yyjson_val *event = yyjson_arr_get(events, i);
            if (needs_full_path(monitor, event)) {
                full = true;
                break;
            }
            paths[i] = fg_json_str(event, "path");
        }
    }
    if (check(monitor, error) != FORGE_OK) {
        status = FORGE_ERR_CANCELLED;
        goto finish;
    }
    if (!full && change->events) {
        status = fg_repo_index_until(monitor->repo, paths, change->events, false, monitor->deadline,
                                     monitor->cancelled, monitor->user, error);
        if (status != FORGE_OK) {
            /* Lost eligibility, replaced path/link, or a bounded delta failure
             * must not leave the runtime using its previous source index. */
            if (status == FORGE_ERR_MEMORY || status == FORGE_ERR_CANCELLED ||
                check(monitor, NULL) != FORGE_OK)
                goto finish;
            full = true;
        } else
            change->delta_scan = true;
    }
    if (full) {
        status = fg_repo_index_until(monitor->repo, NULL, 0, true, monitor->deadline,
                                     monitor->cancelled, monitor->user, error);
        if (status != FORGE_OK)
            goto finish;
        change->full_scan = true;
        monitor->dirty = false;
        if (monitor->watch) {
            yyjson_doc *after = read_batch(monitor, 0, error);
            if (!after) {
                status = error && error->code ? error->code : FORGE_ERR_IO;
                goto finish;
            }
            yyjson_val *observed = yyjson_doc_get_root(after);
            monitor->after_scan_events = yyjson_arr_size(yyjson_obj_get(observed, "events"));
            monitor->dirty = monitor->after_scan_events ||
                             bool_field(observed, "rescan_required") ||
                             bool_field(observed, "more_pending");
            change->events += monitor->after_scan_events;
            yyjson_doc_free(after);
        } else {
            fg_input_snapshot *after = take_inputs(monitor, error);
            if (!after) {
                status = error && error->code ? error->code : FORGE_ERR_IO;
                goto finish;
            }
            monitor->dirty = !fg_input_snapshot_equal(inputs, after);
            fg_input_snapshot_destroy(after);
        }
        signal |= monitor->dirty;
    }
    if (signal && forge_repo_generation(monitor->repo) == before) {
        status = fg_repo_note_change_until(monitor->repo, monitor->deadline, monitor->cancelled,
                                           monitor->user, error);
        if (status != FORGE_OK)
            goto finish;
    }
    if (check(monitor, error) != FORGE_OK) {
        status = FORGE_ERR_CANCELLED;
        goto finish;
    }
    change->generation = forge_repo_generation(monitor->repo);
    change->changed = signal || change->generation != before;
    change->native = monitor->watch != NULL;
    change->duration_ms = (double)(fg_now_ms() - start);
    if (inputs) {
        fg_input_snapshot_destroy(monitor->fallback_inputs);
        monitor->fallback_inputs = inputs;
        inputs = NULL;
    }
    if (!write_change(monitor, doc, change, error))
        status = FORGE_ERR_MEMORY;
    else if (error)
        memset(error, 0, sizeof(*error));
finish:
    fg_input_snapshot_destroy(inputs);
    free(paths);
    yyjson_doc_free(doc);
    return status;
}
void fg_repo_change_free(fg_repo_change *change) {
    if (change) {
        free(change->json);
        memset(change, 0, sizeof(*change));
    }
}
void fg_repo_monitor_destroy(fg_repo_monitor *monitor) {
    if (monitor) {
        forge_watch_destroy(monitor->watch);
        fg_input_snapshot_destroy(monitor->fallback_inputs);
        free(monitor);
    }
}
