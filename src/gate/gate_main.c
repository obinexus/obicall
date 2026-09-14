#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obicall/obicall.h"
#include "osal.h"
#include "procutil.h"

/*
 * obicall-gated: the publication gate. The only authority that can commit
 * a result (docs/ARCHITECTURE.md "Publication gate invariants"). All
 * broker<->gate traffic is broker-initiated request/response - HEARTBEAT
 * doubles as the promotion-readiness report and its reply
 * (OBICALL_MSG_STATUS_REPLY) is how a broker learns it has been granted
 * or has lost ownership, rather than the gate pushing grants
 * asynchronously. This trades a little latency (bounded by the broker's
 * heartbeat interval) for a single request/response connection per
 * broker instead of a separate push channel.
 */

#define MAX_CONNECTIONS 16u

typedef struct broker_view {
    int known;
    obicall_gate_readiness_t readiness;
    int64_t last_heartbeat_ns;
} broker_view_t;

typedef struct gate_ctx {
    osal_mutex_t* mu;
    obicall_gate_state_t state;
    broker_view_t brokers[3]; /* indexed by obicall_broker_id_t: 1=A, 2=B */

    char state_file[1024];
    uint8_t required_config_digest[OBICALL_DIGEST_LEN];
    uint32_t required_checkpoint_schema_version;
    uint64_t max_allowed_replay_gap;
    int64_t result_validity_ns;
    int64_t suspect_timeout_ns;
    int64_t confirm_timeout_ns;

    int64_t start_time_ns;
    uint8_t run_token[OBICALL_RUN_TOKEN_LEN];
    volatile int shutdown_requested;
} gate_ctx_t;

static gate_ctx_t g_ctx;

static int persist_locked(void) {
    char tmp[1040];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_ctx.state_file);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    obicall_status_t st = obicall_gate_persist_state(f, &g_ctx.state);
    fclose(f);
    if (st != OBICALL_OK) return -1;
    return osal_file_atomic_replace(tmp, g_ctx.state_file);
}

/* Runs with g_ctx.mu held. Bootstraps broker A as the first owner, or
 * evaluates whether the current owner is stale enough to promote the
 * other broker. Never called from anywhere except a just-received
 * heartbeat, so promotion authority always traces back to the gate's own
 * receipt of a liveness signal - never a broker's private belief. */
static void evaluate_promotion_locked(int64_t now) {
    if (g_ctx.state.owner_broker_id == OBICALL_BROKER_NONE) {
        if (g_ctx.brokers[OBICALL_BROKER_A].known && g_ctx.brokers[OBICALL_BROKER_A].readiness.healthy) {
            uint64_t new_epoch;
            if (obicall_gate_promote(&g_ctx.state, OBICALL_BROKER_A, now, &new_epoch) == OBICALL_OK) {
                persist_locked();
                fprintf(stderr, "obicall-gated: bootstrap grant epoch %llu to broker A\n",
                        (unsigned long long)new_epoch);
            }
        }
        return;
    }

    uint32_t owner = g_ctx.state.owner_broker_id;
    broker_view_t* ov = &g_ctx.brokers[owner];
    if (!ov->known) return;
    int64_t age = now - ov->last_heartbeat_ns;
    if (age <= g_ctx.confirm_timeout_ns) return;

    uint32_t other = (owner == OBICALL_BROKER_A) ? OBICALL_BROKER_B : OBICALL_BROKER_A;
    broker_view_t* sv = &g_ctx.brokers[other];
    if (!sv->known) return;

    uint64_t committed_high_water = 0;
    for (uint32_t i = 0; i < g_ctx.state.pipeline_count; ++i) {
        if (g_ctx.state.pipelines[i].last_committed_window_seq > committed_high_water) {
            committed_high_water = g_ctx.state.pipelines[i].last_committed_window_seq;
        }
    }

    obicall_gate_promotion_requirements_t req;
    memcpy(req.required_config_digest, g_ctx.required_config_digest, OBICALL_DIGEST_LEN);
    req.required_checkpoint_schema_version = g_ctx.required_checkpoint_schema_version;
    req.max_allowed_replay_gap = g_ctx.max_allowed_replay_gap;

    if (obicall_gate_shadow_promotable(&sv->readiness, &req, committed_high_water)) {
        uint64_t new_epoch;
        if (obicall_gate_promote(&g_ctx.state, other, now, &new_epoch) == OBICALL_OK) {
            persist_locked();
            fprintf(stderr, "obicall-gated: promoted broker %u to epoch %llu (owner %u stale for %lldms)\n",
                    other, (unsigned long long)new_epoch, owner, (long long)(age / 1000000));
        }
    }
}

