#include "internal.h"
#include "forge/state.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static const char *const empty_fields =
    "{\"facts\":[],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],\"remaining\":[]}";
static const char *const initial_fields =
    "{\"facts\":[\"Source is Go\"],\"hypotheses\":[\"Boundary condition\"],"
    "\"decisions\":[\"Preserve public API\"],\"relevant_files\":[\"calc.go\"],"
    "\"remaining\":[\"Run tests\"]}";

static yyjson_doc *snapshot(const forge_working_state *state) {
    forge_error e = {0};
    char *text = forge_working_state_json(state, &e);
    assert(text);
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    forge_free(text);
    assert(doc);
    return doc;
}

static yyjson_val *member(yyjson_doc *doc, const char *name) {
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), name);
    assert(value);
    return value;
}

static void expect_text(yyjson_val *object, const char *name, const char *expected) {
    yyjson_val *value = yyjson_obj_get(object, name);
    assert(yyjson_is_str(value));
    assert(!strcmp(yyjson_get_str(value), expected));
}

static void expect_same_member(yyjson_doc *left, yyjson_doc *right, const char *name) {
    char *a = yyjson_val_write(member(left, name), 0, NULL);
    char *b = yyjson_val_write(member(right, name), 0, NULL);
    assert(a && b && !strcmp(a, b));
    free(a);
    free(b);
}

static void expect_rejected_update(forge_working_state *state, const char *update,
                                   forge_status expected) {
    forge_error e = {0};
    char *before = forge_working_state_json(state, &e);
    assert(before);
    assert(forge_working_state_update_json(state, update, &e) == expected);
    assert(e.code == expected);
    char *after = forge_working_state_json(state, &e);
    assert(after && !strcmp(before, after));
    forge_free(before);
    forge_free(after);
}

static char *sized_fields(size_t items, size_t item_bytes) {
    fg_buf b = {0};
    assert(fg_buf_puts(&b, "{\"facts\":["));
    for (size_t i = 0; i < items; i++) {
        assert(fg_buf_puts(&b, i ? ",\"" : "\""));
        for (size_t j = 0; j < item_bytes; j++)
            assert(fg_buf_puts(&b, "x"));
        assert(fg_buf_puts(&b, "\""));
    }
    assert(fg_buf_puts(
        &b, "],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],\"remaining\":[]}"));
    return fg_buf_take(&b);
}

static char *quoted_notes(size_t bytes) {
    char *text = malloc(bytes + 3);
    assert(text);
    text[0] = '"';
    memset(text + 1, 'n', bytes);
    text[bytes + 1] = '"';
    text[bytes + 2] = 0;
    return text;
}

