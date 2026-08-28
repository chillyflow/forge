#include "internal.h"
#include "forge/diagnostics.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static yyjson_doc *parse_bytes(const char *bytes, size_t length,
                               const forge_diagnostic_options *options) {
    forge_error error = {0};
    char *json = forge_diagnostics_parse(bytes, length, options, &error);
    assert(json && error.code == FORGE_OK);
    assert(strlen(json) <= (options ? options->max_json_bytes : 1024u * 1024u));
    assert(fg_utf8_valid(json, strlen(json)));
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    assert(doc);
    forge_free(json);
    return doc;
}

static yyjson_doc *parse(const char *text, forge_diagnostic_adapter adapter) {
    forge_diagnostic_options options = forge_diagnostics_default_options();
    options.adapter = adapter;
    return parse_bytes(text, strlen(text), &options);
}

static yyjson_val *records(yyjson_doc *doc) {
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), "diagnostics");
    assert(yyjson_is_arr(value));
    return value;
}

static yyjson_val *record_at(yyjson_doc *doc, size_t index) {
    yyjson_val *value = yyjson_arr_get(records(doc), index);
    assert(yyjson_is_obj(value));
    return value;
}

static const char *string_at(yyjson_val *object, const char *key) {
    yyjson_val *value = yyjson_obj_get(object, key);
    assert(yyjson_is_str(value));
    return yyjson_get_str(value);
}

static uint64_t stat_at(yyjson_doc *doc, const char *key) {
    yyjson_val *summary = yyjson_obj_get(yyjson_doc_get_root(doc), "summary");
    yyjson_val *value = yyjson_obj_get(summary, key);
    assert(yyjson_is_uint(value));
    return yyjson_get_uint(value);
}

static bool flag_at(yyjson_doc *doc, const char *key) {
    yyjson_val *value = yyjson_obj_get(yyjson_doc_get_root(doc), key);
    assert(yyjson_is_bool(value));
    return yyjson_get_bool(value);
}

static yyjson_val *find_record(yyjson_doc *doc, const char *key, const char *text) {
    size_t i, count;
    yyjson_val *record;
    yyjson_arr_foreach(records(doc), i, count, record) {
        const char *value = yyjson_get_str(yyjson_obj_get(record, key));
        if (value && !strcmp(value, text))
            return record;
    }
    assert(!"expected diagnostic record not found");
    return NULL;
}

static char *render(yyjson_doc *doc, size_t budget) {
    char *json = yyjson_write(doc, 0, NULL);
    assert(json);
    forge_error error = {0};
    size_t visible = SIZE_MAX;
    char *text = forge_diagnostics_render(json, strlen(json), budget, &visible, &error);
    assert(text && error.code == FORGE_OK);
    assert(strlen(text) == visible && visible <= budget);
    assert(fg_utf8_valid(text, visible));
    free(json);
    return text;
}

