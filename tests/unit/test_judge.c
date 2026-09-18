/* Judge unit tests: permutation, payload/response handling through the
 * deterministic transport stub, retry, fail-open error paths, the retrieval
 * glue, raw recording, and the [judge] configuration table. No network access
 * runs in the ordinary CTest; `--live` invokes one real request when
 * TYPESAFE_API_KEY is set (exit 77 otherwise, the repo's skip convention). */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "internal.h"
#include "forge/config.h"
#include "forge/judge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define TEST_PATH 4096

static void test_permutation(void) {
    const double scores[] = {0.1, 0.9, 0.5};
    size_t order[3];
    fg_rerank_permutation(scores, 3, order);
    assert(order[0] == 1 && order[1] == 2 && order[2] == 0);
    /* Ties keep the earlier index; invalid values sort last. */
    const double ties[] = {0.5, 0.5, 0.9};
    fg_rerank_permutation(ties, 3, order);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 1);
    double nan_value = strtod("nan", NULL);
    const double invalid[] = {0.5, nan_value, 0.7, 1.5, -0.2};
    fg_rerank_permutation(invalid, 5, order);
    assert(order[0] == 2 && order[1] == 0 && order[2] == 1 && order[3] == 3 && order[4] == 4);
    fg_rerank_permutation(scores, 0, order);
    fg_rerank_permutation(scores, 1, order);
    assert(order[0] == 0);
}

typedef struct {
    const char *response;
    size_t calls;
    char request[65536];
    size_t request_len;
    bool fail_first;
} stub_state;

static forge_status stub_transport(const char *request, size_t request_len, char **response,
                                   size_t *response_len, void *userdata, forge_error *e) {
    stub_state *s = userdata;
    if (s->calls == 0 && request_len < sizeof(s->request)) {
        memcpy(s->request, request, request_len);
        s->request[request_len] = 0;
        s->request_len = request_len;
    }
    s->calls++;
    if (s->fail_first && s->calls == 1)
        return fg_error(e, FORGE_ERR_IO, "stub transient failure");
    if (!s->response)
        return fg_error(e, FORGE_ERR_IO, "stub has no response");
    size_t length = strlen(s->response);
    char *copy = malloc(length + 1);
    if (!copy)
        return fg_error(e, FORGE_ERR_MEMORY, "stub allocation failed");
    memcpy(copy, s->response, length + 1);
    *response = copy;
    *response_len = length;
    return FORGE_OK;
}

static void make_response(char *out, size_t capacity, const char *model, const double *scores,
                          size_t count) {
    size_t used = (size_t)snprintf(out, capacity, "{\"model\":\"%s\",\"answers\":{", model);
    for (size_t i = 0; i < count; i++) {
        used += (size_t)snprintf(out + used, capacity - used,
                                 "%s\"c%zu\":{\"type\":\"noul\",\"noul\":%.4f}", i ? "," : "", i,
                                 scores[i]);
    }
    snprintf(out + used, capacity - used,
             "},\"usage\":{\"input_tokens\":123,\"output_tokens\":45}}");
}

static forge_judge *make_judge(stub_state *stub, const char *record_dir, size_t cap) {
    forge_judge_options options = {0};
    options.transport = stub_transport;
    options.transport_userdata = stub;
    options.timeout_ms = 500;
    options.max_candidates = cap;
    options.record_dir = record_dir;
    forge_error error = {0};
    forge_judge *judge = forge_judge_create(&options, &error);
    assert(judge && error.code == FORGE_OK);
    return judge;
}