static void test_model_updates_are_transactional(void) {
    forge_error e = {0};
    char goal[] = "Fix the original task";
    forge_working_state *state = forge_working_state_create(goal, &e);
    assert(state);
    goal[0] = 'X';
    assert(forge_working_state_update_json(state, initial_fields, &e) == FORGE_OK);
    forge_state_observation observation = {1, "apply_patch", "calc.go", FORGE_OK, "Patched Add",
                                           1, true};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    assert(forge_working_state_set_validation(state, 1, FORGE_STATE_FAILED, "go test exited 1",
                                              &e) == FORGE_OK);
    yyjson_doc *evidence = snapshot(state);
    assert(forge_working_state_update_json(state, "\"I think every test passed\"", &e) == FORGE_OK);
    yyjson_doc *updated = snapshot(state);
    expect_text(yyjson_doc_get_root(updated), "goal", "Fix the original task");
    expect_text(member(updated, "validation"), "status", "failed");
    expect_same_member(evidence, updated, "observed_changes");
    expect_same_member(evidence, updated, "recent_outcomes");
    expect_same_member(evidence, updated, "facts");
    expect_same_member(evidence, updated, "decisions");
    yyjson_doc_free(evidence);
    yyjson_doc_free(updated);

    const char *bad[] = {
        "{}",
        "[]",
        "null",
        "42",
        "true",
        "\"notes\" trailing",
        "\"notes\" \"more\"",
        "{\"facts\":[\"new allocation\"],\"hypotheses\":[3],\"decisions\":[],"
        "\"relevant_files\":[],\"remaining\":[]}",
        "{\"facts\":[],\"facts\":[],\"decisions\":[],\"relevant_files\":[],\"remaining\":[]}",
        "{\"goal\":\"Replace task\",\"hypotheses\":[],\"decisions\":[],"
        "\"relevant_files\":[],\"remaining\":[]}",
        "{\"facts\":[],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],"
        "\"remaining\":[],\"validation\":{\"status\":\"passed\"}}",
        "{\"facts\":[],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],"
        "\"remaining\":[],\"observed_changes\":[]}",
        "{\"facts\":[\"secret\\u0000suffix\"],\"hypotheses\":[],\"decisions\":[],"
        "\"relevant_files\":[],\"remaining\":[]}",
        "{\"facts\\u0000\":[],\"hypotheses\":[],\"decisions\":[],"
        "\"relevant_files\":[],\"remaining\":[]}",
        "\"legacy\\u0000suffix\"",
        "\"\\ud800\"",
        NULL};
    for (size_t i = 0; bad[i]; i++)
        expect_rejected_update(state, bad[i], FORGE_ERR_PARSE);
    assert(forge_working_state_update_json(state, empty_fields, &e) == FORGE_OK);
    updated = snapshot(state);
    expect_text(yyjson_doc_get_root(updated), "model_notes", "I think every test passed");
    expect_text(member(updated, "validation"), "status", "failed");
    assert(yyjson_arr_size(member(updated, "facts")) == 0);
    yyjson_doc_free(updated);
    forge_working_state_destroy(state);
}

static void test_model_limits(void) {
    forge_error e = {0};
    forge_working_state *state = forge_working_state_create("Bounded memory", &e);
    assert(state);
    char *fields = sized_fields(32, 1);
    assert(forge_working_state_update_json(state, fields, &e) == FORGE_OK);
    free(fields);
    fields = sized_fields(33, 1);
    expect_rejected_update(state, fields, FORGE_ERR_LIMIT);
    free(fields);
    fields = sized_fields(1, 513);
    expect_rejected_update(state, fields, FORGE_ERR_LIMIT);
    free(fields);
    fields = sized_fields(16, 512);
    assert(forge_working_state_update_json(state, fields, &e) == FORGE_OK);
    free(fields);
    yyjson_doc *doc = snapshot(state);
    assert(yyjson_get_uint(member(doc, "model_memory_bytes")) == 8192);
    yyjson_doc_free(doc);
    expect_rejected_update(state, "\"n\"", FORGE_ERR_LIMIT);
    fields = sized_fields(17, 512);
    expect_rejected_update(state, fields, FORGE_ERR_LIMIT);
    free(fields);
    assert(forge_working_state_update_json(state, empty_fields, &e) == FORGE_OK);
    char *notes = quoted_notes(8192);
    assert(forge_working_state_update_json(state, notes, &e) == FORGE_OK);
    free(notes);
    fields = sized_fields(1, 1);
    expect_rejected_update(state, fields, FORGE_ERR_LIMIT);
    free(fields);
    notes = quoted_notes(8193);
    expect_rejected_update(state, notes, FORGE_ERR_LIMIT);
    free(notes);
    char *oversized_json = malloc(65538);
    assert(oversized_json);
    memset(oversized_json, ' ', 65537);
    oversized_json[65537] = 0;
    expect_rejected_update(state, oversized_json, FORGE_ERR_LIMIT);
    free(oversized_json);
    forge_working_state_destroy(state);
}

