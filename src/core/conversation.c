#include "internal.h"
#include "conversation.h"
#include <ctype.h>

#define CONVERSATION_MAX_BYTES (16u * 1024u * 1024u)
#define CONVERSATION_DEFAULT_BYTES (256u * 1024u)
#define CONVERSATION_DEFAULT_TURNS 16u
#define CONVERSATION_MAX_TURNS 1024u
#define CONVERSATION_MAX_SEGMENTS 2048u
#define QUESTION_MAX_BYTES 4096u
#define ANSWER_MAX_BYTES 8192u

typedef struct {
    forge_segment_kind kind;
    char *text;
    size_t parent;        /* Earlier segment slot, or SIZE_MAX. */
    uint64_t original_id; /* Used only during capture. */
} conversation_segment;

typedef struct {
    conversation_segment *segments;
    size_t count, bytes;
} conversation_turn;

struct forge_conversation {
    conversation_turn *turns;
    size_t count, bytes, segments, max_bytes, max_turns, omitted;
    bool blocked;
};

static void free_turn(conversation_turn *turn) {
    for (size_t i = 0; i < turn->count; i++)
        free(turn->segments[i].text);
    free(turn->segments);
    memset(turn, 0, sizeof(*turn));
}

forge_conversation *forge_conversation_create(size_t max_bytes, size_t max_turns, forge_error *e) {
    if (!max_bytes)
        max_bytes = CONVERSATION_DEFAULT_BYTES;
    if (!max_turns)
        max_turns = CONVERSATION_DEFAULT_TURNS;
    if (max_bytes > CONVERSATION_MAX_BYTES || max_turns > CONVERSATION_MAX_TURNS) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Conversation limits exceed 16 MiB or 1024 user turns");
        return NULL;
    }
    forge_conversation *c = calloc(1, sizeof(*c));
    if (!c) {
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate conversation");
        return NULL;
    }
    c->turns = calloc(max_turns, sizeof(*c->turns));
    if (!c->turns) {
        free(c);
        fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate conversation turns");
        return NULL;
    }
    c->max_bytes = max_bytes;
    c->max_turns = max_turns;
    return c;
}

forge_status forge_conversation_reset(forge_conversation *c, forge_error *e) {
    if (!c)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Conversation is required");
    for (size_t i = 0; i < c->count; i++)
        free_turn(&c->turns[i]);
    c->count = c->bytes = c->segments = c->omitted = 0;
    c->blocked = false;
    return FORGE_OK;
}

void forge_conversation_destroy(forge_conversation *c) {
    if (c) {
        forge_conversation_reset(c, NULL);
        free(c->turns);
        free(c);
    }
}

void fg_conversation_swap(forge_conversation *left, forge_conversation *right) {
    if (left && right && left != right) {
        forge_conversation temporary = *left;
        *left = *right;
        *right = temporary;
    }
}

forge_conversation *forge_conversation_clone(const forge_conversation *source, forge_error *e) {
    if (!source) {
        fg_error(e, FORGE_ERR_ARGUMENT, "Conversation to clone is required");
        return NULL;
    }
    forge_conversation *copy = forge_conversation_create(source->max_bytes, source->max_turns, e);
    if (!copy)
        return NULL;
    copy->blocked = source->blocked;
    copy->omitted = source->omitted;
    for (size_t i = 0; i < source->count; i++) {
        const conversation_turn *from = &source->turns[i];
        conversation_turn *to = &copy->turns[copy->count++];
        to->segments = calloc(from->count, sizeof(*to->segments));
        if (!to->segments)
            goto fail;
        for (size_t j = 0; j < from->count; j++) {
            to->segments[j] = from->segments[j];
            to->segments[j].text = fg_strdup(from->segments[j].text);
            if (!to->segments[j].text)
                goto fail;
            to->count++;
        }
        to->bytes = from->bytes;
        copy->bytes += to->bytes;
        copy->segments += to->count;
    }
    return copy;
fail:
    forge_conversation_destroy(copy);
    fg_error(e, FORGE_ERR_MEMORY, "Cannot clone conversation evidence");
    return NULL;
}

