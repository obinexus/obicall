#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obicall/obicall.h"
#include "osal.h"
#include "procutil.h"

/*
 * obicall-journald: the bounded sensor admission journal. Every provider
 * worker (regardless of which broker it belongs to) submits observations
 * here; both brokers read the identical admitted stream from here (see
 * docs/ARCHITECTURE.md "Windowing model" - one admitted observation is one
 * window). This process is a deliberate single point of failure for the
 * demo's shared input path (docs/FAULT_TOLERANCE.md) - only the gate and
 * the two brokers are replicated.
 */

#define MAX_SENSOR_TRACKS 32u
#define RING_CAPACITY 4096u
#define MAX_CONNECTIONS 16u

typedef struct ring_entry {
    uint64_t window_seq;
    uint32_t admission_reason;
    obicall_observation_t obs;
} ring_entry_t;

typedef struct conn_slot {
    int in_use;
    osal_socket_t* sock;
    obicall_wire_role_t role;
    uint64_t next_seq_to_send; /* only meaningful for BROKER subscriber connections */
    int caught_up;
} conn_slot_t;

typedef struct journal_state {
    osal_mutex_t* mu;
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    obicall_journal_config_t cfg;

    obicall_sensor_track_t tracks[MAX_SENSOR_TRACKS];
    uint32_t track_count;

    ring_entry_t ring[RING_CAPACITY];
    uint64_t ring_head; /* number of entries ever written; window_seq of next = ring_head+1 */
    uint64_t next_window_seq;

    conn_slot_t conns[MAX_CONNECTIONS];

    FILE* segment_file;
    uint8_t run_token[OBICALL_RUN_TOKEN_LEN];
    volatile int shutdown_requested;
} journal_state_t;

static journal_state_t g_state;

static obicall_sensor_track_t* find_or_create_track(const char* sensor_id) {
    for (uint32_t i = 0; i < g_state.track_count; ++i) {
        if (strncmp(g_state.tracks[i].sensor_id, sensor_id, OBICALL_SENSOR_ID_LEN) == 0) {
            return &g_state.tracks[i];
        }
    }
    if (g_state.track_count >= MAX_SENSOR_TRACKS) return NULL;
    obicall_sensor_track_t* t = &g_state.tracks[g_state.track_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->sensor_id, sensor_id, OBICALL_SENSOR_ID_LEN - 1);
    return t;
}

/* Backpressure depth = how far the slowest connected broker subscriber
 * lags behind the next window about to be assigned. Overflow policy here
 * is always reject-newest (the incoming observation is refused); the ring
 * buffer's own fixed capacity separately, always retires the oldest
 * replay-able record once RING_CAPACITY is exceeded regardless of
 * subscriber lag - see docs/FAULT_TOLERANCE.md. A pipeline with no
 * connected broker yet reports zero pending depth (nothing to lag behind). */
static uint32_t compute_pending_depth_locked(void) {
    uint64_t slowest = g_state.next_window_seq;
    int any = 0;
    for (uint32_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (g_state.conns[i].in_use && g_state.conns[i].role == OBICALL_ROLE_BROKER) {
            if (!any || g_state.conns[i].next_seq_to_send < slowest) slowest = g_state.conns[i].next_seq_to_send;
            any = 1;
        }
    }
    if (!any) return 0;
    return (uint32_t)(g_state.next_window_seq > slowest ? g_state.next_window_seq - slowest : 0);
}

/* Runs with g_state.mu held. Assigns a window_seq, appends to the ring
 * and durable segment, and returns the decision. */
static obicall_admission_reason_t admit_locked(const obicall_observation_t* obs,
                                                uint64_t* out_window_seq) {
    obicall_sensor_track_t* track = find_or_create_track(obs->sensor_id);
    if (!track) return OBICALL_REJECT_OVERFLOW;

    uint32_t pending = compute_pending_depth_locked();
    obicall_admission_reason_t reason = obicall_journal_admit(&g_state.cfg, track, obs, pending);

    uint64_t window_seq = g_state.next_window_seq++;
    *out_window_seq = window_seq;

    ring_entry_t* slot = &g_state.ring[window_seq % RING_CAPACITY];
    slot->window_seq = window_seq;
    slot->admission_reason = (uint32_t)reason;
    slot->obs = *obs;
    g_state.ring_head = window_seq;

    if (g_state.segment_file) {
        obicall_journal_append_record(g_state.segment_file, window_seq, osal_monotonic_ns(), reason, obs);
    }
    return reason;
}

