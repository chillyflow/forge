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
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define PROBE_FILE "forge_process_probe.exe"
#define PATH_SEPARATOR ";"
#else
#include <unistd.h>
#define PROBE_FILE "forge_process_probe"
#define PATH_SEPARATOR ":"
#endif

static const char *marker_name = "process-child.marker";
static char temporary[FG_PATH_MAX], workspace[FG_PATH_MAX], nested[FG_PATH_MAX];
static char repository_bin[FG_PATH_MAX], trusted_bin[FG_PATH_MAX];
static char repository_probe[FG_PATH_MAX], trusted_probe[FG_PATH_MAX], self[FG_PATH_MAX];

static void make_directory(const char *path) {
#ifdef _WIN32
    assert(_mkdir(path) == 0);
#else
    assert(mkdir(path, 0700) == 0);
#endif
}

static void remove_directory(const char *path) {
#ifdef _WIN32
    assert(_rmdir(path) == 0);
#else
    assert(rmdir(path) == 0);
#endif
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void set_path(const char *path) {
    /* This changes only this test process, and main restores its original PATH. */
#ifdef _WIN32
    assert(_putenv_s("PATH", path ? path : "") == 0);
#else
    assert((path ? setenv("PATH", path, 1) : unsetenv("PATH")) == 0);
#endif
}

static void copy_program(const char *source, const char *destination) {
    FILE *input = fopen(source, "rb");
    FILE *output = fopen(destination, "wb");
    assert(input && output);
    char buffer[16384];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0)
        assert(fwrite(buffer, 1, count, output) == count);
    assert(!ferror(input));
    assert(fclose(input) == 0);
    assert(fclose(output) == 0);
#ifndef _WIN32
    assert(chmod(destination, 0700) == 0);
#endif
}

static void setup(const char *argv0) {
#ifdef _WIN32
    (void)argv0;
    char parent[FG_PATH_MAX];
    DWORD length = GetTempPathA(sizeof(parent), parent);
    assert(length && length < sizeof(parent));
    assert(GetTempFileNameA(parent, "fgp", 0, temporary));
    assert(DeleteFileA(temporary));
    make_directory(temporary);
    length = GetModuleFileNameA(NULL, self, sizeof(self));
    assert(length && length < sizeof(self));
#else
    strcpy(temporary, "/tmp/forge-process-XXXXXX");
    assert(mkdtemp(temporary));
    assert(realpath(argv0, self));
#endif
    assert(fg_path_join(workspace, temporary, "workspace"));
    assert(fg_path_join(nested, workspace, "module"));
    assert(fg_path_join(repository_bin, workspace, "bin"));
    /* A common prefix is not containment: this sibling must remain usable. */
    assert(fg_path_join(trusted_bin, temporary, "workspace-trusted"));
    make_directory(workspace);
    make_directory(nested);
    make_directory(repository_bin);
    make_directory(trusted_bin);
    assert(fg_path_join(repository_probe, repository_bin, PROBE_FILE));
    assert(fg_path_join(trusted_probe, trusted_bin, PROBE_FILE));
    copy_program(self, trusted_probe);

    /* An executable candidate that cannot run catches selecting the forbidden
     * first PATH directory without depending on a shell or an installed tool. */
    FILE *file = fopen(repository_probe, "wb");
    assert(file);
    assert(fputs("repository-owned executable candidate\n", file) >= 0);
    assert(fclose(file) == 0);
#ifndef _WIN32
    assert(chmod(repository_probe, 0700) == 0);
#endif
}

static void marker_path(const char *directory, char path[FG_PATH_MAX]) {
    assert(fg_path_join(path, directory, marker_name));
}

static void expect_no_marker(void) {
    char path[FG_PATH_MAX];
    marker_path(workspace, path);
    assert(!exists(path));
    marker_path(nested, path);
    assert(!exists(path));
}

static void expect_marker(const char *directory) {
    char path[FG_PATH_MAX];
    marker_path(directory, path);
    assert(exists(path));
    assert(remove(path) == 0);
    expect_no_marker();
}

static void expect_started(fg_process_result *result) {
    assert(result->started && result->exit_code == 0);
    assert(!result->cancelled && !result->timed_out && !result->truncated);
    assert(result->out && strstr(result->out, "process probe started"));
    assert(result->err && result->err_len == 0);
    fg_process_free(result);
}

