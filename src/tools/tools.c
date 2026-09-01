#include "internal.h"
#include "forge/memory.h"
#include "forge/retrieval.h"
#include "core/digest.h"
#include "edit_journal.h"
#include "tree_sitter/api.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
extern const TSLanguage *tree_sitter_go(void);
static const fg_tool_def definitions[] = {
    {"apply_patch",
     "Replace one exact unique text span; empty old_text creates a new file. Parent directory must "
     "exist. old_text must match the CURRENT file content byte for byte. Both strings are JSON: "
     "write each line break as \\n and each tab as \\t; a replacement of several statements is one "
     "string containing \\n, not a single line. In Go, statements cannot share a line without a "
     "semicolon, so write each statement on its own line with \\n.",
     "path:string old_text:string new_text:string", NULL, FORGE_CAP_WRITE},
    {"apply_hunk",
     "Replace an inclusive 1-based line range in an existing text file. file_sha256 must equal "
     "the full-file anchor returned by read_file, so stale line numbers cannot edit changed "
     "bytes. new_text is the exact replacement, including any required final line break. Prefer "
     "this for narrow edits; use apply_patch to create files or replace a non-line text span.",
     "path:string start:line end:line file_sha256:string new_text:string", NULL, FORGE_CAP_WRITE},
    {"expand_output",
     "Read up to 8192 bytes of recorded UTF-8 tool text by id. A byte offset inside a character "
     "advances to the next character.",
     "id:uint offset:uint", NULL, FORGE_CAP_READ},
    {"find_symbol", "Inspect a Go symbol. depth 0=signature, 1=body, 2=neighbors, 3=file.",
     "name:string depth:uint", NULL, FORGE_CAP_READ},
    {"get_references", "Find Go identifier occurrences; these are syntactic, not type-resolved.",
     "name:string", NULL, FORGE_CAP_READ},
    /* Git may execute configured clean/process filters while inspecting the
     * worktree. Disabling external diff/textconv is not process isolation. */
    {"git_diff", "Inspect Git diff; configured filters require process authorization.", "", NULL,
     FORGE_CAP_PROCESS},
    {"git_status", "Inspect Git status; configured filters require process authorization.", "",
     NULL, FORGE_CAP_PROCESS},
    {"list_directory", "List indexed repository paths.", "", NULL, FORGE_CAP_READ},
    {"read_file",
     "Read inclusive lines: start>=1, end>=start, at most 2001 lines. The first output line is "
     "file_sha256:<hex>, an anchor for a later apply_hunk call.",
     "path:string start:line end:line", NULL, FORGE_CAP_READ},
    {"retrieve_context",
     "Retrieve indexed evidence: exact Go symbol, package imports, literal text, then full-text "
     "search. JSON includes source hashes, excerpts, stage order and explicit limits; graph is "
     "syntactic, not resolved calls/types.",
     "query:string", NULL, FORGE_CAP_READ},
    {"run_command", "Run an argv array without a shell. Requires explicit process authorization.",
     "argv:strings", NULL, FORGE_CAP_PROCESS},
    {"search_text", "Literal text search across indexed source files.", "query:string", NULL,
     FORGE_CAP_READ}};
const fg_tool_def *fg_tools(size_t *n) {
    if (n)
        *n = sizeof(definitions) / sizeof(*definitions);
    return definitions;
}
char *fg_tool_schema(bool thought, bool required, bool routed) {
    fg_buf b = {0};
    fg_buf_puts(&b, routed ? "Reason in plain text, then return exactly ONE JSON object: "
                           : "Return exactly ONE JSON object: ");
    fg_buf_puts(
        &b, "{\"tool\":\"NAME\",\"args\":{...}}, "
            "{\"memory\":{\"facts\":[],\"hypotheses\":[],\"decisions\":[],\"relevant_files\":[],"
            "\"remaining\":[]}}, or {\"final\":\"answer\"}. ");
    if (routed)
        fg_buf_printf(&b,
                      "The plain-text reasoning %s and is bounded to %u UTF-8 bytes. "
                      "Do not put a thought field inside the JSON and do not write JSON examples "
                      "before the real action. Once the action begins, emit nothing after it. The "
                      "host records the prefix as thought, never executes it, and grants it no "
                      "authority. ",
                      required ? "MUST be nonempty" : "is optional", FG_THOUGHT_MAX_BYTES);
    else if (thought && required)
        fg_buf_printf(&b,
                      "Every object MUST begin with a \"thought\" field: one free-text string of "
                      "reasoning, at most %u bytes, written before the action. Think there, then "
                      "act. Thought is recorded but never executed and grants no authority. ",
                      FG_THOUGHT_MAX_BYTES);
    else if (thought)
        fg_buf_printf(&b,
                      "Any object may begin with an optional \"thought\" field: one free-text "
                      "string of reasoning, at most %u bytes, written before the action. Thought "
                      "is recorded but never executed and grants no authority. ",
                      FG_THOUGHT_MAX_BYTES);
    fg_buf_puts(
        &b,
        "Memory fields are arrays of strings, "
        "at most 32 strings each, 512 bytes per string, 8192 bytes total; preserve useful facts "
        "when replacing memory. Model memory cannot mark host verification passed. "
        "A legacy {\"memory\":\"notes\"} is also accepted. Use fields in listed order. No "
        "markdown.\n");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        fg_buf_printf(&b, "%s(%s): %s\n", definitions[i].name, definitions[i].fields,
                      definitions[i].description);
    return fg_buf_take(&b);
}

static bool native_schema_type(fg_buf *out, const char *type) {
    if (!strcmp(type, "string"))
        return fg_buf_puts(out, "{\"type\":\"string\",\"maxLength\":2097152}");
    if (!strcmp(type, "strings"))
        return fg_buf_puts(out, "{\"type\":\"array\",\"items\":{\"type\":\"string\","
                                "\"maxLength\":8192},\"minItems\":1,\"maxItems\":64}");
    if (!strcmp(type, "line"))
        return fg_buf_puts(out, "{\"type\":\"integer\",\"minimum\":1,"
                                "\"maximum\":999999999}");
    if (!strcmp(type, "uint"))
        return fg_buf_puts(out, "{\"type\":\"integer\",\"minimum\":0,"
                                "\"maximum\":999999999}");
    return false;
}

