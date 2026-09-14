#include "test_helpers.h"

/* Drives a real obicall-gated subprocess directly over the wire protocol
 * (no broker process involved) to exercise: durable persistence across a
 * real process restart, rejection of a replayed duplicate window after
 * recovery, and refusal to start on a corrupted state file. */

#ifndef OBICALL_GATED_PATH
#error "OBICALL_GATED_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_RUNTIME_DIR
#error "OBICALL_TEST_RUNTIME_DIR must be defined by the build"
#endif

static int spawn_gate(const char* state_file, uint8_t token[OBICALL_RUN_TOKEN_LEN], uint16_t* out_port,
                       osal_process_t** out_proc) {
    char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    procutil_hex_encode(token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));
    procutil_clear_port_file(OBICALL_TEST_RUNTIME_DIR, "gate");

    const char* argv[] = {OBICALL_GATED_PATH,
                           "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                           "--token", token_hex,
                           "--state-file", state_file,
                           "--result-validity-ms", "2000",
                           "--suspect-timeout-ms", "300",
                           "--confirm-timeout-ms", "700",
                           NULL};
    osal_process_spawn_opts_t opts;
    opts.exe_path = OBICALL_GATED_PATH;
    opts.argv = argv;
    opts.envp = NULL;
    if (osal_process_spawn(&opts, out_proc) != 0) return -1;
    return procutil_read_port_file(OBICALL_TEST_RUNTIME_DIR, "gate", 5000, out_port);
}

static int send_heartbeat_get_reply(uint16_t port, uint8_t* token, uint32_t broker_id, uint64_t last_window_seq,
                                     obicall_msg_status_reply_t* out_reply) {
    osal_socket_t* conn = NULL;
    if (procutil_client_connect(port, token, OBICALL_ROLE_BROKER, 2000, &conn) != 0) return -1;
    obicall_msg_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.broker_id = broker_id;
    hb.last_window_seq = last_window_seq;
    hb.checkpoint_schema_version = OBICALL_CHECKPOINT_SCHEMA_VERSION;
    hb.healthy = 1;
    uint8_t buf[128];
    uint32_t len = 0;
    obicall_wire_encode_heartbeat(&hb, buf, sizeof(buf), &len);
    int rc = -1;
    if (osal_send_frame(conn, OBICALL_MSG_HEARTBEAT, buf, len, 2000) == 0) {
        uint8_t mt, rbuf[64];
        uint32_t rlen;
        if (osal_recv_frame(conn, &mt, rbuf, sizeof(rbuf), &rlen, 2000) == 0 && mt == OBICALL_MSG_STATUS_REPLY) {
            rc = obicall_wire_decode_status_reply(rbuf, rlen, out_reply) == OBICALL_OK ? 0 : -1;
        }
    }
    osal_close(conn);
    return rc;
}

static int publish_get_ack(uint16_t port, uint8_t* token, uint32_t broker_id, uint64_t epoch, uint64_t window_seq,
                            obicall_msg_result_ack_t* out_ack) {
    osal_socket_t* conn = NULL;
    if (procutil_client_connect(port, token, OBICALL_ROLE_BROKER, 2000, &conn) != 0) return -1;
    obicall_result_t r;
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.schema_version = OBICALL_RESULT_SCHEMA_VERSION;
    strncpy(r.pipeline_id, "test-pipeline", OBICALL_PIPELINE_ID_LEN - 1);
    r.window_seq = window_seq;
    r.broker_id = broker_id;
    r.epoch = epoch;
    r.status = OBICALL_RESULT_VALID;
    r.timestamp_ns = osal_monotonic_ns();
    r.valid_until_ns = r.timestamp_ns + 2000000000LL;
    uint8_t buf[1024];
    uint32_t len = 0;
    obicall_wire_encode_result(&r, buf, sizeof(buf), &len);
    int rc = -1;
    if (osal_send_frame(conn, OBICALL_MSG_RESULT_PUBLISH, buf, len, 2000) == 0) {
        uint8_t mt, abuf[64];
        uint32_t alen;
        if (osal_recv_frame(conn, &mt, abuf, sizeof(abuf), &alen, 2000) == 0 && mt == OBICALL_MSG_RESULT_ACK) {
            rc = obicall_wire_decode_result_ack(abuf, alen, out_ack) == OBICALL_OK ? 0 : -1;
        }
    }
    osal_close(conn);
    return rc;
}