static void test_compiler_text(void) {
    const char *text = "z.c:9:2: warning: unused value [-Wunused-value]\n"
                       "C:\\work\\a.c:4:7: error: missing name\n"
                       "C:\\work\\a.c:4:7: error: missing name\n";
    yyjson_doc *doc = parse(text, FORGE_DIAGNOSTICS_AUTO);
    assert(yyjson_arr_size(records(doc)) == 2);
    yyjson_val *first = record_at(doc, 0);
    assert(!strcmp(string_at(first, "adapter"), "compiler"));
    assert(!strcmp(string_at(first, "severity"), "error"));
    assert(yyjson_get_uint(yyjson_obj_get(first, "occurrences")) == 2);
    yyjson_val *location = yyjson_obj_get(first, "location");
    assert(!strcmp(string_at(location, "path"), "C:\\work\\a.c"));
    assert(yyjson_get_uint(yyjson_obj_get(location, "line")) == 4);
    assert(yyjson_get_uint(yyjson_obj_get(location, "column")) == 7);
    assert(!strcmp(string_at(location, "column_unit"), "unknown"));
    assert(yyjson_is_null(yyjson_obj_get(location, "column_origin")));
    assert(stat_at(doc, "duplicate_occurrences") == 1 && !flag_at(doc, "incomplete"));
    assert(!strcmp(string_at(record_at(doc, 1), "code"), "-Wunused-value"));
    char *view = render(doc, 512);
    assert(strstr(view, "missing name [x2]"));
    forge_free(view);
    yyjson_doc_free(doc);

    doc = parse("\x1b[31ma.c:2:3: error: bad token\x1b[0m\n", FORGE_DIAGNOSTICS_CLANG);
    first = record_at(doc, 0);
    location = yyjson_obj_get(first, "location");
    assert(flag_at(doc, "ansi_removed"));
    assert(!strcmp(string_at(first, "adapter"), "clang"));
    assert(!strcmp(string_at(location, "column_unit"), "byte"));
    assert(yyjson_get_sint(yyjson_obj_get(location, "column_origin")) == 1);
    yyjson_doc_free(doc);

    const char *invalid[] = {"a.c:-1:2: error: bad", "a.c:0:2: error: bad",
                             "a.c:4294967296:2: error: bad", "a.c:2:-1: error: bad",
                             "a.c:2:4294967296: error: bad"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        doc = parse(invalid[i], FORGE_DIAGNOSTICS_GCC);
        first = record_at(doc, 0);
        assert(!strcmp(string_at(first, "adapter"), "generic"));
        assert(yyjson_is_null(yyjson_obj_get(yyjson_obj_get(first, "location"), "line")));
        yyjson_doc_free(doc);
    }
}

static void test_gcc_and_lint_json(void) {
    const char *gcc =
        "[{\"kind\":\"warning\",\"message\":\"unused result\",\"option\":\"-Wunused-result\","
        "\"column-origin\":0,\"locations\":[{\"caret\":{\"file\":\"a.c\",\"line\":8,"
        "\"column\":5,\"byte-column\":0,\"display-column\":3}}],"
        "\"children\":[{\"kind\":\"note\",\"message\":\"declared here\",\"locations\":[]}]}]";
    yyjson_doc *doc = parse(gcc, FORGE_DIAGNOSTICS_AUTO);
    yyjson_val *record = record_at(doc, 0), *location = yyjson_obj_get(record, "location");
    assert(!strcmp(string_at(record, "adapter"), "gcc"));
    assert(!strcmp(string_at(record, "format"), "gcc_json"));
    assert(!strcmp(string_at(location, "column_unit"), "byte"));
    assert(yyjson_get_uint(yyjson_obj_get(location, "column")) == 0);
    assert(!yyjson_is_null(yyjson_obj_get(location, "column")));
    assert(yyjson_get_sint(yyjson_obj_get(location, "column_origin")) == 0);
    assert(yyjson_arr_size(yyjson_obj_get(record, "details")) == 1);
    assert(yyjson_arr_size(yyjson_obj_get(record, "stack")) == 0);
    yyjson_doc_free(doc);

    const char *lint =
        "{\"Issues\":[{\"FromLinter\":\"unused\",\"Text\":\"unused helper\",\"Severity\":\"\","
        "\"Pos\":{\"Filename\":\"util.go\",\"Line\":12,\"Column\":2},"
        "\"SourceLines\":[\"func helper() {}\"]}],\"Report\":{}}";
    doc = parse(lint, FORGE_DIAGNOSTICS_AUTO);
    record = record_at(doc, 0);
    assert(!strcmp(string_at(record, "adapter"), "golangci_lint"));
    assert(!strcmp(string_at(record, "kind"), "lint"));
    assert(!strcmp(string_at(record, "severity"), "unknown"));
    assert(!strcmp(string_at(record, "code"), "unused"));
    assert(!strcmp(string_at(yyjson_obj_get(record, "location"), "column_unit"), "byte"));
    yyjson_doc_free(doc);

    doc = parse("a.go:6:3: fmt.Printf call has wrong argument\n", FORGE_DIAGNOSTICS_GO_VET);
    record = record_at(doc, 0);
    assert(!strcmp(string_at(record, "adapter"), "go_vet"));
    assert(!strcmp(string_at(record, "kind"), "lint"));
    assert(!strcmp(string_at(record, "severity"), "unknown"));
    yyjson_doc_free(doc);
    doc = parse("a.go:6:3: fmt.Printf call has wrong argument\n", FORGE_DIAGNOSTICS_AUTO);
    assert(!strcmp(string_at(record_at(doc, 0), "adapter"), "generic"));
    assert(!strcmp(string_at(record_at(doc, 0), "format"), "location_text"));
    yyjson_doc_free(doc);
    doc = parse("a.go:6:3: unused local (ineffassign)\n", FORGE_DIAGNOSTICS_GOLANGCI_LINT);
    assert(!strcmp(string_at(record_at(doc, 0), "code"), "ineffassign"));
    yyjson_doc_free(doc);
}