static bool native_schema_function(fg_buf *out, const char *name, const char *description,
                                   const char *fields, bool comma) {
    char *qname = fg_json_string(name), *qdescription = fg_json_string(description);
    bool ok = qname && qdescription && (!comma || fg_buf_puts(out, ",")) &&
              fg_buf_printf(out,
                            "{\"type\":\"function\",\"function\":{\"name\":%s,"
                            "\"description\":%s,\"parameters\":{\"type\":\"object\","
                            "\"properties\":{",
                            qname, qdescription);
    free(qname);
    free(qdescription);
    if (!ok)
        return false;
    size_t count = 0;
    const char *cursor = fields;
    while (*cursor) {
        while (*cursor == ' ')
            cursor++;
        const char *end = strchr(cursor, ' ');
        if (!end)
            end = cursor + strlen(cursor);
        const char *colon = memchr(cursor, ':', (size_t)(end - cursor));
        if (!colon || colon == cursor || colon + 1 == end || (size_t)(colon - cursor) >= 64)
            return false;
        char field[64], type[32];
        size_t field_length = (size_t)(colon - cursor), type_length = (size_t)(end - colon - 1);
        if (type_length >= sizeof(type))
            return false;
        memcpy(field, cursor, field_length);
        field[field_length] = 0;
        memcpy(type, colon + 1, type_length);
        type[type_length] = 0;
        char *quoted = fg_json_string(field);
        ok = quoted && (!count || fg_buf_puts(out, ",")) && fg_buf_printf(out, "%s:", quoted) &&
             native_schema_type(out, type);
        free(quoted);
        if (!ok)
            return false;
        count++;
        cursor = end;
    }
    if (!fg_buf_puts(out, "},\"required\":["))
        return false;
    cursor = fields;
    size_t required = 0;
    while (*cursor) {
        while (*cursor == ' ')
            cursor++;
        const char *end = strchr(cursor, ' ');
        if (!end)
            end = cursor + strlen(cursor);
        const char *colon = memchr(cursor, ':', (size_t)(end - cursor));
        if (!colon || (size_t)(colon - cursor) >= 64)
            return false;
        char field[64];
        size_t length = (size_t)(colon - cursor);
        memcpy(field, cursor, length);
        field[length] = 0;
        char *quoted = fg_json_string(field);
        ok = quoted && (!required || fg_buf_puts(out, ",")) && fg_buf_puts(out, quoted);
        free(quoted);
        if (!ok)
            return false;
        required++;
        cursor = end;
    }
    return fg_buf_puts(out, "],\"additionalProperties\":false}}}");
}

char *fg_tool_native_schema(void) {
    fg_buf out = {0};
    if (!fg_buf_puts(&out, "["))
        return NULL;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!native_schema_function(&out, definitions[i].name, definitions[i].description,
                                    definitions[i].fields, i != 0))
            goto fail;
    if (!native_schema_function(
            &out, "final",
            "Finish the task. Call only after required validation; answer must accurately state "
            "what changed and what was tested.",
            "answer:string", true))
        goto fail;
    if (!fg_buf_puts(
            &out, ",{\"type\":\"function\",\"function\":{\"name\":\"memory\","
                  "\"description\":\"Replace bounded working memory while continuing the task.\","
                  "\"parameters\":{\"type\":\"object\",\"properties\":{"
                  "\"facts\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"maxLength\":512},"
                  "\"maxItems\":32},"
                  "\"hypotheses\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"maxLength\":"
                  "512},\"maxItems\":32},"
                  "\"decisions\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"maxLength\":"
                  "512},\"maxItems\":32},"
                  "\"relevant_files\":{\"type\":\"array\",\"items\":{\"type\":\"string\","
                  "\"maxLength\":512},\"maxItems\":32},"
                  "\"remaining\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"maxLength\":"
                  "512},\"maxItems\":32}},"
                  "\"required\":[\"facts\",\"hypotheses\",\"decisions\",\"relevant_files\","
                  "\"remaining\"],"
                  "\"additionalProperties\":false}}}]"))
        goto fail;
    return fg_buf_take(&out);
fail:
    fg_buf_clear(&out);
    return NULL;
}

static bool native_known_tool(const char *name) {
    size_t count = 0;
    const fg_tool_def *tools = fg_tools(&count);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(name, tools[i].name))
            return true;
    return !strcmp(name, "final") || !strcmp(name, "memory");
}

