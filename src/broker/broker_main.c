#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "obicall/obicall.h"
#include "osal.h"
#include "procutil.h"

/*
 * obicall-brokerd: consumes the journal's admitted stream (replay then
 * live-tail on one connection - see docs/ARCHITECTURE.md), runs the
 * Kalman estimator and DGT policy, and reports to the gate. All gate
 * traffic is broker-initiated request/response (see gate_main.c) - this
 * process never assumes ownership on its own; it only acts on what the
 * gate's replies tell it.
 */

#define QUEUE_CAP 512u
#define MAX_TRACKED_SENSORS 8u
#define DGT_ACTION_COUNT 3u
#define DGT_SCENARIO_COUNT 4u

typedef struct queue_entry {
    uint64_t window_seq;
    obicall_observation_t obs;
    uint32_t admitted;
} queue_entry_t;

typedef struct sensor_freshness {
    char sensor_id[OBICALL_SENSOR_ID_LEN];
    int64_t last_seen_ns;
} sensor_freshness_t;

typedef struct broker_ctx {
    uint32_t broker_id;
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint8_t config_digest[OBICALL_DIGEST_LEN];
    int64_t result_validity_ns;
    int64_t sensor_fresh_window_ns;

    osal_mutex_t* mu;
    queue_entry_t queue[QUEUE_CAP];
    uint32_t qhead, qtail;

    sensor_freshness_t sensors[MAX_TRACKED_SENSORS];
    uint32_t sensor_count;
    char sensor_names[MAX_TRACKED_SENSORS][OBICALL_SENSOR_ID_LEN];
    uint32_t configured_sensor_count;

    obicall_kalman_state_t kf;
    obicall_kalman_params_t kf_params;
    int kf_ready;
    uint64_t last_processed_window_seq;

    obicall_dgt_policy_table_t dgt;
    obicall_dgt_hysteresis_state_t dgt_hyst;
    int dgt_has_hyst;
    double dgt_hysteresis_margin;

    volatile int reader_alive;
} broker_ctx_t;

static broker_ctx_t g_ctx;

static uint32_t sensor_bit(const char* sensor_id) {
    for (uint32_t i = 0; i < g_ctx.configured_sensor_count; ++i) {
        if (strncmp(g_ctx.sensor_names[i], sensor_id, OBICALL_SENSOR_ID_LEN) == 0) return 1u << i;
    }
    return 0;
}

static void touch_sensor_locked(const char* sensor_id, int64_t now) {
    for (uint32_t i = 0; i < g_ctx.sensor_count; ++i) {
        if (strncmp(g_ctx.sensors[i].sensor_id, sensor_id, OBICALL_SENSOR_ID_LEN) == 0) {
            g_ctx.sensors[i].last_seen_ns = now;
            return;
        }
    }
    if (g_ctx.sensor_count < MAX_TRACKED_SENSORS) {
        strncpy(g_ctx.sensors[g_ctx.sensor_count].sensor_id, sensor_id, OBICALL_SENSOR_ID_LEN - 1);
        g_ctx.sensors[g_ctx.sensor_count].last_seen_ns = now;
        g_ctx.sensor_count++;
    }
}

static int sensor_observable_locked(const char* sensor_id, int64_t now) {
    for (uint32_t i = 0; i < g_ctx.sensor_count; ++i) {
        if (strncmp(g_ctx.sensors[i].sensor_id, sensor_id, OBICALL_SENSOR_ID_LEN) == 0) {
            return (now - g_ctx.sensors[i].last_seen_ns) <= g_ctx.sensor_fresh_window_ns;
        }
    }
    return 0;
}

/* Illustrative, versioned scenario/loss table for a two-sensor
 * (position, inertial) pipeline - synthetic, not measured (docs/DGT.md).
 * Action 2 ("balanced") has the lowest worst-case loss when both sensors
 * are observable; losing either sensor makes the action that depends on
 * it ineligible, which is what actually changes the live estimator's
 * sensor subset (see apply loop below), not merely a logged label. */
