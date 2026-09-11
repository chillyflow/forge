#include "candidate_store.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define STORE_FILES 10000u
#define STORE_BYTES (UINT64_C(64) * 1024u * 1024u)
typedef struct {
    char *path;
    fg_buf contents;
    unsigned mode;
} stored_file;
struct fg_candidate_store {
    fg_input_snapshot *inputs;
    stored_file *files;
    size_t count, capacity;
    char root[FG_PATH_MAX];
};
static bool collect(const char *path, const void *bytes, size_t length, bool final, void *user,
                    forge_error *e) {
    fg_candidate_store *s = user;
    stored_file *file = s->count ? &s->files[s->count - 1] : NULL;
    if (!file || strcmp(file->path, path)) {
        if (s->count == s->capacity) {
            size_t cap = s->capacity ? s->capacity * 2 : 32;
            stored_file *next = realloc(s->files, cap * sizeof(*next));
            if (!next)
                goto memory;
            s->files = next;
            s->capacity = cap;
        }
        file = &s->files[s->count++];
        memset(file, 0, sizeof(*file));
        file->path = fg_strdup(path);
        if (!file->path)
            goto memory;
    }
    if (length && !fg_buf_add(&file->contents, bytes, length))
        goto memory;
    if (final) {
        char full[FG_PATH_MAX];
        struct stat st;
        if (!fg_safe_path(s->root, path, false, full, e) || stat(full, &st))
            return false;
        file->mode = (unsigned)st.st_mode & 0777u;
    }
    return true;
memory:
    fg_error(e, FORGE_ERR_MEMORY, "Cannot retain candidate contents");
    return false;
}
static int compare_file(const void *a, const void *b) {
    return strcmp(((const stored_file *)a)->path, ((const stored_file *)b)->path);
}
fg_candidate_store *fg_candidate_store_take(const char *root, forge_cancel_fn cancel, void *user,
                                            uint64_t deadline, forge_error *e) {
    fg_candidate_store *s = calloc(1, sizeof(*s));
    if (!s) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate candidate store");
        return NULL;
    }
    if (!fg_workspace(root, s->root, e))
        goto fail;
    s->inputs = fg_input_snapshot_take_visit(s->root, STORE_FILES, STORE_BYTES, cancel, user,
                                             deadline, collect, s, e);
    if (!s->inputs)
        goto fail;
    qsort(s->files, s->count, sizeof(*s->files), compare_file);
    return s;