forge_status fg_native_action_normalize(const char *message, bool include_thought, char **action,
                                        forge_error *error) {
    if (!message || !action)
        return fg_error(error, FORGE_ERR_ARGUMENT, "Missing native action input");
    *action = NULL;
    size_t length = strlen(message);
    yyjson_doc *document = yyjson_read(message, length, 0);
    yyjson_val *root = document ? yyjson_doc_get_root(document) : NULL;
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE,
                        "Native response is not an assistant message object");
    }
    yyjson_val *role_value = yyjson_obj_get(root, "role");
    const char *role = yyjson_is_str(role_value) ? yyjson_get_str(role_value) : NULL;
    yyjson_val *content_value = yyjson_obj_get(root, "content");
    const char *content = yyjson_is_str(content_value) ? yyjson_get_str(content_value) : NULL;
    yyjson_val *calls = yyjson_obj_get(root, "tool_calls");
    if (!role || yyjson_get_len(role_value) != strlen(role) || strcmp(role, "assistant")) {
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE, "Native response role must be assistant");
    }
    if (!content_value || !content || yyjson_get_len(content_value) != strlen(content) ||
        *content) {
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE,
                        "Native response must use one tool call, not assistant content");
    }
    if (!calls || !yyjson_is_arr(calls) || yyjson_arr_size(calls) != 1) {
        yyjson_doc_free(document);
        return fg_error(
            error, FORGE_ERR_PARSE,
            "Native response must contain exactly one tool call; parallel calls are disabled");
    }
    yyjson_val *call = yyjson_arr_get(calls, 0);
    yyjson_val *type_value = call ? yyjson_obj_get(call, "type") : NULL;
    const char *type = yyjson_is_str(type_value) ? yyjson_get_str(type_value) : NULL;
    yyjson_val *function = call ? yyjson_obj_get(call, "function") : NULL;
    yyjson_val *name_value = function ? yyjson_obj_get(function, "name") : NULL;
    const char *name = yyjson_is_str(name_value) ? yyjson_get_str(name_value) : NULL;
    yyjson_val *encoded_arguments = function ? yyjson_obj_get(function, "arguments") : NULL;
    if (!call || !yyjson_is_obj(call) || !type || strcmp(type, "function") || !name ||
        !encoded_arguments || yyjson_get_len(type_value) != strlen(type) ||
        yyjson_get_len(name_value) != strlen(name)) {
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE,
                        "Native tool call requires type=function, name, and arguments");
    }
    yyjson_val *id_value = yyjson_obj_get(call, "id");
    const char *id = yyjson_is_str(id_value) ? yyjson_get_str(id_value) : NULL;
    size_t expected_call_fields = id_value ? 3 : 2;
    size_t expected_root_fields = yyjson_obj_get(root, "reasoning_content") ? 4 : 3;
    if (yyjson_obj_size(root) != expected_root_fields ||
        yyjson_obj_size(call) != expected_call_fields || yyjson_obj_size(function) != 2 ||
        (id_value && (!id || !*id || yyjson_get_len(id_value) != strlen(id)))) {
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE,
                        "Native assistant message or tool call contains malformed fields");
    }
    if (!native_known_tool(name)) {
        forge_status status = fg_error(error, FORGE_ERR_UNSUPPORTED,
                                       "Native response called unsupported tool: %s", name);
        yyjson_doc_free(document);
        return status;
    }
    yyjson_doc *arguments_document = NULL;
    yyjson_val *arguments = NULL;
    if (yyjson_is_str(encoded_arguments)) {
        const char *text = yyjson_get_str(encoded_arguments);
        size_t text_length = yyjson_get_len(encoded_arguments);
        if (!text || text_length != strlen(text)) {
            yyjson_doc_free(document);
            return fg_error(error, FORGE_ERR_PARSE,
                            "Native tool arguments contain an embedded NUL byte");
        }
        arguments_document = yyjson_read(text, text_length, 0);
        arguments = arguments_document ? yyjson_doc_get_root(arguments_document) : NULL;
    } else
        arguments = encoded_arguments;
    if (!arguments || !yyjson_is_obj(arguments)) {
        yyjson_doc_free(arguments_document);
        yyjson_doc_free(document);
        return fg_error(error, FORGE_ERR_PARSE,
                        "Native tool arguments must encode one JSON object");
    }
    if (strcmp(name, "final") && strcmp(name, "memory") &&
        !fg_tool_validate(name, arguments, error)) {
        yyjson_doc_free(arguments_document);
        yyjson_doc_free(document);
        return error && error->code ? error->code : FORGE_ERR_PARSE;
    }
    const char *thought = NULL;
    char *thought_copy = NULL;
    yyjson_val *thought_value = yyjson_obj_get(root, "reasoning_content");
    if (thought_value) {
        const char *parsed_thought =
            yyjson_is_str(thought_value) ? yyjson_get_str(thought_value) : NULL;
        size_t thought_length = yyjson_get_len(thought_value);
        if (!parsed_thought || thought_length != strlen(parsed_thought) ||
            !fg_utf8_valid(parsed_thought, thought_length)) {
            yyjson_doc_free(arguments_document);
            yyjson_doc_free(document);
            return fg_error(error, FORGE_ERR_PARSE, "Native reasoning content is invalid");
        }
        /* Like routed decode prose, native reasoning is non-executable text and
         * its full form is already retained in the raw model_output event. Keep
         * the normalized ACTION envelope under the identical thought limit by
         * retaining a valid UTF-8 prefix instead of rejecting an otherwise
         * valid, policy-checked tool call. */
        if (include_thought && thought_length) {
            size_t retained = fg_utf8_prefix(parsed_thought, thought_length, FG_THOUGHT_MAX_BYTES);
            thought_copy = malloc(retained + 1);
            if (!thought_copy) {
                yyjson_doc_free(arguments_document);
                yyjson_doc_free(document);
                return fg_error(error, FORGE_ERR_MEMORY, "Cannot retain native reasoning");
            }
            memcpy(thought_copy, parsed_thought, retained);
            thought_copy[retained] = 0;
            thought = thought_copy;
        }
    }
    char *arguments_json = yyjson_val_write(arguments, 0, NULL);
    char *thought_json = thought ? fg_json_string(thought) : NULL;
    fg_buf out = {0};
    bool ok = arguments_json && (!thought || thought_json) && fg_buf_puts(&out, "{");
    if (ok && thought)
        ok = fg_buf_printf(&out, "\"thought\":%s,", thought_json);
    if (ok && !strcmp(name, "final")) {
        const char *answer = fg_json_str(arguments, "answer");
        yyjson_val *answer_value = yyjson_obj_get(arguments, "answer");
        char *quoted = answer ? fg_json_string(answer) : NULL;
        ok = answer && answer_value && yyjson_obj_size(arguments) == 1 &&
             yyjson_get_len(answer_value) == strlen(answer) && quoted &&
             fg_buf_printf(&out, "\"final\":%s}", quoted);
        free(quoted);
        if (!ok)
            fg_error(error, FORGE_ERR_PARSE,
                     "Native final call requires exactly one string answer");
    } else if (ok && !strcmp(name, "memory"))
        ok = fg_buf_printf(&out, "\"memory\":%s}", arguments_json);
    else if (ok) {
        char *quoted = fg_json_string(name);
        ok = quoted && fg_buf_printf(&out, "\"tool\":%s,\"args\":%s}", quoted, arguments_json);
        free(quoted);
    }
    free(thought_json);
    free(thought_copy);
    free(arguments_json);
    yyjson_doc_free(arguments_document);
    yyjson_doc_free(document);
    if (!ok) {
        fg_buf_clear(&out);
        if (!error || !error->code)
            return fg_error(error, FORGE_ERR_MEMORY, "Cannot normalize native tool call");
        return error->code;
    }
    *action = fg_buf_take(&out);
    return *action ? FORGE_OK
                   : fg_error(error, FORGE_ERR_MEMORY, "Cannot allocate normalized native action");
}
static void literal(fg_buf *b, const char *s) {
    char *q = fg_json_string(s);
    if (!q)
        b->failed = true;
    else {
        fg_buf_puts(b, q);
        free(q);
    }
}
char *fg_tool_grammar(bool thought, bool required, bool routed) {
    fg_buf b = {0};
    bool inline_thought = thought && !routed;
    bool inline_required = inline_thought && required;
    fg_buf_puts(&b, "root ::= ws (final | memory");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        fg_buf_printf(&b, " | call%zu", i);
    fg_buf_puts(&b, ") ws\n");
    if (inline_thought) {
        fg_buf_puts(&b, "thought ::= ");
        literal(&b, "\"thought\":");
        fg_buf_puts(&b, " ws string ws ");
        literal(&b, ",");
        fg_buf_puts(&b, " ws\n");
    }
    fg_buf_puts(&b, "final ::= ");
    literal(&b, "{");
    fg_buf_puts(&b,
                !inline_thought ? " ws " : (inline_required ? " ws thought " : " ws thought? "));
    literal(&b, "\"final\":");
    fg_buf_puts(&b, " ws string ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\n");
    fg_buf_puts(&b, "memory ::= ");
    literal(&b, "{");
    fg_buf_puts(&b,
                !inline_thought ? " ws " : (inline_required ? " ws thought " : " ws thought? "));
    literal(&b, "\"memory\":");
    fg_buf_puts(&b, " ws (string | workingstate) ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\n");
    fg_buf_puts(&b, "workingstate ::= ");
    literal(&b, "{");
    const char *memory_fields[] = {"facts", "hypotheses", "decisions", "relevant_files",
                                   "remaining"};
    for (size_t i = 0; i < sizeof(memory_fields) / sizeof(*memory_fields); i++) {
        fg_buf field = {0};
        fg_buf_printf(&field, "%s\"%s\":", i ? "," : "", memory_fields[i]);
        fg_buf_puts(&b, " ws ");
        literal(&b, field.data);
        fg_buf_clear(&field);
        fg_buf_puts(&b, " ws memoryitems");
    }
    fg_buf_puts(&b, " ws ");
    literal(&b, "}");
    fg_buf_puts(&b, "\nmemoryitems ::= \"[\" ws (string (ws \",\" ws string){0,31})? ws \"]\"\n");
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        fg_buf_printf(&b, "call%zu ::= ", i);
        literal(&b, "{");
        fg_buf_puts(&b, !inline_thought ? " ws "
                                        : (inline_required ? " ws thought " : " ws thought? "));
        fg_buf prefix = {0};
        fg_buf_printf(&prefix, "\"tool\":\"%s\",\"args\":{", definitions[i].name);
        literal(&b, prefix.data);
        fg_buf_clear(&prefix);
        const char *p = definitions[i].fields;
        bool first = true;
        while (*p) {
            char key[32], type[32];
            size_t k = 0, t = 0;
            while (*p && *p != ':')
                key[k++] = *p++;
            key[k] = 0;
            if (*p)
                p++;
            while (*p && *p != ' ')
                type[t++] = *p++;
            type[t] = 0;
            if (*p)
                p++;
            fg_buf_puts(&b, " ws ");
            fg_buf keylit = {0};
            fg_buf_printf(&keylit, "%s\"%s\":", first ? "" : ",", key);
            literal(&b, keylit.data);
            fg_buf_clear(&keylit);
            fg_buf_printf(&b, " ws %s",
                          !strcmp(type, "line") ? "line"
                          : !strcmp(type, "uint")
                              ? "uint"
                              : (!strcmp(type, "strings") ? "strings" : "string"));
            first = false;
        }
        fg_buf_puts(&b, " ws ");
        literal(&b, "}}");
        fg_buf_puts(&b, "\n");
    }
    fg_buf_puts(&b,
                "ws ::= [ \\t\\n\\r]*\nline ::= [1-9] [0-9]{0,8}\nuint ::= \"0\" | line\nstrings "
                "::= \"[\" ws string "
                "(ws \",\" ws string){0,63} ws \"]\"\nstring ::= \"\\\"\" char* \"\\\"\"\nchar ::= "
                "[^\"\\\\\\x00-\\x1F] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]{4})\n");
    return fg_buf_take(&b);
}
bool fg_tool_validate(const char *name, yyjson_val *args, forge_error *e) {
    const fg_tool_def *def = NULL;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!strcmp(name, definitions[i].name))
            def = &definitions[i];
    if (!def || !yyjson_is_obj(args)) {
        fg_error(e, FORGE_ERR_PARSE, "Unknown tool or non-object arguments");
        return false;
    }
    const char *p = def->fields;
    size_t expected = 0;
    while (*p) {
        char key[32], type[32];
        size_t k = 0, t = 0;
        while (*p && *p != ':')
            key[k++] = *p++;
        key[k] = 0;
        if (*p)
            p++;
        while (*p && *p != ' ')
            type[t++] = *p++;
        type[t] = 0;
        if (*p)
            p++;
        expected++;
        yyjson_val *v = yyjson_obj_get(args, key);
        bool ok = false;
        if (!strcmp(type, "string"))
            ok = yyjson_is_str(v) && yyjson_get_len(v) == strlen(yyjson_get_str(v)) &&
                 yyjson_get_len(v) <= 2u * 1024u * 1024u;
        else if (!strcmp(type, "uint"))
            ok = yyjson_is_uint(v) && yyjson_get_uint(v) <= 999999999;
        else if (!strcmp(type, "line"))
            ok = yyjson_is_uint(v) && yyjson_get_uint(v) >= 1 && yyjson_get_uint(v) <= 999999999;
        else if (yyjson_is_arr(v) && yyjson_arr_size(v) > 0 && yyjson_arr_size(v) <= 64) {
            ok = true;
            size_t i, max;
            yyjson_val *item;
            yyjson_arr_foreach(v, i, max, item) {
                if (!yyjson_is_str(item) || yyjson_get_len(item) != strlen(yyjson_get_str(item)) ||
                    yyjson_get_len(item) > 8192)
                    ok = false;
            }
        }
        if (!ok) {
            fg_error(e, FORGE_ERR_PARSE, "Invalid or missing %s for %s", key, name);
            return false;
        }
    }
    if (yyjson_obj_size(args) != expected) {
        fg_error(e, FORGE_ERR_PARSE, "Unexpected or duplicate tool argument");
        return false;
    }
    return true;
}
uint64_t fg_tool_signature(const char *name, yyjson_val *args, uint64_t generation,
                           uint64_t diagnostic_hash) {
    fg_buf canonical = {0};
    fg_buf_printf(&canonical, "%s:%llu:%llu", name, (unsigned long long)generation,
                  (unsigned long long)diagnostic_hash);
    /* Registry field order makes JSON whitespace/key order irrelevant to loops. */
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++) {
        if (strcmp(name, definitions[i].name))
            continue;
        const char *field = definitions[i].fields;
        while (*field) {
            char key[32];
            size_t n = 0;
            while (*field && *field != ':')
                key[n++] = *field++;
            key[n] = 0;
            while (*field && *field != ' ')
                field++;
            if (*field)
                field++;
            char *value = yyjson_val_write(yyjson_obj_get(args, key), 0, NULL);
            if (!value)
                canonical.failed = true;
            fg_buf_printf(&canonical, "|%s=%s", key, value ? value : "null");
            free(value);
        }
        break;
    }
    uint64_t hash = canonical.failed ? 0 : fg_hash(canonical.data, canonical.len);
    fg_buf_clear(&canonical);
    return hash;
}
static char *read_lines(fg_tool_context *c, yyjson_val *args, forge_error *e) {
    char full[FG_PATH_MAX];
    const char *path = fg_json_str(args, "path");
    size_t start, end;
    fg_json_uint(args, "start", &start, 1);
    fg_json_uint(args, "end", &end, 200);
    if (!start || end < start || end - start > 2000) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid line range (maximum 2001 lines)");
        return NULL;
    }
    if (!fg_safe_path(c->root, path, false, full, e))
        return NULL;
    forge_file_view *view =
        forge_file_view_open(full, c->config.limits.max_file_bytes, FORGE_FILE_VIEW_READ, e);
    if (!view)
        return NULL;
    forge_slice file = forge_file_view_slice(view);
    if ((file.len && memchr(file.ptr, 0, file.len)) || !fg_utf8_valid(file.ptr, file.len)) {
        forge_file_view_close(view);
        fg_error(e, FORGE_ERR_PARSE, "Binary or invalid UTF-8 files are not supported");
        return NULL;
    }
    char sha256[65];
    if (!fg_sha256_hex(file.ptr, file.len, sha256)) {
        forge_file_view_close(view);
        fg_error(e, FORGE_ERR_MEMORY, "Cannot hash file view");
        return NULL;
    }
    size_t line = 1, offset = 0;
    fg_buf b = {0};
    fg_buf_printf(&b, "file_sha256:%s\n", sha256);
    while (offset < file.len && line <= end) {
        const char *p = file.ptr + offset;
        const char *z = memchr(p, '\n', file.len - offset);
        size_t n = z ? (size_t)(z - p) : file.len - offset;
        if (line >= start) {
            fg_buf_printf(&b, "%zu: ", line);
            fg_buf_add(&b, p, n);
            fg_buf_puts(&b, "\n");
        }
        if (!z)
            break;
        offset += n + 1;
        line++;
    }
    forge_file_view_close(view);
    return fg_buf_take(&b);
}

