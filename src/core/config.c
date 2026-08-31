#include "internal.h"
#include "forge/config.h"
#include "tomlc17.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

typedef struct {
    char paths[FORGE_CONFIG_MAX_INHERITANCE][FG_PATH_MAX];
    size_t depth;
} config_chain;

static void clear_error(forge_error *e) {
    if (e) {
        e->code = FORGE_OK;
        e->message[0] = 0;
    }
}

void forge_config_init(forge_config *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->model = forge_default_model_config();
    config->limits = forge_default_limits();
    config->checkpoint_cache = forge_default_checkpoint_cache_options();
    config->semantic_output = true;
    config->thought = true;
    config->thought_required = false;
    /* Measured: retaining an elicited thought in the stored ACTION segment
     * costs accuracy and prompt tokens with no evidence benefit, because the
     * raw response is already persisted to the session log before the strip
     * (15/15 discordant replicate pairs favor stripping, sign test p=6.1e-05;
     * benchmark/results/2026-08-30-elicited-sweep). --thought-history opts in. */
    config->thought_in_history = false;
    config->thought_routed = false;
    config->thought_native = false;
    config->compact_context = true;
}

void forge_config_destroy(forge_config *config) {
    if (!config)
        return;
    free(config->_owned_model_path);
    free(config->_owned_script_path);
    free(config->_owned_chat_template);
    memset(config, 0, sizeof(*config));
}

static forge_status config_copy(forge_config *dst, const forge_config *src, forge_error *e) {
    *dst = *src;
    dst->_owned_model_path = fg_strdup(src->model.model_path);
    dst->_owned_script_path = fg_strdup(src->model.script_path);
    dst->_owned_chat_template = fg_strdup(src->model.chat_template);
    if ((src->model.model_path && !dst->_owned_model_path) ||
        (src->model.script_path && !dst->_owned_script_path) ||
        (src->model.chat_template && !dst->_owned_chat_template)) {
        forge_config_destroy(dst);
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot copy configuration strings");
    }
    dst->model.model_path = dst->_owned_model_path;
    dst->model.script_path = dst->_owned_script_path;
    dst->model.chat_template = dst->_owned_chat_template;
    return FORGE_OK;
}

/* tomlc17's optional UTF-8 check is process-global. Validate input here instead
 * of changing parser options and introducing races with other parser users. */
static bool valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i++];
        if (c == 0)
            return false;
        if (c < 0x80)
            continue;
        unsigned count, code, minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            code = c & 0x1fu;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            code = c & 0x0fu;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            code = c & 7u;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (n - i < count)
            return false;
        for (unsigned k = 0; k < count; k++) {
            unsigned next = s[i++];
            if ((next & 0xc0u) != 0x80u)
                return false;
            code = (code << 6) | (next & 0x3fu);
        }
        if (code < minimum || code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu))
            return false;
    }
    return true;
}

static bool path_text_valid(const char *path) {
    if (!path || !*path || strlen(path) >= FG_PATH_MAX)
        return false;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++)
        if (*p < 32 || *p == 127)
            return false;
    return true;
}

static forge_status absolute_path(const char *base, const char *path, char out[FG_PATH_MAX],
                                  forge_error *e) {
    if (!path_text_valid(path))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Path must be nonempty, contain no control "
                        "characters, and be shorter than %d bytes",
                        FG_PATH_MAX);
    char cwd[FG_PATH_MAX], joined[FG_PATH_MAX];
    if (!base) {
#ifdef _WIN32
        if (!_getcwd(cwd, sizeof(cwd)))
#else
        if (!getcwd(cwd, sizeof(cwd)))
#endif
            return fg_error(e, FORGE_ERR_IO, "Cannot determine current directory");
        base = cwd;
    }