static void test_validation_and_generations(void) {
    forge_error e = {0};
    forge_working_state *state = forge_working_state_create("Verify observed changes", &e);
    assert(state);
    assert(forge_working_state_update_json(state, initial_fields, &e) == FORGE_OK);
    assert(forge_working_state_update_json(state, "\"Remember the failed approach\"", &e) ==
           FORGE_OK);
    assert(forge_working_state_set_validation(state, 4, FORGE_STATE_PASSED,
                                              "Independent verifier exited zero", &e) == FORGE_OK);
    forge_state_observation observation = {
        1, "read_file", "pkg/a.go", FORGE_OK, "Read current source", 4, false};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    yyjson_doc *doc = snapshot(state);
    expect_text(member(doc, "validation"), "status", "passed");
    assert(yyjson_get_bool(member(doc, "model_notes_stale")));
    assert(yyjson_get_bool(member(doc, "model_fields_stale")));
    yyjson_doc_free(doc);

    observation = (forge_state_observation){
        2, "run_command", NULL, FORGE_ERR_CONFLICT, "go test exited 1", 5, false};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    doc = snapshot(state);
    expect_text(member(doc, "validation"), "status", "unverified");
    assert(yyjson_get_uint(yyjson_obj_get(member(doc, "validation"), "generation")) == 5);
    expect_text(yyjson_arr_get(member(doc, "recent_outcomes"), 1), "result", "conflict");
    assert(yyjson_arr_size(member(doc, "facts")) == 1);
    yyjson_doc_free(doc);
    char *before = forge_working_state_json(state, &e);
    assert(forge_working_state_set_validation(state, 4, FORGE_STATE_PASSED, "Stale verifier", &e) ==
           FORGE_ERR_CONFLICT);
    observation.generation = 4;
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_CONFLICT);
    char *after = forge_working_state_json(state, &e);
    assert(before && after && !strcmp(before, after));
    free(before);
    free(after);
    assert(forge_working_state_update_json(state, "\"Current note\"", &e) == FORGE_OK);
    doc = snapshot(state);
    assert(!yyjson_get_bool(member(doc, "model_notes_stale")));
    assert(yyjson_get_bool(member(doc, "model_fields_stale")));
    yyjson_doc_free(doc);
    assert(forge_working_state_update_json(state, initial_fields, &e) == FORGE_OK);
    doc = snapshot(state);
    assert(!yyjson_get_bool(member(doc, "model_memory_stale")));
    yyjson_doc_free(doc);

    observation =
        (forge_state_observation){3, "apply_patch", "pkg\\a.go", FORGE_OK, "Patched", 6, true};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    assert(forge_working_state_set_validation(state, 6, FORGE_STATE_PASSED, "Passed", &e) ==
           FORGE_OK);
    observation.path = "pkg/a.go";
    observation.tool_call_id = 4;
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    doc = snapshot(state);
    expect_text(member(doc, "validation"), "status", "unverified");
    assert(yyjson_arr_size(member(doc, "observed_changes")) == 1);
    yyjson_val *change = yyjson_arr_get(member(doc, "observed_changes"), 0);
    expect_text(change, "path", "pkg/a.go");
    assert(yyjson_get_uint(yyjson_obj_get(change, "observations")) == 2);
    yyjson_doc_free(doc);
    const char *names[] = {"unverified", "passed", "failed", "denied", "not_applicable"};
    for (int i = FORGE_STATE_UNVERIFIED; i <= FORGE_STATE_NOT_APPLICABLE; i++) {
        assert(forge_working_state_set_validation(state, 6, (forge_state_validation_status)i,
                                                  "Host-only validation", &e) == FORGE_OK);
        doc = snapshot(state);
        expect_text(member(doc, "validation"), "status", names[i]);
        yyjson_doc_free(doc);
    }
    assert(forge_working_state_set_validation(state, 6, (forge_state_validation_status)99,
                                              "Invalid", &e) == FORGE_ERR_ARGUMENT);
    forge_working_state_destroy(state);
}

