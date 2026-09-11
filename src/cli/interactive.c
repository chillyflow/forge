#include "internal.h"
#include "interactive.h"
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#define INTERACTIVE_LINE_BYTES 65536u

typedef struct {
    forge_agent_config config;
    uint64_t deadline;
    bool eof;
    bool discard_lf;
#ifdef _WIN32
    WCHAR high_surrogate;
#endif
} interactive_state;

static bool stopped(const interactive_state *state) {
    return (state->config.cancelled && state->config.cancelled(state->config.userdata)) ||
           (state->deadline && fg_now_ms() >= state->deadline);
}

/* Read without stdio read-ahead: a piped line after a task belongs to the first
 * question or subsequent task. Polling also makes an unanswered question obey
 * cancellation and the existing run deadline on both Windows and POSIX. */
static forge_status read_character(interactive_state *state, char bytes[5], size_t *length,
                                   bool *backspace, forge_error *e) {
    *length = 0;
    *backspace = false;
    for (;;) {
        if (stopped(state))
            return fg_error(e, FORGE_ERR_CANCELLED,
                            "Interactive input cancelled or deadline reached");
#ifdef _WIN32
        HANDLE input = (HANDLE)_get_osfhandle(_fileno(stdin));
        if (input == INVALID_HANDLE_VALUE)
            return fg_error(e, FORGE_ERR_IO, "Interactive standard input is unavailable");
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode)) {
            DWORD wait = WaitForSingleObject(input, 25);
            if (wait == WAIT_TIMEOUT)
                continue;
            if (wait != WAIT_OBJECT_0)
                return fg_error(e, FORGE_ERR_IO, "Cannot wait for console input");
            INPUT_RECORD record;
            DWORD count = 0;
            if (!ReadConsoleInputW(input, &record, 1, &count))
                return fg_error(e, FORGE_ERR_IO, "Cannot read console input");
            if (!count || record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
                continue;
            WCHAR value = record.Event.KeyEvent.uChar.UnicodeChar;
            if (value == 3)
                return fg_error(e, FORGE_ERR_CANCELLED, "Interactive input cancelled");
            if (value == 4 || value == 26) {
                state->eof = true;
                return FORGE_OK;
            }
            if (value == 8) {
                *backspace = true;
                return FORGE_OK;
            }
            if (!value)
                continue;
            if (value == L'\r') {
                bytes[0] = '\n';
                *length = 1;
                fputc('\n', stderr);
                return FORGE_OK;
            }
            if (value >= 0xD800 && value <= 0xDBFF) {
                state->high_surrogate = value;
                continue;
            }
            WCHAR wide[2] = {value, 0};
            int wide_count = 1;
            if (state->high_surrogate && value >= 0xDC00 && value <= 0xDFFF) {
                wide[0] = state->high_surrogate;
                wide[1] = value;
                wide_count = 2;
            }
            state->high_surrogate = 0;
            int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, wide_count,
                                                bytes, 4, NULL, NULL);
            if (!converted)
                continue;
            *length = (size_t)converted;
            fwrite(bytes, 1, *length, stderr);
            fflush(stderr);
            return FORGE_OK;
        }
        if (GetFileType(input) == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!PeekNamedPipe(input, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) {
                    state->eof = true;
                    return FORGE_OK;
                }
                return fg_error(e, FORGE_ERR_IO, "Cannot inspect piped interactive input");
            }
            if (!available) {
                Sleep(25);
                continue;
            }
        }
        DWORD count = 0;
        if (!ReadFile(input, bytes, 1, &count, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                state->eof = true;
                return FORGE_OK;
            }
            return fg_error(e, FORGE_ERR_IO, "Cannot read interactive input");
        }
        *length = count;
        if (!count)
            state->eof = true;
        return FORGE_OK;
#else
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&input, 1, 25);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return fg_error(e, FORGE_ERR_IO, "Cannot wait for interactive input");
        }
        if (!ready)
            continue;
        ssize_t count = read(STDIN_FILENO, bytes, 1);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return fg_error(e, FORGE_ERR_IO, "Cannot read interactive input");
        }
        *length = (size_t)count;
        if (!count)
            state->eof = true;
        return FORGE_OK;
#endif
    }
}