#ifdef _WIN32
    bool drive = isalpha((unsigned char)path[0]) && path[1] == ':';
    bool unc = (path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/');
    if ((drive && path[2] != '/' && path[2] != '\\') ||
        (!unc && (path[0] == '/' || path[0] == '\\')))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Use a full drive/UNC path or a relative path, not a drive-relative "
                        "or root-relative Windows path: %s",
                        path);
    if (drive || unc) {
        strcpy(joined, path);
    } else if (!fg_path_join(joined, base, path)) {
        return fg_error(e, FORGE_ERR_LIMIT, "Resolved path is too long: %s", path);
    }
    if (!_fullpath(out, joined, FG_PATH_MAX))
        return fg_error(e, FORGE_ERR_IO, "Cannot resolve path: %s", path);
#else
    if (path[0] == '/') {
        strcpy(joined, path);
    } else if (!fg_path_join(joined, base, path)) {
        return fg_error(e, FORGE_ERR_LIMIT, "Resolved path is too long: %s", path);
    }
    /* Lexical normalization permits a model file that has not been installed.
     * Configuration source paths are likewise independent of target existence. */
    size_t used = 1;
    out[0] = '/';
    out[1] = 0;
    const char *p = joined;
    while (*p) {
        while (*p == '/')
            p++;
        const char *begin = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - begin);
        if (!n || (n == 1 && begin[0] == '.'))
            continue;
        if (n == 2 && begin[0] == '.' && begin[1] == '.') {
            while (used > 1 && out[used - 1] != '/')
                used--;
            if (used > 1)
                used--;
            out[used] = 0;
            continue;
        }
        if (used + n + 2 > FG_PATH_MAX)
            return fg_error(e, FORGE_ERR_LIMIT, "Resolved path is too long: %s", path);
        if (used > 1)
            out[used++] = '/';
        memcpy(out + used, begin, n);
        used += n;
        out[used] = 0;
    }
#endif
    return FORGE_OK;
}

static void parent_path(const char *path, char out[FG_PATH_MAX]) {
    strcpy(out, path);
    size_t n = strlen(out);
    while (n && out[n - 1] != '/'
#ifdef _WIN32
           && out[n - 1] != '\\'
#endif
    )
        n--;
    /* Keep the trailing separator so both C:\\ and / remain absolute. */
    out[n] = 0;
}