static int go_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool go_escape_valid(const char *text, size_t length, bool allow_string_hex_tail) {
    if (length < 2 || text[0] != '\\')
        return false;
    if (length == 2 && strchr("abfnrtv\\'\"", (unsigned char)text[1]))
        return true;
    size_t digits = 0;
    size_t offset = 0;
    unsigned base = 0;
    uint32_t value = 0;
    if (text[1] == 'x') {
        digits = 2;
        offset = 2;
        base = 16;
    } else if (text[1] == 'u') {
        digits = 4;
        offset = 2;
        base = 16;
    } else if (text[1] == 'U') {
        digits = 8;
        offset = 2;
        base = 16;
    } else {
        digits = 3;
        offset = 1;
        base = 8;
    }
    size_t required = digits + offset;
    if (length != required && !(text[1] == 'x' && allow_string_hex_tail && length > required))
        return false;
    for (size_t i = 0; i < digits; i++) {
        int digit = go_hex_digit((unsigned char)text[i + offset]);
        if (digit < 0 || (unsigned)digit >= base)
            return false;
        value = value * base + (unsigned)digit;
    }
    if (base == 8)
        return value <= 255;
    if (text[1] == 'x') {
        for (size_t i = required; i < length; i++)
            if (go_hex_digit((unsigned char)text[i]) < 0)
                return false;
        return true;
    }
    return value <= UINT32_C(0x10ffff) && !(value >= UINT32_C(0xd800) && value <= UINT32_C(0xdfff));
}