static forge_status read_line(interactive_state *state, size_t limit, char **line, forge_error *e) {
    *line = NULL;
    fg_buf buffer = {0};
    bool overflow = false;
    for (;;) {
        char bytes[5] = {0};
        size_t length = 0;
        bool backspace = false;
        forge_status status = read_character(state, bytes, &length, &backspace, e);
        if (status != FORGE_OK) {
            fg_buf_clear(&buffer);
            return status;
        }
        if (backspace) {
            if (buffer.len) {
                do {
                    buffer.len--;
                } while (buffer.len && ((unsigned char)buffer.data[buffer.len] & 0xC0) == 0x80);
                buffer.data[buffer.len] = 0;
                fputs("\b \b", stderr);
                fflush(stderr);
            }
            continue;
        }
        if (state->eof && !buffer.len && !overflow) {
            fg_buf_clear(&buffer);
            return FORGE_OK;
        }
        if (length == 1 && state->discard_lf && bytes[0] == '\n') {
            state->discard_lf = false;
            continue;
        }
        state->discard_lf = length == 1 && bytes[0] == '\r';
        bool ended = state->eof || (length == 1 && (bytes[0] == '\r' || bytes[0] == '\n'));
        if (ended) {
            if (overflow) {
                fg_buf_clear(&buffer);
                return fg_error(e, FORGE_ERR_LIMIT, "Interactive line exceeds %zu bytes", limit);
            }
            if (!buffer.data && !fg_buf_puts(&buffer, "")) {
                fg_buf_clear(&buffer);
                return fg_error(e, FORGE_ERR_MEMORY, "Cannot allocate interactive input");
            }
            if (!fg_utf8_valid(buffer.data, buffer.len)) {
                fg_buf_clear(&buffer);
                return fg_error(e, FORGE_ERR_ARGUMENT, "Interactive input must be valid UTF-8");
            }
            *line = fg_buf_take(&buffer);
            return FORGE_OK;
        }
        if (length && memchr(bytes, 0, length)) {
            fg_buf_clear(&buffer);
            return fg_error(e, FORGE_ERR_ARGUMENT, "Interactive input cannot contain NUL bytes");
        }
        if (length > limit - buffer.len)
            overflow = true;
        if (!overflow && !fg_buf_add(&buffer, bytes, length)) {
            fg_buf_clear(&buffer);
            return fg_error(e, FORGE_ERR_MEMORY, "Cannot retain interactive input");
        }
    }
}

static forge_status question(const char *prompt, char *answer, size_t capacity, void *userdata,
                             forge_error *e) {
    interactive_state *state = userdata;
    fprintf(stderr, "\nQuestion: %s\nAnswer> ", prompt);
    fflush(stderr);
    char *line = NULL;
    forge_status status = read_line(state, capacity - 1, &line, e);
    if (status != FORGE_OK)
        return status;
    if (!line)
        return fg_error(e, FORGE_ERR_CANCELLED, "End of input while waiting for the user's answer");
    if (!strcmp(line, "/decline"))
        status = fg_error(e, FORGE_ERR_POLICY, "The user declined to answer this question");
    else if (!strcmp(line, "/quit") || !strcmp(line, "/cancel")) {
        state->eof = true;
        status = fg_error(e, FORGE_ERR_CANCELLED, "The user cancelled the question");
    } else if (!*line)
        status = fg_error(e, FORGE_ERR_POLICY, "The user provided no answer");
    else
        memcpy(answer, line, strlen(line) + 1);
    free(line);
    return status;
}

static forge_status multiline(interactive_state *state, char **request, forge_error *e) {
    fg_buf text = {0};
    for (;;) {
        fputs("...> ", stderr);
        fflush(stderr);
        char *line = NULL;
        forge_status status = read_line(state, INTERACTIVE_LINE_BYTES, &line, e);
        if (status != FORGE_OK) {
            fg_buf_clear(&text);
            return status;
        }
        if (!line) {
            fg_buf_clear(&text);
            return fg_error(e, FORGE_ERR_CANCELLED,
                            "End of input before /end; task was not submitted");
        }
        if (!strcmp(line, "/end")) {
            free(line);
            if (!text.len) {
                fg_buf_clear(&text);
                return fg_error(e, FORGE_ERR_ARGUMENT, "Multiline task is empty");
            }
            *request = fg_buf_take(&text);
            return FORGE_OK;
        }
        if (!strcmp(line, "/cancel")) {
            free(line);
            fg_buf_clear(&text);
            return FORGE_OK;
        }
        size_t length = strlen(line);
        if (length + 1 > INTERACTIVE_LINE_BYTES - text.len) {
            free(line);
            fg_buf_clear(&text);
            return fg_error(e, FORGE_ERR_LIMIT, "Multiline task exceeds 65536 bytes");
        }
        bool ok = fg_buf_puts(&text, line) && fg_buf_puts(&text, "\n");
        free(line);
        if (!ok) {
            fg_buf_clear(&text);
            return fg_error(e, FORGE_ERR_MEMORY, "Cannot retain multiline task");
        }
    }
}