static void test_payload_and_scores(void) {
    stub_state stub = {0};
    char response[512];
    const double expected[] = {0.9, 0.1, 0.5};
    make_response(response, sizeof(response), "jev-test-1", expected, 3);
    stub.response = response;
    forge_judge *judge = make_judge(&stub, NULL, 0);
    const char *paths[] = {"src/a.c", "src/b.c", "docs/c.md"};
    const char *snippets[] = {"alpha body", "beta body", "gamma body"};
    const char *stages[] = {"literal", "fts5", "package_graph"};
    double scores[3] = {0};
    forge_error error = {0};
    assert(forge_judge_rerank(judge, "find alpha", 3, paths, snippets, stages, scores, &error) ==
           FORGE_OK);
    assert(scores[0] == 0.9 && scores[1] == 0.1 && scores[2] == 0.5);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 0 && stats.candidates_scored == 3);
    assert(stats.input_tokens == 123 && stats.output_tokens == 45);
    assert(!strcmp(stats.last_model, "jev-test-1") && stats.last_latency_ms >= 0);
    /* The captured request is one batched call with one Noul per candidate. */
    yyjson_doc *doc = yyjson_read(stub.request, stub.request_len, 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(root, "model")), "jev-latest"));
    yyjson_val *state = yyjson_obj_get(root, "state");
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(state, "query")), "find alpha"));
    yyjson_val *candidates = yyjson_obj_get(state, "candidates");
    assert(yyjson_arr_size(candidates) == 3);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(candidates, 1), "path")),
                  "src/b.c"));
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(candidates, 2), "snippet")),
                  "gamma body"));
    yyjson_val *questions = yyjson_obj_get(root, "questions");
    yyjson_val *q1 = yyjson_obj_get(questions, "c1");
    assert(q1 && !strcmp(yyjson_get_str(yyjson_obj_get(q1, "type")), "noul"));
    assert(strstr(yyjson_get_str(yyjson_obj_get(q1, "instructions")), "candidates[1]"));
    yyjson_val *criteria = yyjson_obj_get(q1, "criteria");
    assert(yyjson_obj_get(criteria, "true") && yyjson_obj_get(criteria, "false"));
    yyjson_doc_free(doc);
    forge_judge_destroy(judge);
}

static void test_retry_once(void) {
    stub_state stub = {0};
    char response[256];
    const double expected[] = {0.7};
    make_response(response, sizeof(response), "jev-retry", expected, 1);
    stub.response = response;
    stub.fail_first = true; /* One transient IO failure is retried exactly once. */
    forge_judge *judge = make_judge(&stub, NULL, 0);
    const char *paths[] = {"p"};
    const char *snippets[] = {"s"};
    const char *stages[] = {"literal"};
    double scores[1] = {0};
    forge_error error = {0};
    assert(forge_judge_rerank(judge, "q", 1, paths, snippets, stages, scores, &error) == FORGE_OK);
    assert(stub.calls == 2 && scores[0] == 0.7);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 0);
    forge_judge_destroy(judge);
}

static void test_fail_open_errors(void) {
    const char *paths[] = {"p"};
    const char *snippets[] = {"s"};
    const char *stages[] = {"literal"};
    double scores[1] = {0};
    /* Malformed JSON, missing answer, and out-of-range answers are parse
     * failures; callers keep their deterministic order. */
    const char *bodies[] = {"{oops", "{\"model\":\"m\",\"answers\":{}}",
                            "{\"model\":\"m\",\"answers\":{\"c0\":{\"type\":\"noul\",\"noul\":1.5}}}"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(*bodies); i++) {
        stub_state stub = {0};
        stub.response = bodies[i];
        forge_judge *judge = make_judge(&stub, NULL, 0);
        forge_error error = {0};
        assert(forge_judge_rerank(judge, "q", 1, paths, snippets, stages, scores, &error) ==
               FORGE_ERR_PARSE);
        forge_judge_stats stats = {0};
        forge_judge_metrics(judge, &stats);
        assert(stats.calls == 1 && stats.failures == 1);
        forge_judge_destroy(judge);
    }
    /* A transport error that persists fails the call and is counted once. */
    stub_state failing = {0};
    forge_judge *judge = make_judge(&failing, NULL, 0);
    forge_error error = {0};
    assert(forge_judge_rerank(judge, "q", 1, paths, snippets, stages, scores, &error) ==
           FORGE_ERR_IO);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 1);
    forge_judge_destroy(judge);
    /* Above the configured cap the call fails before any transport use. */
    stub_state capped = {0};
    capped.response = "{\"answers\":{\"c0\":{\"noul\":0.5},\"c1\":{\"noul\":0.5}}}";
    judge = make_judge(&capped, NULL, 2);
    double two[2] = {0};
    const char *paths2[] = {"p", "p2"};
    const char *snippets2[] = {"s", "s2"};
    const char *stages2[] = {"literal", "literal"};
    assert(forge_judge_rerank(judge, "q", 2, paths2, snippets2, stages2, two, &error) == FORGE_OK);
    assert(capped.calls == 1);
    assert(forge_judge_rerank(judge, "q", 3, paths2, snippets2, stages2, two, &error) ==
           FORGE_ERR_LIMIT);
    assert(capped.calls == 1);
    forge_judge_destroy(judge);
}