static void init_default_dgt_table(void) {
    obicall_dgt_policy_table_t* t = &g_ctx.dgt;
    memset(t, 0, sizeof(*t));
    t->struct_size = sizeof(*t);
    t->schema_version = OBICALL_DGT_POLICY_SCHEMA_VERSION;
    t->policy_version = 1;
    t->action_count = DGT_ACTION_COUNT;
    t->scenario_count = DGT_SCENARIO_COUNT;

    strncpy(t->actions[0].name, "position_only", OBICALL_MAX_NAME_LEN - 1);
    t->actions[0].action_id = 0;
    t->actions[0].sensor_subset_mask = 0x1;
    t->actions[0].estimator_mode = OBICALL_ESTIMATOR_MODE_KALMAN_CV;

    strncpy(t->actions[1].name, "inertial_only", OBICALL_MAX_NAME_LEN - 1);
    t->actions[1].action_id = 1;
    t->actions[1].sensor_subset_mask = 0x2;
    t->actions[1].estimator_mode = OBICALL_ESTIMATOR_MODE_KALMAN_CV;

    strncpy(t->actions[2].name, "balanced", OBICALL_MAX_NAME_LEN - 1);
    t->actions[2].action_id = 2;
    t->actions[2].sensor_subset_mask = 0x3;
    t->actions[2].estimator_mode = OBICALL_ESTIMATOR_MODE_KALMAN_CV;

    static const char* scenario_names[DGT_SCENARIO_COUNT] = {"nominal", "position_dropout", "inertial_bias",
                                                               "processing_delay"};
    for (uint32_t s = 0; s < DGT_SCENARIO_COUNT; ++s) {
        t->scenarios[s].scenario_id = s;
        strncpy(t->scenarios[s].name, scenario_names[s], OBICALL_MAX_NAME_LEN - 1);
    }

    /* cost[action][scenario] = {estimation_error, delay, coverage_loss, resource_cost} */
    double costs[DGT_ACTION_COUNT][DGT_SCENARIO_COUNT][4] = {
        {{0.20, 0.10, 0.30, 0.10}, {0.90, 0.10, 0.90, 0.10}, {0.20, 0.10, 0.30, 0.10}, {0.30, 0.60, 0.30, 0.10}},
        {{0.35, 0.10, 0.30, 0.10}, {0.35, 0.10, 0.30, 0.10}, {0.90, 0.10, 0.30, 0.10}, {0.35, 0.10, 0.30, 0.10}},
        {{0.15, 0.10, 0.10, 0.20}, {0.40, 0.10, 0.50, 0.20}, {0.40, 0.10, 0.20, 0.20}, {0.30, 0.35, 0.15, 0.20}},
    };
    for (uint32_t a = 0; a < DGT_ACTION_COUNT; ++a) {
        for (uint32_t s = 0; s < DGT_SCENARIO_COUNT; ++s) {
            t->cost[a][s].estimation_error = costs[a][s][0];
            t->cost[a][s].delay = costs[a][s][1];
            t->cost[a][s].coverage_loss = costs[a][s][2];
            t->cost[a][s].resource_cost = costs[a][s][3];
        }
    }

    t->weights.w_estimation_error = 0.5;
    t->weights.w_delay = 0.2;
    t->weights.w_coverage_loss = 0.2;
    t->weights.w_resource_cost = 0.1;
    t->scale_estimation_error = 1.0;
    t->scale_delay = 1.0;
    t->scale_coverage_loss = 1.0;
    t->scale_resource_cost = 1.0;
}

typedef struct reader_args {
    osal_socket_t* sock;
} reader_args_t;