static void test_cargo_and_rustc(void) {
    const char *cargo =
        "{\"reason\":\"compiler-artifact\",\"package_id\":\"path+file:///demo#0.1.0\"}\n"
        "{\"reason\":\"compiler-message\",\"package_id\":\"path+file:///demo#0.1.0\","
        "\"message\":{\"message\":\"mismatched types\",\"code\":{\"code\":\"E0308\"},"
        "\"level\":\"error\",\"spans\":[{\"file_name\":\"src/lib.rs\",\"line_start\":5,"
        "\"column_start\":7,\"is_primary\":true,\"label\":\"integer required\","
        "\"text\":[{\"text\":\"let x: u32 = value;\"}]}],"
        "\"children\":[{\"message\":\"convert the value\",\"level\":\"help\",\"spans\":[]}]} }\n"
        "{\"reason\":\"build-finished\",\"success\":false}\n";
    yyjson_doc *doc = parse(cargo, FORGE_DIAGNOSTICS_AUTO);
    yyjson_val *record = find_record(doc, "code", "E0308");
    assert(!strcmp(string_at(record, "adapter"), "cargo"));
    assert(!strcmp(string_at(record, "format"), "cargo_json"));
    assert(!strcmp(string_at(record, "package"), "path+file:///demo#0.1.0"));
    assert(!strcmp(string_at(yyjson_obj_get(record, "location"), "column_unit"), "unicode_scalar"));
    assert(yyjson_arr_size(yyjson_obj_get(record, "details")) == 3);
    assert(yyjson_arr_size(yyjson_obj_get(record, "stack")) == 0);
    assert(yyjson_is_null(yyjson_obj_get(record, "expected")));
    assert(stat_at(doc, "ignored_events") == 1);
    assert(!strcmp(string_at(find_record(doc, "kind", "build_failure"), "message"),
                   "cargo build failed"));
    yyjson_doc_free(doc);

    doc = parse("{\"reason\":\"build-finished\",\"success\":true}", FORGE_DIAGNOSTICS_AUTO);
    assert(yyjson_arr_size(records(doc)) == 0 && stat_at(doc, "test_pass_events") == 0);
    assert(stat_at(doc, "ignored_events") == 1);
    yyjson_doc_free(doc);
    doc = parse("{\"$message_type\":\"diagnostic\",\"message\":\"bad value\",\"level\":\"error\","
                "\"spans\":[{\"is_primary\":true,\"file_name\":\"x.rs\",\"line_start\":-2,\"column_"
                "start\":1}]}",
                FORGE_DIAGNOSTICS_AUTO);
    assert(flag_at(doc, "incomplete") && stat_at(doc, "malformed_records") == 1);
    assert(!strcmp(string_at(record_at(doc, 0), "adapter"), "rustc"));
    assert(yyjson_is_null(yyjson_obj_get(yyjson_obj_get(record_at(doc, 0), "location"), "line")));
    yyjson_doc_free(doc);

    doc = parse("test tests::sum ... FAILED\n"
                "thread 'tests::sum' panicked at src/lib.rs:7:2:\n"
                "assertion `left == right` failed\n"
                "  left: 3\n  right: 4\n"
                "stack backtrace:\n  0: example::tests::sum\n    at src/lib.rs:7:2\n"
                "error[E0308]: mismatched types\n --> src/lib.rs:11:8\n",
                FORGE_DIAGNOSTICS_CARGO);
    record = find_record(doc, "format", "rust_panic");
    assert(!strcmp(string_at(record, "thread"), "tests::sum"));
    assert(yyjson_is_null(yyjson_obj_get(record, "test")));
    assert(!strcmp(string_at(record, "left"), "3") && !strcmp(string_at(record, "right"), "4"));
    assert(yyjson_is_null(yyjson_obj_get(record, "expected")));
    assert(yyjson_arr_size(yyjson_obj_get(record, "stack")) == 1);
    assert(!strcmp(string_at(yyjson_obj_get(record, "location"), "path"), "src/lib.rs"));
    assert(!strcmp(string_at(find_record(doc, "test", "tests::sum"), "kind"), "test_failure"));
    record = find_record(doc, "code", "E0308");
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_obj_get(record, "location"), "line")) == 11);
    yyjson_doc_free(doc);
}