static void test_retrieval_glue(void) {
    stub_state stub = {0};
    char response[256];
    const double expected[] = {0.25, 0.75};
    make_response(response, sizeof(response), "jev-glue", expected, 2);
    stub.response = response;
    forge_judge *judge = make_judge(&stub, NULL, 0);
    const char *paths[] = {"p", "q2"};
    const char *snippets[] = {"s", "t"};
    const char *stages[] = {"literal", "fts5"};
    double scores[2] = {0};
    forge_rerank_info info = {0};
    forge_error error = {0};
    assert(forge_judge_rerank_retrieval(judge, "query", 2, paths, snippets, stages, scores, &info,
                                        &error) == FORGE_OK);
    assert(info.reported && info.model && !strcmp(info.model, "jev-glue"));
    assert(info.input_tokens == 123 && info.output_tokens == 45);
    assert(scores[1] > scores[0]);
    /* A failed call leaves info unreported and the caller informed. */
    forge_rerank_info missing = {0};
    stub.response = "{bad";
    assert(forge_judge_rerank_retrieval(judge, "query", 2, paths, snippets, stages, scores,
                                        &missing, &error) == FORGE_ERR_PARSE);
    assert(!missing.reported);
    /* NULL judge is fail-open, not a crash. */
    assert(forge_judge_rerank_retrieval(NULL, "query", 2, paths, snippets, stages, scores, &missing,
                                        &error) != FORGE_OK);
    forge_judge_destroy(judge);
}