fail:
    fg_candidate_store_destroy(s);
    return NULL;
}
void fg_candidate_store_destroy(fg_candidate_store *s) {
    if (!s)
        return;
    for (size_t i = 0; i < s->count; ++i) {
        free(s->files[i].path);
        fg_buf_clear(&s->files[i].contents);
    }
    free(s->files);
    fg_input_snapshot_destroy(s->inputs);
    free(s);
}
static const stored_file *lookup(const fg_candidate_store *s, const char *path) {
    stored_file key = {0};
    key.path = (char *)path;
    return bsearch(&key, s->files, s->count, sizeof(*s->files), compare_file);
}
static bool same(const stored_file *a, const stored_file *b) {
    return a && b && a->contents.len == b->contents.len && a->mode == b->mode &&
           (!a->contents.len || !memcmp(a->contents.data, b->contents.data, a->contents.len));
}
bool fg_candidate_store_equal(const fg_candidate_store *a, const fg_candidate_store *b) {
    if (!a || !b || a->count != b->count)
        return false;
    for (size_t i = 0; i < a->count; ++i)
        if (strcmp(a->files[i].path, b->files[i].path) || !same(&a->files[i], &b->files[i]))
            return false;
    return true;
}
size_t fg_candidate_store_cost(const fg_candidate_store *base, const fg_candidate_store *next) {
    size_t cost = 0;
    for (size_t i = 0; i < next->count; ++i) {
        const stored_file *old = lookup(base, next->files[i].path);
        if (!same(old, &next->files[i]))
            cost += 1 + next->files[i].contents.len + (old ? old->contents.len : 0);
    }
    for (size_t i = 0; i < base->count; ++i)
        if (!lookup(next, base->files[i].path))
            cost += 1 + base->files[i].contents.len;
    return cost;
}
static bool parents(const char *root, const char *relative, forge_error *e) {
    char path[FG_PATH_MAX], part[FG_PATH_MAX];
    if (!fg_relative_path(relative, part, e))
        return false;
    for (char *p = part; *p; ++p) {
        if (*p != '/')
            continue;
        *p = 0;
        bool ok = fg_safe_path(root, part, true, path, e) && fg_mkdir(path, e);
        *p = '/';
        if (!ok)
            return false;
    }
    return true;
}
bool fg_candidate_store_materialize_until(const fg_candidate_store *s, const char *root,
                                          forge_cancel_fn cancel, void *user, uint64_t deadline,
                                          forge_error *e) {
    if (!s || !fg_mkdir(root, e))
        return false;
    fg_input_snapshot *empty = fg_input_snapshot_take(root, 1, 1, NULL, NULL, 0, e);
    if (!empty)
        return false;
    fg_candidate_store *existing = fg_candidate_store_take(root, NULL, NULL, 0, e);
    fg_input_snapshot_destroy(empty);
    bool vacant = existing && !existing->count;
    fg_candidate_store_destroy(existing);
    if (!vacant) {
        fg_error(e, FORGE_ERR_CONFLICT, "Candidate destination must be empty");
        return false;
    }
    for (size_t i = 0; i < s->count; ++i) {
        if ((cancel && cancel(user)) || (deadline && fg_now_ms() >= deadline)) {
            fg_error(e, FORGE_ERR_CANCELLED, "Candidate copy cancelled");
            return false;
        }
        const stored_file *file = &s->files[i];
        char full[FG_PATH_MAX];
        if (!parents(root, file->path, e) || !fg_safe_path(root, file->path, true, full, e))
            return false;
        FILE *f = fopen(full, "wbx");
        if (!f) {
            fg_error(e, FORGE_ERR_IO, "Cannot create candidate file");
            return false;
        }
        bool ok = !file->contents.len ||
                  fwrite(file->contents.data, 1, file->contents.len, f) == file->contents.len;
        if (fclose(f))
            ok = false;
#ifdef _WIN32
        if (ok && !SetFileAttributesA(full, (file->mode & 0200u) ? FILE_ATTRIBUTE_NORMAL
                                                                 : FILE_ATTRIBUTE_READONLY))
            ok = false;
#else
        if (ok && chmod(full, file->mode))
            ok = false;
#endif
        if (!ok) {
            fg_error(e, FORGE_ERR_IO, "Cannot write candidate file");
            return false;
        }
    }
    return true;
}
bool fg_candidate_store_materialize(const fg_candidate_store *s, const char *root, forge_error *e) {
    return fg_candidate_store_materialize_until(s, root, NULL, NULL, 0, e);
}
static bool permitted(fg_tool_context *c, const char *path, forge_error *e) {
    char *quoted = fg_json_string(path);
    fg_buf args = {0};
    if (quoted)
        fg_buf_printf(&args, "{\"path\":%s,\"operation\":\"candidate_restore\"}", quoted);
    free(quoted);
    bool ok =
        args.data && (c->config.policy ? c->config.policy("candidate_restore", FORGE_CAP_WRITE,
                                                          args.data, c->config.userdata)
                                       : c->config.allow_write);
    fg_buf_clear(&args);
    if (!ok)
        fg_error(e, FORGE_ERR_POLICY, "Candidate application requires write permission");
    return ok;
}
static bool replace_file(const stored_file *before, const stored_file *after, fg_tool_context *c,
                         forge_error *e) {
    const char *path = after ? after->path : before->path;
    char full[FG_PATH_MAX], temp[FG_PATH_MAX], random[17], artifact[128];
    if (!parents(c->root, path, e) || !fg_safe_path(c->root, path, true, full, e))
        return false;
    if (!fg_random_hex(random, 8) ||
        snprintf(temp, sizeof(temp), "%s.forge-%s.tmp", full, random) >= (int)sizeof(temp)) {
        fg_error(e, FORGE_ERR_IO, "Cannot stage candidate replacement");
        return false;
    }
    size_t id = ++c->call_id;
    size_t reserve = (before ? before->contents.len : 0) + (after ? after->contents.len : 0) + 2048;
    if (c->session->edit_bytes_reserved > c->session->edit_bytes_limit ||
        reserve > c->session->edit_bytes_limit - c->session->edit_bytes_reserved) {
        fg_error(e, FORGE_ERR_LIMIT, "Candidate journal budget exhausted");
        return false;
    }
    c->session->edit_bytes_reserved += reserve;
    snprintf(artifact, sizeof(artifact), "candidate-%06zu.before", id);
    if (!fg_session_artifact_bytes(c->session, artifact, before ? before->contents.data : "",
                                   before ? before->contents.len : 0, e))
        return false;
    snprintf(artifact, sizeof(artifact), "candidate-%06zu.after", id);
    if (!fg_session_artifact_bytes(c->session, artifact, after ? after->contents.data : "",
                                   after ? after->contents.len : 0, e))
        return false;
    char *quoted = fg_json_string(path);
    fg_buf manifest = {0};
    if (quoted)
        fg_buf_printf(&manifest,
                      "{\"id\":%zu,\"path\":%s,\"before_exists\":%s,\"after_exists\":%s,"
                      "\"before\":\"candidate-%06zu.before\",\"after\":\"candidate-%06zu.after\"}",
                      id, quoted, before ? "true" : "false", after ? "true" : "false", id, id);
    free(quoted);
    if (!manifest.data ||
        !fg_session_emit(c->session, "candidate_edit_prepared", manifest.data, e)) {
        fg_buf_clear(&manifest);
        return false;
    }
    fg_buf_clear(&manifest);
    if (before) {
        size_t n = 0;
        char *current = fg_read_file(full, STORE_BYTES, &n, e);
        bool equal = current && n == before->contents.len &&
                     (!n || !memcmp(current, before->contents.data, n));
        free(current);
        if (!equal) {
            fg_error(e, FORGE_ERR_CONFLICT, "Candidate source changed before replacement");
            return false;
        }
    } else {
        struct stat st;
        if (!stat(full, &st)) {
            fg_error(e, FORGE_ERR_CONFLICT, "Candidate path appeared concurrently");
            return false;
        }
    }
    bool ok = true;
    if (after) {
        FILE *f = fopen(temp, "wbx");
        bool created = f != NULL;
        if (!f)
            ok = false;
        else {
            ok = !after->contents.len ||
                 fwrite(after->contents.data, 1, after->contents.len, f) == after->contents.len;
            if (fclose(f))
                ok = false;
        }
#ifdef _WIN32
        if (ok && !SetFileAttributesA(temp, (after->mode & 0200u) ? FILE_ATTRIBUTE_NORMAL
                                                                  : FILE_ATTRIBUTE_READONLY))
            ok = false;
#else
        if (ok && chmod(temp, after->mode))
            ok = false;
#endif
        if (ok && !fg_safe_path(c->root, path, true, full, e))
            ok = false;
#ifdef _WIN32
        if (ok)
            ok = MoveFileExA(temp, full,
                             before ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                    : MOVEFILE_WRITE_THROUGH) != 0;
#else
        if (ok)
            ok = rename(temp, full) == 0;
#endif
        if (!ok && created) {
#ifdef _WIN32
            SetFileAttributesA(temp, FILE_ATTRIBUTE_NORMAL);
#endif
            remove(temp);
        }
    } else
        ok = remove(full) == 0;
    char outcome[96];
    snprintf(outcome, sizeof(outcome), "{\"id\":%zu,\"applied\":%s}", id, ok ? "true" : "false");
    bool recorded = fg_session_emit(c->session, "candidate_edit_outcome", outcome, e);
    if (!ok)
        fg_error(e, FORGE_ERR_IO,
                 "Candidate replacement failed; journal retains recovery contents");
    return ok && recorded;
}
static bool apply(const fg_candidate_store *expected, const fg_candidate_store *desired,
                  fg_tool_context *c, bool reserve_restore, forge_error *e) {
    if (!expected || !desired || !c || !c->session)
        return false;
    fg_candidate_store *current =
        fg_candidate_store_take(c->root, c->config.cancelled, c->config.userdata, c->deadline, e);
    if (!current)
        return false;
    bool equal = fg_candidate_store_equal(current, expected);
    fg_candidate_store_destroy(current);
    if (!equal) {
        fg_error(e, FORGE_ERR_CONFLICT,
                 "Workspace changed outside candidate selection; restore refused");
        return false;
    }
    size_t reserve = 0;
    for (size_t i = 0; i < desired->count; ++i) {
        const stored_file *old = lookup(expected, desired->files[i].path);
        if (same(old, &desired->files[i]))
            continue;
        reserve += (old ? old->contents.len : 0) + desired->files[i].contents.len + 2048;
        char path[FG_PATH_MAX], full[FG_PATH_MAX];
        if (!fg_relative_path(desired->files[i].path, path, e))
            return false;
        for (char *p = path; *p; ++p) {
            if (*p != '/')
                continue;
            *p = 0;
            bool conflict = lookup(expected, path) != NULL;
            *p = '/';
            if (conflict) {
                fg_error(e, FORGE_ERR_UNSUPPORTED,
                         "Candidate file/directory type changes require manual repair");
                return false;
            }
        }
        struct stat st;
        if (fg_path_join(full, c->root, path) && !stat(full, &st) &&
            (st.st_mode & S_IFMT) == S_IFDIR) {
            fg_error(e, FORGE_ERR_UNSUPPORTED,
                     "Candidate directory/file type changes require manual repair");
            return false;
        }
    }
    for (size_t i = 0; i < expected->count; ++i)
        if (!lookup(desired, expected->files[i].path))
            reserve += expected->files[i].contents.len + 2048;
    if (c->session->edit_bytes_reserved > c->session->edit_bytes_limit ||
        reserve > (c->session->edit_bytes_limit - c->session->edit_bytes_reserved) /
                      (reserve_restore ? 2u : 1u)) {
        fg_error(e, FORGE_ERR_LIMIT, "Candidate journal budget exhausted before application");
        return false;
    }
    /* Authorize every path before any mutation. */
    for (size_t i = 0; i < desired->count; ++i)
        if (!same(lookup(expected, desired->files[i].path), &desired->files[i]) &&
            !permitted(c, desired->files[i].path, e))
            return false;
    for (size_t i = 0; i < expected->count; ++i)
        if (!lookup(desired, expected->files[i].path) && !permitted(c, expected->files[i].path, e))
            return false;
    for (size_t i = 0; i < desired->count; ++i) {
        const stored_file *old = lookup(expected, desired->files[i].path);
        if (!same(old, &desired->files[i]) && !replace_file(old, &desired->files[i], c, e))
            return false;
    }
    for (size_t i = 0; i < expected->count; ++i)
        if (!lookup(desired, expected->files[i].path) &&
            !replace_file(&expected->files[i], NULL, c, e))
            return false;
    current =
        fg_candidate_store_take(c->root, c->config.cancelled, c->config.userdata, c->deadline, e);
    if (!current)
        return false;
    equal = fg_candidate_store_equal(current, desired);
    fg_candidate_store_destroy(current);
    if (!equal)
        fg_error(e, FORGE_ERR_CONFLICT,
                 "Candidate application did not produce the intended workspace");
    return equal;
}

