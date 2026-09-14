#include "test_helpers.h"

/* Real dynamic loading (dlopen/LoadLibraryExW of the actual compiled
 * provider_c_sim artifact) + real IPC (framed TCP, real AUTH_HELLO, real
 * wire codec) driven end to end through obicall-workerd as a genuine
 * subprocess - not a canned/synthetic observation. Paths are baked in at
 * compile time (see tests/CMakeLists.txt) so this doesn't depend on the
 * test's working directory. */

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
                           "--worker-name", "test_worker_abi",
                           "--instance-seed", "42",
                           "--tick-ms", "10",
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

    if (fake_journal_accept_and_authenticate(&fj, 5000) != 0) {
        fprintf(stderr, "FAIL: worker did not connect/authenticate in time\n");
        failures++;
    } else {
        int got_valid = 0;
        for (int i = 0; i < 5 && !got_valid; ++i) {
            obicall_observation_t obs;
            if (fake_journal_recv_observation(&fj, 3000, &obs) != 0) {
                fprintf(stderr, "FAIL: did not receive an observation in time\n");
                failures++;
                break;
            }
            if (strcmp(obs.sensor_id, "cam_position_a") != 0) continue; /* skip a stray if any */
            if (obs.payload_shape != OBICALL_SHAPE_POSITION_2D || obs.payload_count != 2) {
                fprintf(stderr, "FAIL: unexpected payload shape\n");
                failures++;
                break;
            }
            if (obicall_observation_validate(&obs, NULL, NULL) != OBICALL_OK) {
                fprintf(stderr, "FAIL: real observation failed core validation\n");
                failures++;
                break;
            }
            fprintf(stderr, "ok: received real observation sensor=%s x=%.3f y=%.3f seq=%llu\n", obs.sensor_id,
                    obs.payload[0], obs.payload[1], (unsigned long long)obs.sequence);
            got_valid = 1;
        }
        if (!got_valid && failures == 0) {
            fprintf(stderr, "FAIL: never received the expected sensor's observation\n");
            failures++;
        }
    }

    osal_process_kill(proc);
    osal_process_wait(proc, 2000, NULL);
    osal_process_close(proc);
    fake_journal_stop(&fj);

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
