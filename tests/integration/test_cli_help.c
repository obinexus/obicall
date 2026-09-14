/* CLI regression test for --help / -h / help support (see src/cli/main.c).
 *
 * Requirements under test:
 *   - `obicall --help`, `obicall -h`, and `obicall help` each print the
 *     usage text to stdout and exit 0.
 *   - `obicall <unknown-command>` prints an error to stderr, still prints
 *     the usage text (to stderr), and exits nonzero.
 *   - `obicall` with no arguments also prints usage to stderr and exits
 *     nonzero (pre-existing behavior, must stay unchanged).
 *
 * osal_process_spawn (test_helpers.h) does not support redirecting a
 * child's stdout/stderr independently, which this test needs in order to
 * check the two streams separately - so this test intentionally does not
 * use it, and instead shells out via the standard C `system()` with file
 * redirection understood by both cmd.exe and a POSIX sh. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OBICALL_TEST_CLI_PATH
#error "OBICALL_TEST_CLI_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_RUNTIME_DIR
#error "OBICALL_TEST_RUNTIME_DIR must be defined by the build"
#endif

#if defined(_WIN32)
#include <direct.h>
static void ensure_dir(const char* path) { _mkdir(path); }
#else
#include <sys/stat.h>
#include <sys/wait.h>
static void ensure_dir(const char* path) { mkdir(path, 0755); }
#endif

/* system()'s return value is a raw wait status on POSIX (needs
 * WEXITSTATUS) but is already the child's exit code on Windows. */
static int child_exit_code(int system_rc) {
#if defined(_WIN32)
    return system_rc;
#else
    if (system_rc == -1) return -1;
    return WIFEXITED(system_rc) ? WEXITSTATUS(system_rc) : -1;
#endif
}

static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int run_case(const char* label, const char* arg, int expect_exit_zero, int expect_stdout_usage,
                     int expect_stderr_usage) {
    char out_path[512], err_path[512], cmd[1200];
    snprintf(out_path, sizeof(out_path), "%s/cli_help_%s.out", OBICALL_TEST_RUNTIME_DIR, label);
    snprintf(err_path, sizeof(err_path), "%s/cli_help_%s.err", OBICALL_TEST_RUNTIME_DIR, label);
#if defined(_WIN32)
    /* system() on Windows runs the string via `cmd.exe /c <string>`, and
     * cmd.exe has a documented quirk: when that string itself starts and
     * ends with a quote (as it does here, from quoting the exe path), cmd
     * strips what it thinks is a single enclosing quote pair, which is
     * wrong when there are further quoted segments after it (the
     * redirection targets) and breaks the path. Wrapping the whole
     * command in one extra pair of quotes is the standard workaround. */
    const char* q = "\"";
#else
    const char* q = "";
#endif
    if (arg)
        snprintf(cmd, sizeof(cmd), "%s\"%s\" %s >\"%s\" 2>\"%s\"%s", q, OBICALL_TEST_CLI_PATH, arg, out_path,
                  err_path, q);
    else
        snprintf(cmd, sizeof(cmd), "%s\"%s\" >\"%s\" 2>\"%s\"%s", q, OBICALL_TEST_CLI_PATH, out_path, err_path, q);

    int exit_code = child_exit_code(system(cmd));
    int failures = 0;

    if (expect_exit_zero && exit_code != 0) {
        fprintf(stderr, "FAIL [%s]: expected exit 0, got %d\n", label, exit_code);
        failures++;
    }
    if (!expect_exit_zero && exit_code == 0) {
        fprintf(stderr, "FAIL [%s]: expected a nonzero exit, got 0\n", label);
        failures++;
    }

    int stdout_has_usage = file_contains(out_path, "usage: obicall");
    int stderr_has_usage = file_contains(err_path, "usage: obicall");

    if (expect_stdout_usage && !stdout_has_usage) {
        fprintf(stderr, "FAIL [%s]: expected usage text on stdout, was not found\n", label);
        failures++;
    }
    if (!expect_stdout_usage && stdout_has_usage) {
        fprintf(stderr, "FAIL [%s]: usage text leaked onto stdout\n", label);
        failures++;
    }
    if (expect_stderr_usage && !stderr_has_usage) {
        fprintf(stderr, "FAIL [%s]: expected usage text on stderr, was not found\n", label);
        failures++;
    }
    if (!expect_stderr_usage && stderr_has_usage) {
        fprintf(stderr, "FAIL [%s]: unexpected usage text on stderr\n", label);
        failures++;
    }
    /* An explicit help request must not also write anything to stderr. */
    if (expect_stdout_usage && file_size(err_path) > 0) {
        fprintf(stderr, "FAIL [%s]: explicit help request wrote to stderr\n", label);
        failures++;
    }

    if (failures == 0) fprintf(stderr, "ok [%s]: exit=%d\n", label, exit_code);
    return failures;
}

int main(void) {
    ensure_dir(OBICALL_TEST_RUNTIME_DIR);
    int failures = 0;

    failures += run_case("long_help", "--help", 1, 1, 0);
    failures += run_case("short_help", "-h", 1, 1, 0);
    failures += run_case("bare_help", "help", 1, 1, 0);
    failures += run_case("no_args", NULL, 0, 0, 1);
    failures += run_case("unknown_command", "bogus-command", 0, 0, 1);

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