static void test_go_events_and_fragments(void) {
    const char *events =
        "{\"Action\":\"output\",\"Package\":\"example/"
        "math\",\"Test\":\"TestA\",\"Output\":\"a_test.go:7: expected=4\"}\n"
        "{\"Action\":\"output\",\"Package\":\"example/"
        "math\",\"Test\":\"TestB\",\"Output\":\"b_test.go:8: expected=6 actual=5\\n\"}\n"
        "{\"Action\":\"output\",\"Package\":\"example/math\",\"Test\":\"TestA\",\"Output\":\" "
        "actual=3\\n\"}\n"
        "{\"Action\":\"output\",\"Package\":\"example/"
        "math\",\"Test\":\"TestA\",\"Output\":\"goroutine 1 "
        "[running]:\\nexample/math.Add()\\n\\ta.go:9 +0x12\\n\"}\n"
        "{\"Action\":\"fail\",\"Package\":\"example/math\",\"Test\":\"TestA\"}\n"
        "{\"Action\":\"fail\",\"Package\":\"example/math\",\"Test\":\"TestB\"}\n"
        "{\"Action\":\"pass\",\"Package\":\"example/math\",\"Test\":\"TestOK\"}\n"
        "{\"Action\":\"fail\",\"Package\":\"example/math\"}\n";
    yyjson_doc *doc = parse(events, FORGE_DIAGNOSTICS_AUTO);
    assert(stat_at(doc, "test_pass_events") == 1 && stat_at(doc, "test_fail_events") == 2);
    assert(stat_at(doc, "package_fail_events") == 1 && stat_at(doc, "go_fail_events") == 3);
    yyjson_val *record = find_record(doc, "expected", "4");
    assert(!strcmp(string_at(record, "actual"), "3"));
    assert(!strcmp(string_at(record, "test"), "TestA"));
    assert(!strcmp(string_at(record, "package"), "example/math"));
    yyjson_val *stack = yyjson_obj_get(record, "stack");
    assert(yyjson_arr_size(stack) == 1);
    assert(!strcmp(string_at(yyjson_arr_get_first(stack), "function"), "example/math.Add()"));
    assert(!strcmp(string_at(find_record(doc, "kind", "package_failure"), "message"),
                   "package failed"));
    char *view = render(doc, 1024);
    assert(strstr(view, "TestA") && strstr(view, "expected=4") && strstr(view, "pass=1"));
    free(view);
    yyjson_doc_free(doc);

    const char *one =
        "{\"Action\":\"output\",\"Package\":\"p\",\"Output\":\"x.go:2: error: bad\\n\"}\n";
    fg_buf duplicated = {0};
    fg_buf_puts(&duplicated, one);
    fg_buf_puts(&duplicated, one);
    doc = parse(duplicated.data, FORGE_DIAGNOSTICS_AUTO);
    assert(yyjson_arr_size(records(doc)) == 1 && stat_at(doc, "duplicate_occurrences") == 1);
    forge_error error = {0};
    view = fg_compress_output(duplicated.data, 512, NULL, &error);
    assert(view && strstr(view, "[x2]"));
    free(view);
    yyjson_doc_free(doc);
    fg_buf_clear(&duplicated);

    const char *a = "{\"Time\":\"one\",\"Action\":\"fail\",\"Package\":\"p\",\"Test\":\"TestA\"}\n"
                    "{\"Action\":\"output\",\"Output\":\"a_test.go:9: expected 4 got 3\\n\"}\n";
    const char *b = "{\"Action\":\"output\",\"Output\":\"a_test.go:9: expected 4 got 3\\n\"}\n"
                    "{\"Elapsed\":2.3,\"Time\":\"two\",\"Action\":\"fail\",\"Package\":\"p\","
                    "\"Test\":\"TestA\"}\n";
    assert(fg_diagnostic_hash(a) == fg_diagnostic_hash(b));
    assert(fg_diagnostic_hash("x.c:1: error: one\n") != fg_diagnostic_hash("x.c:1: error: two\n"));
    assert(fg_diagnostic_hash(one) == fg_diagnostic_hash(one));
}