static bool go_single_rune(const char *text, size_t length) {
    if (!length || text[0] == '\n' || text[0] == '\r')
        return false;
    unsigned char first = (unsigned char)text[0];
    size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    return length == width;
}

static bool validate_go_lexemes(TSNode root, const char *text, size_t length, TSPoint *point,
                                const char **kind) {
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool valid = true, done = false;
    while (!done) {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        const char *type = ts_node_type(node);
        uint32_t first = ts_node_start_byte(node), last = ts_node_end_byte(node);
        if (last > length || first > last) {
            valid = false;
            *kind = "invalid token bounds";
        } else if (!strcmp(type, "escape_sequence") &&
                   !go_escape_valid(text + first, last - first, true)) {
            valid = false;
            *kind = "invalid escape sequence";
        } else if (!strcmp(type, "rune_literal")) {
            size_t token_length = last - first;
            const char *token = text + first;
            bool rune_valid =
                token_length >= 3 && token[0] == '\'' && token[token_length - 1] == '\'';
            if (rune_valid) {
                const char *body = token + 1;
                size_t body_length = token_length - 2;
                rune_valid = body[0] == '\\' ? go_escape_valid(body, body_length, false)
                                             : go_single_rune(body, body_length);
            }
            if (!rune_valid) {
                valid = false;
                *kind = "invalid rune literal";
            }
        }
        if (!valid) {
            *point = ts_node_start_point(node);
            break;
        }
        if (ts_tree_cursor_goto_first_child(&cursor))
            continue;
        while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
            if (!ts_tree_cursor_goto_parent(&cursor)) {
                done = true;
                break;
            }
        }
    }
    ts_tree_cursor_delete(&cursor);
    return valid;
}

static bool validate_go_candidate(const char *path, const char *text, size_t len, forge_error *e) {
    const char *ext = strrchr(path, '.');
    if (!ext || strcmp(ext, ".go"))
        return true;
    if (!fg_utf8_valid(text, len)) {
        fg_error(e, FORGE_ERR_PARSE,
                 "Proposed Go edit rejected before commit: source is not valid UTF-8");
        return false;
    }
    if (len > UINT32_MAX) {
        fg_error(e, FORGE_ERR_LIMIT, "Proposed Go file exceeds parser input limit");
        return false;
    }
    TSParser *parser = ts_parser_new();
    if (!parser) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate staged Go syntax parser");
        return false;
    }
    if (!ts_parser_set_language(parser, tree_sitter_go())) {
        ts_parser_delete(parser);
        fg_error(e, FORGE_ERR_PARSE, "Go parser ABI mismatch");
        return false;
    }
    TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)len);
    ts_parser_delete(parser);
    if (!tree) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot parse staged Go edit");
        return false;
    }
    TSNode root = ts_tree_root_node(tree);
    TSPoint structural_point = ts_node_start_point(root);
    const char *structural_kind = NULL;
    bool package_seen = false, declarations_started = false;
    uint32_t children = ts_node_named_child_count(root);
    for (uint32_t i = 0; i < children && !structural_kind; i++) {
        TSNode node = ts_node_named_child(root, i);
        const char *type = ts_node_type(node);
        if (!strcmp(type, "comment"))
            continue;
        if (!strcmp(type, "package_clause")) {
            TSNode name = {0};
            uint32_t names = 0, package_children = ts_node_named_child_count(node);
            for (uint32_t child_index = 0; child_index < package_children; child_index++) {
                TSNode child = ts_node_named_child(node, child_index);
                if (!strcmp(ts_node_type(child), "comment"))
                    continue;
                name = child;
                names++;
            }
            if (package_seen || declarations_started || names != 1) {
                structural_kind = "invalid package clause placement";
                structural_point = ts_node_start_point(node);
                break;
            }
            uint32_t first = ts_node_start_byte(name), last = ts_node_end_byte(name);
            if (last <= first || last > len || (last - first == 1 && text[first] == '_')) {
                structural_kind = "invalid package name";
                structural_point = ts_node_start_point(name);
                break;
            }
            package_seen = true;
            continue;
        }
        if (!strcmp(type, "import_declaration")) {
            if (!package_seen || declarations_started) {
                structural_kind = "import outside the package import section";
                structural_point = ts_node_start_point(node);
            }
            continue;
        }
        bool declaration = !strcmp(type, "const_declaration") || !strcmp(type, "var_declaration") ||
                           !strcmp(type, "type_declaration") ||
                           !strcmp(type, "function_declaration") ||
                           !strcmp(type, "method_declaration");
        if (!package_seen || !declaration) {
            structural_kind =
                package_seen ? "invalid top-level Go construct" : "missing leading package clause";
            structural_point = ts_node_start_point(node);
            break;
        }
        declarations_started = true;
    }
    if (!package_seen && !structural_kind)
        structural_kind = "missing package clause";
    TSPoint lexical_point = ts_node_start_point(root);
    const char *lexical_kind = NULL;
    bool lexically_valid =
        !structural_kind && validate_go_lexemes(root, text, len, &lexical_point, &lexical_kind);
    if (!ts_node_has_error(root) && !structural_kind && lexically_valid) {
        ts_tree_delete(tree);
        return true;
    }

    TSPoint point = structural_kind    ? structural_point
                    : !lexically_valid ? lexical_point
                                       : ts_node_start_point(root);
    const char *kind = structural_kind    ? structural_kind
                       : !lexically_valid ? lexical_kind
                                          : ts_node_type(root);
    if (!structural_kind && lexically_valid) {
        TSTreeCursor cursor = ts_tree_cursor_new(root);
        bool done = false;
        while (!done) {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_error(node) || ts_node_is_missing(node)) {
                point = ts_node_start_point(node);
                kind = ts_node_is_missing(node) ? "missing syntax" : ts_node_type(node);
                break;
            }
            if (ts_node_has_error(node) && ts_tree_cursor_goto_first_child(&cursor))
                continue;
            while (!ts_tree_cursor_goto_next_sibling(&cursor)) {
                if (!ts_tree_cursor_goto_parent(&cursor)) {
                    done = true;
                    break;
                }
            }
        }
        ts_tree_cursor_delete(&cursor);
    }
    ts_tree_delete(tree);
    fg_error(e, FORGE_ERR_PARSE,
             "Proposed Go edit rejected before commit: syntax error at %s:%u:%u (%s)", path,
             point.row + 1, point.column + 1, kind);
    return false;
}