static bool same_path(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static forge_status schema_error(forge_error *e, toml_datum_t value, const char *key,
                                 const char *message) {
    return fg_error(e, FORGE_ERR_PARSE, "%s:%d:%d: %s: %s",
                    value.source ? value.source : "<config>", value.lineno, value.colno, key,
                    message);
}

static bool string_valid(toml_datum_t value, size_t max_length) {
    return value.type == TOML_STRING && value.u.str.len > 0 &&
           (size_t)value.u.str.len <= max_length &&
           strlen(value.u.str.ptr) == (size_t)value.u.str.len;
}

typedef enum {
    CFG_SIZE,
    CFG_U64,
    CFG_INT,
    CFG_U32,
    CFG_BOOL,
    CFG_FLOAT,
    CFG_CONTEXT,
    CFG_PATH,
    CFG_TEMPLATE,
    CFG_GPU,
    CFG_TIMEOUT,
    CFG_NETWORK,
    CFG_SPECULATIVE,
    CFG_LANGUAGES,
    CFG_THINKING
} config_kind;

typedef struct {
    const char *table, *key;
    config_kind kind;
    size_t offset;
    uint64_t minimum, maximum;
} config_field;

#define MODEL_OFFSET(member) (offsetof(forge_config, model) + offsetof(forge_model_config, member))
#define LIMIT_OFFSET(member) (offsetof(forge_config, limits) + offsetof(forge_limits, member))
#define CACHE_OFFSET(member)                                                                       \
    (offsetof(forge_config, checkpoint_cache) + offsetof(forge_checkpoint_cache_options, member))
static const config_field fields[] = {
    {"model", "path", CFG_PATH, 0, 0, 0},
    {"model", "chat_template", CFG_TEMPLATE, 0, 0, 0},
    {"model", "enable_thinking", CFG_THINKING, 0, 0, 0},
    {"model", "context", CFG_CONTEXT, 0, 128, 1048576},
    {"inference", "gpu_layers", CFG_GPU, 0, 0, 65535},
    {"inference", "threads", CFG_INT, MODEL_OFFSET(threads), 0, 1024},
    {"inference", "seed", CFG_U32, MODEL_OFFSET(seed), 0, UINT32_MAX},
    {"inference", "temperature", CFG_FLOAT, MODEL_OFFSET(temperature), 0, 2},
    {"inference", "reuse_prefix", CFG_BOOL, MODEL_OFFSET(reuse_prefix), 0, 0},
    {"inference", "grammar_fast_path", CFG_BOOL, MODEL_OFFSET(grammar_fast_path), 0, 0},
    {"inference", "speculative", CFG_SPECULATIVE, 0, 0, 0},
    {"inference.checkpoints", "enabled", CFG_BOOL, offsetof(forge_config, checkpoint_cache_enabled),
     0, 0},
    {"inference.checkpoints", "max_bytes", CFG_SIZE, CACHE_OFFSET(max_bytes), 4096,
     FORGE_CHECKPOINT_CACHE_MAX_BYTES},
    {"inference.checkpoints", "max_entries", CFG_SIZE, CACHE_OFFSET(max_entries), 1,
     FORGE_CHECKPOINT_CACHE_MAX_ENTRIES},
    {"inference.checkpoints", "min_prefix_tokens", CFG_SIZE, CACHE_OFFSET(min_prefix_tokens), 1,
     1048576},
    {"inference.checkpoints", "max_captures_per_prompt", CFG_SIZE,
     CACHE_OFFSET(max_captures_per_prompt), 1, FORGE_CHECKPOINT_CACHE_MAX_ANCHORS},
    {"agent", "output_reserve", CFG_SIZE, LIMIT_OFFSET(output_reserve), 1, 1048575},
    {"agent", "max_turns", CFG_SIZE, LIMIT_OFFSET(max_turns), 1, 1000},
    {"agent", "max_tokens", CFG_SIZE, LIMIT_OFFSET(max_generated_tokens), 1, INT32_MAX},
    {"agent", "max_input", CFG_SIZE, LIMIT_OFFSET(max_input_tokens), 1, INT32_MAX},
    {"agent", "max_tool_bytes", CFG_SIZE, LIMIT_OFFSET(max_tool_bytes), 1, FG_MAX_JSON},
    {"agent", "max_file_bytes", CFG_SIZE, LIMIT_OFFSET(max_file_bytes), 1, FG_MAX_JSON},
    {"agent", "wall_timeout_ms", CFG_U64, LIMIT_OFFSET(wall_timeout_ms), 1, 604800000},
    {"agent", "semantic_output", CFG_BOOL, offsetof(forge_config, semantic_output), 0, 0},
    {"agent", "compact_context", CFG_BOOL, offsetof(forge_config, compact_context), 0, 0},
    {"tools.shell", "timeout", CFG_TIMEOUT, 0, 1, 86400},
    {"tools.shell", "network", CFG_NETWORK, 0, 0, 0},
    {"index", "languages", CFG_LANGUAGES, 0, 0, 0},
};
#undef MODEL_OFFSET
#undef LIMIT_OFFSET
#undef CACHE_OFFSET

static forge_status apply_field(forge_config *config, const config_field *field, toml_datum_t value,
                                const char *source, forge_error *e) {
    char key[128];
    snprintf(key, sizeof(key), "%s.%s", field->table, field->key);
    unsigned char *dest = (unsigned char *)config + field->offset;
    switch (field->kind) {
    case CFG_PATH:
    case CFG_TEMPLATE: {
        size_t limit = field->kind == CFG_PATH ? FG_PATH_MAX - 1 : 65536;
        if (!string_valid(value, limit))
            return schema_error(e, value, key,
                                "expected a nonempty string without embedded "
                                "NUL bytes, within the supported length");
        char resolved[FG_PATH_MAX], base[FG_PATH_MAX];
        const char *text = value.u.str.ptr;
        if (field->kind == CFG_PATH) {
            parent_path(source, base);
            forge_error path_error = {0};
            if (absolute_path(base, text, resolved, &path_error) != FORGE_OK)
                return schema_error(e, value, key, path_error.message);
            text = resolved;
        }
        char *owned = fg_strdup(text);
        if (!owned)
            return fg_error(e, FORGE_ERR_MEMORY, "%s: Cannot allocate %s", source, key);
        if (field->kind == CFG_PATH) {
            free(config->_owned_model_path);
            config->_owned_model_path = owned;
            config->model.model_path = owned;
        } else {
            free(config->_owned_chat_template);
            config->_owned_chat_template = owned;
            config->model.chat_template = owned;
        }
        return FORGE_OK;
    }
    case CFG_BOOL:
    case CFG_NETWORK:
    case CFG_SPECULATIVE:
    case CFG_THINKING:
        if (value.type != TOML_BOOLEAN)
            return schema_error(e, value, key, "expected a boolean");
        if (field->kind == CFG_NETWORK) {
            config->shell_network =
                value.u.boolean ? FORGE_SHELL_NETWORK_ALLOW : FORGE_SHELL_NETWORK_DENY;
        } else if (field->kind == CFG_SPECULATIVE) {
            if (value.u.boolean)
                return schema_error(e, value, key,
                                    "speculative decoding is not implemented; "
                                    "only false is supported");
        } else if (field->kind == CFG_THINKING) {
            config->model.thinking = value.u.boolean ? FORGE_THINKING_ENABLED
                                                     : FORGE_THINKING_DISABLED;
        } else {
            *(bool *)dest = value.u.boolean;
        }
        return FORGE_OK;
    case CFG_GPU:
        if (string_valid(value, 4) && !strcmp(value.u.str.ptr, "auto")) {
            config->model.gpu_layers = FORGE_GPU_LAYERS_AUTO;
            return FORGE_OK;
        }
        if (value.type != TOML_INT64 || value.u.int64 < -1 || value.u.int64 > 65535)
            return schema_error(e, value, key, "expected \"auto\" or an integer in [-1, 65535]");
        config->model.gpu_layers = (int)value.u.int64;
        return FORGE_OK;
    case CFG_FLOAT: {
        double number;
        if (value.type == TOML_FP64)
            number = value.u.fp64;
        else if (value.type == TOML_INT64)
            number = (double)value.u.int64;
        else
            return schema_error(e, value, key, "expected a finite number in [0, 2]");
        if (!isfinite(number) || number < 0 || number > 2)
            return schema_error(e, value, key, "expected a finite number in [0, 2]");
        *(float *)dest = (float)number;
        return FORGE_OK;
    }
    case CFG_LANGUAGES:
        if (value.type != TOML_ARRAY || value.u.arr.size != 1 ||
            !string_valid(value.u.arr.elem[0], 2) || strcmp(value.u.arr.elem[0].u.str.ptr, "go"))
            return schema_error(e, value, key,
                                "only [\"go\"] is supported by the current "
                                "repository index; other languages are not implemented");
        return FORGE_OK;
    default:
        break;
    }
    if (value.type != TOML_INT64 || value.u.int64 < 0 || (uint64_t)value.u.int64 < field->minimum ||
        (uint64_t)value.u.int64 > field->maximum) {
        char message[160];
        snprintf(message, sizeof(message), "expected an integer in [%" PRIu64 ", %" PRIu64 "]",
                 field->minimum, field->maximum);
        return schema_error(e, value, key, message);
    }
    uint64_t number = (uint64_t)value.u.int64;
    switch (field->kind) {
    case CFG_SIZE:
        *(size_t *)dest = (size_t)number;
        break;
    case CFG_U64:
        *(uint64_t *)dest = number;
        break;
    case CFG_INT:
        *(int *)dest = (int)number;
        break;
    case CFG_U32:
        *(uint32_t *)dest = (uint32_t)number;
        break;
    case CFG_CONTEXT:
        config->model.context_tokens = config->limits.context_tokens = (size_t)number;
        break;
    case CFG_TIMEOUT:
        config->limits.command_timeout_ms = number * 1000;
        break;
    default:
        return schema_error(e, value, key, "unsupported configuration field");
    }
    return FORGE_OK;
}

static const char *child_table(const char *table, const char *key) {
    static const char *const top[] = {"model", "inference", "agent", "tools", "index"};
    if (!*table) {
        for (size_t i = 0; i < sizeof(top) / sizeof(top[0]); i++)
            if (!strcmp(key, top[i]))
                return top[i];
    } else if (!strcmp(table, "tools") && !strcmp(key, "shell")) {
        return "tools.shell";
    } else if (!strcmp(table, "inference") && !strcmp(key, "checkpoints")) {
        return "inference.checkpoints";
    }
    return NULL;
}

static forge_status apply_table(forge_config *config, toml_datum_t table, const char *name,
                                const char *source, forge_error *e) {
    for (int32_t i = 0; i < table.u.tab.size; i++) {
        const char *key = table.u.tab.key[i];
        toml_datum_t value = table.u.tab.value[i];
        char full[160];
        snprintf(full, sizeof(full), "%s%s%s", name, *name ? "." : "", key);
        if (strlen(key) != (size_t)table.u.tab.len[i])
            return schema_error(e, value, full, "keys must not contain embedded NUL bytes");
        if (!*name && !strcmp(key, "extends"))
            continue; /* Validated and loaded before applying this table. */
        const char *child = child_table(name, key);
        if (child) {
            if (value.type != TOML_TABLE)
                return schema_error(e, value, full, "expected a table");
            forge_status status = apply_table(config, value, child, source, e);
            if (status != FORGE_OK)
                return status;
            continue;
        }
        const config_field *field = NULL;
        for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
            if (!strcmp(name, fields[k].table) && !strcmp(key, fields[k].key)) {
                field = &fields[k];
                break;
            }
        }
        if (!field)
            return schema_error(e, value, full, "unknown configuration key or table");
        forge_status status = apply_field(config, field, value, source, e);
        if (status != FORGE_OK)
            return status;
    }
    return FORGE_OK;
}