static void test_feedback_payload_and_result(void) {
    stub_state stub = {0};
    stub.response =
        "{\"model\":\"jev-feedback\",\"answers\":{"
        "\"failure_family\":{\"type\":\"choice\",\"choice\":\"code_defect\","
        "\"probabilities\":{\"code_defect\":0.72,\"test_or_requirement_mismatch\":0.05,"
        "\"environment_or_dependency\":0.04,\"missing_evidence\":0.09,\"unknown\":0.10},"
        "\"confidence\":0.68},"
        "\"evidence_gap\":{\"type\":\"noul\",\"noul\":0.21},"
        "\"repair_readiness\":{\"type\":\"score\",\"score\":2.4,"
        "\"legend\":{\"0\":\"No actionable signal\",\"1\":\"Needs more inspection\","
        "\"2\":\"Concrete suspect\",\"3\":\"Actionable repair likely\"},"
        "\"probabilities\":{\"0\":0.02,\"1\":0.08,\"2\":0.38,\"3\":0.52},"
        "\"confidence\":0.71},"
        "\"next_action\":{\"type\":\"choice\",\"choice\":\"patch_code\","
        "\"probabilities\":{\"inspect_source\":0.11,\"patch_code\":0.78,"
        "\"run_validation\":0.03,\"defer\":0.08},\"confidence\":0.74}},"
        "\"usage\":{\"input_tokens\":111,\"output_tokens\":22}}";
    forge_judge *judge = make_judge(&stub, NULL, 0);
    forge_judge_feedback_request request = {0};
    request.task = "Repair value() without changing tests.";
    request.validation_summary = "AssertionError: 0 != 2";
    request.failed_command = "{\"cwd\":\".\",\"argv\":[\"python\",\"-m\",\"unittest\"]}";
    request.current_path = "value.py";
    request.current_source = "def value():\n    return 0\n";
    request.last_delta = "{\"tool\":\"apply_patch\",\"args\":{\"path\":\"value.py\"}}";
    request.remaining_actions = 5;
    request.candidate_attempts = 2;
    request.input_hash = 0x1234;
    request.initial_hash = 0x5678;
    request.repeated_failure = true;
    request.bounded_repair = true;
    forge_judge_feedback_result result = {0};
    forge_error error = {0};
    assert(forge_judge_feedback(judge, &request, &result, &error) == FORGE_OK);
    assert(result.available);
    assert(!strcmp(result.model, "jev-feedback"));
    assert(!strcmp(result.failure_family, "code_defect"));
    assert(result.failure_confidence > 0.67 && result.failure_confidence < 0.69);
    assert(result.evidence_gap > 0.20 && result.evidence_gap < 0.22);
    assert(result.repair_readiness > 2.39 && result.repair_readiness < 2.41);
    assert(!strcmp(result.next_action, "patch_code"));
    assert(result.next_action_confidence > 0.73 && result.next_action_confidence < 0.75);
    assert(result.input_tokens == 111 && result.output_tokens == 22);
    yyjson_doc *doc = yyjson_read(stub.request, stub.request_len, 0);
    assert(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(root, "model")), "jev-latest"));
    yyjson_val *state = yyjson_obj_get(root, "state");
    assert(!strcmp(yyjson_get_str(yyjson_obj_get(state, "task")), request.task));
    assert(!strcmp(fg_json_str(yyjson_obj_get(state, "validation"), "summary"),
                   request.validation_summary));
    assert(!strcmp(fg_json_str(yyjson_obj_get(state, "current"), "source"), request.current_source));
    yyjson_val *questions = yyjson_obj_get(root, "questions");
    assert(yyjson_obj_size(questions) == 4);
    assert(!strcmp(fg_json_str(yyjson_obj_get(questions, "failure_family"), "type"), "choice"));
    assert(!strcmp(fg_json_str(yyjson_obj_get(questions, "evidence_gap"), "type"), "noul"));
    assert(!strcmp(fg_json_str(yyjson_obj_get(questions, "repair_readiness"), "type"), "score"));
    assert(!strcmp(fg_json_str(yyjson_obj_get(questions, "next_action"), "type"), "choice"));
    assert(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(questions, "failure_family"), "criteria"),
                          "missing_evidence"));
    assert(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(questions, "next_action"), "criteria"),
                          "defer"));
    yyjson_doc_free(doc);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 0 && stats.candidates_scored == 0);
    assert(stats.input_tokens == 111 && stats.output_tokens == 22);
    forge_judge_destroy(judge);
}

static void test_feedback_fail_open_errors(void) {
    stub_state stub = {0};
    stub.response = "{\"model\":\"m\",\"answers\":{}}";
    forge_judge *judge = make_judge(&stub, NULL, 0);
    forge_judge_feedback_request request = {0};
    request.task = "task";
    request.validation_summary = "failure";
    forge_judge_feedback_result result = {0};
    forge_error error = {0};
    assert(forge_judge_feedback(judge, &request, &result, &error) == FORGE_ERR_PARSE);
    assert(!result.available);
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    assert(stats.calls == 1 && stats.failures == 1);
    forge_judge_destroy(judge);
}