static char *commit_edit(fg_tool_context *c, const char *path, char full[FG_PATH_MAX], bool exists,
                         const struct stat *st, char *text, size_t len, fg_buf *out, bool *changed,
                         forge_error *e) {
#ifdef _WIN32
    (void)st;
#endif
    char temp[FG_PATH_MAX], random[17];
    if (!fg_random_hex(random, 8) ||
        snprintf(temp, sizeof(temp), "%s.forge-%s.tmp", full, random) >= (int)sizeof(temp)) {
        free(text);
        fg_buf_clear(out);
        fg_error(e, FORGE_ERR_IO, "Cannot create patch staging path");
        return NULL;
    }
    FILE *f = fopen(temp, "wbx");
    if (!f) {
        free(text);
        fg_buf_clear(out);
        fg_error(e, FORGE_ERR_IO, "Cannot exclusively create patch staging file");
        return NULL;
    }
    bool ok = fwrite(out->data, 1, out->len, f) == out->len;
    if (fclose(f) != 0)
        ok = false;
#ifndef _WIN32
    if (ok && exists && chmod(temp, st->st_mode & 0777) != 0)
        ok = false;
#endif
    if (!ok) {
        free(text);
        fg_buf_clear(out);
        remove(temp);
        fg_error(e, FORGE_ERR_IO, "Cannot write patch staging file");
        return NULL;
    }
    fg_edit_record edit = {0};
    if (!fg_edit_prepare(c, path, exists, (forge_slice){text, len},
                         (forge_slice){out->data, out->len}, &edit, e)) {
        free(text);
        fg_buf_clear(out);
        remove(temp);
        return NULL;
    }

    forge_error syntax = {0};
    if (!validate_go_candidate(path, out->data, out->len, &syntax)) {
        remove(temp);
        forge_error recording = {0};
        if (!fg_edit_finish(c, &edit, false, syntax.code, &recording)) {
            if (e)
                *e = recording;
        } else if (e) {
            *e = syntax;
        }
        free(text);
        fg_buf_clear(out);
        return NULL;
    }

    /* Recheck the exact source before atomic replacement to catch ordinary concurrent edits. */
    if (exists) {
        size_t current_len = 0;
        char *current = fg_read_file(full, c->config.limits.max_file_bytes, &current_len, e);
        ok = current && current_len == len && !memcmp(current, text, len);
        free(current);
    }
    if (ok && !exists) {
        struct stat current;
        if (stat(full, &current) == 0)
            ok = false;
    }
    if (ok && !fg_safe_path(c->root, path, true, full, e))
        ok = false;
    if (ok && ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
               (c->deadline && fg_now_ms() >= c->deadline))) {
        fg_error(e, FORGE_ERR_CANCELLED, "Patch cancelled before target replacement");
        ok = false;
    }
#ifdef _WIN32
    if (ok)
        ok = MoveFileExA(temp, full,
                         exists ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                                : MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (ok)
        ok = rename(temp, full) == 0;
#endif
    free(text);
    fg_buf_clear(out);
    if (!ok) {
        remove(temp);
        if (!e || !e->code)
            fg_error(e, FORGE_ERR_CONFLICT, "Atomic patch failed or source changed concurrently");
        forge_status reason = e && e->code ? e->code : FORGE_ERR_CONFLICT;
        fg_edit_finish(c, &edit, false, reason, e);
        return NULL;
    }
    *changed = true;
    if (!fg_edit_finish(c, &edit, true, FORGE_OK, e))
        return NULL;
    fg_buf result = {0};
    fg_buf_printf(&result, "Patched %s.\nRecorded edit diff: %s\n", path, edit.diff);
    const char *ext = strrchr(path, '.');
    if (ext && !strcmp(ext, ".go")) {
        fg_buf_puts(&result, "Staged Go syntax validation passed before commit.\n");
        char *target = fg_repo_targets(c->repo, path, e);
        if (target) {
            fg_buf_printf(
                &result, "Suggested targeted validation (requires command approval): %s\n", target);
            free(target);
        }
    }
    return fg_buf_take(&result);
}

static char *patch(fg_tool_context *c, yyjson_val *args, bool *changed, forge_error *e) {
    const char *path = fg_json_str(args, "path"), *old = fg_json_str(args, "old_text"),
               *replacement = fg_json_str(args, "new_text");
    if (*old && !strcmp(old, replacement)) {
        fg_error(e, FORGE_ERR_CONFLICT,
                 "No edit performed: old_text and new_text are identical. Supply different "
                 "replacement text; encode line breaks as \\n when changing multiple statements.");
        return NULL;
    }
    char full[FG_PATH_MAX];
    if (!fg_safe_path(c->root, path, true, full, e))
        return NULL;
    struct stat st;
    bool exists = stat(full, &st) == 0;
    if (exists && (!*old || (st.st_mode & S_IFMT) != S_IFREG)) {
        fg_error(e, FORGE_ERR_CONFLICT,
                 "Empty old_text is only for creating a missing regular file");
        return NULL;
    }
    if (!exists && *old) {
        fg_error(e, FORGE_ERR_NOT_FOUND, "Patch target does not exist");
        return NULL;
    }
#ifdef _WIN32
    if (exists) {
        HANDLE h = CreateFileA(full, FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        bool linked = h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &info) ||
                      info.nNumberOfLinks > 1;
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (linked) {
            fg_error(e, FORGE_ERR_POLICY, "Refusing a hard-linked or inaccessible patch target");
            return NULL;
        }
    }
#endif
    size_t len = 0;
    char *text =
        exists ? fg_read_file(full, c->config.limits.max_file_bytes, &len, e) : fg_strdup("");
    if (!text)
        return NULL;
    if (memchr(text, 0, len)) {
        free(text);
        fg_error(e, FORGE_ERR_PARSE, "Cannot patch binary content");
        return NULL;
    }
    char *match = *old ? strstr(text, old) : text;
    if (!match || (*old && strstr(match + 1, old))) {
        /* Telling the caller to re-read the file is not a usable recovery: once
         * the repository and the last diagnostic are unchanged, a repeated read
         * is rejected as a repeated action, so the agent is instructed to do the
         * one thing it is then blocked from doing. Return the current text so
         * the next old_text can be copied from it instead. */
        fg_buf report = {0};
        fg_buf_printf(&report,
                      "TOOL_ERROR [conflict]: old_text must match exactly once in %s. It does "
                      "not match the current content; the file may have changed since you last "
                      "read it. Current content follows. Copy the text you intend to replace "
                      "from it verbatim, including only bytes that are present.\n",
                      path);
        size_t show = len > 8192 ? 8192 : len;
        fg_buf_add(&report, text, show);
        if (len > show)
            fg_buf_puts(&report, "\n[truncated]");
        fg_error(e, FORGE_ERR_CONFLICT, "old_text must match exactly once in %s", path);
        free(text);
        if (report.failed) {
            fg_buf_clear(&report);
            return NULL;
        }
        return fg_buf_take(&report);
    }
    fg_buf out = {0};
    size_t prefix = (size_t)(match - text);
    fg_buf_add(&out, text, prefix);
    fg_buf_puts(&out, replacement);
    fg_buf_puts(&out, match + strlen(old));
    if (out.failed || out.len > c->config.limits.max_file_bytes) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_LIMIT, "Patched file exceeds file budget");
        return NULL;
    }
    if (exists && out.len == len && !memcmp(out.data, text, len)) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_CONFLICT,
                 "No edit performed: old_text and new_text are identical. Supply different "
                 "replacement text; encode line breaks as \\n when changing multiple statements.");
        return NULL;
    }
    return commit_edit(c, path, full, exists, &st, text, len, &out, changed, e);
}