static forge_status load_document(forge_config *config, const char *path, const char *document,
                                  size_t length, config_chain *chain, forge_error *e) {
    if (chain->depth >= FORGE_CONFIG_MAX_INHERITANCE)
        return fg_error(e, FORGE_ERR_LIMIT, "%s: Configuration inheritance exceeds %u files", path,
                        FORGE_CONFIG_MAX_INHERITANCE);
    for (size_t i = 0; i < chain->depth; i++)
        if (same_path(path, chain->paths[i]))
            return fg_error(e, FORGE_ERR_PARSE, "%s: Configuration inheritance cycle", path);
    strcpy(chain->paths[chain->depth++], path);
    char *owned_document = NULL;
    forge_status status = FORGE_OK;
    if (!document) {
#ifdef _WIN32
        struct _stat64 st;
        bool regular = _stat64(path, &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFREG;
#else
        struct stat st;
        bool regular = stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
        if (!regular) {
            status = fg_error(e, FORGE_ERR_IO, "%s: Configuration must be a readable regular file",
                              path);
            goto done;
        }
        forge_error read_error = {0};
        owned_document = fg_read_file(path, FORGE_CONFIG_MAX_BYTES, &length, &read_error);
        if (!owned_document) {
            status = fg_error(e, read_error.code, "%s: %s", path, read_error.message);
            goto done;
        }
        document = owned_document;
    }
    if (length > FORGE_CONFIG_MAX_BYTES) {
        status = fg_error(e, FORGE_ERR_LIMIT, "%s: Configuration exceeds %u bytes", path,
                          FORGE_CONFIG_MAX_BYTES);
        goto done;
    }
    if (!valid_utf8((const unsigned char *)document, length)) {
        status = fg_error(e, FORGE_ERR_PARSE,
                          "%s: Configuration must contain valid UTF-8 "
                          "without raw NUL bytes",
                          path);
        goto done;
    }
    toml_result_t result = toml_parse_named(document, (int)length, path);
    if (!result.ok) {
        status = fg_error(e, FORGE_ERR_PARSE, "%s: %s", path, result.errmsg);
        toml_free(result);
        goto done;
    }
    toml_datum_t parent = toml_get(result.toptab, "extends");
    if (parent.type != TOML_UNKNOWN) {
        if (!string_valid(parent, FG_PATH_MAX - 1)) {
            status = schema_error(e, parent, "extends", "expected one nonempty file path");
        } else {
            char base[FG_PATH_MAX], resolved[FG_PATH_MAX];
            parent_path(path, base);
            status = absolute_path(base, parent.u.str.ptr, resolved, e);
            if (status == FORGE_OK)
                status = load_document(config, resolved, NULL, 0, chain, e);
        }
    }
    if (status == FORGE_OK)
        status = apply_table(config, result.toptab, "", path, e);
    toml_free(result);
done:
    free(owned_document);
    chain->depth--;
    return status;
}

static forge_status load_config(forge_config *config, const char *path, const char *document,
                                size_t length, forge_error *e) {
    if (!config || !path)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Configuration and source path are required");
    char absolute[FG_PATH_MAX];
    forge_status status = absolute_path(NULL, path, absolute, e);
    if (status != FORGE_OK)
        return status;
    forge_config next;
    status = config_copy(&next, config, e);
    if (status != FORGE_OK)
        return status;
    config_chain chain = {0};
    status = load_document(&next, absolute, document, length, &chain, e);
    if (status == FORGE_OK) {
        forge_error validation = {0};
        status = forge_config_validate(&next, &validation);
        if (status != FORGE_OK)
            fg_error(e, status, "%s: %s", absolute, validation.message);
    }
    if (status != FORGE_OK) {
        forge_config_destroy(&next);
        return status;
    }
    forge_config_destroy(config);
    *config = next;
    clear_error(e);
    return FORGE_OK;
}

forge_status forge_config_load(forge_config *config, const char *path, forge_error *e) {
    return load_config(config, path, NULL, 0, e);
}

forge_status forge_config_parse(forge_config *config, const char *document, size_t length,
                                const char *source_path, forge_error *e) {
    if (!document)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Configuration document is required");
    return load_config(config, source_path, document, length, e);
}

forge_status forge_config_validate(const forge_config *config, forge_error *e) {
    if (!config)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Configuration is required");
    const forge_model_config *model = &config->model;
    const forge_limits *limits = &config->limits;
    if (!config->thought && (config->thought_required || config->thought_routed))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Thought routing or required thought needs the thought channel enabled");
    if (!config->thought_routed &&
        (config->thought_cue || config->thought_budget || config->thought_budget_unbounded ||
         config->thought_native))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Thought budget and cue controls need thought routing enabled");
    if (config->thought_native &&
        (config->thought_cue || config->thought_budget || config->thought_budget_unbounded))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Native thought cannot use a host cue or thought budget");
    if (config->thought_budget > INT32_MAX)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Thought budget must be in [1, 2147483647]");
    if (config->thought_cue && strchr(config->thought_cue, '{'))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "The reasoning cue must not contain an action-opening brace");
    if (model->context_tokens < 128 || model->context_tokens > 1048576 ||
        model->context_tokens != limits->context_tokens)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "model.context must be in [128, 1048576] "
                        "and agree with agent context capacity");
    if (limits->output_reserve == 0 || limits->output_reserve >= limits->context_tokens)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "agent.output_reserve must be positive and "
                        "smaller than model.context");
    if (limits->max_turns == 0 || limits->max_turns > 1000)
        return fg_error(e, FORGE_ERR_ARGUMENT, "agent.max_turns must be in [1, 1000]");
    if (!limits->max_generated_tokens || limits->max_generated_tokens > INT32_MAX ||
        !limits->max_input_tokens || limits->max_input_tokens > INT32_MAX)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "agent.max_tokens and agent.max_input "
                        "must be in [1, 2147483647]");
    if (!limits->max_tool_bytes || limits->max_tool_bytes > FG_MAX_JSON ||
        !limits->max_file_bytes || limits->max_file_bytes > FG_MAX_JSON)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "agent.max_tool_bytes and agent.max_file_bytes "
                        "must be in [1, 16777216]");
    if (!limits->command_timeout_ms || limits->command_timeout_ms > UINT64_C(86400000))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "tools.shell.timeout must be in [1, 86400] "
                        "seconds (CLI timeout in [1, 86400000] milliseconds)");
    if (!limits->wall_timeout_ms || limits->wall_timeout_ms > UINT64_C(604800000))
        return fg_error(e, FORGE_ERR_ARGUMENT, "agent.wall_timeout_ms must be in [1, 604800000]");
    if (model->gpu_layers < FORGE_GPU_LAYERS_AUTO || model->gpu_layers > 65535)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "inference.gpu_layers must be auto, -1, or [0, 65535]");
    if (model->threads < 0 || model->threads > 1024)
        return fg_error(e, FORGE_ERR_ARGUMENT, "inference.threads must be in [0, 1024]");
    if (!isfinite(model->temperature) || model->temperature < 0 || model->temperature > 2)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "inference.temperature must be finite and in [0, 2]");
    if ((unsigned)model->thinking > FORGE_THINKING_DISABLED)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid model thinking mode");
    if (model->model_path && model->script_path)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Choose a model path or an explicit script "
                        "fixture, not both");
    if ((model->model_path && !path_text_valid(model->model_path)) ||
        (model->script_path && !path_text_valid(model->script_path)))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Model/fixture paths must be nonempty, within "
                        "the path limit, and contain no control characters");
    if (model->chat_template && (!*model->chat_template || strlen(model->chat_template) > 65536))
        return fg_error(e, FORGE_ERR_ARGUMENT, "model.chat_template must contain 1..65536 bytes");
    if ((unsigned)config->shell_network > FORGE_SHELL_NETWORK_ALLOW)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid shell network policy");
    const forge_checkpoint_cache_options *cache = &config->checkpoint_cache;
    if (cache->max_bytes < 4096 || cache->max_bytes > FORGE_CHECKPOINT_CACHE_MAX_BYTES ||
        !cache->max_entries || cache->max_entries > FORGE_CHECKPOINT_CACHE_MAX_ENTRIES ||
        !cache->min_prefix_tokens || cache->min_prefix_tokens > 1048576 ||
        !cache->max_captures_per_prompt ||
        cache->max_captures_per_prompt > FORGE_CHECKPOINT_CACHE_MAX_ANCHORS)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid inference.checkpoints limits");
    clear_error(e);
    return FORGE_OK;
}

forge_status forge_config_check_exec(const forge_config *config, bool allow_exec, forge_error *e) {
    if (!config)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Configuration is required");
    if ((unsigned)config->shell_network > FORGE_SHELL_NETWORK_ALLOW)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid shell network policy");
    if (allow_exec && config->shell_network == FORGE_SHELL_NETWORK_DENY)
        return fg_error(e, FORGE_ERR_POLICY,
                        "tools.shell.network=false forbids shell execution: "
                        "this build cannot enforce a network sandbox. Remove --allow-exec, "
                        "or explicitly choose network=true and accept unsandboxed execution");
    clear_error(e);
    return FORGE_OK;
}
