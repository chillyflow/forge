#include "internal.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static size_t count_chars(const char *text, void *user) {
    size_t multiplier = user ? *(const size_t *)user : 1;
    return strlen(text) * multiplier;
}
static size_t count_overflow(const char *text, void *user) {
    (void)text;
    (void)user;
    return SIZE_MAX;
}
static forge_segment_view view(const forge_context *c, size_t index) {
    forge_segment_view v = {0};
    assert(forge_context_get(c, index, &v));
    return v;
}
static char *render(forge_context *c, size_t *tokens, size_t *evicted) {
    forge_error error = {0};
    char *text = forge_context_plan(c, tokens, evicted, &error);
    if (!text)
        fprintf(stderr, "context plan: %s\n", error.message);
    assert(text);
    return text;
}
static char *snapshot(const forge_context *c) {
    forge_error error = {0};
    char *json = forge_context_export(c, &error);
    if (!json)
        fprintf(stderr, "context export: %s\n", error.message);
    assert(json);
    return json;
}
static void test_shared_dependency_budget(void) {
    /* Each one-byte segment costs 17. The diamond's closure is 68, not 85:
     * its shared root must be charged once, even within a single bundle. */
    forge_context *c = forge_context_create(78, 10, count_chars, NULL);
    assert(c);
    uint64_t root = forge_context_add(c, FORGE_SEG_SOURCE, "r", 1, false, 0, 1);
    uint64_t left = forge_context_add(c, FORGE_SEG_SOURCE, "l", 1, false, root, 1);
    uint64_t right = forge_context_add(c, FORGE_SEG_SOURCE, "t", 1, false, root, 1);
    uint64_t merge = forge_context_add(c, FORGE_SEG_RESULT, "d", 10, true, right, 1);
    assert(root && left && right && merge);
    assert(forge_context_add_dependency(c, merge, left) == FORGE_OK);
    assert(forge_context_add_dependency(c, merge, right) == FORGE_OK);
    assert(forge_context_dependency_count(c, merge) == 2);
    uint64_t dependency = 0;
    assert(forge_context_get_dependency(c, merge, 0, &dependency) && dependency == left);
    assert(forge_context_get_dependency(c, merge, 1, &dependency) && dependency == right);
    assert(!forge_context_get_dependency(c, merge, 2, &dependency));
    assert(!forge_context_get_dependency(c, merge, 0, NULL));
    forge_segment_view v = view(c, 3);
    assert(v.dependency == left && v.dependency_count == 2);
    assert(forge_context_add(c, FORGE_SEG_SOURCE, "not selected", 100, false, 0, 1));
    size_t tokens, evicted;
    char *prompt = render(c, &tokens, &evicted);
    assert(tokens <= 68 && evicted == 1);
    for (size_t i = 0; i < 4; i++)
        assert(view(c, i).selected);
    assert(!view(c, 4).selected);
    free(prompt);
    /* Shared closure is also not charged again for another pinned root. */
    forge_context_pin(c, left, true);
    prompt = render(c, &tokens, &evicted);
    assert(view(c, 3).selected && evicted == 1);
    free(prompt);
    assert(forge_context_add_dependency(c, root, merge) == FORGE_ERR_CONFLICT);
    assert(forge_context_add_dependency(c, merge, merge) == FORGE_ERR_CONFLICT);
    assert(forge_context_add_dependency(c, merge, 99999) == FORGE_ERR_NOT_FOUND);
    assert(forge_context_add_dependency(c, 99999, root) == FORGE_ERR_NOT_FOUND);
    size_t before = forge_context_size(c);
    assert(!forge_context_add(c, FORGE_SEG_SOURCE, "missing parent", 1, false, 99999, 1));
    assert(!forge_context_add(c, (forge_segment_kind)99, "bad kind", 1, false, 0, 1));
    assert(forge_context_size(c) == before);
    forge_context_destroy(c);
}
static void test_immutable_and_identical_updates(void) {
    forge_context *c = forge_context_create(1000, 100, count_chars, NULL);
    assert(c);
    uint64_t source = forge_context_add(c, FORGE_SEG_SOURCE, "source", 10, false, 0, 1);
    uint64_t sealed = forge_context_add(c, FORGE_SEG_SYSTEM, "fixed", 100, true, 0, 4);
    forge_context_bind_source(c, sealed, 42);
    assert(forge_context_set_flags(c, sealed, true, true) == FORGE_OK);
    char *prompt = render(c, NULL, NULL);
    free(prompt);
    char *before = snapshot(c);
    assert(forge_context_update(c, sealed, "fixed", 999) == FORGE_OK);
    forge_context_bind_source(c, sealed, 123);
    assert(forge_context_set_flags(c, sealed, true, true) == FORGE_OK);
    char *after = snapshot(c);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    assert(forge_context_update(c, sealed, "changed", 4) == FORGE_ERR_POLICY);
    assert(forge_context_add_dependency(c, sealed, source) == FORGE_ERR_POLICY);
    assert(forge_context_set_flags(c, sealed, false, true) == FORGE_ERR_POLICY);
    assert(forge_context_set_flags(c, sealed, true, false) == FORGE_ERR_POLICY);
    forge_segment_view v = view(c, 1);
    assert(v.immutable && v.cacheable && v.source_hash == 42 && v.generation == 4 &&
           v.version == 1);
    uint64_t child = forge_context_add(c, FORGE_SEG_RESULT, "derived", 40, true, source, 1);
    assert(child);
    uint64_t hash = view(c, 0).content_hash;
    assert(forge_context_update(c, source, "source", 2) == FORGE_OK);
    v = view(c, 0);
    assert(v.version == 1 && v.generation == 2 && v.content_hash == hash);
    assert(!view(c, 2).stale);
    assert(forge_context_update(c, source, "new source", 3) == FORGE_OK);
    assert(view(c, 0).version == 2 && view(c, 0).content_hash != hash);
    assert(view(c, 2).stale && !view(c, 2).pinned);
    forge_context_destroy(c);
}
static void test_transitive_source_invalidation(void) {
    forge_context *c = forge_context_create(2000, 100, count_chars, NULL);
    assert(c);
    uint64_t source = forge_context_add(c, FORGE_SEG_SOURCE, "raw", 40, true, 0, 1);
    uint64_t left = forge_context_add(c, FORGE_SEG_SOURCE, "left", 40, true, source, 99);
    uint64_t right = forge_context_add(c, FORGE_SEG_SOURCE, "right", 40, true, source, 99);
    uint64_t merge = forge_context_add(c, FORGE_SEG_RESULT, "merged", 40, true, left, 99);
    assert(forge_context_add_dependency(c, merge, right) == FORGE_OK);
    uint64_t unrelated = forge_context_add(c, FORGE_SEG_SOURCE, "unrelated", 40, true, 0, 1);
    uint64_t all = forge_context_add(c, FORGE_SEG_SOURCE, "repository-wide", 40, true, 0, 1);
    forge_context_bind_source(c, source, 101);
    forge_context_bind_source(c, unrelated, 202);
    forge_context_bind_source(c, all, UINT64_MAX);
    forge_context_invalidate(c, 101, 1);
    for (size_t i = 0; i < 6; i++)
        assert(!view(c, i).stale);
    forge_context_invalidate(c, 101, 2);
    for (size_t i = 0; i < 4; i++)
        assert(view(c, i).stale && !view(c, i).pinned);
    assert(!view(c, 4).stale && view(c, 5).stale);
    forge_context_pin(c, merge, true);
    assert(!view(c, 3).pinned);
    char *prompt = render(c, NULL, NULL);
    assert(strstr(prompt, "unrelated") && !strstr(prompt, "merged"));
    free(prompt);
    assert(forge_context_update(c, merge, "merged", 2) == FORGE_ERR_CONFLICT);
    assert(forge_context_update(c, source, "raw", 2) == FORGE_OK);
    assert(!view(c, 0).stale && view(c, 0).version == 1);
    assert(view(c, 1).stale && view(c, 3).stale);
    assert(forge_context_update(c, left, "left", 2) == FORGE_OK);
    assert(forge_context_update(c, right, "right", 2) == FORGE_OK);
    assert(forge_context_update(c, merge, "merged", 2) == FORGE_OK);
    assert(!view(c, 3).stale && view(c, 3).version == 1);
    forge_context_invalidate(c, 0, 3);
    for (size_t i = 0; i < 6; i++)
        assert(view(c, i).stale);
    uint64_t newly_derived =
        forge_context_add(c, FORGE_SEG_RESULT, "still stale", 40, true, source, 3);
    assert(newly_derived && view(c, 6).stale && !view(c, 6).pinned);
    forge_context_destroy(c);
}
static forge_context *snapshot_fixture(uint64_t *memory_id) {
    forge_context *c = forge_context_create(360, 40, count_chars, NULL);
    assert(c);
    uint64_t system = forge_context_add(c, FORGE_SEG_SYSTEM, "You are Forge.", 100, true, 0, 0);
    uint64_t tools = forge_context_add(c, FORGE_SEG_TOOLS, "tools", 100, true, 0, 0);
    uint64_t task = forge_context_add(c, FORGE_SEG_TASK, "repair", 100, true, 0, 0);
    assert(forge_context_set_flags(c, system, true, true) == FORGE_OK);
    assert(forge_context_set_flags(c, tools, true, true) == FORGE_OK);
    assert(forge_context_set_flags(c, task, true, true) == FORGE_OK);
    *memory_id = forge_context_add(c, FORGE_SEG_MEMORY, "current plan", 90, true, 0, 0);
    uint64_t source = forge_context_add(c, FORGE_SEG_SOURCE, "root fact", 30, false, 0, 7);
    forge_context_bind_source(c, source, 111);
    assert(forge_context_set_flags(c, source, false, true) == FORGE_OK);
    uint64_t derived = forge_context_add(c, FORGE_SEG_SOURCE, "derived fact", 30, false, source, 7);
    uint64_t action = forge_context_add(c, FORGE_SEG_ACTION, "read_file", 20, false, 0, 7);
    uint64_t result = forge_context_add(c, FORGE_SEG_RESULT, "result: \"ok\"\nUTF-8: \xc3\xa9", 70,
                                        true, action, 7);
    assert(forge_context_add_dependency(c, result, derived) == FORGE_OK);
    char oversized[801];
    memset(oversized, 'x', 800);
    oversized[800] = 0;
    assert(forge_context_add(c, FORGE_SEG_SOURCE, oversized, -3, false, 0, 0));
    uint64_t stale = forge_context_add(c, FORGE_SEG_SOURCE, "old", 30, false, 0, 0);
    forge_context_bind_source(c, stale, 222);
    assert(forge_context_add(c, FORGE_SEG_SOURCE, "old summary", 30, false, stale, 0));
    forge_context_invalidate(c, 222, 1);
    return c;
}
static void test_snapshot_roundtrip_and_stable_prefix(void) {
    uint64_t memory;
    forge_context *c = snapshot_fixture(&memory);
    size_t tokens, evicted;
    char *prompt = render(c, &tokens, &evicted);
    assert(evicted == 1);
    const char *state = strstr(prompt, "[WORKING_STATE]");
    assert(state && strstr(prompt, "[SOURCE]") < strstr(prompt, "[ACTION]"));
    assert(strstr(prompt, "[ACTION]") < strstr(prompt, "[TOOL_RESULT]"));
    assert(strstr(prompt, "[TOOL_RESULT]") < state);
    size_t prefix = (size_t)(state - prompt);
    char *json = snapshot(c);
    forge_error error = {0};
    forge_context *copy = forge_context_import(json, count_chars, NULL, &error);
    if (!copy)
        fprintf(stderr, "context import: %s\n", error.message);
    assert(copy);
    char *again = snapshot(copy);
    assert(!strcmp(json, again));
    free(again);
    size_t copy_tokens, copy_evicted;
    char *copy_prompt = render(copy, &copy_tokens, &copy_evicted);
    assert(!strcmp(prompt, copy_prompt) && tokens == copy_tokens && evicted == copy_evicted);
    assert(forge_context_update(c, memory, "new working state", 8) == FORGE_OK);
    assert(forge_context_update(copy, memory, "new working state", 8) == FORGE_OK);
    char *changed = render(c, NULL, NULL), *copy_changed = render(copy, NULL, NULL);
    assert(!strcmp(changed, copy_changed));
    assert(!strncmp(prompt, changed, prefix));
    assert(strstr(changed, "[WORKING_STATE]") == changed + prefix);
    assert(strcmp(prompt + prefix, changed + prefix));
    for (size_t i = 0; i < forge_context_size(c); i++) {
        forge_segment_view a = view(c, i), b = view(copy, i);
        assert(a.id == b.id && a.version == b.version && a.content_hash == b.content_hash);
        assert(a.source_hash == b.source_hash && a.dependency_count == b.dependency_count);
        assert(a.stale == b.stale && a.selected == b.selected);
    }
    size_t other_counter = 2;
    assert(!forge_context_import(json, count_chars, &other_counter, &error));
    assert(error.code == FORGE_ERR_PARSE && strstr(error.message, "counter"));
    free(prompt);
    free(copy_prompt);
    free(changed);
    free(copy_changed);
    free(json);
    forge_context_destroy(copy);
    forge_context_destroy(c);
}
static void put_uint(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, uint64_t value) {
    assert(yyjson_mut_obj_put(o, yyjson_mut_str(d, key), yyjson_mut_uint(d, value)));
}
static void put_bool(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, bool value) {
    assert(yyjson_mut_obj_put(o, yyjson_mut_str(d, key), yyjson_mut_bool(d, value)));
}
static void test_reject_invalid_snapshots(void) {
    uint64_t memory;
    forge_context *c = snapshot_fixture(&memory);
    char *prompt = render(c, NULL, NULL);
    free(prompt);
    char *json = snapshot(c);
    for (unsigned corruption = 0; corruption < 19; corruption++) {
        yyjson_doc *read = yyjson_read(json, strlen(json), 0);
        assert(read);
        yyjson_mut_doc *d = yyjson_doc_mut_copy(read, NULL);
        yyjson_doc_free(read);
        assert(d);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
        yyjson_mut_val *items = yyjson_mut_obj_get(root, "segments");
        yyjson_mut_val *first = yyjson_mut_arr_get(items, 0);
        yyjson_mut_val *second = yyjson_mut_arr_get(items, 1);
        yyjson_mut_val *result = yyjson_mut_arr_get(items, 7);
        switch (corruption) {
        case 0:
            put_uint(d, root, "schema_version", 2);
            break;
        case 1:
            put_uint(d, root, "reserve", 360);
            break;
        case 2:
            put_uint(d, second, "id", 1);
            break;
        case 3:
            put_uint(d, first, "kind", 99);
            break;
        case 4:
            put_uint(d, first, "content_hash", 0);
            break;
        case 5:
            put_uint(d, first, "tokens", 1);
            break;
        case 6:
            put_uint(d, root, "next_id", 1);
            break;
        case 7:
            assert(yyjson_mut_obj_put(first, yyjson_mut_str(d, "immutable"),
                                      yyjson_mut_str(d, "true")));
            break;
        case 8:
            assert(yyjson_mut_arr_add_uint(d, yyjson_mut_obj_get(result, "dependencies"), 999));
            break;
        case 9:
            assert(yyjson_mut_arr_add_uint(d, yyjson_mut_obj_get(result, "dependencies"), 8));
            break;
        case 10:
            assert(yyjson_mut_arr_add_uint(d, yyjson_mut_obj_get(first, "dependencies"), 8));
            break;
        case 11:
            assert(yyjson_mut_arr_add_uint(d, yyjson_mut_obj_get(result, "dependencies"), 7));
            break;
        case 12:
            put_bool(d, yyjson_mut_arr_get(items, 10), "stale", false);
            break;
        case 13:
            put_bool(d, yyjson_mut_arr_get(items, 4), "selected", false);
            break;
        case 14:
            put_uint(d, root, "planned_tokens", 1);
            break;
        case 15:
            put_bool(d, yyjson_mut_arr_get(items, 9), "selected", true);
            break;
        case 16:
            put_bool(d, yyjson_mut_arr_get(items, 9), "pinned", true);
            break;
        case 17:
            assert(yyjson_mut_obj_add_uint(d, root, "capacity", 360));
            break;
        case 18:
            put_uint(d, first, "version", 0);
            break;
        }
        char *invalid = yyjson_mut_write(d, 0, NULL);
        yyjson_mut_doc_free(d);
        assert(invalid);
        forge_error error = {0};
        forge_context *rejected = forge_context_import(invalid, count_chars, NULL, &error);
        if (rejected)
            fprintf(stderr, "Accepted corrupted context snapshot case %u\n", corruption);
        assert(!rejected && error.code == FORGE_ERR_PARSE);
        free(invalid);
    }
    forge_error error = {0};
    assert(!forge_context_import("{", count_chars, NULL, &error));
    assert(error.code == FORGE_ERR_PARSE);
    assert(!forge_context_import(json, NULL, NULL, &error));
    assert(error.code == FORGE_ERR_ARGUMENT);
    assert(!forge_context_import(json, count_overflow, NULL, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    free(json);
    forge_context_destroy(c);
}
static void test_empty_unplanned_and_limits(void) {
    forge_context *c = forge_context_create(1000, 100, count_chars, NULL);
    assert(c);
    char *json = snapshot(c);
    forge_error error = {0};
    forge_context *copy = forge_context_import(json, count_chars, NULL, &error);
    assert(copy && forge_context_size(copy) == 0);
    free(json);
    char *prompt = render(copy, NULL, NULL);
    assert(!*prompt);
    free(prompt);
    forge_context_destroy(copy);
    uint64_t mutable = forge_context_add(c, FORGE_SEG_MEMORY, "not planned yet", 1, true, 0, 3);
    assert(mutable);
    json = snapshot(c);
    copy = forge_context_import(json, count_chars, NULL, &error);
    assert(copy && !view(copy, 0).selected && view(copy, 0).pinned);
    char *second = snapshot(copy);
    assert(!strcmp(json, second));
    free(json);
    free(second);
    forge_context_destroy(copy);
    forge_context_destroy(c);
    c = forge_context_create(100, 10, count_overflow, NULL);
    assert(c && !forge_context_add(c, FORGE_SEG_SOURCE, "overflow", 1, false, 0, 0));
    forge_context_destroy(c);
    c = forge_context_create(20, 1, count_chars, NULL);
    assert(c && forge_context_add(c, FORGE_SEG_SYSTEM, "cannot fit", 100, true, 0, 0));
    size_t tokens = 999, evicted = 999;
    assert(!forge_context_plan(c, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT && !tokens && !evicted && !view(c, 0).selected);
    json = snapshot(c);
    copy = forge_context_import(json, count_chars, NULL, &error);
    assert(copy); /* Failed planning leaves a consistent, unplanned snapshot. */
    free(json);
    forge_context_destroy(copy);
    forge_context_destroy(c);
    c = forge_context_create(10000, 100, count_chars, NULL);
    assert(c);
    uint64_t ids[258];
    for (size_t i = 0; i < 258; i++) {
        ids[i] = forge_context_add(c, FORGE_SEG_SOURCE, "x", 1, false, 0, 0);
        assert(ids[i]);
    }
    for (size_t i = 0; i < 256; i++)
        assert(forge_context_add_dependency(c, ids[257], ids[i]) == FORGE_OK);
    assert(forge_context_add_dependency(c, ids[257], ids[256]) == FORGE_ERR_LIMIT);
    assert(forge_context_dependency_count(c, ids[257]) == 256);
    forge_context_destroy(c);
}
static void test_deep_graph_and_id_version_limits(void) {
    forge_context *c = forge_context_create(100000, 100, count_chars, NULL);
    assert(c);
    uint64_t first = 0, previous = 0;
    for (size_t i = 0; i < 4096; i++) {
        previous = forge_context_add(c, FORGE_SEG_SOURCE, "x", 1, i == 4095, previous, 0);
        assert(previous);
        if (!i)
            first = previous;
    }
    assert(!forge_context_add(c, FORGE_SEG_SOURCE, "one too many", 1, false, 0, 0));
    forge_context_bind_source(c, first, 7);
    char *prompt = render(c, NULL, NULL);
    free(prompt);
    for (size_t i = 0; i < 4096; i++)
        assert(view(c, i).selected);
    forge_context_invalidate(c, 7, 1);
    assert(view(c, 4095).stale && !view(c, 4095).pinned);
    forge_context_destroy(c);

    c = forge_context_create(1000, 100, count_chars, NULL);
    assert(c && forge_context_add(c, FORGE_SEG_SOURCE, "version boundary", 1, false, 0, 0));
    char *json = snapshot(c);
    yyjson_doc *read = yyjson_read(json, strlen(json), 0);
    assert(read);
    yyjson_mut_doc *d = yyjson_doc_mut_copy(read, NULL);
    yyjson_doc_free(read);
    free(json);
    assert(d);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    yyjson_mut_val *item = yyjson_mut_arr_get(yyjson_mut_obj_get(root, "segments"), 0);
    put_uint(d, root, "next_id", UINT64_MAX);
    put_uint(d, item, "version", UINT64_MAX);
    json = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    assert(json);
    forge_error error = {0};
    forge_context *copy = forge_context_import(json, count_chars, NULL, &error);
    assert(copy);
    assert(!forge_context_add(copy, FORGE_SEG_SOURCE, "ID must not wrap", 1, false, 0, 0));
    assert(forge_context_update(copy, 1, "version must not wrap", 1) == FORGE_ERR_LIMIT);
    assert(forge_context_update(copy, 1, "version boundary", 1) == FORGE_OK);
    assert(view(copy, 0).version == UINT64_MAX && view(copy, 0).generation == 1);
    free(json);
    forge_context_destroy(copy);
    forge_context_destroy(c);
}
static void test_stable_cache_anchor(void) {
    forge_error error = {0};
    size_t end = SIZE_MAX;
    forge_context *c = forge_context_create(4096, 64, count_chars, NULL);
    assert(c);
    assert(forge_context_cache_anchor(c, "", &end, &error) == FORGE_ERR_CONFLICT && !end);
    uint64_t system = forge_context_add(c, FORGE_SEG_SYSTEM, "system caf\xc3\xa9", 10, true, 0, 0);
    uint64_t tools = forge_context_add(c, FORGE_SEG_TOOLS, "tools", 10, true, system, 0);
    uint64_t task = forge_context_add(c, FORGE_SEG_TASK, "mutable task", 10, true, 0, 0);
    assert(system && tools && task);
    char *prompt = render(c, NULL, NULL);
    assert(forge_context_cache_anchor(c, prompt, &end, &error) == FORGE_OK && !end);
    free(prompt);
    assert(forge_context_set_flags(c, system, true, true) == FORGE_OK);
    assert(forge_context_set_flags(c, tools, true, true) == FORGE_OK);
    prompt = render(c, NULL, NULL);
    assert(forge_context_cache_anchor(c, prompt, &end, &error) == FORGE_OK && end);
    const char *tail = strstr(prompt, "\n[TASK]");
    assert(tail && end == (size_t)(tail - prompt));
    size_t previous = end;
    char *stable = malloc(previous);
    assert(stable);
    memcpy(stable, prompt, previous);
    assert(forge_context_cache_anchor(c, "different", &end, &error) == FORGE_ERR_CONFLICT && !end);
    assert(forge_context_cache_anchor(c, "bad\xff", &end, &error) == FORGE_ERR_ARGUMENT && !end);
    assert(forge_context_update(c, task, "changed task", 1) == FORGE_OK);
    assert(forge_context_cache_anchor(c, prompt, &end, &error) == FORGE_ERR_CONFLICT);
    free(prompt);
    prompt = render(c, NULL, NULL);
    assert(forge_context_cache_anchor(c, prompt, &end, &error) == FORGE_OK && end == previous);
    assert(!memcmp(stable, prompt, end));
    free(stable);
    free(prompt);
    forge_context_destroy(c);

    c = forge_context_create(4096, 64, count_chars, NULL);
    assert(c);
    task = forge_context_add(c, FORGE_SEG_TASK, "non-prefix dependency", 10, true, 0, 0);
    system = forge_context_add(c, FORGE_SEG_SYSTEM, "system", 10, true, task, 0);
    assert(forge_context_set_flags(c, task, true, true) == FORGE_OK);
    assert(forge_context_set_flags(c, system, true, true) == FORGE_OK);
    prompt = render(c, NULL, NULL);
    assert(forge_context_cache_anchor(c, prompt, &end, &error) == FORGE_OK && !end);
    free(prompt);
    forge_context_destroy(c);
}
int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_shared_dependency_budget();
    test_immutable_and_identical_updates();
    test_transitive_source_invalidation();
    test_snapshot_roundtrip_and_stable_prefix();
    test_reject_invalid_snapshots();
    test_empty_unplanned_and_limits();
    test_deep_graph_and_id_version_limits();
    test_stable_cache_anchor();
    puts("Context DAG and snapshot tests passed");
    return 0;
}