static void test_pytest(void) {
    const char *text = "================ FAILURES ================\n"
                       "________________ test_sum _________________\n"
                       "    def test_sum():\n"
                       ">       assert total == 4\n"
                       "E       assert 3 == 4\n"
                       "tests/test_math.py:8: AssertionError\n"
                       "FAILED tests/test_math.py::test_sum - assert 3 == 4\n";
    yyjson_doc *doc = parse(text, FORGE_DIAGNOSTICS_AUTO);
    yyjson_val *record = find_record(doc, "test", "test_sum");
    assert(!strcmp(string_at(record, "adapter"), "pytest"));
    assert(!strcmp(string_at(record, "message"), "assert 3 == 4"));
    assert(yyjson_is_null(yyjson_obj_get(record, "expected")));
    assert(yyjson_is_null(yyjson_obj_get(record, "actual")));
    assert(yyjson_arr_size(yyjson_obj_get(record, "stack")) == 1);
    assert(!strcmp(string_at(yyjson_obj_get(record, "location"), "path"), "tests/test_math.py"));
    assert(!strcmp(string_at(find_record(doc, "test", "tests/test_math.py::test_sum"), "format"),
                   "pytest_summary"));
    yyjson_doc_free(doc);

    doc = parse("Traceback (most recent call last):\n"
                "  File \"tests/test_math.py\", line 8, in test_sum\n"
                "AssertionError: expected four\n",
                FORGE_DIAGNOSTICS_PYTEST);
    record = record_at(doc, 0);
    assert(!strcmp(string_at(record, "message"), "AssertionError: expected four"));
    assert(!strcmp(string_at(record, "severity"), "error"));
    assert(yyjson_arr_size(yyjson_obj_get(record, "stack")) == 1);
    assert(yyjson_is_null(yyjson_obj_get(record, "test")));
    yyjson_doc_free(doc);
}

