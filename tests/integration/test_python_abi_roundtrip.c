#include "test_helpers.h"

/* Real ctypes-driven round trip: spawns python/obicall/provider_worker.py
 * as a genuine subprocess, which loads the real compiled libobicall via
 * ctypes, builds an observation, validates and wire-encodes it through
 * the real C functions (not a Python reimplementation), and sends it
 * over a real socket using the real framing - a fake journal listener
 * here decodes it with the same C decoder to close the loop. Skips
 * (exit 125, see tests/CMakeLists.txt SKIP_RETURN_CODE) if no Python
 * interpreter is available. */

#ifndef OBICALL_TEST_PY_SCRIPT
#error "OBICALL_TEST_PY_SCRIPT must be defined by the build"
#endif
#ifndef OBICALL_TEST_MANIFEST_PY
#error "OBICALL_TEST_MANIFEST_PY must be defined by the build"
#endif
#ifndef OBICALL_TEST_CORE_LIB_PATH
#error "OBICALL_TEST_CORE_LIB_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_RUNTIME_DIR
#error "OBICALL_TEST_RUNTIME_DIR must be defined by the build"
#endif

#define SKIP_CODE 125

static int find_python(char* out, size_t cap) {
    const char* env = getenv("OBICALL_PYTHON");
    if (env) {
        FILE* f = fopen(env, "rb");
        if (f) { fclose(f); strncpy(out, env, cap - 1); return 0; }
    }
    if (procutil_find_on_path("python3", out, cap) == 0) return 0;
    if (procutil_find_on_path("python", out, cap) == 0) return 0;
    return -1;
}

int main(void) {
    osal_net_init();
    osal_mkdir_p(OBICALL_TEST_RUNTIME_DIR);

    char python_exe[1024];
    if (find_python(python_exe, sizeof(python_exe)) != 0) {
        fprintf(stderr, "SKIP: no Python interpreter found (set OBICALL_PYTHON or add python3 to PATH)\n");
        return SKIP_CODE;
    }

    fake_journal_t fj;
    if (fake_journal_start(&fj) != 0) {
        fprintf(stderr, "FAIL: could not start fake journal listener\n");
        return 1;
    }
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)fj.port);
    char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    procutil_hex_encode(fj.token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));

    const char* argv[] = {python_exe,
                           OBICALL_TEST_PY_SCRIPT,
                           "--manifest", OBICALL_TEST_MANIFEST_PY,
                           "--journal-port", port_s,
                           "--token", token_hex,
                           "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                           "--worker-name", "test_worker_py",
                           "--instance-seed", "7",
                           "--core-lib", OBICALL_TEST_CORE_LIB_PATH,
                           NULL};
    osal_process_spawn_opts_t opts;
    opts.exe_path = python_exe;
    opts.argv = argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    if (osal_process_spawn(&opts, &proc) != 0) {
        fprintf(stderr, "FAIL: could not spawn python worker\n");
        return 1;
    }

    int failures = 0;
    if (fake_journal_accept_and_authenticate(&fj, 8000) != 0) {
        fprintf(stderr, "FAIL: python worker did not connect/authenticate in time\n");
        failures++;
    } else {
        obicall_observation_t obs;
        if (fake_journal_recv_observation(&fj, 5000, &obs) != 0) {
            fprintf(stderr, "FAIL: did not receive an observation from the python worker\n");
            failures++;
        } else if (strcmp(obs.sensor_id, "imu_a") != 0) {
            fprintf(stderr, "FAIL: unexpected sensor_id '%s'\n", obs.sensor_id);
            failures++;
        } else if (obicall_observation_validate(&obs, NULL, NULL) != OBICALL_OK) {
            fprintf(stderr, "FAIL: observation from python worker failed core validation\n");
            failures++;
        } else {
            fprintf(stderr, "ok: received real observation from python worker sensor=%s x=%.3f y=%.3f seq=%llu\n",
                    obs.sensor_id, obs.payload[0], obs.payload[1], (unsigned long long)obs.sequence);
        }
    }

    osal_process_kill(proc);
    osal_process_wait(proc, 2000, NULL);
    osal_process_close(proc);
    fake_journal_stop(&fj);

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