static bool line_span(const char *text, size_t len, size_t start, size_t end, size_t *first,
                      size_t *last) {
    if (!len && start == 1 && end == 1) {
        *first = *last = 0;
        return true;
    }
    size_t line = 1, offset = 0;
    bool found = false;
    while (offset < len) {
        const char *newline = memchr(text + offset, '\n', len - offset);
        size_t next = newline ? (size_t)(newline - text) + 1 : len;
        if (line == start) {
            *first = offset;
            found = true;
        }
        if (line == end) {
            if (!found)
                return false;
            *last = next;
            return true;
        }
        if (!newline)
            break;
        offset = next;
        line++;
    }
    return false;
}

static char *hunk(fg_tool_context *c, yyjson_val *args, bool *changed, forge_error *e) {
    const char *path = fg_json_str(args, "path");
    const char *anchor = fg_json_str(args, "file_sha256");
    const char *replacement = fg_json_str(args, "new_text");
    size_t start, end;
    fg_json_uint(args, "start", &start, 0);
    fg_json_uint(args, "end", &end, 0);
    if (!start || end < start || end - start > 2000) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Invalid hunk line range (maximum 2001 lines)");
        return NULL;
    }
    if (!fg_sha256_valid_hex(anchor)) {
        fg_error(e, FORGE_ERR_ARGUMENT, "file_sha256 must be 64 lowercase hexadecimal digits");
        return NULL;
    }
    char full[FG_PATH_MAX];
    if (!fg_safe_path(c->root, path, true, full, e))
        return NULL;
    struct stat st;
    if (stat(full, &st) != 0) {
        fg_error(e, FORGE_ERR_NOT_FOUND, "Hunk target does not exist");
        return NULL;
    }
    if ((st.st_mode & S_IFMT) != S_IFREG) {
        fg_error(e, FORGE_ERR_CONFLICT, "Hunk target is not a regular file");
        return NULL;
    }
#ifdef _WIN32
    HANDLE handle = CreateFileA(full, FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    bool linked = handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
                  info.nNumberOfLinks > 1;
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    if (linked) {
        fg_error(e, FORGE_ERR_POLICY, "Refusing a hard-linked or inaccessible hunk target");
        return NULL;
    }
#endif
    size_t len = 0;
    char *text = fg_read_file(full, c->config.limits.max_file_bytes, &len, e);
    if (!text)
        return NULL;
    if ((len && memchr(text, 0, len)) || !fg_utf8_valid(text, len)) {
        free(text);
        fg_error(e, FORGE_ERR_PARSE, "Cannot apply a line hunk to binary or invalid UTF-8 content");
        return NULL;
    }
    char current[65];
    if (!fg_sha256_hex(text, len, current)) {
        free(text);
        fg_error(e, FORGE_ERR_MEMORY, "Cannot hash hunk target");
        return NULL;
    }
    if (strcmp(anchor, current)) {
        fg_buf report = {0};
        fg_buf_printf(&report,
                      "TOOL_ERROR [conflict]: stale file_sha256 for %s; current file_sha256 is "
                      "%s. Current selected content follows; use this hash for a revised hunk.\n",
                      path, current);
        size_t first = 0, last = 0;
        if (line_span(text, len, start, end, &first, &last)) {
            size_t show = FG_MIN(last - first, (size_t)8192);
            fg_buf_add(&report, text + first, show);
            if (last - first > show)
                fg_buf_puts(&report, "\n[truncated]");
        } else
            fg_buf_puts(&report, "[requested line range is no longer present]\n");
        free(text);
        fg_error(e, FORGE_ERR_CONFLICT, "Stale file_sha256 for %s", path);
        if (report.failed) {
            fg_buf_clear(&report);
            return NULL;
        }
        return fg_buf_take(&report);
    }
    size_t first = 0, last = 0;
    if (!line_span(text, len, start, end, &first, &last)) {
        free(text);
        fg_error(e, FORGE_ERR_ARGUMENT, "Hunk line range is outside the current file");
        return NULL;
    }
    fg_buf out = {0};
    fg_buf_add(&out, text, first);
    fg_buf_puts(&out, replacement);
    fg_buf_add(&out, text + last, len - last);
    if (out.failed || out.len > c->config.limits.max_file_bytes) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_LIMIT, "Patched file exceeds file budget");
        return NULL;
    }
    if (out.len == len && !memcmp(out.data, text, len)) {
        free(text);
        fg_buf_clear(&out);
        fg_error(e, FORGE_ERR_CONFLICT,
                 "No hunk edit performed: new_text is identical to the selected line span");
        return NULL;
    }
    return commit_edit(c, path, full, true, &st, text, len, &out, changed, e);
}