static void test_recent_evidence_and_unicode(void) {
    forge_error e = {0};
    const char *goal = "修复 café / 日本語 / 🛠";
    forge_working_state *state = forge_working_state_create(goal, &e);
    assert(state);
    const char *fields =
        "{\"facts\":[\"函数 café 🛠\"],\"hypotheses\":[],\"decisions\":[\"保留接口\"],"
        "\"relevant_files\":[\"src/日本語.go\"],\"remaining\":[\"验证\"]}";
    assert(forge_working_state_update_json(state, fields, &e) == FORGE_OK);
    assert(forge_working_state_update_json(state, "\"\\ud83d\\udee0\"", &e) == FORGE_OK);
    char detail[601];
    memset(detail, 'd', 600);
    detail[600] = 0;
    for (uint64_t i = 0; i < 37; i++) {
        forge_state_observation observation = {i, "run_command", NULL, FORGE_OK, detail, 1, false};
        assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    }
    yyjson_doc *doc = snapshot(state);
    expect_text(yyjson_doc_get_root(doc), "goal", goal);
    expect_text(yyjson_doc_get_root(doc), "model_notes", "🛠");
    yyjson_val *recent = member(doc, "recent_outcomes");
    assert(yyjson_arr_size(recent) == 32);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(recent, 0), "tool_call_id")) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(recent, 31), "tool_call_id")) == 36);
    yyjson_val *overflow = member(doc, "overflow");
    assert(yyjson_get_uint(yyjson_obj_get(overflow, "recent_outcomes_evicted")) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(overflow, "outcome_detail_bytes_omitted")) == 37u * 88u);
    expect_text(member(doc, "validation"), "status", "unverified");
    yyjson_doc_free(doc);

    char boundary[520];
    memset(boundary, 'a', 511);
    strcpy(boundary + 511, "🛠b");
    forge_state_observation observation = {37, "read_file", "src/日本語.go", FORGE_OK, boundary,
                                           1,  false};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    assert(forge_working_state_set_validation(state, 1, FORGE_STATE_FAILED, boundary, &e) ==
           FORGE_OK);
    doc = snapshot(state);
    yyjson_val *last = yyjson_arr_get(member(doc, "recent_outcomes"), 31);
    assert(yyjson_get_len(yyjson_obj_get(last, "detail")) == 511);
    assert(yyjson_get_uint(yyjson_obj_get(last, "detail_bytes_omitted")) == 5);
    assert(yyjson_get_uint(yyjson_obj_get(member(doc, "validation"), "detail_bytes_omitted")) == 5);
    expect_text(last, "path", "src/日本語.go");
    yyjson_doc_free(doc);
    char *a = forge_working_state_json(state, &e), *b = forge_working_state_json(state, &e);
    assert(a && b && !strcmp(a, b));
    free(a);
    free(b);
    forge_working_state_destroy(state);
}

static void test_changed_path_capacity_is_fail_closed(void) {
    forge_error e = {0};
    forge_working_state *state = forge_working_state_create("Keep every changed path", &e);
    assert(state);
    char path[64];
    for (size_t i = 0; i < 1024; i++) {
        snprintf(path, sizeof(path), "pkg/file_%04zu.go", i);
        forge_state_observation observation = {(uint64_t)i, "apply_patch",   path, FORGE_OK,
                                               "Changed",   (uint64_t)i + 1, true};
        assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    }
    forge_state_observation repeat = {1024, "apply_patch", path, FORGE_OK, "Another change",
                                      1024, true};
    assert(forge_working_state_observe(state, &repeat, &e) == FORGE_OK);
    yyjson_doc *doc = snapshot(state);
    assert(yyjson_arr_size(member(doc, "observed_changes")) == 1024);
    assert(yyjson_arr_size(member(doc, "recent_outcomes")) == 32);
    assert(yyjson_get_uint(yyjson_obj_get(member(doc, "overflow"), "recent_outcomes_evicted")) ==
           993);
    yyjson_doc_free(doc);
    char *view = forge_working_state_context_json(state, 20000, &e);
    assert(view && strlen(view) <= 20000);
    free(view);
    assert(forge_working_state_set_validation(state, 1024, FORGE_STATE_PASSED, "Verifier", &e) ==
           FORGE_OK);
    repeat.path = "pkg/one_more.go";
    repeat.generation = 1025;
    assert(forge_working_state_observe(state, &repeat, &e) == FORGE_ERR_LIMIT);
    doc = snapshot(state);
    assert(yyjson_arr_size(member(doc, "observed_changes")) == 1024);
    assert(yyjson_get_bool(member(doc, "evidence_incomplete")));
    assert(yyjson_get_uint(yyjson_obj_get(member(doc, "overflow"), "changed_paths_rejected")) == 1);
    expect_text(member(doc, "validation"), "status", "unverified");
    yyjson_doc_free(doc);
    assert(forge_working_state_set_validation(state, 1025, FORGE_STATE_PASSED,
                                              "Must not hide missing evidence",
                                              &e) == FORGE_ERR_CONFLICT);
    forge_working_state_destroy(state);
}