forge_status fg_cli_interactive(const forge_agent_config *base, size_t history_bytes,
                                size_t history_turns, forge_event_fn events, void *event_userdata,
                                forge_error *e) {
    if (!base || !base->model || base->model->config.prompt_protocol != FORGE_PROMPT_NATIVE)
        return fg_error(e, FORGE_ERR_ARGUMENT,
                        "Interactive conversations require native prompt protocol");
    forge_conversation *conversation = forge_conversation_create(history_bytes, history_turns, e);
    if (!conversation)
        return e && e->code ? e->code : FORGE_ERR_MEMORY;
    interactive_state state = {0};
    state.config = *base;
    state.config.conversation = conversation;
    state.config.ask_user = question;
    state.config.question_userdata = &state;
    fputs("Forge conversation. Requests continue the current task. /new [task] clears history; "
          "/begin then /end submits multiline input; /quit exits. During questions, /decline "
          "refuses an answer and /cancel stops the session.\n",
          stderr);
    forge_status result = FORGE_OK;
    while (!state.eof) {
        state.deadline = 0;
        if (stopped(&state)) {
            result = fg_error(e, FORGE_ERR_CANCELLED, "Interactive session cancelled");
            break;
        }
        fputs("Task> ", stderr);
        fflush(stderr);
        char *request = NULL;
        forge_status status = read_line(&state, INTERACTIVE_LINE_BYTES, &request, e);
        if (status != FORGE_OK) {
            result = status;
            break;
        }
        if (!request)
            break;
        if (!*request || !strcmp(request, "/help")) {
            if (*request)
                fputs("/new [task], /begin ... /end, /quit; questions: /decline or /cancel.\n",
                      stderr);
            free(request);
            continue;
        }
        if (!strcmp(request, "/quit") || !strcmp(request, "/exit")) {
            free(request);
            break;
        }
        if (!strcmp(request, "/new") || !strncmp(request, "/new ", 5)) {
            forge_conversation_reset(conversation, NULL);
            fputs("Conversation history cleared. Workspace and permissions are unchanged.\n",
                  stderr);
            if (!strcmp(request, "/new")) {
                free(request);
                continue;
            }
            memmove(request, request + 5, strlen(request + 5) + 1);
        } else if (!strcmp(request, "/begin")) {
            free(request);
            request = NULL;
            status = multiline(&state, &request, e);
            if (status != FORGE_OK) {
                result = status;
                break;
            }
            if (!request)
                continue;
        } else if (request[0] == '/') {
            fputs("Unknown conversation command. Use /help.\n", stderr);
            free(request);
            continue;
        }
        if (!*request) {
            free(request);
            continue;
        }
        uint64_t now = fg_now_ms();
        state.deadline = base->limits.wall_timeout_ms > UINT64_MAX - now
                             ? UINT64_MAX
                             : now + base->limits.wall_timeout_ms;
        forge_agent *agent = forge_agent_create(&state.config, e);
        status = agent          ? forge_agent_run(agent, request, events, event_userdata, e)
                 : e && e->code ? e->code
                                : FORGE_ERR_MEMORY;
        free(request);
        if (agent) {
            const forge_metrics *metrics = forge_agent_metrics(agent);
            fprintf(
                stderr,
                "Session: %s\n%s turns=%zu prompt=%zu generated=%zu cached=%zu elapsed=%.0fms\n",
                forge_agent_session(agent), metrics->simulated ? "SIMULATED" : "INFERENCE",
                metrics->turns, metrics->prompt_tokens, metrics->generated_tokens,
                metrics->cached_tokens, metrics->duration_ms);
        }
        forge_agent_destroy(agent);
        if (status != FORGE_OK) {
            fprintf(stderr, "forge: %s: %s\n", forge_status_string(status),
                    e && *e->message ? e->message : "Interactive task failed");
            result = status;
            if (status == FORGE_ERR_CANCELLED || state.eof)
                break;
        }
    }
    forge_conversation_destroy(conversation);
    return result;
}