bool fg_candidate_store_apply(const fg_candidate_store *expected, const fg_candidate_store *desired,
                              fg_tool_context *c, forge_error *e) {
    return apply(expected, desired, c, false, e);
}
bool fg_candidate_store_apply_reserving_restore(const fg_candidate_store *expected,
                                                const fg_candidate_store *desired,
                                                fg_tool_context *c, forge_error *e) {
    return apply(expected, desired, c, true, e);
}
bool fg_candidate_store_recover(const fg_candidate_store *baseline,
                                const fg_candidate_store *attempted, fg_tool_context *c,
                                uint64_t cleanup_deadline, forge_error *e) {
    if (!baseline || !attempted || !c || !c->session) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Candidate recovery requires complete input evidence");
        return false;
    }
    forge_error recovery = {0};
    fg_tool_context cleanup = *c;
    cleanup.config.cancelled = NULL;
    cleanup.deadline = cleanup_deadline;
    fg_candidate_store *current =
        fg_candidate_store_take(c->root, NULL, NULL, cleanup_deadline, &recovery);
    if (!current) {
        fg_error(e, recovery.code ? recovery.code : FORGE_ERR_CONFLICT,
                 "Cannot verify workspace for candidate recovery; journal retained: %s",
                 recovery.message);
        return false;
    }
    bool unchanged = fg_candidate_store_equal(current, baseline);
    bool fully_applied = fg_candidate_store_equal(current, attempted);
    fg_candidate_store_destroy(current);
    if (unchanged)
        return true;
    if (!fully_applied) {
        fg_error(e, FORGE_ERR_CONFLICT,
                 "Partial or unexpected candidate application; guarded restore refused and journal "
                 "retained");
        return false;
    }
    bool restored = fg_candidate_store_apply(attempted, baseline, &cleanup, &recovery);
    c->call_id = cleanup.call_id;
    if (!restored)
        fg_error(e, recovery.code ? recovery.code : FORGE_ERR_CONFLICT,
                 "Candidate recovery failed; journal retained: %s", recovery.message);
    return restored;
}