static void test_generic_malformed_and_binary(void) {
    const char *unknown = "{\n  \"Action\": \"not-a-Go-action\",\n  \"answer\": 42\n}";
    yyjson_doc *doc = parse(unknown, FORGE_DIAGNOSTICS_AUTO);
    assert(yyjson_arr_size(records(doc)) == 1);
    assert(!strcmp(string_at(record_at(doc, 0), "format"), "generic_json"));
    assert(stat_at(doc, "malformed_records") == 0 &&
           stat_at(doc, "unrecognized_json_records") == 1);
    assert(!flag_at(doc, "incomplete"));
    yyjson_doc_free(doc);

    doc = parse("{\"Action\":\"output\",\"Output\":7}", FORGE_DIAGNOSTICS_AUTO);
    assert(stat_at(doc, "malformed_records") == 1 && flag_at(doc, "incomplete"));
    assert(!strcmp(string_at(record_at(doc, 0), "adapter"), "generic"));
    yyjson_doc_free(doc);
    doc = parse("{\"Action\":\"pass\",\"Test\":12}", FORGE_DIAGNOSTICS_AUTO);
    assert(stat_at(doc, "malformed_records") == 1 && stat_at(doc, "test_pass_events") == 0);
    yyjson_doc_free(doc);
    doc = parse("  {not json\n", FORGE_DIAGNOSTICS_AUTO);
    assert(stat_at(doc, "malformed_records") == 1 && flag_at(doc, "incomplete"));
    yyjson_doc_free(doc);
    doc = parse("FAILED tests/x.py::test_x - bad\n", FORGE_DIAGNOSTICS_GENERIC);
    assert(!strcmp(string_at(record_at(doc, 0), "adapter"), "generic"));
    yyjson_doc_free(doc);
    doc = parse("ordinary stdout\n", FORGE_DIAGNOSTICS_CLANG);
    assert(!strcmp(string_at(record_at(doc, 0), "adapter"), "generic"));
    yyjson_doc_free(doc);

    const char binary[] = "error: before\0after \xff\x01\xe2\x82\xac\n"
                          "error: before\\x00after \\xff\\x01\xe2\x82\xac\n";
    doc = parse_bytes(binary, sizeof(binary) - 1, NULL);
    assert(yyjson_arr_size(records(doc)) == 2 && stat_at(doc, "duplicate_occurrences") == 0);
    assert(flag_at(doc, "bytes_rendered"));
    char *view = render(doc, 512);
    assert(strstr(view, "before\\x00after \\xff\\x01\xe2\x82\xac"));
    free(view);
    yyjson_doc_free(doc);

    doc = parse("x.go:9: unexpected=4 actual=3\n", FORGE_DIAGNOSTICS_GO_VET);
    assert(yyjson_is_null(yyjson_obj_get(record_at(doc, 0), "expected")));
    yyjson_doc_free(doc);
}