forge_status fg_conversation_seed(const forge_conversation *c, forge_context *ctx,
                                  size_t *start_index, forge_error *e) {
    if (!ctx || !start_index)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Context and conversation start are required");
    *start_index = forge_context_size(ctx);
    if (!c)
        return FORGE_OK;
    if (c->blocked)
        return fg_error(e, FORGE_ERR_LIMIT,
                        "The last exchange could not be retained completely; use /new to reset "
                        "conversation history before continuing");
    if (c->omitted) {
        char notice[256];
        snprintf(notice, sizeof(notice),
                 "CONVERSATION_HISTORY: %zu older user exchanges were omitted as complete "
                 "units to respect the configured history bound. Retained evidence follows "
                 "verbatim; inspect the workspace again before relying on old file contents.",
                 c->omitted);
        if (!forge_context_add(ctx, FORGE_SEG_SOURCE, notice, 100, true, 0, 0))
            return fg_error(e, FORGE_ERR_MEMORY, "Cannot retain conversation limit notice");
    } else if (c->count &&
               !forge_context_add(ctx, FORGE_SEG_SOURCE,
                                  "CONVERSATION_HISTORY: previous user requests and actual "
                                  "assistant/tool exchanges follow. Earlier file contents and "
                                  "test results are historical evidence, not current validation.",
                                  100, true, 0, 0))
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot identify historical conversation evidence");
    for (size_t i = 0; i < c->count; i++) {
        const conversation_turn *turn = &c->turns[i];
        uint64_t ids[CONVERSATION_MAX_SEGMENTS] = {0};
        for (size_t j = 0; j < turn->count; j++) {
            const conversation_segment *segment = &turn->segments[j];
            uint64_t dependency = segment->parent == SIZE_MAX ? 0 : ids[segment->parent];
            forge_segment_kind kind =
                segment->kind == FORGE_SEG_TASK ? FORGE_SEG_SOURCE : segment->kind;
            ids[j] = forge_context_add(ctx, kind, segment->text, 100, true, dependency, 0);
            if (!ids[j] || forge_context_set_flags(ctx, ids[j], true, true) != FORGE_OK)
                return fg_error(e, FORGE_ERR_MEMORY, "Cannot seed complete conversation evidence");
        }
    }
    *start_index = forge_context_size(ctx);
    return FORGE_OK;
}

static void evict_turn(forge_conversation *c) {
    c->bytes -= c->turns[0].bytes;
    c->segments -= c->turns[0].count;
    free_turn(&c->turns[0]);
    c->count--;
    memmove(c->turns, c->turns + 1, c->count * sizeof(*c->turns));
    memset(&c->turns[c->count], 0, sizeof(*c->turns));
    if (c->omitted != SIZE_MAX)
        c->omitted++;
}

forge_status fg_conversation_capture(forge_conversation *c, const forge_context *ctx,
                                     size_t start_index, forge_error *e) {
    if (!c)
        return FORGE_OK;
    if (!ctx || start_index > forge_context_size(ctx))
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid conversation capture range");
    if (c->blocked)
        return fg_error(e, FORGE_ERR_LIMIT, "Conversation requires an explicit /new reset");
    size_t available = forge_context_size(ctx) - start_index;
    if (!available)
        return FORGE_OK;
    conversation_turn turn = {0};
    turn.segments = calloc(FG_MIN(available, CONVERSATION_MAX_SEGMENTS), sizeof(*turn.segments));
    if (!turn.segments) {
        c->blocked = true;
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot capture conversation");
    }
    forge_status status = FORGE_OK;
    for (size_t i = start_index; i < forge_context_size(ctx); i++) {
        forge_segment_view view;
        if (!forge_context_get(ctx, i, &view)) {
            status = fg_error(e, FORGE_ERR_ARGUMENT, "Cannot read conversation segment");
            goto fail;
        }
        if (view.kind != FORGE_SEG_TASK && view.kind != FORGE_SEG_SOURCE &&
            view.kind != FORGE_SEG_ACTION && view.kind != FORGE_SEG_RESULT)
            continue;
        size_t length = strlen(view.text);
        if (turn.count == CONVERSATION_MAX_SEGMENTS || length > c->max_bytes - turn.bytes) {
            status = fg_error(e, FORGE_ERR_LIMIT,
                              "Current exchange exceeds conversation history bounds; its audit "
                              "log is retained, but use /new before another request");
            goto fail;
        }
        conversation_segment *segment = &turn.segments[turn.count];
        segment->kind = view.kind;
        segment->original_id = view.id;
        segment->parent = SIZE_MAX;
        if (view.kind == FORGE_SEG_RESULT) {
            for (size_t j = 0; j < turn.count; j++) {
                if (turn.segments[j].kind != FORGE_SEG_ACTION)
                    continue;
                for (size_t k = 0; k < view.dependency_count; k++) {
                    uint64_t dependency = 0;
                    if (forge_context_get_dependency(ctx, view.id, k, &dependency) &&
                        dependency == turn.segments[j].original_id) {
                        if (segment->parent != SIZE_MAX) {
                            status = fg_error(e, FORGE_ERR_CONFLICT,
                                              "Conversation result has multiple action parents");
                            goto fail;
                        }
                        segment->parent = j;
                    }
                }
            }
            if (segment->parent == SIZE_MAX) {
                status = fg_error(e, FORGE_ERR_CONFLICT,
                                  "Conversation result has no retained assistant action");
                goto fail;
            }
        }
        segment->text = fg_strdup(view.text);
        if (!segment->text) {
            status = fg_error(e, FORGE_ERR_MEMORY, "Cannot copy conversation evidence");
            goto fail;
        }
        turn.count++;
        turn.bytes += length;
    }
    if (!turn.count) { /* Initialization failed before a user exchange began. */
        free_turn(&turn);
        return FORGE_OK;
    }
    for (size_t i = 0; i < turn.count; i++) {
        if (turn.segments[i].kind != FORGE_SEG_ACTION)
            continue;
        size_t results = 0;
        for (size_t j = i + 1; j < turn.count; j++)
            results += turn.segments[j].parent == i ? 1u : 0u;
        if (results != 1) {
            status = fg_error(e, FORGE_ERR_CONFLICT,
                              "Incomplete assistant/tool exchange cannot be retained; use /new");
            goto fail;
        }
    }
    while (c->count && (c->count >= c->max_turns || turn.bytes > c->max_bytes - c->bytes ||
                        turn.count > CONVERSATION_MAX_SEGMENTS - c->segments))
        evict_turn(c);
    c->turns[c->count++] = turn;
    c->bytes += turn.bytes;
    c->segments += turn.count;
    return FORGE_OK;
fail:
    free_turn(&turn);
    c->blocked = true;
    return status;
}