static void test_context_keeps_core_and_reports_omissions(void) {
    forge_error e = {0};
    forge_working_state *state = forge_working_state_create("A goal that must never disappear", &e);
    assert(state);
    assert(forge_working_state_update_json(state, initial_fields, &e) == FORGE_OK);
    assert(forge_working_state_update_json(state, "\"Keep this model-authored note\"", &e) ==
           FORGE_OK);
    char path[64], detail[513];
    for (size_t i = 0; i < sizeof(detail) - 1; i++)
        detail[i] = i % 3 == 0 ? '"' : (i % 3 == 1 ? '\n' : '\\');
    detail[sizeof(detail) - 1] = 0;
    for (uint64_t i = 0; i < 40; i++) {
        snprintf(path, sizeof(path), "pkg/file_%02llu.go", (unsigned long long)i);
        forge_state_observation observation = {i,      "apply_patch", path, FORGE_OK,
                                               detail, i + 1,         true};
        assert(forge_working_state_observe(state, &observation, &e) == FORGE_OK);
    }
    forge_state_observation newest = {
        40, "apply_patch", "pkg/file_00.go", FORGE_OK, "Changed this old path again", 41, true};
    assert(forge_working_state_observe(state, &newest, &e) == FORGE_OK);
    assert(forge_working_state_set_validation(state, 41, FORGE_STATE_PASSED,
                                              "Observed verifier success", &e) == FORGE_OK);
    yyjson_doc *full = snapshot(state);
    char *before = forge_working_state_json(state, &e);
    char *core_text = forge_working_state_context_core_json(state, &e);
    assert(core_text);
    yyjson_doc *core_view = yyjson_read(core_text, strlen(core_text), 0);
    assert(core_view);
    expect_same_member(full, core_view, "goal");
    expect_same_member(full, core_view, "model_notes");
    expect_same_member(full, core_view, "facts");
    expect_same_member(full, core_view, "decisions");
    expect_same_member(full, core_view, "validation");
    assert(yyjson_arr_size(member(core_view, "observed_changes")) == 0);
    assert(yyjson_arr_size(member(core_view, "recent_outcomes")) == 0);
    assert(yyjson_get_uint(
               yyjson_obj_get(member(core_view, "context_omitted"), "observed_changes")) == 40);
    assert(yyjson_get_uint(
               yyjson_obj_get(member(core_view, "context_omitted"), "recent_outcomes")) == 32);
    free(core_text);
    yyjson_doc_free(core_view);
    const size_t budgets[] = {1, 1024, 1536, 2048, 3072, 4096, 10240, 1024u * 1024u};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(*budgets); i++) {
        char *text = forge_working_state_context_json(state, budgets[i], &e);
        if (!text) {
            assert(e.code == FORGE_ERR_LIMIT);
            assert(budgets[i] < 2048);
            continue;
        }
        assert(strlen(text) <= budgets[i]);
        yyjson_doc *view = yyjson_read(text, strlen(text), 0);
        free(text);
        assert(view);
        const char *core[] = {"goal",
                              "model_notes",
                              "facts",
                              "hypotheses",
                              "decisions",
                              "relevant_files",
                              "remaining",
                              "validation",
                              "overflow",
                              "model_fields_stale",
                              "model_notes_stale",
                              NULL};
        for (size_t j = 0; core[j]; j++)
            expect_same_member(full, view, core[j]);
        expect_text(yyjson_doc_get_root(view), "full_state_artifact", "working_state.json");
        yyjson_val *omitted = member(view, "context_omitted");
        size_t changes_kept = yyjson_arr_size(member(view, "observed_changes"));
        size_t outcomes_kept = yyjson_arr_size(member(view, "recent_outcomes"));
        assert(changes_kept + yyjson_get_uint(yyjson_obj_get(omitted, "observed_changes")) == 40);
        assert(outcomes_kept + yyjson_get_uint(yyjson_obj_get(omitted, "recent_outcomes")) == 32);
        if (budgets[i] >= 2048) {
            assert(changes_kept && outcomes_kept);
            expect_text(yyjson_arr_get(member(view, "observed_changes"), 0), "path",
                        "pkg/file_00.go");
            assert(yyjson_get_uint(yyjson_obj_get(
                       yyjson_arr_get(member(view, "recent_outcomes"), 0), "tool_call_id")) == 40);
        }
        if (budgets[i] == 1024u * 1024u) {
            assert(changes_kept == 40);
            assert(outcomes_kept == 32);
        }
        yyjson_doc_free(view);
    }
    char *after = forge_working_state_json(state, &e);
    assert(before && after && !strcmp(before, after));
    free(before);
    free(after);
    yyjson_doc_free(full);
    forge_working_state_destroy(state);

    state = forge_working_state_create("Large model state cannot be silently cut", &e);
    assert(state);
    char *notes = quoted_notes(8192);
    assert(forge_working_state_update_json(state, notes, &e) == FORGE_OK);
    free(notes);
    assert(!forge_working_state_context_json(state, 4096, &e));
    assert(e.code == FORGE_ERR_LIMIT);
    core_text = forge_working_state_context_core_json(state, &e);
    assert(core_text);
    core_view = yyjson_read(core_text, strlen(core_text), 0);
    assert(core_view && yyjson_get_len(member(core_view, "model_notes")) == 8192);
    free(core_text);
    yyjson_doc_free(core_view);
    full = snapshot(state);
    assert(yyjson_get_len(member(full, "model_notes")) == 8192);
    yyjson_doc_free(full);
    forge_working_state_destroy(state);
}