static void handle_heartbeat(osal_socket_t* sock, const uint8_t* payload, uint32_t len) {
    obicall_msg_heartbeat_t hb;
    if (obicall_wire_decode_heartbeat(payload, len, &hb) != OBICALL_OK) return;
    if (hb.broker_id != OBICALL_BROKER_A && hb.broker_id != OBICALL_BROKER_B) return;

    int64_t now = osal_monotonic_ns();
    osal_mutex_lock(g_ctx.mu);
    broker_view_t* v = &g_ctx.brokers[hb.broker_id];
    v->known = 1;
    v->last_heartbeat_ns = now;
    v->readiness.healthy = hb.healthy;
    memcpy(v->readiness.config_digest, hb.config_digest, OBICALL_DIGEST_LEN);
    v->readiness.checkpoint_schema_version = hb.checkpoint_schema_version;
    v->readiness.last_processed_window_seq = hb.last_window_seq;

    evaluate_promotion_locked(now);

    obicall_msg_status_reply_t reply;
    reply.running = 1;
    reply.gate_epoch = g_ctx.state.epoch;
    reply.owner_broker_id = g_ctx.state.owner_broker_id;
    reply.last_committed_window_seq = 0;
    for (uint32_t i = 0; i < g_ctx.state.pipeline_count; ++i) {
        if (g_ctx.state.pipelines[i].last_committed_window_seq > reply.last_committed_window_seq) {
            reply.last_committed_window_seq = g_ctx.state.pipelines[i].last_committed_window_seq;
        }
    }
    reply.uptime_ns = now - g_ctx.start_time_ns;
    osal_mutex_unlock(g_ctx.mu);

    uint8_t buf[64];
    uint32_t out_len = 0;
    if (obicall_wire_encode_status_reply(&reply, buf, sizeof(buf), &out_len) == OBICALL_OK) {
        osal_send_frame(sock, OBICALL_MSG_STATUS_REPLY, buf, out_len, 2000);
    }
}

