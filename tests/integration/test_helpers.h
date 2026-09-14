#ifndef OBICALL_INTEGRATION_TEST_HELPERS_H
#define OBICALL_INTEGRATION_TEST_HELPERS_H

/* Shared helpers for process-level integration tests: a minimal fake
 * "journal" listener (accept one connection, verify AUTH_HELLO, then
 * decode whatever OBSERVATION_SUBMIT frames arrive) and small process
 * spawn/path utilities. Deliberately not reusing obicall-journald itself
 * so these tests can isolate "does the worker really speak the real wire
 * protocol" from "does the real journal's admission logic also work"
 * (that combination is covered by the demo/run-based tests instead). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obicall/obicall.h"
#include "osal.h"
#include "procutil.h"

typedef struct fake_journal {
    osal_socket_t* listener;
    osal_socket_t* conn;
    uint16_t port;
    uint8_t token[OBICALL_RUN_TOKEN_LEN];
} fake_journal_t;

static inline int fake_journal_start(fake_journal_t* fj) {
    memset(fj, 0, sizeof(*fj));
    procutil_random_token(fj->token);
    fj->port = 0;
    return osal_listen_loopback(&fj->port, &fj->listener);
}

/* Blocks (bounded) until a worker connects and completes the AUTH_HELLO
 * handshake. Returns 0 on success. */
static inline int fake_journal_accept_and_authenticate(fake_journal_t* fj, int timeout_ms) {
    osal_socket_t* conn = NULL;
    int r = osal_accept(fj->listener, timeout_ms, &conn);
    if (r != 1) return -1;
    obicall_wire_role_t role;
    if (procutil_server_verify_hello(conn, fj->token, timeout_ms, &role) != 0) {
        osal_close(conn);
        return -1;
    }
    fj->conn = conn;
    return 0;
}

/* Waits for the next OBSERVATION_SUBMIT frame and decodes it. Returns 0
 * on success, -1 on timeout/error. */
static inline int fake_journal_recv_observation(fake_journal_t* fj, int timeout_ms, obicall_observation_t* out) {
    for (;;) {
        uint8_t msg_type;
        uint8_t payload[2048];
        uint32_t payload_len;
        int r = osal_recv_frame(fj->conn, &msg_type, payload, sizeof(payload), &payload_len, timeout_ms);
        if (r != 0) return -1;
        if (msg_type != OBICALL_MSG_OBSERVATION_SUBMIT) continue;
        return obicall_wire_decode_observation(payload, payload_len, out) == OBICALL_OK ? 0 : -1;
    }
}

static inline void fake_journal_stop(fake_journal_t* fj) {
    if (fj->conn) osal_close(fj->conn);
    if (fj->listener) osal_close(fj->listener);
}

static inline int spawn_and_wait(const char* exe_path, const char** argv, int argc, int timeout_ms,
                                  int* out_exit_code) {
    (void)argc;
    osal_process_spawn_opts_t opts;
    opts.exe_path = exe_path;
    opts.argv = argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    if (osal_process_spawn(&opts, &proc) != 0) return -1;
    int r = osal_process_wait(proc, timeout_ms, out_exit_code);
    if (r == 0) {
        osal_process_kill(proc);
        osal_process_wait(proc, 2000, NULL);
    }
    osal_process_close(proc);
    return r == 1 ? 0 : -1;
}

#endif /* OBICALL_INTEGRATION_TEST_HELPERS_H */