static void expect_not_started(const fg_process_result *result) {
    assert(!result->started && result->exit_code == -1);
    assert(!result->out && !result->err && !result->out_len && !result->err_len);
    expect_no_marker();
}

static void test_workspace_path_boundary(void) {
    const char *command[] = {"forge_process_probe", "--process-probe", marker_name, NULL};
    forge_error error = {0};
    fg_process_result result = {0};
    char path[FG_PATH_MAX * 2 + 2];
    int length =
        snprintf(path, sizeof(path), "%s%s%s", repository_bin, PATH_SEPARATOR, trusted_bin);
    assert(length > 0 && (size_t)length < sizeof(path));
    set_path(path);
    assert(fg_process_at(workspace, nested, command, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_OK);
    expect_started(&result);
    expect_marker(nested);

    /* The compatibility wrapper keeps cwd equal to the trust root. */
    assert(fg_process(workspace, command, 5000, 4096, NULL, NULL, &result, &error) == FORGE_OK);
    expect_started(&result);
    expect_marker(workspace);

    char spelled_root[FG_PATH_MAX];
    assert(fg_path_join(spelled_root, nested, ".."));
    assert(fg_process_at(spelled_root, nested, command, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_OK);
    expect_started(&result);
    expect_marker(nested);

    set_path(repository_bin);
    assert(fg_process_at(workspace, nested, command, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_ERR_NOT_FOUND);
    expect_not_started(&result);
    fg_process_free(&result);

#ifndef _WIN32
    /* Every absolute PATH entry is inside a filesystem-root workspace. */
    set_path(trusted_bin);
    assert(fg_process_at("/", nested, command, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_ERR_NOT_FOUND);
    expect_not_started(&result);
    fg_process_free(&result);
#endif
}

static void test_workspace_alias(void) {
    char alias[FG_PATH_MAX], alias_bin[FG_PATH_MAX];
    assert(fg_path_join(alias, temporary, "workspace-alias"));
#ifdef _WIN32
    /* No elevation or developer-mode setting changes; optional when the host
     * does not permit creating directory symlinks. The bin endpoint is not a
     * symlink, so filtering must resolve its ancestor rather than only stat it. */
    if (!CreateSymbolicLinkA(alias, workspace, SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
        puts("process test: directory symlink case unavailable on this host");
        return;
    }
#else
    assert(symlink(workspace, alias) == 0);
#endif
    assert(fg_path_join(alias_bin, alias, "bin"));
    set_path(alias_bin);
    const char *command[] = {"forge_process_probe", "--process-probe", marker_name, NULL};
    forge_error error = {0};
    fg_process_result result = {0};
    assert(fg_process_at(workspace, nested, command, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_ERR_NOT_FOUND);
    expect_not_started(&result);
    fg_process_free(&result);
#ifdef _WIN32
    assert(RemoveDirectoryA(alias));
#else
    assert(unlink(alias) == 0);
#endif
}

static void test_explicit_paths(void) {
    /* Explicit argv[0] paths are authorized by the caller's existing process
     * permission, not the implicit PATH rule. Relative paths use the child cwd. */
    copy_program(self, repository_probe);
    set_path(repository_bin);
    const char *relative[] = {"../bin/" PROBE_FILE, "--process-probe", marker_name, NULL};
    const char *absolute[] = {repository_probe, "--process-probe", marker_name, NULL};
    forge_error error = {0};
    fg_process_result result = {0};
    assert(fg_process_at(workspace, nested, relative, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_OK);
    expect_started(&result);
    expect_marker(nested);
    assert(fg_process_at(workspace, nested, absolute, 86400000, 4096, NULL, NULL, &result,
                         &error) == FORGE_OK);
    expect_started(&result);
    expect_marker(nested);
}

typedef struct {
    unsigned calls, cancel_on;
    bool delay;
} cancellation;

static bool cancel_process(void *userdata) {
    cancellation *state = userdata;
    state->calls++;
    if (state->delay) {
#ifdef _WIN32
        Sleep(40);
#else
        struct timespec remaining = {0, 40000000};
        while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
        }
#endif
    }
    return state->cancel_on && state->calls >= state->cancel_on;
}

static void test_prelaunch_cancellation(void) {
    const char *command[] = {repository_probe, "--process-probe", marker_name, NULL};
    forge_error error = {0};
    fg_process_result result = {0};
    cancellation state = {0, 1, false};
    assert(fg_process_at(workspace, nested, command, 5000, 4096, cancel_process, &state, &result,
                         &error) == FORGE_ERR_CANCELLED);
    assert(state.calls == 1 && result.cancelled && !result.timed_out);
    expect_not_started(&result);
    fg_process_free(&result);

    /* Cancellation wins before either directory or executable resolution. */
    char missing[FG_PATH_MAX];
    assert(fg_path_join(missing, workspace, "missing"));
    state.calls = 0;
    assert(fg_process_at(missing, missing, command, 5000, 4096, cancel_process, &state, &result,
                         &error) == FORGE_ERR_CANCELLED);
    assert(state.calls == 1 && result.cancelled);
    expect_not_started(&result);
    fg_process_free(&result);
    const char *not_found[] = {"forge_nonexistent_process_for_test", NULL};
    state.calls = 0;
    assert(fg_process_at(workspace, nested, not_found, 5000, 4096, cancel_process, &state, &result,
                         &error) == FORGE_ERR_CANCELLED);
    assert(state.calls == 1 && result.cancelled);
    expect_not_started(&result);
    fg_process_free(&result);

    /* The second check occurs after preparation, immediately before OS spawn. */
    state = (cancellation){0, 2, false};
    assert(fg_process_at(workspace, nested, command, 5000, 4096, cancel_process, &state, &result,
                         &error) == FORGE_ERR_CANCELLED);
    assert(state.calls == 2 && result.cancelled);
    expect_not_started(&result);
    fg_process_free(&result);

    state = (cancellation){0, 0, true};
    assert(fg_process_at(workspace, nested, command, 1, 4096, cancel_process, &state, &result,
                         &error) == FORGE_ERR_LIMIT);
    assert(state.calls == 1 && result.timed_out && !result.cancelled);
    expect_not_started(&result);
    fg_process_free(&result);
}

static void test_invalid_requests(void) {
    const char *command[] = {repository_probe, "--process-probe", marker_name, NULL};
    uint64_t invalid_timeouts[] = {0, 86400001, UINT64_MAX};
    forge_error error = {0};
    fg_process_result result = {0};
    for (size_t i = 0; i < sizeof(invalid_timeouts) / sizeof(invalid_timeouts[0]); i++) {
        result.exit_code = 0;
        result.started = true;
        assert(fg_process_at(workspace, nested, command, invalid_timeouts[i], 4096, NULL, NULL,
                             &result, &error) == FORGE_ERR_ARGUMENT);
        expect_not_started(&result);
        fg_process_free(&result);
    }
    assert(fg_process_at(workspace, nested, command, 5000, 4096, NULL, NULL, NULL, &error) ==
           FORGE_ERR_ARGUMENT);
    expect_no_marker();
    assert(fg_process_at(workspace, nested, command, 5000, 0, NULL, NULL, &result, &error) ==
           FORGE_ERR_ARGUMENT);
    expect_not_started(&result);
    fg_process_free(&result);
    const char *not_found[] = {"forge_nonexistent_process_for_test", NULL};
    set_path(trusted_bin);
    assert(fg_process_at(workspace, nested, not_found, 5000, 4096, NULL, NULL, &result, &error) ==
           FORGE_ERR_NOT_FOUND);
    expect_not_started(&result);
    fg_process_free(&result);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Keep assertion failures in test output instead of opening a CRT dialog. */
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (argc == 3 && !strcmp(argv[1], "--process-probe")) {
        FILE *marker = fopen(argv[2], "wb");
        if (!marker)
            return 91;
        if (fputs("child ran\n", marker) < 0 || fclose(marker) != 0)
            return 92;
        puts("process probe started");
        return 0;
    }
    const char *environment_path = getenv("PATH");
    char *original_path = environment_path ? fg_strdup(environment_path) : NULL;
    assert(!environment_path || original_path);
    setup(argv[0]);
    test_workspace_path_boundary();
    test_workspace_alias();
    test_explicit_paths();
    test_prelaunch_cancellation();
    test_invalid_requests();
    set_path(original_path);
    free(original_path);
    expect_no_marker();
    assert(remove(repository_probe) == 0);
    assert(remove(trusted_probe) == 0);
    remove_directory(repository_bin);
    remove_directory(trusted_bin);
    remove_directory(nested);
    remove_directory(workspace);
    remove_directory(temporary);
    puts("process tests passed");
    return 0;
}