static char *run(fg_tool_context *c, const char *const *argv, forge_error *e) {
    fg_process_result r = {0};
    uint64_t now = fg_now_ms();
    if ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
        (c->deadline && now >= c->deadline)) {
        fg_error(e, FORGE_ERR_CANCELLED, "Command cancelled or absolute deadline reached");
        return NULL;
    }
    uint64_t left = c->deadline ? c->deadline - now : c->config.limits.command_timeout_ms;
    forge_status status =
        fg_process(c->root, argv, FG_MIN(left, c->config.limits.command_timeout_ms),
                   c->config.limits.max_tool_bytes, c->config.cancelled, c->config.userdata, &r, e);
    /* A launched command may have changed files even if capture later fails. */
    c->process_ran = r.started;
    c->process = r;
    c->process.out = c->process.err = NULL;
    if (status != FORGE_OK) {
        fg_process_free(&r);
        return NULL;
    }
    char artifact[64];
    snprintf(artifact, sizeof(artifact), "tool/%06zu.stdout", c->call_id);
    if (!fg_session_artifact_bytes(c->session, artifact, r.out, r.out_len, e)) {
        fg_process_free(&r);
        return NULL;
    }
    snprintf(artifact, sizeof(artifact), "tool/%06zu.stderr", c->call_id);
    if (!fg_session_artifact_bytes(c->session, artifact, r.err, r.err_len, e)) {
        fg_process_free(&r);
        return NULL;
    }
    char *result = fg_process_render(&r);
    fg_process_free(&r);
    return result;
}
char *fg_tool_execute(fg_tool_context *c, const char *name, yyjson_val *args, bool *changed,
                      forge_error *e) {
    *changed = false;
    c->process_ran = false;
    c->evidence_failed = false;
    memset(&c->process, 0, sizeof(c->process));
    if (!fg_tool_validate(name, args, e))
        return NULL;
    if (!strcmp(name, "apply_patch")) {
        const char *old = fg_json_str(args, "old_text");
        const char *replacement = fg_json_str(args, "new_text");
        /* File creation with two empty strings still changes directory state;
         * every byte-level no-op is rejected before policy or filesystem work. */
        if (old && *old && replacement && !strcmp(old, replacement)) {
            fg_error(
                e, FORGE_ERR_CONFLICT,
                "No edit performed: old_text and new_text are identical. Supply different "
                "replacement text; encode line breaks as \\n when changing multiple statements.");
            return NULL;
        }
    }
    const fg_tool_def *def = NULL;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(*definitions); i++)
        if (!strcmp(name, definitions[i].name))
            def = &definitions[i];
    bool allowed = def->capability == FORGE_CAP_READ ||
                   (def->capability == FORGE_CAP_WRITE && c->config.allow_write) ||
                   (def->capability == FORGE_CAP_PROCESS && c->config.allow_exec);
    if (c->config.policy) {
        char *json = yyjson_val_write(args, 0, NULL);
        allowed = c->config.policy(name, def->capability, json ? json : "{}", c->config.userdata);
        free(json);
    }
    if (!allowed) {
        fg_error(e, FORGE_ERR_POLICY, "%s denied: explicit %s approval is required", name,
                 def->capability == FORGE_CAP_WRITE     ? "write"
                 : def->capability == FORGE_CAP_PROCESS ? "unsandboxed process"
                                                        : "read");
        return NULL;
    }
    if ((c->config.cancelled && c->config.cancelled(c->config.userdata)) ||
        (c->deadline && fg_now_ms() >= c->deadline)) {
        fg_error(e, FORGE_ERR_CANCELLED,
                 "Tool cancelled or deadline reached after policy approval");
        return NULL;
    }
    if (!strcmp(name, "read_file"))
        return read_lines(c, args, e);
    if (!strcmp(name, "apply_patch"))
        return patch(c, args, changed, e);
    if (!strcmp(name, "apply_hunk"))
        return hunk(c, args, changed, e);
    if (!strcmp(name, "find_symbol")) {
        size_t depth;
        fg_json_uint(args, "depth", &depth, 0);
        return forge_repo_inspect(c->repo, fg_json_str(args, "name"), (int)depth, e);
    }
    if (!strcmp(name, "get_references"))
        return forge_repo_references(c->repo, fg_json_str(args, "name"), e);
    if (!strcmp(name, "search_text"))
        return fg_repo_search(c->repo, fg_json_str(args, "query"), 50, e);
    if (!strcmp(name, "retrieve_context")) {
        forge_retrieval_options options = forge_default_retrieval_options();
        options.max_output_bytes =
            FG_MIN(options.max_output_bytes, c->config.limits.max_tool_bytes);
        options.deadline_ms = c->deadline;
        options.cancelled = c->config.cancelled;
        options.userdata = c->config.userdata;
        if (c->config.model && c->config.model->count) {
            options.count_tokens = fg_model_count;
            options.count_userdata = c->config.model;
            options.max_output_tokens = c->config.limits.context_tokens / 4;
        }
        return forge_repo_retrieve(c->repo, fg_json_str(args, "query"), &options, NULL, e);
    }
    if (!strcmp(name, "list_directory"))
        return forge_repo_summary(c->repo, e);
    if (!strcmp(name, "expand_output")) {
        size_t id, offset;
        fg_json_uint(args, "id", &id, 0);
        fg_json_uint(args, "offset", &offset, 0);
        if (!id || id >= c->call_id) {
            fg_error(e, FORGE_ERR_ARGUMENT, "Unknown prior tool output id");
            return NULL;
        }
        char file[64], path[FG_PATH_MAX];
        snprintf(file, sizeof(file), "tool/%06zu.raw", id);
        fg_path_join(path, c->session->dir, file);
        size_t len;
        char *raw = fg_read_file(path, c->config.limits.max_tool_bytes * 8 + 1024, &len, e);
        if (!raw)
            return NULL;
        if (memchr(raw, 0, len) || !fg_utf8_valid(raw, len)) {
            free(raw);
            fg_error(e, FORGE_ERR_PARSE,
                     "Recorded output is not valid UTF-8 text; inspect the exact stream artifacts");
            return NULL;
        }
        if (offset > len) {
            free(raw);
            fg_error(e, FORGE_ERR_ARGUMENT, "Output offset out of range");
            return NULL;
        }
        fg_buf b = {0};
        offset = fg_utf8_forward(raw, len, offset);
        size_t take = fg_utf8_prefix(raw + offset, len - offset, 8192);
        fg_buf_add(&b, raw + offset, take);
        free(raw);
        return fg_buf_take(&b);
    }
    if (!strcmp(name, "git_diff")) {
        const char *v[] = {
            "git", "-c", "core.fsmonitor=false", "diff", "--no-ext-diff", "--no-textconv",
            "--",  NULL};
        return run(c, v, e);
    }
    if (!strcmp(name, "git_status")) {
        const char *v[] = {"git",
                           "-c",
                           "core.fsmonitor=false",
                           "status",
                           "--porcelain=v1",
                           "--untracked-files=normal",
                           NULL};
        return run(c, v, e);
    }
    if (!strcmp(name, "run_command")) {
        yyjson_val *arr = yyjson_obj_get(args, "argv");
        const char *v[65] = {0};
        size_t i, n;
        yyjson_val *item;
        yyjson_arr_foreach(arr, i, n, item) v[i] = yyjson_get_str(item);
        return run(c, v, e);
    }
    fg_error(e, FORGE_ERR_ARGUMENT, "Unimplemented tool");
    return NULL;
}