static void test_budget(void) {
    stub_state stub = {0};
    forge_judge *judge = make_judge(&stub, NULL, 0); /* timeout_ms 500. */
    assert(forge_judge_budget_ms(judge) == 1500);
    assert(forge_judge_budget_ms(NULL) == 0);
    forge_judge_destroy(judge);
    forge_judge_options options = {0};
    options.timeout_ms = FORGE_JUDGE_DEFAULT_TIMEOUT_MS;
    forge_error error = {0};
    judge = forge_judge_create(&options, &error);
    assert(judge && error.code == FORGE_OK);
    assert(forge_judge_budget_ms(judge) == 2 * FORGE_JUDGE_DEFAULT_TIMEOUT_MS + 500);
    forge_judge_destroy(judge);
}

static void create_test_directory(char directory[TEST_PATH]) {
#ifdef _WIN32
    char temporary[TEST_PATH];
    DWORD n = GetTempPathA(sizeof(temporary), temporary);
    assert(n > 0 && n < sizeof(temporary));
    assert(GetTempFileNameA(temporary, "fgj", 0, directory));
    assert(DeleteFileA(directory));
    assert(_mkdir(directory) == 0);
#else
    strcpy(directory, "/tmp/forge-judge-test-XXXXXX");
    assert(mkdtemp(directory));
#endif
}

static size_t scan_records(const char *directory, bool remove_files) {
    size_t count = 0;
#ifdef _WIN32
    char pattern[TEST_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\judge-*.json", directory);
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return 0;
    do {
        count++;
        if (remove_files) {
            char path[TEST_PATH];
            snprintf(path, sizeof(path), "%s\\%s", directory, data.cFileName);
            (void)DeleteFileA(path);
        }
    } while (FindNextFileA(handle, &data));
    FindClose(handle);
#else
    DIR *dir = opendir(directory);
    if (!dir)
        return 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "judge-", 6) || !strstr(entry->d_name, ".json"))
            continue;
        count++;
        if (remove_files) {
            char path[TEST_PATH];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            (void)remove(path);
        }
    }
    closedir(dir);
#endif
    return count;
}

static bool read_first_record(const char *directory, char *out, size_t capacity) {
#ifdef _WIN32
    char pattern[TEST_PATH], path[TEST_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\judge-*.json", directory);
    WIN32_FIND_DATAA data;
    HANDLE handle = FindFirstFileA(pattern, &data);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    snprintf(path, sizeof(path), "%s\\%s", directory, data.cFileName);
    FindClose(handle);
#else
    char path[TEST_PATH] = {0};
    DIR *dir = opendir(directory);
    if (!dir)
        return false;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "judge-", 6) || !strstr(entry->d_name, ".json"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        break;
    }
    closedir(dir);
    if (!path[0])
        return false;
#endif
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    size_t n = fread(out, 1, capacity - 1, file);
    out[n] = 0;
    fclose(file);
    return true;
}

static void test_recording(void) {
    char directory[TEST_PATH];
    create_test_directory(directory);
    size_t before = scan_records(directory, false);
    assert(before == 0);
    stub_state stub = {0};
    char response[256];
    const double expected[] = {0.6, 0.4};
    make_response(response, sizeof(response), "jev-record", expected, 2);
    stub.response = response;
    forge_judge *judge = make_judge(&stub, directory, 0);
    const char *paths[] = {"p", "q"};
    const char *snippets[] = {"alpha", "beta"};
    const char *stages[] = {"literal", "literal"};
    double scores[2] = {0};
    forge_error error = {0};
    assert(forge_judge_rerank(judge, "record me", 2, paths, snippets, stages, scores, &error) ==
           FORGE_OK);
    stub.response = "{broken";
    assert(forge_judge_rerank(judge, "record me", 2, paths, snippets, stages, scores, &error) ==
           FORGE_ERR_PARSE);
    forge_judge_destroy(judge);
    assert(scan_records(directory, false) == 2); /* Success and failure both archived. */
    char record[8192];
    assert(read_first_record(directory, record, sizeof(record)));
    assert(strstr(record, "\"request_id\": \"\""));
    assert(strstr(record, "\"response_date\": \"\""));
    assert(scan_records(directory, true) == 2);
#ifdef _WIN32
    assert(_rmdir(directory) == 0);
#else
    assert(rmdir(directory) == 0);
#endif
}