static void test_invalid_host_inputs(void) {
    forge_error e = {0};
    assert(!forge_working_state_create(NULL, &e));
    assert(!forge_working_state_create("", &e));
    assert(!forge_working_state_create("\xc0\xaf", &e));
    forge_working_state *state = forge_working_state_create("Validate input", &e);
    assert(state);
    forge_state_observation observation = {0, "tool", "../bad", FORGE_OK, "", 0, true};
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    observation.path = "a//b";
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    observation.path = "C:\\bad";
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    observation.path = NULL;
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    observation.changed = false;
    observation.detail = "\xed\xa0\x80";
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    observation.detail = "";
    observation.result = (forge_status)99;
    assert(forge_working_state_observe(state, &observation, &e) == FORGE_ERR_ARGUMENT);
    char oversized_path[4097];
    memset(oversized_path, 'a', sizeof(oversized_path) - 1);
    oversized_path[sizeof(oversized_path) - 1] = 0;
    observation.result = FORGE_OK;
    observation.path = oversized_path;
    assert(forge_working_state_observe(state, &observation, NULL) == FORGE_ERR_LIMIT);
    assert(!forge_working_state_json(NULL, &e));
    assert(!forge_working_state_context_json(NULL, 1024, &e));
    assert(forge_working_state_update_json(NULL, empty_fields, &e) == FORGE_ERR_ARGUMENT);
    forge_working_state_destroy(state);
    forge_working_state_destroy(NULL);
}

int main(void) {
    test_model_updates_are_transactional();
    test_model_limits();
    test_validation_and_generations();
    test_recent_evidence_and_unicode();
    test_changed_path_capacity_is_fail_closed();
    test_context_keeps_core_and_reports_omissions();
    test_invalid_host_inputs();
    puts("working-state tests passed");
    return 0;
}