static void test_ranking_and_limits(void) {
    const char *a = "z.c:8: warning: later\na.c:3: error: first\nb.c:5: note: detail\n";
    const char *b = "b.c:5: note: detail\na.c:3: error: first\nz.c:8: warning: later\n";
    yyjson_doc *left = parse(a, FORGE_DIAGNOSTICS_AUTO), *right = parse(b, FORGE_DIAGNOSTICS_AUTO);
    assert(yyjson_arr_size(records(left)) == 3);
    for (size_t i = 0; i < 3; i++)
        assert(!strcmp(string_at(record_at(left, i), "fingerprint"),
                       string_at(record_at(right, i), "fingerprint")));
    yyjson_doc_free(left);
    yyjson_doc_free(right);

    forge_diagnostic_options options = forge_diagnostics_default_options();
    options.max_diagnostics = 1;
    const char *rank = "progress one\nprogress two\na.c:9: error: final important error\n";
    yyjson_doc *doc = parse_bytes(rank, strlen(rank), &options);
    assert(yyjson_arr_size(records(doc)) == 1 && stat_at(doc, "omitted_diagnostics") == 2);
    assert(!strcmp(string_at(record_at(doc, 0), "severity"), "error"));
    assert(flag_at(doc, "incomplete"));
    yyjson_doc_free(doc);

    options = forge_diagnostics_default_options();
    options.max_input_bytes = 8;
    doc = parse_bytes("error: bad tail", 15, &options);
    assert(flag_at(doc, "input_truncated") && flag_at(doc, "incomplete"));
    assert(yyjson_get_uint(yyjson_obj_get(yyjson_doc_get_root(doc), "parsed_bytes")) == 8);
    yyjson_doc_free(doc);
    options = forge_diagnostics_default_options();
    options.input_truncated = true;
    doc = parse_bytes("", 0, &options);
    assert(flag_at(doc, "input_truncated") && flag_at(doc, "incomplete"));
    char *view = render(doc, 64);
    assert(strstr(view, "incomplete"));
    free(view);
    yyjson_doc_free(doc);

    fg_buf long_text = {0};
    for (size_t i = 0; i < 2; i++) {
        fg_buf_puts(&long_text, "a.c:1: error: ");
        for (size_t j = 0; j < 70; j++)
            fg_buf_puts(&long_text, "x");
        fg_buf_printf(&long_text, "%c\n", i ? 'b' : 'a');
    }
    options = forge_diagnostics_default_options();
    options.max_text_bytes = 64;
    doc = parse_bytes(long_text.data, long_text.len, &options);
    assert(yyjson_arr_size(records(doc)) == 2 && stat_at(doc, "duplicate_occurrences") == 0);
    assert(flag_at(doc, "incomplete") && stat_at(doc, "clipped_fields") > 0);
    assert(strcmp(string_at(record_at(doc, 0), "fingerprint"),
                  string_at(record_at(doc, 1), "fingerprint")));
    yyjson_doc_free(doc);
    fg_buf_clear(&long_text);

    size_t n = 256u * 1024u + 1u;
    char *line = malloc(n);
    assert(line);
    memset(line, 'x', n);
    doc = parse_bytes(line, n, NULL);
    assert(flag_at(doc, "line_truncated") && flag_at(doc, "incomplete"));
    yyjson_doc_free(doc);
    free(line);
    for (size_t i = 0; i < 65; i++)
        fg_buf_printf(
            &long_text,
            "{\"Action\":\"output\",\"Test\":\"T%zu\",\"Output\":\"a.go:1: error: bad\\n\"}\n", i);
    doc = parse_bytes(long_text.data, long_text.len, NULL);
    assert(flag_at(doc, "stream_limit_reached") && flag_at(doc, "incomplete"));
    assert(stat_at(doc, "omitted_diagnostics") >= 1);
    yyjson_doc_free(doc);
    fg_buf_clear(&long_text);

    for (size_t i = 0; i < 40; i++)
        fg_buf_printf(&long_text, "x.c:%zu: error: distinct diagnostic message\n", i + 1);
    options = forge_diagnostics_default_options();
    options.max_json_bytes = 4096;
    doc = parse_bytes(long_text.data, long_text.len, &options);
    assert(stat_at(doc, "omitted_diagnostics") > 0 && flag_at(doc, "incomplete"));
    yyjson_doc_free(doc);
    fg_buf_clear(&long_text);
}

