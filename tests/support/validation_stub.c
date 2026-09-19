/* Deterministic stand-in for the validation interpreter used by scripted
 * agent tests (forge_judge_instrumentation_unit). The test copies it beside
 * its fixture as python[.exe] and puts that directory first on a private PATH,
 * so validation planning, process execution and evidence capture exercise the
 * real code path without a language toolchain or network.
 *
 * Behavior: syntax/compile invocations exit 0 silently; the FIRST unittest
 * invocation reports a deterministic failure and then records a marker beside
 * the executable, so later unittest invocations succeed. The marker lives
 * outside the workspace, so validation input snapshots stay unchanged and a
 * repair can be accepted after the observed failure. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#define PATH_SEP '/'
#endif

static bool executable_dir(char *out, size_t capacity) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)capacity);
    if (!n || n >= capacity)
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", out, capacity - 1);
    if (n <= 0)
        return false;
    out[n] = 0;
#endif
    char *slash = strrchr(out, PATH_SEP);
    if (!slash)
        return false;
    *slash = 0;
    return true;
}

int main(int argc, char **argv) {
    bool unittest_run = false;
    for (int i = 1; i < argc; i++)
        if (strstr(argv[i], "unittest.main"))
            unittest_run = true;
    if (!unittest_run)
        return 0; /* Syntax/compile stages succeed and print nothing. */
    char directory[4096], state[4096];
    if (!executable_dir(directory, sizeof(directory)))
        return 1;
    snprintf(state, sizeof(state), "%s%cpython_stub.state", directory, PATH_SEP);
    FILE *probe = fopen(state, "rb");
    if (probe) {
        fclose(probe);
        printf("OK\n");
        return 0;
    }
    FILE *marker = fopen(state, "wb");
    if (marker) {
        fputs("failed\n", marker);
        fclose(marker);
    }
    printf("F\n"
           "======================================================================\n"
           "FAIL: test_value (test_value.ValueTests)\n"
           "----------------------------------------------------------------------\n"
           "Traceback (most recent call last):\n"
           "  File \"./test_value.py\", line 7, in test_value\n"
           "    self.assertEqual(value(), 2)\n"
           "AssertionError: 0 != 2\n\n"
           "----------------------------------------------------------------------\n"
           "Ran 1 test in 0.001s\n\n"
           "FAILED (failures=1)\n");
    return 1;
}
