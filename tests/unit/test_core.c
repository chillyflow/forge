#include "internal.h"
#include <assert.h>
#ifndef _WIN32
#include <signal.h>
#endif
/* Do not compile out assertions in Release builds. */
#ifdef NDEBUG
#undef NDEBUG
#include <assert.h>
#endif
static size_t count(const char *s, void *u) {
    (void)u;
    return strlen(s);
}
static void test_utf8_and_byte_rendering(void) {
    const char text[] = "a\xc2\xa2\xe2\x82\xac\xf0\x9f\x8c\x8d"
                        "z";
    const size_t boundaries[] = {0, 1, 3, 6, 10, 11};
    assert(fg_utf8_valid(text, sizeof(text) - 1));
    assert(fg_utf8_valid(NULL, 0));
    assert(!fg_utf8_valid(NULL, 1));
    for (size_t cut = 0; cut <= sizeof(text); cut++) {
        size_t before = 0, after = sizeof(text) - 1;
        for (size_t i = 0; i < sizeof(boundaries) / sizeof(*boundaries); i++) {
            if (boundaries[i] <= cut)
                before = boundaries[i];
            if (boundaries[i] >= cut) {
                after = boundaries[i];
                break;
            }
        }
        size_t prefix = fg_utf8_prefix(text, sizeof(text) - 1, cut);
        size_t forward = fg_utf8_forward(text, sizeof(text) - 1, cut);
        assert(prefix == before && forward == after);
        assert(fg_utf8_valid(text, prefix));
        assert(fg_utf8_valid(text + forward, sizeof(text) - 1 - forward));
    }
    const char *invalid[] = {"\x80",
                             "\xc0\x80",
                             "\xc2",
                             "\xe0\x80\x80",
                             "\xed\xa0\x80",
                             "\xe2\x82",
                             "\xf0\x80\x80\x80",
                             "\xf4\x90\x80\x80",
                             "\xf5\x80\x80\x80",
                             "\xff"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        assert(!fg_utf8_valid(invalid[i], strlen(invalid[i])));
    const char limits[] = "\xe0\xa0\x80\xed\x9f\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf";
    assert(fg_utf8_valid(limits, sizeof(limits) - 1));

    const char binary[] = "a\0b\n\t\r\x01\x7f\xff\xed\xa0\x80\xe2\x82\xac"
                          "z";
    const char *expected = "a\\x00b\n\t\r\\x01\\x7f\\xff\\xed\\xa0\\x80\xe2\x82\xac"
                           "z";
    char *rendered = fg_render_bytes(binary, sizeof(binary) - 1);
    assert(rendered && !strcmp(rendered, expected));
    assert(fg_utf8_valid(rendered, strlen(rendered)));
    free(rendered);
    assert(!fg_render_bytes(NULL, 1));
    rendered = fg_render_bytes(NULL, 0);
    assert(rendered && !*rendered);
    free(rendered);

    fg_process_result process = {0};
    process.out = (char *)binary;
    process.out_len = sizeof(binary) - 1;
    process.err = (char *)"\xc2";
    process.err_len = 1;
    rendered = fg_process_render(&process);
    assert(rendered && strstr(rendered, expected) && strstr(rendered, "stderr:\n\\xc2"));
    assert(fg_utf8_valid(rendered, strlen(rendered)));
    free(rendered);
}
static void assert_valid_compression(const char *raw, size_t first_budget, size_t last_budget) {
    for (size_t budget = first_budget; budget <= last_budget; budget++) {
        forge_error error = {0};
        size_t visible = SIZE_MAX;
        char *compressed = fg_compress_output(raw, budget, &visible, &error);
        assert(compressed && error.code == FORGE_OK);
        assert(visible == strlen(compressed) && visible <= budget);
        assert(fg_utf8_valid(compressed, visible));
        free(compressed);
    }
}
static void test_diagnostic_byte_boundaries(void) {
    forge_error error = {0};
    fg_buf line = {0}, raw = {0};
    fg_buf_puts(&line, "x.go:1: error ");
    while (line.len < 1023)
        fg_buf_puts(&line, "a");
    fg_buf_puts(&line, "\xe2\x82\xac"
                       "after clip");
    char *quoted = fg_json_string(line.data);
    assert(quoted);
    fg_buf_printf(&raw, "{\"Action\":\"output\",\"Output\":%s}\n", quoted);
    free(quoted);
    char *compressed = fg_compress_output(raw.data, 2048, NULL, &error);
    assert(compressed && strlen(compressed) == 1024 && compressed[1023] == '\n');
    assert(fg_utf8_valid(compressed, strlen(compressed)));
    assert(!strstr(compressed, "after clip"));
    free(compressed);
    fg_buf_clear(&line);
    fg_buf_clear(&raw);

    /* Exercise generic prefix, tail-start and final boundaries at every byte. */
    fg_buf_puts(&raw, "x.go:1: error ");
    for (size_t i = 0; i < 250; i++)
        fg_buf_puts(&raw, "\xe2\x82\xac");
    fg_buf_puts(&raw, "\nprogress ");
    for (size_t i = 0; i < 700; i++)
        fg_buf_puts(&raw, "\xf0\x9f\x8c\x8d");
    fg_buf_puts(&raw, "\n");
    assert_valid_compression(raw.data, 64, 1536);
    fg_buf_clear(&raw);

    /* No relevant diagnostics: Go's fallback tail must also accept budget<80. */
    fg_buf_puts(&raw, "{\"Action\":\"output\",\"Output\":\"");
    for (size_t i = 0; i < 200; i++)
        fg_buf_puts(&raw, "\xc2\xa2");
    fg_buf_puts(&raw, "\"}\n");
    assert_valid_compression(raw.data, 64, 256);
    fg_buf_clear(&raw);

    /* Long failure identities can reach the final output budget boundary. */
    fg_buf_puts(&raw, "{\"Action\":\"fail\",\"Package\":\"");
    for (size_t i = 0; i < 200; i++)
        fg_buf_puts(&raw, "\xe2\x82\xac");
    fg_buf_puts(&raw, "\"}\n");
    assert_valid_compression(raw.data, 64, 256);
    fg_buf_clear(&raw);

    const char *nul_json = "{\"Action\":\"output\",\"Output\":\"before\\u0000x.go:9: "
                           "error after NUL \\u03a9\\n\"}\n";
    compressed = fg_compress_output(nul_json, 1024, NULL, &error);
    assert(compressed && strstr(compressed, "before\\x00x.go:9: error after NUL \xce\xa9"));
    assert(fg_utf8_valid(compressed, strlen(compressed)));
    free(compressed);
    assert(fg_diagnostic_hash(nul_json) !=
           fg_diagnostic_hash("{\"Action\":\"output\",\"Output\":\"before\\u0000x.go:9: "
                              "error different after NUL \\u03a9\\n\"}\n"));
    compressed = fg_compress_output("error invalid \xff"
                                    " and control \x01"
                                    "after",
                                    256, NULL, &error);
    assert(compressed && strstr(compressed, "\\xff and control \\x01after"));
    assert(fg_utf8_valid(compressed, strlen(compressed)));
    free(compressed);
}
int main(void) {
    test_utf8_and_byte_rendering();
    test_diagnostic_byte_boundaries();
    fg_buf b = {0};
    assert(fg_buf_puts(&b, "abc"));
    assert(fg_buf_printf(&b, "%d", 123));
    char *s = fg_buf_take(&b);
    assert(!strcmp(s, "abc123"));
    free(s);
    forge_error e = {0};
    char path[FG_PATH_MAX];
    assert(!fg_safe_path(".", "../secret", true, path, &e));
    assert(e.code == FORGE_ERR_POLICY);
    assert(!fg_safe_path(".", "C:/secret", true, path, &e));
    assert(!fg_safe_path(".", ".git/config", true, path, &e));
    forge_context *c = forge_context_create(300, 50, count, NULL);
    assert(c);
    uint64_t a = forge_context_add(c, FORGE_SEG_SYSTEM, "stable system", 100, true, 0, 0);
    assert(a);
    uint64_t action = forge_context_add(c, FORGE_SEG_ACTION, "read_file", 1, false, 0, 0);
    forge_context_add(c, FORGE_SEG_RESULT, "important result", 90, false, action, 0);
    size_t tokens = 0, evicted = 0;
    s = forge_context_plan(c, &tokens, &evicted, &e);
    assert(s && tokens <= 250);
    assert(strstr(s, "read_file") < strstr(s, "important result"));
    free(s);
    assert(forge_context_update(c, a, "new system", 1) == FORGE_OK);
    s = forge_context_plan(c, &tokens, &evicted, &e);
    assert(s && strstr(s, "new system"));
    free(s);
    forge_context_destroy(c);
    c = forge_context_create(30, 10, count, NULL);
    forge_context_add(c, FORGE_SEG_SYSTEM, "This pinned text cannot possibly fit", 100, true, 0, 0);
    assert(!forge_context_plan(c, NULL, NULL, &e));
    assert(e.code == FORGE_ERR_LIMIT);
    forge_context_destroy(c);
    const char *j = "{\"path\":\"x\",\"start\":1,\"end\":2}";
    yyjson_doc *d = yyjson_read(j, strlen(j), 0);
    assert(d && fg_tool_validate("read_file", yyjson_doc_get_root(d), &e));
    uint64_t signature = fg_tool_signature("read_file", yyjson_doc_get_root(d), 4, 7);
    yyjson_doc_free(d);
    j = "{ \"end\": 2, \"path\": \"x\", \"start\": 1 }";
    d = yyjson_read(j, strlen(j), 0);
    assert(signature == fg_tool_signature("read_file", yyjson_doc_get_root(d), 4, 7));
    assert(signature != fg_tool_signature("read_file", yyjson_doc_get_root(d), 5, 7));
    assert(signature != fg_tool_signature("read_file", yyjson_doc_get_root(d), 4, 8));
    yyjson_doc_free(d);
    j = "{\"path\":\"x\",\"start\":0,\"end\":2}";
    d = yyjson_read(j, strlen(j), 0);
    assert(!fg_tool_validate("read_file", yyjson_doc_get_root(d), &e));
    yyjson_doc_free(d);
    j = "{\"path\":\"x\",\"start\":-1,\"end\":2}";
    d = yyjson_read(j, strlen(j), 0);
    assert(!fg_tool_validate("read_file", yyjson_doc_get_root(d), &e));
    yyjson_doc_free(d);
    s = fg_tool_grammar();
    assert(s && strstr(s, "call0") && strstr(s, "root ::="));
    free(s);
    const char *raw =
        "{\"Action\":\"pass\",\"Test\":\"TestOK\"}\n{\"Action\":\"fail\",\"Package\":\"example/"
        "math\",\"Test\":\"TestAdd\"}\n{\"Action\":\"output\",\"Output\":\"math_test.go:9: "
        "expected 4 got 3\\n\"}\n";
    const char *later =
        "{\"Time\":\"later\",\"Action\":\"output\",\"Output\":\"math_test.go:9: "
        "expected 4 got 3\\n\"}\n{\"Time\":\"later\",\"Elapsed\":42,\"Action\":\"fail\","
        "\"Test\":\"TestAdd\",\"Package\":\"example/math\"}\n";
    assert(fg_diagnostic_hash(raw) == fg_diagnostic_hash(later));
    assert(fg_diagnostic_hash(raw) != fg_diagnostic_hash("math_test.go:9: expected 5 got 3\n"));
    s = fg_compress_output(raw, 1024, NULL, &e);
    assert(s && strstr(s, "TestAdd") && strstr(s, "expected 4") && strstr(s, "pass=1"));
    free(s);
    raw = "exit_code=0\n{\"answer\":42}\nordinary stdout\n";
    s = fg_compress_output(raw, 256, NULL, &e);
    assert(s && !strcmp(s, raw));
    free(s);
    fg_buf_puts(&b, "exit_code=0\n");
    for (size_t i = 0; i < 100; i++)
        fg_buf_puts(&b, "verbose progress\n");
    fg_buf_puts(&b, "result at end\n");
    s = fg_compress_output(b.data, 256, NULL, &e);
    assert(s && strlen(s) <= 256 && strstr(s, "result at end"));
    free(s);
    fg_buf_clear(&b);
#ifndef _WIN32
    void (*old_handler)(int) = signal(SIGCHLD, SIG_IGN);
    const char *command[] = {"/bin/sh", "-c", "exit 0", NULL};
    fg_process_result result = {0};
    assert(fg_process(".", command, 1000, 4096, NULL, NULL, &result, &e) == FORGE_OK);
    signal(SIGCHLD, old_handler);
    assert(result.exit_code == -1);
    fg_process_free(&result);
#endif
    puts("core tests passed");
    return 0;
}