int main(void) {
    osal_net_init();
    osal_mkdir_p(OBICALL_TEST_RUNTIME_DIR);
    int failures = 0;

    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s/test_gate_restart.state", OBICALL_TEST_RUNTIME_DIR);
    remove(state_file);

    uint8_t token[OBICALL_RUN_TOKEN_LEN];
    procutil_random_token(token);

    /* --- First run: bootstrap, commit one window, kill. --- */
    osal_process_t* proc1 = NULL;
    uint16_t port1 = 0;
    if (spawn_gate(state_file, token, &port1, &proc1) != 0) {
        fprintf(stderr, "FAIL: gate did not start (first run)\n");
        return 1;
    }
    obicall_msg_status_reply_t reply;
    if (send_heartbeat_get_reply(port1, token, OBICALL_BROKER_A, 0, &reply) != 0 || reply.owner_broker_id != OBICALL_BROKER_A) {
        fprintf(stderr, "FAIL: bootstrap grant to A did not happen\n");
        failures++;
    }
    obicall_msg_result_ack_t ack;
    if (publish_get_ack(port1, token, OBICALL_BROKER_A, reply.gate_epoch, 1, &ack) != 0 || !ack.accepted) {
        fprintf(stderr, "FAIL: first publish was not committed\n");
        failures++;
    }
    osal_process_kill(proc1);
    osal_process_wait(proc1, 2000, NULL);
    osal_process_close(proc1);

    FILE* check = fopen(state_file, "rb");
    if (!check) {
        fprintf(stderr, "FAIL: state file was not persisted\n");
        failures++;
    } else {
        fclose(check);
    }

    /* --- Second run: recover, confirm epoch/owner survived, and that
     * replaying the same window again is rejected as a duplicate. --- */
    osal_process_t* proc2 = NULL;
    uint16_t port2 = 0;
    if (spawn_gate(state_file, token, &port2, &proc2) != 0) {
        fprintf(stderr, "FAIL: gate did not start (second run)\n");
        failures++;
    } else {
        obicall_msg_status_reply_t reply2;
        /* A's heartbeat should see itself still recognized as owner
         * (recovered state), not treated as a fresh bootstrap needing a
         * new epoch. */
        if (send_heartbeat_get_reply(port2, token, OBICALL_BROKER_A, 1, &reply2) != 0) {
            fprintf(stderr, "FAIL: heartbeat after restart failed\n");
            failures++;
        } else if (reply2.owner_broker_id != OBICALL_BROKER_A || reply2.gate_epoch != reply.gate_epoch) {
            fprintf(stderr, "FAIL: recovered state mismatch (owner=%u epoch=%llu, expected owner=%u epoch=%llu)\n",
                    reply2.owner_broker_id, (unsigned long long)reply2.gate_epoch, OBICALL_BROKER_A,
                    (unsigned long long)reply.gate_epoch);
            failures++;
        } else if (reply2.last_committed_window_seq != 1) {
            fprintf(stderr, "FAIL: committed window high-water mark lost across restart\n");
            failures++;
        } else {
            fprintf(stderr, "ok: gate recovered epoch=%llu owner=A last_committed=1\n",
                    (unsigned long long)reply2.gate_epoch);
        }

        obicall_msg_result_ack_t ack2;
        if (publish_get_ack(port2, token, OBICALL_BROKER_A, reply2.gate_epoch, 1, &ack2) != 0) {
            fprintf(stderr, "FAIL: replay publish request failed outright\n");
            failures++;
        } else if (ack2.accepted) {
            fprintf(stderr, "FAIL: replayed duplicate window was committed a second time\n");
            failures++;
        } else {
            fprintf(stderr, "ok: replayed duplicate window correctly rejected (status=%s)\n",
                    obicall_status_string(ack2.reject_status));
        }

        osal_process_kill(proc2);
        osal_process_wait(proc2, 2000, NULL);
        osal_process_close(proc2);
    }

    /* --- Third run: corrupt the state file, gate must refuse to start. --- */
    {
        FILE* f = fopen(state_file, "r+b");
        if (f) {
            fseek(f, 20, SEEK_SET);
            uint8_t b;
            fread(&b, 1, 1, f);
            fseek(f, 20, SEEK_SET);
            b ^= 0xFF;
            fwrite(&b, 1, 1, f);
            fclose(f);
        }
        procutil_clear_port_file(OBICALL_TEST_RUNTIME_DIR, "gate");
        char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
        procutil_hex_encode(token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));
        const char* argv[] = {OBICALL_GATED_PATH,
                               "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                               "--token", token_hex,
                               "--state-file", state_file,
                               NULL};
        osal_process_spawn_opts_t opts;
        opts.exe_path = OBICALL_GATED_PATH;
        opts.argv = argv;
        opts.envp = NULL;
        osal_process_t* proc3 = NULL;
        if (osal_process_spawn(&opts, &proc3) != 0) {
            fprintf(stderr, "FAIL: could not spawn gate for corruption test\n");
            failures++;
        } else {
            int exit_code = -1;
            int r = osal_process_wait(proc3, 3000, &exit_code);
            if (r != 1 || exit_code == 0) {
                fprintf(stderr, "FAIL: gate did not refuse to start on corrupted state (r=%d exit=%d)\n", r,
                        exit_code);
                failures++;
                if (r != 1) { osal_process_kill(proc3); osal_process_wait(proc3, 2000, NULL); }
            } else {
                fprintf(stderr, "ok: gate refused to start on corrupted persisted state (exit=%d)\n", exit_code);
            }
            osal_process_close(proc3);
        }
    }

    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