static void journal_reader_thread(void* arg) {
    reader_args_t* ra = (reader_args_t*)arg;
    osal_socket_t* sock = ra->sock;
    free(ra);

    obicall_msg_replay_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.pipeline_id, g_ctx.pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    req.from_window_seq = 1;
    uint8_t buf[128];
    uint32_t len = 0;
    obicall_wire_encode_replay_request(&req, buf, sizeof(buf), &len);
    if (osal_send_frame(sock, OBICALL_MSG_REPLAY_REQUEST, buf, len, 3000) != 0) {
        g_ctx.reader_alive = 0;
        return;
    }

    obicall_observation_t pending_obs;
    int have_pending = 0;

    for (;;) {
        uint8_t msg_type;
        uint8_t payload[2048];
        uint32_t payload_len;
        int r = osal_recv_frame(sock, &msg_type, payload, sizeof(payload), &payload_len, 3000);
        if (r == -1) { g_ctx.reader_alive = 0; return; }
        if (r == 1) continue;

        if (msg_type == OBICALL_MSG_REPLAY_DATA) {
            if (obicall_wire_decode_observation(payload, payload_len, &pending_obs) == OBICALL_OK) have_pending = 1;
        } else if (msg_type == OBICALL_MSG_ADMISSION_DECISION && have_pending) {
            obicall_msg_admission_decision_t dec;
            if (obicall_wire_decode_admission_decision(payload, payload_len, &dec) == OBICALL_OK) {
                osal_mutex_lock(g_ctx.mu);
                uint32_t next_tail = (g_ctx.qtail + 1) % QUEUE_CAP;
                if (next_tail != g_ctx.qhead) {
                    g_ctx.queue[g_ctx.qtail].window_seq = dec.window_seq;
                    g_ctx.queue[g_ctx.qtail].obs = pending_obs;
                    g_ctx.queue[g_ctx.qtail].admitted = dec.admitted;
                    g_ctx.qtail = next_tail;
                }
                osal_mutex_unlock(g_ctx.mu);
            }
            have_pending = 0;
        }
    }
}

static void compute_result_digest_from_obs(const obicall_observation_t* obs, uint8_t out[OBICALL_DIGEST_LEN]) {
    uint8_t buf[1200];
    uint32_t len = 0;
    if (obicall_wire_encode_observation(obs, buf, sizeof(buf), &len) == OBICALL_OK) {
        obicall_sha256(buf, len, out);
    } else {
        memset(out, 0, OBICALL_DIGEST_LEN);
    }
}

