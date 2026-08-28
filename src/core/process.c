#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "internal.h"
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

void fg_process_free(fg_process_result *r) {
    free(r->out);
    free(r->err);
    memset(r, 0, sizeof(*r));
}
static void capture(fg_buf *b, const char *p, size_t n, size_t cap, bool *truncated) {
    size_t take = FG_MIN(n, cap - b->len);
    if (take < n)
        *truncated = true;
    fg_buf_add(b, p, take);
}
static bool executable_path(const char *root, const char *name, char path[FG_PATH_MAX]) {
    if (strchr(name, '/') || strchr(name, '\\')) {
        if (strlen(name) >= FG_PATH_MAX)
            return false;
        strcpy(path, name);
        return true;
    }
    const char *env = getenv("PATH");
    if (!env)
        return false;
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    const char *p = env;
    while (*p) {
        const char *z = strchr(p, separator);
        size_t n = z ? (size_t)(z - p) : strlen(p);
        char dir[FG_PATH_MAX], canonical[FG_PATH_MAX];
        if (n && n < sizeof(dir)) {
            memcpy(dir, p, n);
            dir[n] = 0;
#ifdef _WIN32
            if (dir[0] == '"' && n > 1 && dir[n - 1] == '"') {
                memmove(dir, dir + 1, n - 2);
                dir[n - 2] = 0;
            }
            bool absolute = strlen(dir) > 2 && dir[1] == ':';
#else
            bool absolute = dir[0] == '/';
#endif
            if (absolute && fg_workspace(dir, canonical, NULL)) {
                size_t root_len = strlen(root);
#ifdef _WIN32
                bool in_root = !_strnicmp(canonical, root, root_len) &&
                               (!canonical[root_len] || canonical[root_len] == '/' ||
                                canonical[root_len] == '\\');
#else
                bool in_root = !strncmp(canonical, root, root_len) &&
                               (!canonical[root_len] || canonical[root_len] == '/');
#endif
                if (!in_root && fg_path_join(path, canonical, name)) {
#ifdef _WIN32
                    if (!strchr(name, '.') && strlen(path) + 4 < FG_PATH_MAX)
                        strcat(path, ".exe");
                    DWORD attr = GetFileAttributesA(path);
                    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                        return true;
#else
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0)
                        return true;
#endif
                }
            }
        }
        if (!z)
            break;
        p = z + 1;
    }
    return false;
}
#ifdef _WIN32
static void quote_arg(fg_buf *b, const char *s) {
    fg_buf_puts(b, "\"");
    size_t slashes = 0;
    for (; *s; s++) {
        if (*s == '\\') {
            slashes++;
            continue;
        }
        if (*s == '"') {
            for (size_t j = 0; j < slashes * 2 + 1; j++)
                fg_buf_puts(b, "\\");
        } else
            for (size_t j = 0; j < slashes; j++)
                fg_buf_puts(b, "\\");
        fg_buf_add(b, s, 1);
        slashes = 0;
    }
    for (size_t j = 0; j < slashes * 2; j++)
        fg_buf_puts(b, "\\");
    fg_buf_puts(b, "\"");
}
static void drain(HANDLE h, fg_buf *b, size_t cap, bool *trunc) {
    DWORD avail = 0, n = 0;
    char block[4096];
    size_t reads = 0;
    while (reads++ < 64 && PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail) {
        if (!ReadFile(h, block, (DWORD)FG_MIN(sizeof(block), avail), &n, NULL) || !n)
            break;
        capture(b, block, n, cap, trunc);
    }
}
#endif
#ifndef _WIN32
static bool child_pipe(int descriptors[2]) {
    if (pipe(descriptors) != 0)
        return false;
    for (size_t i = 0; i < 2; i++) {
        int fd = descriptors[i];
        if (fd < 3) {
            descriptors[i] = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            close(fd);
        } else if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            close(descriptors[0]);
            close(descriptors[1]);
            return false;
        }
        if (descriptors[i] < 0) {
            close(descriptors[1 - i]);
            return false;
        }
    }
    return true;
}
static void close_child_descriptors(long maximum) {
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
        return;
#endif
    for (long fd = 3; fd < maximum; fd++)
        close((int)fd);
}
#endif
forge_status fg_process(const char *root, const char *const *argv, uint64_t timeout,
                        size_t max_bytes, forge_cancel_fn cancel, void *user, fg_process_result *r,
                        forge_error *e) {
    if (!root || !argv || !argv[0] || !*argv[0] || !max_bytes || !timeout)
        return fg_error(e, FORGE_ERR_ARGUMENT, "Invalid process request");
    char executable[FG_PATH_MAX], canonical_root[FG_PATH_MAX];
    if (!fg_workspace(root, canonical_root, e))
        return FORGE_ERR_IO;
    if (!executable_path(canonical_root, argv[0], executable))
        return fg_error(e, FORGE_ERR_NOT_FOUND,
                        "Executable not found outside workspace PATH entries: %s", argv[0]);
    memset(r, 0, sizeof(*r));
    r->exit_code = -1;
    uint64_t start = fg_now_ms();
    fg_buf out = {0}, err = {0};
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL, job = NULL, null_in = NULL;
    PROCESS_INFORMATION pi = {0};
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
    bool attributes_ready = false;
    fg_buf command = {0}, env = {0};
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0))
        goto win_fail;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    null_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, NULL);
    if (null_in == INVALID_HANDLE_VALUE) {
        null_in = NULL;
        goto win_fail;
    }
    for (size_t i = 0; argv[i]; i++) {
        if (i)
            fg_buf_puts(&command, " ");
        quote_arg(&command, argv[i]);
    }
    /* Child processes receive a small allowlist, not API keys or the parent's full environment. */
    const char *keys[] = {"PATH",        "SystemRoot",   "TEMP",    "TMP",
                          "USERPROFILE", "LOCALAPPDATA", "APPDATA", NULL};
    for (size_t i = 0; keys[i]; i++) {
        const char *v = getenv(keys[i]);
        if (v) {
            fg_buf_printf(&env, "%s=%s", keys[i], v);
            fg_buf_add(&env, "", 1);
        }
    }
    fg_buf_add(&env, "", 1);
    job = CreateJobObjectA(NULL, NULL);
    if (!job)
        goto win_fail;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        goto win_fail;
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_bytes);
    attributes = malloc(attribute_bytes);
    if (!attributes || !InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes))
        goto win_fail;
    attributes_ready = true;
    HANDLE inherited[] = {out_w, err_w, null_in};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), NULL, NULL))
        goto win_fail;
    STARTUPINFOEXA si = {0};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.StartupInfo.hStdInput = null_in;
    si.lpAttributeList = attributes;
    if (command.failed || env.failed ||
        !CreateProcessA(executable, command.data, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                        env.data, root, &si.StartupInfo, &pi))
        goto win_fail;
    DeleteProcThreadAttributeList(attributes);
    free(attributes);
    attributes = NULL;
    attributes_ready = false;
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 125);
        goto win_fail;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    pi.hThread = NULL;
    CloseHandle(out_w);
    out_w = NULL;
    CloseHandle(err_w);
    err_w = NULL;
    CloseHandle(null_in);
    null_in = NULL;
    for (;;) {
        drain(out_r, &out, max_bytes, &r->truncated);
        drain(err_r, &err, max_bytes, &r->truncated);
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0)
            break;
        if (cancel && cancel(user)) {
            r->cancelled = true;
            TerminateJobObject(job, 130);
            break;
        }
        if (fg_now_ms() - start >= timeout) {
            r->timed_out = true;
            TerminateJobObject(job, 124);
            break;
        }
    }
    WaitForSingleObject(pi.hProcess, 2000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r->exit_code = (int)code;
    /* Closing the job also stops orphaned descendants before draining their pipes. */
    CloseHandle(job);
    job = NULL;
    drain(out_r, &out, max_bytes, &r->truncated);
    drain(err_r, &err, max_bytes, &r->truncated);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    CloseHandle(err_r);
    fg_buf_clear(&command);
    fg_buf_clear(&env);
    goto collected;
win_fail:
    if (attributes_ready)
        DeleteProcThreadAttributeList(attributes);
    free(attributes);
    if (pi.hThread)
        CloseHandle(pi.hThread);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (out_r)
        CloseHandle(out_r);
    if (out_w)
        CloseHandle(out_w);
    if (err_r)
        CloseHandle(err_r);
    if (err_w)
        CloseHandle(err_w);
    if (job)
        CloseHandle(job);
    if (null_in)
        CloseHandle(null_in);
    fg_buf_clear(&command);
    fg_buf_clear(&env);
    fg_buf_clear(&out);
    fg_buf_clear(&err);
    return fg_error(e, FORGE_ERR_IO, "Unable to start command (Windows error %lu)",
                    (unsigned long)GetLastError());
collected:
#else
    int op[2], ep[2];
    if (!child_pipe(op))
        return fg_error(e, FORGE_ERR_IO, "pipe failed");
    if (!child_pipe(ep)) {
        close(op[0]);
        close(op[1]);
        return fg_error(e, FORGE_ERR_IO, "pipe failed");
    }
    /* Allocate before fork: model backends have worker threads, so the child may
     * only use async-signal-safe operations until execve. */
    fg_buf environment = {0};
    const char *keys[] = {"PATH", "HOME", "TMPDIR"};
    const char *defaults[] = {"/usr/bin:/bin", "", "/tmp"};
    size_t offsets[3];
    for (size_t i = 0; i < 3; i++) {
        offsets[i] = environment.len;
        const char *value = getenv(keys[i]);
        fg_buf_printf(&environment, "%s=%s", keys[i], value ? value : defaults[i]);
        fg_buf_add(&environment, "", 1);
    }
    long maximum_fd = sysconf(_SC_OPEN_MAX);
    int input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (input >= 0 && input < 3) {
        int original = input;
        input = fcntl(original, F_DUPFD_CLOEXEC, 3);
        close(original);
    }
    if (environment.failed || maximum_fd < 0 || maximum_fd > INT_MAX || input < 0) {
        if (input >= 0)
            close(input);
        fg_buf_clear(&environment);
        close(op[0]);
        close(op[1]);
        close(ep[0]);
        close(ep[1]);
        return fg_error(e, FORGE_ERR_IO, "Cannot prepare child environment");
    }
    char *child_environment[] = {environment.data + offsets[0], environment.data + offsets[1],
                                 environment.data + offsets[2], "LANG=C.UTF-8", NULL};
    pid_t pid = fork();
    if (pid < 0) {
        close(input);
        fg_buf_clear(&environment);
        close(op[0]);
        close(op[1]);
        close(ep[0]);
        close(ep[1]);
        return fg_error(e, FORGE_ERR_IO, "fork failed");
    }
    if (pid == 0) {
        if (setpgid(0, 0) != 0 || dup2(op[1], STDOUT_FILENO) < 0 ||
            dup2(ep[1], STDERR_FILENO) < 0 || dup2(input, STDIN_FILENO) < 0 || chdir(root) != 0)
            _exit(126);
        close_child_descriptors(maximum_fd);
        execve(executable, (char *const *)argv, child_environment);
        _exit(127);
    }
    close(input);
    fg_buf_clear(&environment);
    setpgid(pid, pid);
    close(op[1]);
    close(ep[1]);
    fcntl(op[0], F_SETFL, O_NONBLOCK);
    fcntl(ep[0], F_SETFL, O_NONBLOCK);
    int status = 0;
    bool done = false;
    char block[4096];
    while (!done) {
        struct pollfd fds[2] = {{op[0], POLLIN, 0}, {ep[0], POLLIN, 0}};
        poll(fds, 2, 10);
        ssize_t n;
        size_t reads = 0;
        while (reads++ < 64 && (n = read(op[0], block, sizeof(block))) > 0)
            capture(&out, block, (size_t)n, max_bytes, &r->truncated);
        reads = 0;
        while (reads++ < 64 && (n = read(ep[0], block, sizeof(block))) > 0)
            capture(&err, block, (size_t)n, max_bytes, &r->truncated);
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            done = true;
            break;
        }
        if (cancel && cancel(user)) {
            r->cancelled = true;
            break;
        }
        if (fg_now_ms() - start >= timeout) {
            r->timed_out = true;
            break;
        }
        if (got < 0 && errno != EINTR)
            break;
    }
    kill(-pid, SIGKILL);
    if (!done)
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    ssize_t n;
    while ((n = read(op[0], block, sizeof(block))) > 0)
        capture(&out, block, (size_t)n, max_bytes, &r->truncated);
    while ((n = read(ep[0], block, sizeof(block))) > 0)
        capture(&err, block, (size_t)n, max_bytes, &r->truncated);
    close(op[0]);
    close(ep[0]);
    r->exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    r->out_len = out.len;
    r->err_len = err.len;
    r->out = fg_buf_take(&out);
    r->err = fg_buf_take(&err);
    r->duration_ms = (double)(fg_now_ms() - start);
    if (!r->out || !r->err) {
        fg_process_free(r);
        return fg_error(e, FORGE_ERR_MEMORY, "Process output allocation failed");
    }
    return FORGE_OK;
}
