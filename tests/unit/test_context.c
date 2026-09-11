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
static size_t count_complete_prompt(const char *text, void *user) {
    (void)text;
    (void)user;
    return 7;
}
static size_t count_template_overhead(const char *text, void *user) {
    (void)user;
    return strlen(text) + 200;
}
static size_t count_native_messages(const char *text, void *user) {
    (void)user;
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    assert(doc);
    size_t count = yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(doc), "messages"));
    yyjson_doc_free(doc);
    return count * 100;
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
static void test_snapshot_prompt_protocol_roundtrip(void) {
    char *schemas = fg_tool_native_schema();
    assert(schemas);
    forge_context *native = forge_context_create(1000000, 64, count_chars, NULL);
    assert(native && forge_context_set_prompt_protocol(native, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_add(native, FORGE_SEG_SYSTEM, "Native system", 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TOOLS, schemas, 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TASK, "Inspect", 100, true, 0, 0));
    free(schemas);

    char *prompt = render(native, NULL, NULL);
    char *json = snapshot(native);
    yyjson_doc *document = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    assert(root && yyjson_get_uint(yyjson_obj_get(root, "schema_version")) == 2);
    assert(yyjson_get_uint(yyjson_obj_get(root, "prompt_protocol")) == FORGE_PROMPT_NATIVE);
    yyjson_doc_free(document);

    forge_error error = {0};
    forge_context *copy = forge_context_import(json, count_chars, NULL, &error);
    if (!copy)
        fprintf(stderr, "native context import: %s\n", error.message);
    assert(copy);
    char *copy_prompt = render(copy, NULL, NULL);
    assert(!strcmp(prompt, copy_prompt));
    document = yyjson_read(copy_prompt, strlen(copy_prompt), 0);
    root = document ? yyjson_doc_get_root(document) : NULL;
    assert(root && !strcmp(fg_json_str(root, "protocol"), "forge-native-v1"));
    yyjson_doc_free(document);
    char *copy_json = snapshot(copy);
    assert(!strcmp(json, copy_json));
    free(copy_json);
    free(copy_prompt);
    free(json);
    free(prompt);
    forge_context_destroy(copy);
    forge_context_destroy(native);

    forge_context *flat = forge_context_create(4096, 64, count_chars, NULL);
    assert(flat);
    assert(forge_context_add(flat, FORGE_SEG_SYSTEM, "S", 100, true, 0, 0));
    assert(forge_context_add(flat, FORGE_SEG_TOOLS, "T", 100, true, 0, 0));
    assert(forge_context_add(flat, FORGE_SEG_TASK, "U", 100, true, 0, 0));
    char *flat_prompt = render(flat, NULL, NULL);
    char *flat_json = snapshot(flat);
    document = yyjson_read(flat_json, strlen(flat_json), 0);
    assert(document);
    root = yyjson_doc_get_root(document);
    assert(yyjson_get_uint(yyjson_obj_get(root, "schema_version")) == 1);
    assert(!yyjson_obj_get(root, "prompt_protocol"));
    yyjson_doc_free(document);
    char *legacy_json = fg_strdup(flat_json);
    assert(legacy_json);
    forge_context *legacy = forge_context_import(legacy_json, count_chars, NULL, &error);
    if (!legacy)
        fprintf(stderr, "legacy context import: %s\n", error.message);
    assert(legacy);
    char *legacy_prompt = render(legacy, NULL, NULL);
    assert(!strcmp(flat_prompt, legacy_prompt));
    free(legacy_prompt);
    free(legacy_json);
    free(flat_json);
    free(flat_prompt);
    forge_context_destroy(legacy);
    forge_context_destroy(flat);
}
static void test_reject_invalid_snapshots(void) {
    uint64_t memory;
    forge_context *c = snapshot_fixture(&memory);
    char *prompt = render(c, NULL, NULL);
    free(prompt);
    char *json = snapshot(c);
    for (unsigned corruption = 0; corruption < 20; corruption++) {
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
            put_uint(d, root, "schema_version", 3);
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
        case 19:
            put_uint(d, root, "prompt_protocol", FORGE_PROMPT_NATIVE + 1);
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

static void test_flattened_bytes_and_native_role_pairs(void) {
    forge_error error = {0};
    forge_context *flat = forge_context_create(4096, 64, count_chars, NULL);
    assert(flat);
    assert(forge_context_add(flat, FORGE_SEG_SYSTEM, "S", 100, true, 0, 0));
    assert(forge_context_add(flat, FORGE_SEG_TOOLS, "T", 100, true, 0, 0));
    assert(forge_context_add(flat, FORGE_SEG_TASK, "U", 100, true, 0, 0));
    assert(forge_context_set_prompt_counter(NULL, count_complete_prompt) == FORGE_ERR_ARGUMENT);
    assert(forge_context_set_prompt_counter(flat, NULL) == FORGE_ERR_ARGUMENT);
    assert(forge_context_set_prompt_counter(flat, count_complete_prompt) == FORGE_OK);
    size_t prompt_tokens = 0;
    char *prompt = render(flat, &prompt_tokens, NULL);
    assert(prompt_tokens == 7);
    assert(!strcmp(prompt, "\n[SYSTEM]\nS\n\n[TOOLS]\nT\n\n[TASK]\nU\n"));
    free(prompt);
    assert(forge_context_set_prompt_protocol(flat, FORGE_PROMPT_FLATTENED) == FORGE_OK);
    prompt = render(flat, NULL, NULL);
    assert(!strcmp(prompt, "\n[SYSTEM]\nS\n\n[TOOLS]\nT\n\n[TASK]\nU\n"));
    free(prompt);
    assert(forge_context_set_prompt_protocol(flat, (forge_prompt_protocol)99) ==
           FORGE_ERR_ARGUMENT);
    forge_context_destroy(flat);

    char *schemas = fg_tool_native_schema();
    assert(schemas);
    forge_context *native = forge_context_create(1000000, 64, count_chars, NULL);
    assert(native && forge_context_set_prompt_protocol(native, FORGE_PROMPT_NATIVE) == FORGE_OK);
    uint64_t system = forge_context_add(native, FORGE_SEG_SYSTEM, "Native system", 100, true, 0, 0);
    uint64_t tools = forge_context_add(native, FORGE_SEG_TOOLS, schemas, 100, true, 0, 0);
    uint64_t task = forge_context_add(native, FORGE_SEG_TASK, "Inspect", 100, true, 0, 0);
    free(schemas);
    assert(system && tools && task);
    assert(forge_context_set_flags(native, system, true, true) == FORGE_OK);
    assert(forge_context_set_flags(native, tools, true, true) == FORGE_OK);
    uint64_t action =
        forge_context_add(native, FORGE_SEG_ACTION,
                          "{\"thought\":\"inspect first\",\"tool\":\"read_file\",\"args\":{"
                          "\"path\":\"calc.go\",\"start\":1,\"end\":2}}",
                          80, false, 0, 1);
    uint64_t result = forge_context_add(native, FORGE_SEG_RESULT, "file text", 90, true, action, 1);
    uint64_t rejected_final =
        forge_context_add(native, FORGE_SEG_ACTION,
                          "{\"thought\":\"too soon\",\"final\":\"premature\"}", 80, false, 0, 1);
    uint64_t rejection = forge_context_add(
        native, FORGE_SEG_RESULT, "validation rejected the final", 95, true, rejected_final, 1);
    assert(action && result && rejected_final && rejection);
    prompt = render(native, NULL, NULL);
    yyjson_doc *document = yyjson_read(prompt, strlen(prompt), 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    assert(root && !strcmp(fg_json_str(root, "protocol"), "forge-native-v1"));
    assert(yyjson_is_arr(yyjson_obj_get(root, "tools")));
    assert(yyjson_get_uint(yyjson_obj_get(root, "anchor_message_count")) == 1);
    yyjson_val *messages = yyjson_obj_get(root, "messages");
    assert(yyjson_is_arr(messages) && yyjson_arr_size(messages) == 6);
    const char *roles[] = {"system", "user", "assistant", "tool", "assistant", "tool"};
    for (size_t i = 0; i < sizeof(roles) / sizeof(*roles); i++)
        assert(!strcmp(fg_json_str(yyjson_arr_get(messages, i), "role"), roles[i]));
    yyjson_val *first_call =
        yyjson_arr_get(yyjson_obj_get(yyjson_arr_get(messages, 2), "tool_calls"), 0);
    yyjson_val *first_function = yyjson_obj_get(first_call, "function");
    assert(!strcmp(fg_json_str(first_function, "name"), "read_file"));
    assert(!strcmp(fg_json_str(yyjson_arr_get(messages, 2), "reasoning_content"), "inspect first"));
    const char *first_id = fg_json_str(first_call, "id");
    assert(first_id && strlen(first_id) == 9 && first_id[0] == 'f' &&
           !strcmp(first_id, fg_json_str(yyjson_arr_get(messages, 3), "tool_call_id")));
    yyjson_val *final_call =
        yyjson_arr_get(yyjson_obj_get(yyjson_arr_get(messages, 4), "tool_calls"), 0);
    yyjson_val *final_function = yyjson_obj_get(final_call, "function");
    assert(!strcmp(fg_json_str(final_function, "name"), "final"));
    assert(
        !strcmp(fg_json_str(yyjson_obj_get(final_function, "arguments"), "answer"), "premature"));
    assert(!strcmp(fg_json_str(final_call, "id"),
                   fg_json_str(yyjson_arr_get(messages, 5), "tool_call_id")));
    size_t anchor = 0;
    assert(forge_context_cache_anchor(native, prompt, &anchor, &error) == FORGE_OK && anchor);
    assert(anchor < strlen(prompt));
    yyjson_doc_free(document);
    free(prompt);
    forge_context_destroy(native);

    schemas = fg_tool_native_schema();
    assert(schemas);
    native = forge_context_create(1000000, 64, count_chars, NULL);
    assert(native && forge_context_set_prompt_protocol(native, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_add(native, FORGE_SEG_SYSTEM, "Uncached", 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TOOLS, schemas, 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TASK, "Task", 100, true, 0, 0));
    free(schemas);
    prompt = render(native, NULL, NULL);
    document = yyjson_read(prompt, strlen(prompt), 0);
    root = document ? yyjson_doc_get_root(document) : NULL;
    assert(root && yyjson_get_uint(yyjson_obj_get(root, "anchor_message_count")) == 0);
    anchor = SIZE_MAX;
    assert(forge_context_cache_anchor(native, prompt, &anchor, &error) == FORGE_OK && !anchor);
    yyjson_doc_free(document);
    free(prompt);
    forge_context_destroy(native);
}

static void test_native_rejects_ambiguous_result_pairing(void) {
    char *schemas = fg_tool_native_schema();
    assert(schemas);
    forge_context *native = forge_context_create(1000000, 64, count_chars, NULL);
    assert(native && forge_context_set_prompt_protocol(native, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_add(native, FORGE_SEG_SYSTEM, "Native system", 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TOOLS, schemas, 100, true, 0, 0));
    assert(forge_context_add(native, FORGE_SEG_TASK, "Inspect", 100, true, 0, 0));
    free(schemas);

    uint64_t first = forge_context_add(
        native, FORGE_SEG_ACTION, "{\"thought\":\"first\",\"final\":\"first\"}", 80, false, 0, 1);
    uint64_t second = forge_context_add(
        native, FORGE_SEG_ACTION, "{\"thought\":\"second\",\"final\":\"second\"}", 80, false, 0, 1);
    uint64_t result = forge_context_add(native, FORGE_SEG_RESULT, "ambiguous", 90, true, first, 1);
    assert(first && second && result);
    assert(forge_context_add_dependency(native, result, second) == FORGE_OK);

    forge_error error = {0};
    char *prompt = forge_context_plan(native, NULL, NULL, &error);
    assert(!prompt);
    forge_context_destroy(native);
}

static void test_rendered_budget_compaction(void) {
    forge_context *c = forge_context_create(301, 1, count_chars, NULL);
    assert(c && forge_context_set_prompt_counter(c, count_template_overhead) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "goal", 100, true, 0, 0));
    uint64_t shared = forge_context_add(c, FORGE_SEG_SOURCE, "shared", 0, false, 0, 0);
    assert(shared);
    assert(forge_context_add(c, FORGE_SEG_SOURCE, "keep", 100, false, shared, 0));
    assert(forge_context_add(c, FORGE_SEG_SOURCE,
                             "drop: optional evidence with enough text to exhaust the rendered "
                             "budget while the estimates still allow the whole dependency graph",
                             0, false, shared, 0));
    size_t tokens = 0, evicted = 0;
    char *prompt = render(c, &tokens, &evicted);
    assert(tokens == strlen(prompt) + 200 && tokens <= 300 && evicted == 1);
    assert(view(c, 0).selected && view(c, 1).selected && view(c, 2).selected);
    assert(!view(c, 3).selected && !strstr(prompt, "drop:"));
    free(prompt);
    forge_context_pin(c, view(c, 3).id, true);
    forge_error error = {0};
    assert(!forge_context_plan(c, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT && !tokens && !evicted);
    for (size_t i = 0; i < forge_context_size(c); i++)
        assert(!view(c, i).selected);
    forge_context_destroy(c);

    c = forge_context_create(501, 1, count_chars, NULL);
    assert(c && forge_context_set_prompt_protocol(c, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_set_prompt_counter(c, count_native_messages) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "goal", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TOOLS, "[]", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TASK, "task", 100, true, 0, 0));
    uint64_t action = forge_context_add(c, FORGE_SEG_ACTION,
                                        "{\"tool\":\"read_file\",\"args\":{\"path\":\"old.c\"}}",
                                        80, false, 0, 1);
    assert(action && forge_context_add(c, FORGE_SEG_RESULT, "old", 80, false, action, 1));
    action = forge_context_add(c, FORGE_SEG_ACTION,
                               "{\"tool\":\"read_file\",\"args\":{\"path\":\"new.c\"}}", 80, false,
                               0, 2);
    assert(action && forge_context_add(c, FORGE_SEG_RESULT, "current", 100, true, action, 2));
    prompt = render(c, &tokens, &evicted);
    assert(tokens == 400 && evicted == 2);
    assert(!view(c, 3).selected && !view(c, 4).selected);
    assert(view(c, 5).selected && view(c, 6).selected);
    assert(strstr(prompt, "new.c") && !strstr(prompt, "old.c"));
    free(prompt);
    forge_context_destroy(c);
}

static void test_bounded_native_history_suffix(void) {
    forge_error error = {0};
    forge_context *c = forge_context_create(100000, 64, count_chars, NULL);
    assert(c && forge_context_set_prompt_protocol(c, FORGE_PROMPT_NATIVE) == FORGE_OK);
    uint64_t system = forge_context_add(c, FORGE_SEG_SYSTEM, "Task permissions remain enforced.",
                                        100, true, 0, 0);
    uint64_t tools = forge_context_add(c, FORGE_SEG_TOOLS, "[]", 100, true, 0, 0);
    assert(system && tools);
    assert(forge_context_set_flags(c, system, true, true) == FORGE_OK);
    assert(forge_context_set_flags(c, tools, true, true) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_TASK, "Repair the current task.", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_SOURCE, "Current source identity: candidate-3.",
                             100, true, 0, 3));
    assert(forge_context_add(c, FORGE_SEG_MEMORY,
                             "Observation: validation candidate-3 failed. Hypothesis: unknown.",
                             100, true, 0, 3));
    size_t base_tokens, anchor = 0;
    char *base = render(c, &base_tokens, NULL);
    assert(forge_context_cache_anchor(c, base, &anchor, &error) == FORGE_OK && anchor);
    uint64_t old = forge_context_add(c, FORGE_SEG_ACTION,
                                     "{\"tool\":\"read_file\",\"args\":{\"path\":\"old.c\"}}",
                                     100000, false, 0, 1);
    assert(old && forge_context_add(c, FORGE_SEG_RESULT, "Old short evidence.", 100000, false,
                                    old, 1));
    char large[4096];
    memset(large, 'x', sizeof(large) - 1);
    large[sizeof(large) - 1] = 0;
    uint64_t middle = forge_context_add(c, FORGE_SEG_ACTION,
                                        "{\"tool\":\"read_file\",\"args\":{\"path\":\"middle.c\"}}",
                                        0, false, 0, 2);
    assert(middle && forge_context_add(c, FORGE_SEG_RESULT, large, 0, false, middle, 2));
    uint64_t newest = forge_context_add(c, FORGE_SEG_ACTION,
                                        "{\"tool\":\"read_file\",\"args\":{\"path\":\"new.c\"}}",
                                        0, false, 0, 3);
    assert(newest && forge_context_add(c, FORGE_SEG_RESULT, "Current source evidence.", 0,
                                       false, newest, 3));
    size_t tokens = 0, evicted = 0;
    char *prompt = forge_context_plan_bounded(c, base_tokens + 700, &tokens, &evicted, &error);
    assert(prompt && tokens == strlen(prompt) && tokens <= base_tokens + 700 && evicted == 4);
    assert(!strstr(prompt, "old.c") && !strstr(prompt, "middle.c") && strstr(prompt, "new.c"));
    assert(strstr(prompt, "Task permissions") && strstr(prompt, "Repair the current task."));
    assert(strstr(prompt, "Current source identity: candidate-3"));
    assert(strstr(prompt, "validation candidate-3 failed"));
    for (size_t i = 0; i < 5; i++)
        assert(view(c, i).selected);
    for (size_t i = 5; i < 9; i++)
        assert(!view(c, i).selected);
    assert(view(c, 9).selected && view(c, 10).selected);
    size_t next_anchor = 0;
    assert(forge_context_cache_anchor(c, prompt, &next_anchor, &error) == FORGE_OK);
    assert(next_anchor == anchor && !memcmp(base, prompt, anchor));
    yyjson_doc *doc = yyjson_read(prompt, strlen(prompt), 0);
    assert(doc);
    yyjson_val *messages = yyjson_obj_get(yyjson_doc_get_root(doc), "messages");
    size_t calls = 0;
    for (size_t i = 0; i < yyjson_arr_size(messages); i++) {
        yyjson_val *message = yyjson_arr_get(messages, i);
        const char *role = fg_json_str(message, "role");
        if (!strcmp(role, "assistant")) {
            yyjson_val *call = yyjson_arr_get(yyjson_obj_get(message, "tool_calls"), 0);
            yyjson_val *reply = yyjson_arr_get(messages, ++i);
            assert(reply && !strcmp(fg_json_str(reply, "role"), "tool"));
            assert(!strcmp(fg_json_str(call, "id"), fg_json_str(reply, "tool_call_id")));
            calls++;
        } else
            assert(strcmp(role, "tool"));
    }
    assert(calls == 1);
    yyjson_doc_free(doc);
    char *raw = snapshot(c);
    assert(strstr(raw, "old.c") && strstr(raw, "middle.c") && strstr(raw, large));
    forge_context *copy = forge_context_import(raw, count_chars, NULL, &error);
    assert(copy && forge_context_size(copy) == 11);
    char *again = forge_context_plan_bounded(copy, base_tokens + 700, NULL, NULL, &error);
    assert(again && !strcmp(prompt, again));
    free(again);
    forge_context_destroy(copy);
    free(raw);

    /* Each turn supplies its own budget; the prior compact selection is not
     * destructive, and a later larger turn can retain the entire transcript. */
    char *expanded = forge_context_plan_bounded(c, SIZE_MAX, &tokens, &evicted, &error);
    assert(expanded && !evicted && strstr(expanded, "old.c") && strstr(expanded, "middle.c"));
    assert(forge_context_size(c) == 11 && !strcmp(view(c, 7).text,
                                                "{\"tool\":\"read_file\",\"args\":{\"path\":\"middle.c\"}}"));
    free(expanded);
    free(prompt);
    free(base);
    forge_context_destroy(c);
}

static void test_bounded_floor_monotonic_admission(void) {
    /* A loosened budget must not re-admit a dropped exchange: the admission
     * window slides forward only, keeping the rendered prefix stable. */
    forge_error error = {0};
    forge_context *c = forge_context_create(20000, 100, count_chars, NULL);
    assert(c);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "SYS", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TASK, "TASK", 100, true, 0, 0));
    char old_text[2016], mid_text[2016], new_text[128];
    memset(old_text, 'o', sizeof(old_text) - 1);
    old_text[sizeof(old_text) - 1] = 0;
    memset(mid_text, 'm', sizeof(mid_text) - 1);
    mid_text[sizeof(mid_text) - 1] = 0;
    memset(new_text, 'n', sizeof(new_text) - 1);
    new_text[sizeof(new_text) - 1] = 0;
    assert(forge_context_add(c, FORGE_SEG_SOURCE, old_text, 0, false, 0, 1));
    uint64_t mid = forge_context_add(c, FORGE_SEG_SOURCE, mid_text, 0, false, 0, 2);
    assert(mid);
    assert(forge_context_add(c, FORGE_SEG_SOURCE, new_text, 0, false, 0, 3));
    size_t tokens = 0, evicted = 0;
    char *full = forge_context_plan_bounded(c, SIZE_MAX, &tokens, &evicted, &error);
    assert(full && !evicted);
    assert(strstr(full, "ooo") && strstr(full, "mmm") && strstr(full, "nnn"));

    /* Shrink until the oldest exchange drops; newest-first admission drops it
     * alone first, keeping the middle and newest exchanges. */
    char *narrow = NULL;
    size_t budget = strlen(full);
    while (budget > 0) {
        budget = budget > 10 ? budget - 10 : 0;
        char *candidate = forge_context_plan_bounded(c, budget, NULL, NULL, &error);
        if (!candidate)
            break;
        if (!strstr(candidate, "ooo")) {
            narrow = candidate;
            break;
        }
        free(candidate);
    }
    assert(narrow && !strstr(narrow, "ooo"));
    assert(strstr(narrow, "mmm") && strstr(narrow, "nnn"));
    size_t narrow_tokens = strlen(narrow);

    /* The floor variant with min_id=0 plans exactly like the bounded planner
     * and reports the oldest admitted optional segment. */
    uint64_t oldest = 0;
    char *floored = forge_context_plan_bounded_floor(c, budget, 0, &oldest, NULL, NULL,
                                                     &error);
    assert(floored && !strcmp(floored, narrow) && oldest == mid);
    free(floored);

    /* With the floor set and an infinite budget, the dropped exchange stays
     * dropped: same bytes as the narrow plan. The unfloored planner
     * re-admits it, which is the oscillation this experiment removes. */
    uint64_t oldest2 = 0;
    char *stable = forge_context_plan_bounded_floor(c, SIZE_MAX, oldest, &oldest2, NULL,
                                                    NULL, &error);
    assert(stable && !strcmp(stable, narrow) && oldest2 == oldest);
    free(stable);
    char *expanded = forge_context_plan_bounded(c, SIZE_MAX, NULL, NULL, &error);
    assert(expanded && !strcmp(expanded, full));
    free(expanded);

    /* A budget fitting only mandatory evidence admits no optional history
     * and leaves the caller's floor untouched. */
    size_t tiny_budget = narrow_tokens;
    char *tiny = NULL;
    while (tiny_budget > 0) {
        tiny_budget = tiny_budget > 50 ? tiny_budget - 50 : 0;
        char *candidate = forge_context_plan_bounded_floor(c, tiny_budget, oldest, &oldest2,
                                                            NULL, NULL, &error);
        if (!candidate)
            break;
        if (!strstr(candidate, "nnn")) {
            tiny = candidate;
            break;
        }
        free(candidate);
    }
    assert(tiny && !strstr(tiny, "mmm") && !strstr(tiny, "ooo"));
    assert(strstr(tiny, "SYS") && oldest2 == oldest);
    free(tiny);

    /* Staleness at the floor does not disturb planning: the stale exchange
     * is skipped and newer evidence is still admitted. */
    forge_context_bind_source(c, mid, 7);
    forge_context_invalidate(c, 7, 99);
    char *after_stale = forge_context_plan_bounded_floor(c, SIZE_MAX, oldest, &oldest2, NULL,
                                                          NULL, &error);
    assert(after_stale && !strstr(after_stale, "mmm") && strstr(after_stale, "nnn"));
    assert(oldest2 >= oldest);
    free(after_stale);

    free(narrow);
    free(full);
    forge_context_destroy(c);
}

static void test_bounded_rendered_limits_and_snapshot(void) {
    forge_error error = {0};
    size_t tokens = 1, evicted = 1;
    assert(!forge_context_plan_bounded(NULL, 10, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_ARGUMENT && !tokens && !evicted);
    forge_context *c = forge_context_create(40, 5, count_chars, NULL);
    assert(c);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "S", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TASK, "T", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_SOURCE, "U", 0, false, 0, 0));
    /* Additive estimates total 51, but this complete rendering costs 34. */
    char *prompt = forge_context_plan_bounded(c, SIZE_MAX, &tokens, &evicted, &error);
    assert(prompt && tokens == strlen(prompt) && tokens <= 35 && !evicted);
    size_t full_tokens = tokens;
    char *raw = snapshot(c);
    forge_context *copy = forge_context_import(raw, count_chars, NULL, &error);
    assert(copy && forge_context_size(copy) == 3 && view(copy, 2).selected);
    forge_context_destroy(copy);
    free(raw);
    free(prompt);
    prompt = forge_context_plan_bounded(c, full_tokens - 1, &tokens, &evicted, &error);
    assert(prompt && tokens < full_tokens && evicted == 1 && !view(c, 2).selected);
    free(prompt);
    assert(!forge_context_plan_bounded(c, 0, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT && !tokens && !evicted);
    for (size_t i = 0; i < forge_context_size(c); i++)
        assert(!view(c, i).selected);

    /* A raised caller budget cannot consume the create-time output reserve. */
    assert(forge_context_update(c, view(c, 0).id, "Mandatory evidence cannot fit in 35 tokens.",
                                 1) == FORGE_OK);
    assert(!forge_context_plan_bounded(c, SIZE_MAX, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT && !tokens && !evicted);
    forge_context_destroy(c);

    c = forge_context_create(1000, 10, count_chars, NULL);
    assert(c && forge_context_set_prompt_counter(c, count_template_overhead) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "Mandatory task.", 100, true, 0, 0));
    assert(!forge_context_plan_bounded(c, 200, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT);
    forge_context_destroy(c);

    c = forge_context_create(501, 1, count_chars, NULL);
    assert(c && forge_context_set_prompt_protocol(c, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_set_prompt_counter(c, count_native_messages) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "Mandatory task.", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TOOLS, "[]", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TASK, "Repair.", 100, true, 0, 0));
    uint64_t action = forge_context_add(c, FORGE_SEG_ACTION,
                                        "{\"tool\":\"read_file\",\"args\":{\"path\":\"file.c\"}}",
                                        0, false, 0, 0);
    char evidence[1024];
    memset(evidence, 'x', sizeof(evidence) - 1);
    evidence[sizeof(evidence) - 1] = 0;
    assert(action && forge_context_add(c, FORGE_SEG_RESULT, evidence, 100, true, action, 0));
    prompt = forge_context_plan_bounded(c, 400, &tokens, &evicted, &error);
    assert(prompt && tokens == 400 && !evicted);
    free(prompt);
    assert(!forge_context_plan_bounded(c, 399, &tokens, &evicted, &error));
    assert(error.code == FORGE_ERR_LIMIT && !tokens && !evicted);
    forge_context_destroy(c);
}

static void test_bounded_current_evidence_invalidation(void) {
    forge_error error = {0};
    forge_context *c = forge_context_create(10000, 64, count_chars, NULL);
    assert(c && forge_context_set_prompt_protocol(c, FORGE_PROMPT_NATIVE) == FORGE_OK);
    assert(forge_context_add(c, FORGE_SEG_SYSTEM, "Permissions.", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TOOLS, "[]", 100, true, 0, 0));
    assert(forge_context_add(c, FORGE_SEG_TASK, "Task.", 100, true, 0, 0));
    uint64_t source = forge_context_add(c, FORGE_SEG_SOURCE, "Old source.", 100, true, 0, 1);
    uint64_t validation = forge_context_add(c, FORGE_SEG_MEMORY, "Old passing validation.",
                                            100, true, source, 1);
    assert(source && validation);
    assert(forge_context_update(c, source, "Current source.", 2) == FORGE_OK);
    assert(view(c, 4).stale && !view(c, 4).pinned);
    assert(forge_context_add(c, FORGE_SEG_MEMORY, "Current failure, input identity 2.",
                             100, true, source, 2));
    uint64_t action = forge_context_add(c, FORGE_SEG_ACTION,
                                        "{\"tool\":\"read_file\",\"args\":{\"path\":\"current.c\"}}",
                                        0, false, 0, 2);
    uint64_t result = forge_context_add(c, FORGE_SEG_RESULT, "Current read.", 0, true, action, 2);
    assert(action && result);
    char *prompt = forge_context_plan_bounded(c, 2048, NULL, NULL, &error);
    assert(prompt && strstr(prompt, "Current source.") && strstr(prompt, "Current failure"));
    assert(strstr(prompt, "current.c") && !strstr(prompt, "Old passing validation"));
    assert(!view(c, 4).selected && forge_context_size(c) == 8);
    char *raw = snapshot(c);
    assert(strstr(raw, "Old passing validation"));
    free(raw);
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
    test_snapshot_prompt_protocol_roundtrip();
    test_reject_invalid_snapshots();
    test_empty_unplanned_and_limits();
    test_deep_graph_and_id_version_limits();
    test_stable_cache_anchor();
    test_flattened_bytes_and_native_role_pairs();
    test_native_rejects_ambiguous_result_pairing();
    test_rendered_budget_compaction();
    test_bounded_native_history_suffix();
    test_bounded_floor_monotonic_admission();
    test_bounded_rendered_limits_and_snapshot();
    test_bounded_current_evidence_invalidation();
    puts("Context DAG and snapshot tests passed");
    return 0;
}