int main(int argc, char** argv) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    osal_mutex_init(&g_ctx.mu);
    osal_net_init();

    const char* broker_id_s = procutil_arg_str(argc, argv, "--broker-id", "A");
    const char* journal_port_s = procutil_arg_str(argc, argv, "--journal-port", NULL);
    const char* gate_port_s = procutil_arg_str(argc, argv, "--gate-port", NULL);
    const char* token_hex = procutil_arg_str(argc, argv, "--token", NULL);
    const char* pipeline_id = procutil_arg_str(argc, argv, "--pipeline-id", "default");
    const char* runtime_dir = procutil_arg_str(argc, argv, "--runtime-dir", ".");
    const char* config_digest_hex = procutil_arg_str(argc, argv, "--config-digest", NULL);
    const char* sensor_ids_csv = procutil_arg_str(argc, argv, "--sensor-ids", "");
    int dgt_enabled = procutil_arg_flag(argc, argv, "--dgt-enabled");

    g_ctx.broker_id = (strcmp(broker_id_s, "B") == 0) ? OBICALL_BROKER_B : OBICALL_BROKER_A;
    strncpy(g_ctx.pipeline_id, pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    g_ctx.result_validity_ns = procutil_arg_i64(argc, argv, "--result-validity-ms", 500) * 1000000LL;
    g_ctx.sensor_fresh_window_ns = procutil_arg_i64(argc, argv, "--sensor-fresh-window-ms", 1000) * 1000000LL;
    g_ctx.dgt_hysteresis_margin =
        procutil_arg_f64(argc, argv, "--dgt-hysteresis-margin", OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN);

    g_ctx.kf_params.process_noise_position = procutil_arg_f64(argc, argv, "--process-noise-position", 0.05);
    g_ctx.kf_params.process_noise_velocity = procutil_arg_f64(argc, argv, "--process-noise-velocity", 0.1);
    g_ctx.kf_params.initial_position_variance = procutil_arg_f64(argc, argv, "--initial-position-variance", 10.0);
    g_ctx.kf_params.initial_velocity_variance = procutil_arg_f64(argc, argv, "--initial-velocity-variance", 10.0);
    g_ctx.kf_params.max_prediction_gap_s = procutil_arg_f64(argc, argv, "--max-prediction-gap-s", 30.0);

    {
        char csv[256];
        strncpy(csv, sensor_ids_csv, sizeof(csv) - 1);
        csv[sizeof(csv) - 1] = '\0';
        char* tok = strtok(csv, ",");
        while (tok && g_ctx.configured_sensor_count < MAX_TRACKED_SENSORS) {
            strncpy(g_ctx.sensor_names[g_ctx.configured_sensor_count], tok, OBICALL_SENSOR_ID_LEN - 1);
            g_ctx.configured_sensor_count++;
            tok = strtok(NULL, ",");
        }
    }

    if (!journal_port_s || !gate_port_s || !token_hex) {
        fprintf(stderr, "obicall-brokerd: --journal-port, --gate-port, --token are required\n");
        return 2;
    }
    uint8_t token[OBICALL_RUN_TOKEN_LEN];
    if (strlen(token_hex) != OBICALL_RUN_TOKEN_LEN * 2 ||
        procutil_hex_decode(token_hex, (uint32_t)strlen(token_hex), token, OBICALL_RUN_TOKEN_LEN) != 0) {
        fprintf(stderr, "obicall-brokerd: malformed --token\n");
        return 2;
    }
    if (config_digest_hex && strlen(config_digest_hex) == OBICALL_DIGEST_LEN * 2) {
        procutil_hex_decode(config_digest_hex, (uint32_t)strlen(config_digest_hex), g_ctx.config_digest,
                             OBICALL_DIGEST_LEN);
    }

    init_default_dgt_table();

    uint16_t journal_port = (uint16_t)strtol(journal_port_s, NULL, 10);
    uint16_t gate_port = (uint16_t)strtol(gate_port_s, NULL, 10);

    osal_socket_t* journal_conn = NULL;
    if (procutil_client_connect(journal_port, token, OBICALL_ROLE_BROKER, 3000, &journal_conn) != 0) {
        fprintf(stderr, "obicall-brokerd[%s]: failed to connect to journal\n", broker_id_s);
        return 3;
    }
    osal_socket_t* gate_conn = NULL;
    if (procutil_client_connect(gate_port, token, OBICALL_ROLE_BROKER, 3000, &gate_conn) != 0) {
        fprintf(stderr, "obicall-brokerd[%s]: failed to connect to gate\n", broker_id_s);
        return 4;
    }

    g_ctx.reader_alive = 1;
    reader_args_t* ra = (reader_args_t*)malloc(sizeof(*ra));
    ra->sock = journal_conn;
    osal_thread_start_detached(journal_reader_thread, ra);

    procutil_write_pid_file(runtime_dir, broker_id_s[0] == 'B' ? "broker_B" : "broker_A", procutil_current_pid());

    fprintf(stderr, "obicall-brokerd[%s]: running (pipeline=%s, dgt=%s)\n", broker_id_s, pipeline_id,
            dgt_enabled ? "on" : "off");

    int64_t last_heartbeat = 0;
    const int64_t heartbeat_period_ns = 150 * 1000000LL;
    uint64_t current_epoch_belief = 0;
    uint32_t current_owner_belief = OBICALL_BROKER_NONE;

    static queue_entry_t batch[QUEUE_CAP];
    for (;;) {
        uint32_t batch_n = 0;
        osal_mutex_lock(g_ctx.mu);
        while (g_ctx.qhead != g_ctx.qtail && batch_n < QUEUE_CAP) {
            batch[batch_n++] = g_ctx.queue[g_ctx.qhead];
            g_ctx.qhead = (g_ctx.qhead + 1) % QUEUE_CAP;
        }
        osal_mutex_unlock(g_ctx.mu);

        for (uint32_t i = 0; i < batch_n; ++i) {
            queue_entry_t* e = &batch[i];
            int64_t now = osal_monotonic_ns();

            osal_mutex_lock(g_ctx.mu);
            if (e->admitted) touch_sensor_locked(e->obs.sensor_id, now);

            obicall_dgt_eligibility_t elig;
            memset(&elig, 0, sizeof(elig));
            for (uint32_t a = 0; a < g_ctx.dgt.action_count; ++a) {
                uint64_t mask = g_ctx.dgt.actions[a].sensor_subset_mask;
                int ok = 1;
                for (uint32_t b = 0; b < g_ctx.configured_sensor_count; ++b) {
                    if ((mask & (1ull << b)) && !sensor_observable_locked(g_ctx.sensor_names[b], now)) ok = 0;
                }
                elig.action_eligible[a] = ok && (mask != 0);
            }
            obicall_dgt_decision_t decision;
            memset(&decision, 0, sizeof(decision));
            uint32_t selected_mask = (g_ctx.configured_sensor_count > 0) ? ((1u << g_ctx.configured_sensor_count) - 1u) : 0xFFFFFFFFu;
            if (dgt_enabled && g_ctx.configured_sensor_count > 0) {
                obicall_dgt_hysteresis_state_t* hin = g_ctx.dgt_has_hyst ? &g_ctx.dgt_hyst : NULL;
                obicall_dgt_hysteresis_state_t hout;
                if (obicall_dgt_select_action(&g_ctx.dgt, &elig, hin, g_ctx.dgt_hysteresis_margin, 0, &decision,
                                               &hout) == OBICALL_OK) {
                    g_ctx.dgt_hyst = hout;
                    g_ctx.dgt_has_hyst = 1;
                    selected_mask = (uint32_t)g_ctx.dgt.actions[decision.selected_action_index].sensor_subset_mask;
                }
            }

            int used_in_estimator = 0;
            if (e->admitted && (selected_mask & sensor_bit(e->obs.sensor_id)) &&
                e->obs.payload_shape == OBICALL_SHAPE_POSITION_2D) {
                double cov2x2[4] = {e->obs.covariance[0], e->obs.covariance[1], e->obs.covariance[2],
                                     e->obs.covariance[3]};
                if (!g_ctx.kf_ready) {
                    obicall_kalman_init(&g_ctx.kf, &g_ctx.kf_params, e->obs.payload[0], e->obs.payload[1],
                                         e->obs.sample_time_ns);
                    g_ctx.kf_ready = 1;
                    used_in_estimator = 1;
                } else if (obicall_kalman_update_position(&g_ctx.kf, &g_ctx.kf_params, e->obs.sample_time_ns,
                                                            e->obs.payload[0], e->obs.payload[1], cov2x2) == OBICALL_OK) {
                    used_in_estimator = 1;
                }
            }
            g_ctx.last_processed_window_seq = e->window_seq;

            obicall_result_t result;
            memset(&result, 0, sizeof(result));
            result.struct_size = sizeof(result);
            result.schema_version = OBICALL_RESULT_SCHEMA_VERSION;
            strncpy(result.pipeline_id, g_ctx.pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
            result.window_seq = e->window_seq;
            result.broker_id = g_ctx.broker_id;
            result.epoch = current_epoch_belief;

            if (!g_ctx.kf_ready) {
                result.status = OBICALL_RESULT_INVALID;
            } else {
                double pos_cov_trace = g_ctx.kf.covariance[0] + g_ctx.kf.covariance[5];
                int degraded = (pos_cov_trace > 4.0) || (dgt_enabled && decision.eligible_count > 0 &&
                                                          decision.eligible_count < g_ctx.dgt.action_count);
                result.status = degraded ? OBICALL_RESULT_DEGRADED : OBICALL_RESULT_VALID;
                result.payload_shape = OBICALL_SHAPE_POSITION_2D;
                result.payload_count = 2;
                result.payload[0] = g_ctx.kf.mean[0];
                result.payload[1] = g_ctx.kf.mean[1];
                result.covariance_count = 4;
                result.covariance[0] = g_ctx.kf.covariance[0];
                result.covariance[1] = g_ctx.kf.covariance[1];
                result.covariance[2] = g_ctx.kf.covariance[4];
                result.covariance[3] = g_ctx.kf.covariance[5];
            }
            compute_result_digest_from_obs(&e->obs, result.input_digest);
            memcpy(result.config_digest, g_ctx.config_digest, OBICALL_DIGEST_LEN);
            result.timestamp_ns = now;
            result.valid_until_ns = now + g_ctx.result_validity_ns;
            if (used_in_estimator) {
                result.source_count = 1;
                strncpy(result.sources[0], e->obs.sensor_id, OBICALL_SENSOR_ID_LEN - 1);
            }
            result.dgt_action_id = dgt_enabled ? decision.selected_action_id : 0xFFFFFFFFu;
            osal_mutex_unlock(g_ctx.mu);

            uint8_t rbuf[2048];
            uint32_t rlen = 0;
            if (obicall_wire_encode_result(&result, rbuf, sizeof(rbuf), &rlen) == OBICALL_OK) {
                if (osal_send_frame(gate_conn, OBICALL_MSG_RESULT_PUBLISH, rbuf, rlen, 2000) == 0) {
                    uint8_t mt;
                    uint8_t ackbuf[64];
                    uint32_t acklen;
                    osal_recv_frame(gate_conn, &mt, ackbuf, sizeof(ackbuf), &acklen, 2000);
                }
            }
        }

        int64_t now = osal_monotonic_ns();
        if (now - last_heartbeat >= heartbeat_period_ns) {
            last_heartbeat = now;
            obicall_msg_heartbeat_t hb;
            memset(&hb, 0, sizeof(hb));
            hb.broker_id = g_ctx.broker_id;
            hb.epoch = current_epoch_belief;
            hb.last_window_seq = g_ctx.last_processed_window_seq;
            memcpy(hb.config_digest, g_ctx.config_digest, OBICALL_DIGEST_LEN);
            hb.checkpoint_schema_version = OBICALL_CHECKPOINT_SCHEMA_VERSION;
            hb.healthy = g_ctx.reader_alive ? 1 : 0;
            hb.sent_at_ns = now;

            uint8_t hbuf[128];
            uint32_t hlen = 0;
            if (obicall_wire_encode_heartbeat(&hb, hbuf, sizeof(hbuf), &hlen) == OBICALL_OK &&
                osal_send_frame(gate_conn, OBICALL_MSG_HEARTBEAT, hbuf, hlen, 2000) == 0) {
                uint8_t mt;
                uint8_t sbuf[64];
                uint32_t slen;
                if (osal_recv_frame(gate_conn, &mt, sbuf, sizeof(sbuf), &slen, 2000) == 0 &&
                    mt == OBICALL_MSG_STATUS_REPLY) {
                    obicall_msg_status_reply_t reply;
                    if (obicall_wire_decode_status_reply(sbuf, slen, &reply) == OBICALL_OK) {
                        current_epoch_belief = reply.gate_epoch;
                        if (reply.owner_broker_id != current_owner_belief) {
                            fprintf(stderr, "obicall-brokerd[%s]: gate reports owner=%u epoch=%llu (was owner=%u)\n",
                                    broker_id_s, reply.owner_broker_id, (unsigned long long)reply.gate_epoch,
                                    current_owner_belief);
                            current_owner_belief = reply.owner_broker_id;
                        }
                    }
                }
            }
        }

        if (batch_n == 0) osal_sleep_ms(10);
    }
}
