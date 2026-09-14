#include "test_helpers.h"

/* Confirms the --test-crash-after-ms fault-injection hook (compiled in
 * only under OBICALL_TEST_HOOKS, see worker_main.c) actually terminates
 * the worker process abnormally and on schedule. The supervisor's own
 * bounded-retry restart of a dead child is exercised for real by the
 * broker-failover demo test (test_demo_broker_failover) and was also
 * observed directly during development (docs/VALIDATION.md) - this test
 * isolates just the crash trigger itself. */

#ifndef OBICALL_WORKERD_PATH
#error "OBICALL_WORKERD_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_MANIFEST_C_SIM
#error "OBICALL_TEST_MANIFEST_C_SIM must be defined by the build"
#endif
#ifndef OBICALL_TEST_RUNTIME_DIR
#error "OBICALL_TEST_RUNTIME_DIR must be defined by the build"
#endif

int main(void) {
    osal_net_init();
    osal_mkdir_p(OBICALL_TEST_RUNTIME_DIR);

    fake_journal_t fj;
    if (fake_journal_start(&fj) != 0) {
        fprintf(stderr, "FAIL: could not start fake journal listener\n");
        return 1;
    }
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)fj.port);
    char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    procutil_hex_encode(fj.token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));

    const char* argv[] = {OBICALL_WORKERD_PATH,
                           "--manifest", OBICALL_TEST_MANIFEST_C_SIM,
                           "--journal-port", port_s,
                           "--token", token_hex,
                           "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                           "--worker-name", "test_worker_crash",
                           "--instance-seed", "1",
                           "--tick-ms", "20",
                           "--test-crash-after-ms", "300",
                           NULL};
    osal_process_spawn_opts_t opts;
    opts.exe_path = OBICALL_WORKERD_PATH;
    opts.argv = argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    if (osal_process_spawn(&opts, &proc) != 0) {
        fprintf(stderr, "FAIL: could not spawn obicall-workerd\n");
        return 1;
    }

    int failures = 0;
    /* Must still be alive shortly after start (before the scheduled
     * crash) - proves it's the timer firing, not an unrelated failure. */
    osal_sleep_ms(100);
    if (!osal_process_is_alive(proc)) {
        fprintf(stderr, "FAIL: worker died before its scheduled crash time\n");
        failures++;
    }

    int exit_code = 0;
    int r = osal_process_wait(proc, 3000, &exit_code);
    if (r != 1) {
        fprintf(stderr, "FAIL: worker did not terminate within 3s of its scheduled crash\n");
        failures++;
        osal_process_kill(proc);
        osal_process_wait(proc, 2000, NULL);
    } else if (exit_code == 0) {
        fprintf(stderr, "FAIL: worker exited cleanly (0) instead of crashing\n");
        failures++;
    } else {
        fprintf(stderr, "ok: worker terminated abnormally as scheduled (exit code %d)\n", exit_code);
    }

    osal_process_close(proc);
    fake_journal_stop(&fj);

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