static void test_render_boundaries_and_invalid_requests(void) {
    fg_buf text = {0};
    for (size_t i = 0; i < 20; i++)
        fg_buf_printf(
            &text, "file.c:%zu: error: \xe2\x82\xac\xf0\x9f\x8c\x8d repeated diagnostic\n", i + 1);
    yyjson_doc *doc = parse(text.data, FORGE_DIAGNOSTICS_AUTO);
    for (size_t budget = 64; budget <= 1024; budget++) {
        char *view = render(doc, budget);
        if (budget == 64)
            assert(strstr(view, "incomplete"));
        free(view);
    }
    yyjson_doc_free(doc);
    fg_buf_clear(&text);

    forge_error error = {0};
    assert(!forge_diagnostics_parse(NULL, 1, NULL, &error) && error.code == FORGE_ERR_ARGUMENT);
    forge_diagnostic_options options = forge_diagnostics_default_options();
    options.max_diagnostics = 0;
    assert(!forge_diagnostics_parse("x", 1, &options, &error) && error.code == FORGE_ERR_ARGUMENT);
    options = forge_diagnostics_default_options();
    options.adapter = (forge_diagnostic_adapter)99;
    assert(!forge_diagnostics_parse("x", 1, &options, &error));
    options = forge_diagnostics_default_options();
    options.max_json_bytes = 4095;
    assert(!forge_diagnostics_parse("x", 1, &options, &error));
    options = forge_diagnostics_default_options();
    options.max_text_bytes = 63;
    assert(!forge_diagnostics_parse("x", 1, &options, &error));
    assert(!forge_diagnostics_render("{}", 2, 64, NULL, &error) && error.code == FORGE_ERR_PARSE);
    assert(!forge_diagnostics_render("{}", 2, 63, NULL, &error) &&
           error.code == FORGE_ERR_ARGUMENT);
    const char *bad =
        "{\"schema_version\":1,\"incomplete\":false,\"summary\":{},\"diagnostics\":[{}]}";
    assert(!forge_diagnostics_render(bad, strlen(bad), 64, NULL, &error) &&
           error.code == FORGE_ERR_PARSE);

    /* Deterministic malformed byte smoke cases exercise the length-taking entry point. */
    uint32_t state = 17;
    char bytes[97];
    for (size_t trial = 0; trial < 256; trial++) {
        size_t length = trial % sizeof(bytes);
        for (size_t i = 0; i < length; i++) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[i] = (char)(state >> 24);
        }
        doc = parse_bytes(bytes, length, NULL);
        char *view = render(doc, 96);
        free(view);
        yyjson_doc_free(doc);
    }
}

static void test_compatibility_wrappers(void) {
    forge_error error = {0};
    const char *small = "exit_code=0\n{\"answer\":42}\nordinary stdout\n";
    size_t visible = 0;
    char *view = fg_compress_output(small, 256, &visible, &error);
    assert(view && !strcmp(view, small) && visible == strlen(small));
    free(view);

    fg_buf output = {0}, event = {0};
    fg_buf_puts(&output, "x.go:1: error ");
    while (output.len < 1023)
        fg_buf_puts(&output, "a");
    fg_buf_puts(&output, "\xe2\x82\xac after clip");
    char *quoted = fg_json_string(output.data);
    assert(quoted);
    fg_buf_printf(&event, "{\"Action\":\"output\",\"Output\":%s}\n", quoted);
    free(quoted);
    view = fg_compress_output(event.data, 2048, &visible, &error);
    assert(view && visible == 1024 && view[1023] == '\n');
    assert(fg_utf8_valid(view, visible) && !strstr(view, "after clip"));
    free(view);
    fg_buf_clear(&output);
    fg_buf_clear(&event);

    const char *nul =
        "{\"Action\":\"output\",\"Output\":\"before\\u0000x.go:9: error after NUL \\u03a9\\n\"}\n";
    const char *changed = "{\"Action\":\"output\",\"Output\":\"before\\u0000x.go:9: error changed "
                          "NUL \\u03a9\\n\"}\n";
    view = fg_compress_output(nul, 512, NULL, &error);
    assert(view && strstr(view, "before\\x00x.go:9: error after NUL \xce\xa9"));
    assert(fg_diagnostic_hash(nul) != fg_diagnostic_hash(changed));
    free(view);
}

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_compiler_text();
    test_gcc_and_lint_json();
    test_cargo_and_rustc();
    test_go_events_and_fragments();
    test_pytest();
    test_generic_malformed_and_binary();
    test_ranking_and_limits();
    test_render_boundaries_and_invalid_requests();
    test_compatibility_wrappers();
    puts("diagnostic adapters, limits, and compatibility tests passed");
    return 0;
}
