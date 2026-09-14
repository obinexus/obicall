#include "test_helpers.h"

/* provider_bad_abi (tests/fixtures) declares abi_version_major one past
 * what this build supports; obicall-workerd must detect that from the
 * real obicall_plugin_query_v1 call and refuse to proceed, rather than
 * loading it and behaving unpredictably. */

#ifndef OBICALL_WORKERD_PATH
#error "OBICALL_WORKERD_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_MANIFEST_BAD_ABI
#error "OBICALL_TEST_MANIFEST_BAD_ABI must be defined by the build"
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
                           "--manifest", OBICALL_TEST_MANIFEST_BAD_ABI,
                           "--journal-port", port_s,
                           "--token", token_hex,
                           "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                           "--worker-name", "test_worker_bad_abi",
                           "--instance-seed", "1",
                           NULL};
    osal_process_spawn_opts_t opts;
    opts.exe_path = OBICALL_WORKERD_PATH;
    opts.argv = argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    int failures = 0;

    if (osal_process_spawn(&opts, &proc) != 0) {
        fprintf(stderr, "FAIL: could not spawn obicall-workerd\n");
        return 1;
    }

    /* It must never complete the journal handshake. */
    if (fake_journal_accept_and_authenticate(&fj, 2000) == 0) {
        fprintf(stderr, "FAIL: worker connected to journal despite an incompatible ABI\n");
        failures++;
    }

    int exit_code = -1;
    int r = osal_process_wait(proc, 3000, &exit_code);
    if (r != 1) {
        fprintf(stderr, "FAIL: worker did not exit on its own after refusing the bad plugin\n");
        failures++;
        osal_process_kill(proc);
        osal_process_wait(proc, 2000, NULL);
    } else if (exit_code == 0) {
        fprintf(stderr, "FAIL: worker exited 0 despite an incompatible ABI (should be nonzero)\n");
        failures++;
    } else {
        fprintf(stderr, "ok: worker refused the incompatible plugin (exit code %d)\n", exit_code);
    }

    osal_process_close(proc);
    fake_journal_stop(&fj);

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