static bool nonblank(const char *text) {
    if (text)
        for (const unsigned char *p = (const unsigned char *)text; *p; p++)
            if (!isspace(*p))
                return true;
    return false;
}

forge_status fg_conversation_ask(const forge_agent_config *config, const char *question,
                                 uint64_t deadline, char **output, forge_error *e) {
    if (output)
        *output = NULL;
    if (!config || !output || !nonblank(question) || strlen(question) > QUESTION_MAX_BYTES ||
        !fg_utf8_valid(question, strlen(question)))
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "ask_user requires a nonempty UTF-8 question <=4096 bytes");
    forge_status status;
    char answer[ANSWER_MAX_BYTES + 1] = {0};
    if (!config->ask_user)
        status = fg_error(e, FORGE_ERR_POLICY,
                          "ask_user is unavailable: this run has no interactive question callback");
    else if ((config->cancelled && config->cancelled(config->userdata)) || fg_now_ms() >= deadline)
        status = fg_error(e, FORGE_ERR_CANCELLED, "Question cancelled or run deadline reached");
    else {
        status = config->ask_user(question, answer, sizeof(answer), config->question_userdata, e);
        if ((config->cancelled && config->cancelled(config->userdata)) || fg_now_ms() >= deadline)
            status = fg_error(e, FORGE_ERR_CANCELLED, "Question cancelled or run deadline reached");
        else if (status == FORGE_OK && (answer[ANSWER_MAX_BYTES] || !nonblank(answer) ||
                                        !fg_utf8_valid(answer, strlen(answer))))
            status =
                fg_error(e, FORGE_ERR_ARGUMENT,
                         "Question callback returned an empty, oversized or invalid UTF-8 answer");
    }
    const char *label = status == FORGE_OK              ? "answered"
                        : status == FORGE_ERR_POLICY    ? "declined"
                        : status == FORGE_ERR_CANCELLED ? "cancelled"
                                                        : "error";
    char *quoted = fg_json_string(status == FORGE_OK ? answer
                                  : e && *e->message ? e->message
                                                     : forge_status_string(status));
    fg_buf json = {0};
    bool ok = quoted && fg_buf_printf(&json, "{\"status\":\"%s\",\"%s\":%s}", label,
                                      status == FORGE_OK ? "answer" : "reason", quoted);
    free(quoted);
    if (!ok) {
        fg_buf_clear(&json);
        return fg_error(e, FORGE_ERR_MEMORY, "Cannot retain question answer");
    }
    *output = fg_buf_take(&json);
    return status;
}