static void test_config_table(void) {
    forge_config config;
    forge_config_init(&config);
    const char *document = "[judge]\n"
                           "endpoint = \"https://example.test\"\n"
                           "model = \"jev-pinned\"\n"
                           "api_key_env = \"MY_JUDGE_KEY\"\n"
                           "timeout_ms = 3000\n"
                           "max_candidates = 8\n";
    forge_error error = {0};
    assert(forge_config_parse(&config, document, strlen(document), "test.toml", &error) ==
           FORGE_OK);
    assert(!strcmp(config.judge_endpoint, "https://example.test"));
    assert(!strcmp(config.judge_model, "jev-pinned"));
    assert(!strcmp(config.judge_api_key_env, "MY_JUDGE_KEY"));
    assert(config.judge_timeout_ms == 3000 && config.judge_max_candidates == 8);
    forge_config_destroy(&config);
    /* Unknown keys and out-of-range values are rejected like any other table. */
    forge_config_init(&config);
    const char *unknown = "[judge]\nnope = \"x\"\n";
    assert(forge_config_parse(&config, unknown, strlen(unknown), "test.toml", &error) ==
           FORGE_ERR_PARSE);
    forge_config_init(&config);
    const char *bad_timeout = "[judge]\ntimeout_ms = 50\n";
    assert(forge_config_parse(&config, bad_timeout, strlen(bad_timeout), "test.toml",
                              &error) == FORGE_ERR_PARSE);
    forge_config_destroy(&config);
}

static int live_probe(void) {
    const char *key = getenv("TYPESAFE_API_KEY");
    if (!key || !*key) {
        printf("SKIP: TYPESAFE_API_KEY is not set\n");
        return 77;
    }
    forge_judge_options options = {0};
    options.timeout_ms = 5000;
    forge_error error = {0};
    forge_judge *judge = forge_judge_create(&options, &error);
    if (!judge) {
        printf("live probe: create failed: %s\n", error.message);
        return 1;
    }
    const char *paths[] = {"src/repo/retrieval.c", "docs/SECURITY.md", "src/core/agent.c"};
    const char *snippets[] = {
        "b->source_bytes += n;\nif (!fg_sha256_hex(source, n, computed) || strcmp(sha, computed))"
        "\n    return fail(b, FORGE_ERR_PARSE, \"Indexed retrieval source digest mismatch\");",
        "No telemetry, cloud inference, model download, or automatic sharing is built into the "
        "agent runtime.",
        "raw = fg_tool_execute(&tools, tool, args, &changed, &tool_error);"};
    const char *stages[] = {"literal", "fts5", "literal"};
    double scores[3] = {0};
    forge_status status =
        forge_judge_rerank(judge, "where is the retrieval source digest verified?", 3, paths,
                           snippets, stages, scores, &error);
    if (status != FORGE_OK) {
        printf("live probe: rerank failed: %s (%s)\n", forge_status_string(status), error.message);
        forge_judge_destroy(judge);
        return 1;
    }
    forge_judge_stats stats = {0};
    forge_judge_metrics(judge, &stats);
    printf("live probe ok: model=%s latency=%.0fms input_tokens=%zu output_tokens=%zu "
           "scores=[%.3f, %.3f, %.3f]\n",
           stats.last_model, stats.last_latency_ms, stats.input_tokens, stats.output_tokens,
           scores[0], scores[1], scores[2]);
    forge_judge_destroy(judge);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--live"))
        return live_probe();
    test_permutation();
    test_payload_and_scores();
    test_feedback_payload_and_result();
    test_feedback_fail_open_errors();
    test_retry_once();
    test_fail_open_errors();
    test_retrieval_glue();
    test_budget();
    test_recording();
    test_config_table();
    printf("judge tests passed\n");
    return 0;
}