static void handle_result_publish(osal_socket_t* sock, const uint8_t* payload, uint32_t len) {
    obicall_result_t result;
    if (obicall_wire_decode_result(payload, len, &result) != OBICALL_OK) return;

    int64_t now = osal_monotonic_ns();
    osal_mutex_lock(g_ctx.mu);
    uint32_t pidx = 0;
    obicall_gate_publish_outcome_t outcome = obicall_gate_decide_publish(&g_ctx.state, &result, now, &pidx);
    int persisted_ok = 1;
    if (outcome == OBICALL_GATE_COMMIT) persisted_ok = (persist_locked() == 0);
    osal_mutex_unlock(g_ctx.mu);

    obicall_msg_result_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    strncpy(ack.pipeline_id, result.pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    ack.window_seq = result.window_seq;
    ack.accepted = (outcome == OBICALL_GATE_COMMIT && persisted_ok) ? 1u : 0u;
    switch (outcome) {
        case OBICALL_GATE_COMMIT: ack.reject_status = persisted_ok ? OBICALL_OK : OBICALL_ERR_GATE_PERSISTENCE; break;
        case OBICALL_GATE_SHADOW_RECORDED: ack.reject_status = OBICALL_OK; break;
        case OBICALL_GATE_REJECT_NOT_OWNER: ack.reject_status = OBICALL_ERR_GATE_NOT_OWNER; break;
        case OBICALL_GATE_REJECT_STALE_EPOCH: ack.reject_status = OBICALL_ERR_GATE_STALE_EPOCH; break;
        case OBICALL_GATE_REJECT_DUPLICATE_WINDOW: ack.reject_status = OBICALL_ERR_GATE_DUPLICATE_WINDOW; break;
        case OBICALL_GATE_REJECT_EXPIRED: ack.reject_status = OBICALL_ERR_GATE_EXPIRED; break;
        case OBICALL_GATE_REJECT_INCOMPATIBLE: ack.reject_status = OBICALL_ERR_GATE_INCOMPATIBLE; break;
        default: ack.reject_status = OBICALL_ERR_INTERNAL; break;
    }

    uint8_t buf[128];
    uint32_t out_len = 0;
    if (obicall_wire_encode_result_ack(&ack, buf, sizeof(buf), &out_len) == OBICALL_OK) {
        osal_send_frame(sock, OBICALL_MSG_RESULT_ACK, buf, out_len, 2000);
    }
}

static void handle_status_query(osal_socket_t* sock) {
    int64_t now = osal_monotonic_ns();
    osal_mutex_lock(g_ctx.mu);
    obicall_msg_status_reply_t reply;
    reply.running = 1;
    reply.gate_epoch = g_ctx.state.epoch;
    reply.owner_broker_id = g_ctx.state.owner_broker_id;
    reply.last_committed_window_seq = 0;
    for (uint32_t i = 0; i < g_ctx.state.pipeline_count; ++i) {
        if (g_ctx.state.pipelines[i].last_committed_window_seq > reply.last_committed_window_seq) {
            reply.last_committed_window_seq = g_ctx.state.pipelines[i].last_committed_window_seq;
        }
    }
    reply.uptime_ns = now - g_ctx.start_time_ns;
    osal_mutex_unlock(g_ctx.mu);

    uint8_t buf[64];
    uint32_t out_len = 0;
    if (obicall_wire_encode_status_reply(&reply, buf, sizeof(buf), &out_len) == OBICALL_OK) {
        osal_send_frame(sock, OBICALL_MSG_STATUS_REPLY, buf, out_len, 2000);
    }
}

static void connection_thread(void* arg) {
    osal_socket_t* sock = (osal_socket_t*)arg;
    obicall_wire_role_t role = OBICALL_ROLE_UNKNOWN;
    if (procutil_server_verify_hello(sock, g_ctx.run_token, 5000, &role) != 0) {
        osal_close(sock);
        return;
    }

    for (;;) {
        uint8_t msg_type;
        uint8_t payload[4096];
        uint32_t payload_len;
        int r = osal_recv_frame(sock, &msg_type, payload, sizeof(payload), &payload_len, 3000);
        if (r == -1) break;
        if (r == 1) {
            if (g_ctx.shutdown_requested) break;
            continue;
        }
        switch (msg_type) {
            case OBICALL_MSG_HEARTBEAT: handle_heartbeat(sock, payload, payload_len); break;
            case OBICALL_MSG_RESULT_PUBLISH: handle_result_publish(sock, payload, payload_len); break;
            case OBICALL_MSG_STATUS_QUERY: handle_status_query(sock); break;
            default: break;
        }
        if (role == OBICALL_ROLE_CLI) break; /* CLI issues one query per connection */
    }
    osal_close(sock);
}

int main(int argc, char** argv) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    osal_mutex_init(&g_ctx.mu);
    osal_net_init();

    const char* runtime_dir = procutil_arg_str(argc, argv, "--runtime-dir", ".");
    const char* token_hex = procutil_arg_str(argc, argv, "--token", NULL);
    const char* config_digest_hex = procutil_arg_str(argc, argv, "--config-digest", NULL);
    const char* state_file = procutil_arg_str(argc, argv, "--state-file", NULL);

    if (!token_hex || strlen(token_hex) != OBICALL_RUN_TOKEN_LEN * 2 ||
        procutil_hex_decode(token_hex, (uint32_t)strlen(token_hex), g_ctx.run_token, OBICALL_RUN_TOKEN_LEN) != 0) {
        fprintf(stderr, "obicall-gated: --token is required and must be %u hex chars\n", OBICALL_RUN_TOKEN_LEN * 2);
        return 2;
    }
    if (config_digest_hex && strlen(config_digest_hex) == OBICALL_DIGEST_LEN * 2) {
        procutil_hex_decode(config_digest_hex, (uint32_t)strlen(config_digest_hex), g_ctx.required_config_digest,
                             OBICALL_DIGEST_LEN);
    }
    g_ctx.required_checkpoint_schema_version = OBICALL_CHECKPOINT_SCHEMA_VERSION;
    g_ctx.max_allowed_replay_gap = (uint64_t)procutil_arg_i64(argc, argv, "--max-replay-gap", 50);
    g_ctx.result_validity_ns = procutil_arg_i64(argc, argv, "--result-validity-ms", 500) * 1000000LL;
    g_ctx.suspect_timeout_ns = procutil_arg_i64(argc, argv, "--suspect-timeout-ms", 300) * 1000000LL;
    g_ctx.confirm_timeout_ns = procutil_arg_i64(argc, argv, "--confirm-timeout-ms", 700) * 1000000LL;

    if (state_file) {
        strncpy(g_ctx.state_file, state_file, sizeof(g_ctx.state_file) - 1);
        FILE* f = fopen(state_file, "rb");
        if (f) {
            obicall_status_t st = obicall_gate_load_state(f, &g_ctx.state);
            fclose(f);
            if (st != OBICALL_OK) {
                fprintf(stderr,
                        "obicall-gated: state file %s exists but is not trustworthy (%s) - refusing to start;"
                        " reconcile or remove it manually\n",
                        state_file, obicall_status_string(st));
                return 3;
            }
            fprintf(stderr, "obicall-gated: recovered epoch=%llu owner=%u from %s\n",
                    (unsigned long long)g_ctx.state.epoch, g_ctx.state.owner_broker_id, state_file);
        }
    }
    g_ctx.state.struct_size = sizeof(g_ctx.state);
    g_ctx.state.schema_version = 1;

    g_ctx.start_time_ns = osal_monotonic_ns();

    uint16_t port = 0;
    osal_socket_t* listener = NULL;
    if (osal_listen_loopback(&port, &listener) != 0) {
        fprintf(stderr, "obicall-gated: failed to bind loopback listener\n");
        return 2;
    }
    procutil_write_port_file(runtime_dir, "gate", port);
    procutil_write_pid_file(runtime_dir, "gate", procutil_current_pid());
    fprintf(stderr, "obicall-gated: listening on 127.0.0.1:%u\n", (unsigned)port);

    for (;;) {
        osal_socket_t* conn = NULL;
        int r = osal_accept(listener, 200, &conn);
        if (r == 1) osal_thread_start_detached(connection_thread, conn);
    }
}