static void broadcast_locked(void) {
    /* Each subscriber connection's own reader/writer thread drains
     * ring[next_seq_to_send..ring_head] on its next iteration; nothing to
     * do here beyond having updated ring_head under the lock. */
}

typedef struct conn_ctx {
    conn_slot_t* slot;
} conn_ctx_t;

static void send_ring_entry(osal_socket_t* sock, const ring_entry_t* e) {
    uint8_t buf[1200];
    uint32_t len = 0;
    if (obicall_wire_encode_observation(&e->obs, buf, sizeof(buf), &len) != OBICALL_OK) return;
    osal_send_frame(sock, OBICALL_MSG_REPLAY_DATA, buf, len, 2000);

    obicall_msg_admission_decision_t dec;
    memset(&dec, 0, sizeof(dec));
    strncpy(dec.pipeline_id, g_state.pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    dec.window_seq = e->window_seq;
    strncpy(dec.sensor_id, e->obs.sensor_id, OBICALL_SENSOR_ID_LEN - 1);
    dec.sequence = e->obs.sequence;
    dec.admitted = (e->admission_reason == OBICALL_ADMIT_OK) ? 1u : 0u;
    dec.reason_code = e->admission_reason;
    dec.decided_at_ns = osal_monotonic_ns();
    uint8_t dbuf[256];
    uint32_t dlen = 0;
    if (obicall_wire_encode_admission_decision(&dec, dbuf, sizeof(dbuf), &dlen) == OBICALL_OK) {
        osal_send_frame(sock, OBICALL_MSG_ADMISSION_DECISION, dbuf, dlen, 2000);
    }
}

static void handle_broker_connection(conn_slot_t* slot) {
    uint8_t msg_type;
    uint8_t payload[2048];
    uint32_t payload_len;

    /* First real message: a replay request naming where to start. */
    if (osal_recv_frame(slot->sock, &msg_type, payload, sizeof(payload), &payload_len, 5000) != 0 ||
        msg_type != OBICALL_MSG_REPLAY_REQUEST) {
        return;
    }
    obicall_msg_replay_request_t req;
    if (obicall_wire_decode_replay_request(payload, payload_len, &req) != OBICALL_OK) return;

    osal_mutex_lock(g_state.mu);
    uint64_t cursor = req.from_window_seq;
    if (cursor < 1) cursor = 1; /* window_seq is 1-indexed; 0 means "from the beginning" */
    uint64_t oldest_available =
        (g_state.ring_head + 1 > RING_CAPACITY) ? (g_state.ring_head + 1 - RING_CAPACITY) : 1;
    if (cursor < oldest_available) cursor = oldest_available; /* too far behind: skip to retained window */
    slot->next_seq_to_send = cursor;
    osal_mutex_unlock(g_state.mu);

    for (;;) {
        osal_mutex_lock(g_state.mu);
        int has_more = (cursor >= 1) && (cursor <= g_state.ring_head);
        ring_entry_t entry;
        if (has_more) entry = g_state.ring[cursor % RING_CAPACITY];
        int shutdown = g_state.shutdown_requested;
        osal_mutex_unlock(g_state.mu);

        if (shutdown) return;
        if (!has_more) {
            if (!slot->caught_up) {
                osal_send_frame(slot->sock, OBICALL_MSG_REPLAY_END, NULL, 0, 2000);
                slot->caught_up = 1;
            }
            osal_sleep_ms(20);
            continue;
        }
        send_ring_entry(slot->sock, &entry);
        cursor++;
        osal_mutex_lock(g_state.mu);
        slot->next_seq_to_send = cursor;
        osal_mutex_unlock(g_state.mu);
    }
}

static void handle_worker_connection(conn_slot_t* slot) {
    uint8_t msg_type;
    uint8_t payload[2048];
    uint32_t payload_len;
    for (;;) {
        int r = osal_recv_frame(slot->sock, &msg_type, payload, sizeof(payload), &payload_len, 2000);
        if (r == -1) return; /* hard error/disconnect: give up on this connection */
        if (r == 1) {
            osal_mutex_lock(g_state.mu);
            int shutdown = g_state.shutdown_requested;
            osal_mutex_unlock(g_state.mu);
            if (shutdown) return;
            continue; /* clean timeout: keep waiting for the next observation */
        }
        if (msg_type != OBICALL_MSG_OBSERVATION_SUBMIT) continue;
        obicall_observation_t obs;
        if (obicall_wire_decode_observation(payload, payload_len, &obs) != OBICALL_OK) continue;

        osal_mutex_lock(g_state.mu);
        uint64_t window_seq = 0;
        admit_locked(&obs, &window_seq);
        broadcast_locked();
        osal_mutex_unlock(g_state.mu);
    }
}

static void connection_thread(void* arg) {
    conn_ctx_t* ctx = (conn_ctx_t*)arg;
    conn_slot_t* slot = ctx->slot;
    free(ctx);

    obicall_wire_role_t role = OBICALL_ROLE_UNKNOWN;
    if (procutil_server_verify_hello(slot->sock, g_state.run_token, 5000, &role) != 0) {
        osal_close(slot->sock);
        slot->in_use = 0;
        return;
    }
    slot->role = role;

    if (role == OBICALL_ROLE_BROKER) {
        handle_broker_connection(slot);
    } else if (role == OBICALL_ROLE_WORKER) {
        handle_worker_connection(slot);
    }

    osal_close(slot->sock);
    slot->in_use = 0;
}

int main(int argc, char** argv) {
    memset(&g_state, 0, sizeof(g_state));
    osal_mutex_init(&g_state.mu);
    osal_net_init();

    const char* runtime_dir = procutil_arg_str(argc, argv, "--runtime-dir", ".");
    const char* token_hex = procutil_arg_str(argc, argv, "--token", NULL);
    const char* pipeline_id = procutil_arg_str(argc, argv, "--pipeline-id", "default");
    const char* segment_path = procutil_arg_str(argc, argv, "--segment-file", NULL);

    if (!token_hex || strlen(token_hex) != OBICALL_RUN_TOKEN_LEN * 2) {
        fprintf(stderr, "obicall-journald: --token is required (%u hex chars)\n", OBICALL_RUN_TOKEN_LEN * 2);
        return 2;
    }
    if (procutil_hex_decode(token_hex, (uint32_t)strlen(token_hex), g_state.run_token, OBICALL_RUN_TOKEN_LEN) != 0) {
        fprintf(stderr, "obicall-journald: malformed --token\n");
        return 2;
    }

    strncpy(g_state.pipeline_id, pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    g_state.cfg.reorder_window_n = (uint64_t)procutil_arg_i64(argc, argv, "--reorder-window", 8);
    g_state.cfg.max_lateness_ns = procutil_arg_i64(argc, argv, "--max-lateness-ms", 2000) * 1000000LL;
    g_state.cfg.max_pending_per_pipeline = (uint32_t)procutil_arg_i64(argc, argv, "--max-pending", 4096);
    g_state.cfg.overflow_policy = OBICALL_OVERFLOW_REJECT_OLDEST;
    g_state.next_window_seq = 1;

    if (segment_path) {
        g_state.segment_file = fopen(segment_path, "ab");
        if (!g_state.segment_file) {
            fprintf(stderr, "obicall-journald: cannot open segment file %s\n", segment_path);
            return 2;
        }
    }

    uint16_t port = 0;
    osal_socket_t* listener = NULL;
    if (osal_listen_loopback(&port, &listener) != 0) {
        fprintf(stderr, "obicall-journald: failed to bind loopback listener\n");
        return 2;
    }
    procutil_write_port_file(runtime_dir, "journal", port);
    procutil_write_pid_file(runtime_dir, "journal", procutil_current_pid());

    fprintf(stderr, "obicall-journald: listening on 127.0.0.1:%u (pipeline=%s)\n", (unsigned)port, pipeline_id);

    for (;;) {
        osal_socket_t* conn = NULL;
        int r = osal_accept(listener, 200, &conn);
        if (r == 1) {
            uint32_t idx;
            for (idx = 0; idx < MAX_CONNECTIONS; ++idx) {
                if (!g_state.conns[idx].in_use) break;
            }
            if (idx == MAX_CONNECTIONS) {
                osal_close(conn);
            } else {
                g_state.conns[idx].in_use = 1;
                g_state.conns[idx].sock = conn;
                g_state.conns[idx].role = OBICALL_ROLE_UNKNOWN;
                g_state.conns[idx].next_seq_to_send = 1;
                g_state.conns[idx].caught_up = 0;
                conn_ctx_t* ctx = (conn_ctx_t*)malloc(sizeof(*ctx));
                ctx->slot = &g_state.conns[idx];
                osal_thread_start_detached(connection_thread, ctx);
            }
        }
    }
}
