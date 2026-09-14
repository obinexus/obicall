#include "obicall/wire.h"

#include <string.h>

#include "wire_cursor.h"

static int32_t rc_i32(rcursor_t* c, int* ok) { return (int32_t)rc_u32(c, ok); }
static void wc_i32(wcursor_t* c, int32_t v, int* ok) { wc_u32(c, (uint32_t)v, ok); }

obicall_status_t OBICALL_CALL obicall_wire_encode_header(const obicall_wire_header_t* header,
                                                           uint8_t out[OBICALL_WIRE_HEADER_LEN]) {
    if (!header || !out) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, OBICALL_WIRE_HEADER_LEN, 0};
    int ok = 1;
    wc_u8(&c, OBICALL_WIRE_MAGIC0, &ok);
    wc_u8(&c, OBICALL_WIRE_MAGIC1, &ok);
    wc_u8(&c, OBICALL_WIRE_MAGIC2, &ok);
    wc_u8(&c, OBICALL_WIRE_MAGIC3, &ok);
    wc_u8(&c, header->wire_version, &ok);
    wc_u8(&c, header->msg_type, &ok);
    wc_u16(&c, header->flags, &ok);
    wc_u32(&c, header->payload_len, &ok);
    wc_u32(&c, header->crc32, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_decode_header(const uint8_t in[OBICALL_WIRE_HEADER_LEN],
                                                           obicall_wire_header_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    rcursor_t c = {in, OBICALL_WIRE_HEADER_LEN, 0};
    int ok = 1;
    uint8_t m0 = rc_u8(&c, &ok), m1 = rc_u8(&c, &ok), m2 = rc_u8(&c, &ok), m3 = rc_u8(&c, &ok);
    if (!ok || m0 != OBICALL_WIRE_MAGIC0 || m1 != OBICALL_WIRE_MAGIC1 || m2 != OBICALL_WIRE_MAGIC2 ||
        m3 != OBICALL_WIRE_MAGIC3) {
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    out->wire_version = rc_u8(&c, &ok);
    out->msg_type = rc_u8(&c, &ok);
    out->flags = rc_u16(&c, &ok);
    out->payload_len = rc_u32(&c, &ok);
    out->crc32 = rc_u32(&c, &ok);
    if (!ok) return OBICALL_ERR_WIRE_MALFORMED;
    if (out->wire_version != OBICALL_WIRE_VERSION) return OBICALL_ERR_WIRE_VERSION_UNSUPPORTED;
    if (out->payload_len > OBICALL_WIRE_MAX_PAYLOAD) return OBICALL_ERR_WIRE_TOO_LARGE;
    return OBICALL_OK;
}

/* ---- observation ---------------------------------------------------- */

obicall_status_t OBICALL_CALL obicall_wire_encode_observation(const obicall_observation_t* obs,
                                                                uint8_t* out, uint32_t out_cap,
                                                                uint32_t* out_len) {
    if (!obs || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    if (obs->payload_count > OBICALL_MAX_PAYLOAD_DOUBLES ||
        obs->covariance_count > OBICALL_MAX_COV_DOUBLES) {
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, obs->struct_size, &ok);
    wc_u32(&c, obs->schema_version, &ok);
    wc_fixed(&c, obs->sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
    wc_u64(&c, obs->source_boot_id, &ok);
    wc_u64(&c, obs->sequence, &ok);
    wc_i64(&c, obs->sample_time_ns, &ok);
    wc_i64(&c, obs->arrival_time_ns, &ok);
    wc_u32(&c, obs->clock_domain, &ok);
    wc_f64(&c, obs->time_uncertainty_s, &ok);
    wc_u32(&c, obs->coordinate_frame, &ok);
    wc_u32(&c, obs->units, &ok);
    wc_u32(&c, obs->calibration_version, &ok);
    wc_u32(&c, obs->payload_shape, &ok);
    wc_u32(&c, obs->payload_count, &ok);
    for (uint32_t i = 0; i < obs->payload_count; ++i) wc_f64(&c, obs->payload[i], &ok);
    wc_u32(&c, obs->covariance_count, &ok);
    for (uint32_t i = 0; i < obs->covariance_count; ++i) wc_f64(&c, obs->covariance[i], &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_wire_decode_observation(const uint8_t* in, uint32_t in_len,
                                                                obicall_observation_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->struct_size = rc_u32(&c, &ok);
    out->schema_version = rc_u32(&c, &ok);
    rc_fixed(&c, out->sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
    out->source_boot_id = rc_u64(&c, &ok);
    out->sequence = rc_u64(&c, &ok);
    out->sample_time_ns = rc_i64(&c, &ok);
    out->arrival_time_ns = rc_i64(&c, &ok);
    out->clock_domain = rc_u32(&c, &ok);
    out->time_uncertainty_s = rc_f64(&c, &ok);
    out->coordinate_frame = rc_u32(&c, &ok);
    out->units = rc_u32(&c, &ok);
    out->calibration_version = rc_u32(&c, &ok);
    out->payload_shape = rc_u32(&c, &ok);
    out->payload_count = rc_u32(&c, &ok);
    if (!ok || out->payload_count > OBICALL_MAX_PAYLOAD_DOUBLES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->payload_count; ++i) out->payload[i] = rc_f64(&c, &ok);
    out->covariance_count = rc_u32(&c, &ok);
    if (!ok || out->covariance_count > OBICALL_MAX_COV_DOUBLES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->covariance_count; ++i) out->covariance[i] = rc_f64(&c, &ok);
    out->struct_size = sizeof(*out); /* the decoded native struct is this compiler's layout */
    if (!ok) return OBICALL_ERR_WIRE_MALFORMED;
    return OBICALL_OK;
}

/* ---- result ----------------------------------------------------------*/

obicall_status_t OBICALL_CALL obicall_wire_encode_result(const obicall_result_t* r, uint8_t* out,
                                                           uint32_t out_cap, uint32_t* out_len) {
    if (!r || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    if (r->payload_count > OBICALL_MAX_PAYLOAD_DOUBLES || r->covariance_count > OBICALL_MAX_COV_DOUBLES ||
        r->source_count > OBICALL_MAX_SOURCES) {
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, r->struct_size, &ok);
    wc_u32(&c, r->schema_version, &ok);
    wc_fixed(&c, r->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    wc_u64(&c, r->window_seq, &ok);
    wc_u32(&c, r->broker_id, &ok);
    wc_u64(&c, r->epoch, &ok);
    wc_u32(&c, r->status, &ok);
    wc_u32(&c, r->payload_shape, &ok);
    wc_u32(&c, r->payload_count, &ok);
    for (uint32_t i = 0; i < r->payload_count; ++i) wc_f64(&c, r->payload[i], &ok);
    wc_u32(&c, r->covariance_count, &ok);
    for (uint32_t i = 0; i < r->covariance_count; ++i) wc_f64(&c, r->covariance[i], &ok);
    wc_bytes(&c, r->input_digest, OBICALL_DIGEST_LEN, &ok);
    wc_bytes(&c, r->config_digest, OBICALL_DIGEST_LEN, &ok);
    wc_i64(&c, r->timestamp_ns, &ok);
    wc_i64(&c, r->valid_until_ns, &ok);
    wc_u32(&c, r->source_count, &ok);
    for (uint32_t i = 0; i < r->source_count; ++i) wc_fixed(&c, r->sources[i], OBICALL_SENSOR_ID_LEN, &ok);
    wc_u32(&c, r->dgt_action_id, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_wire_decode_result(const uint8_t* in, uint32_t in_len,
                                                           obicall_result_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->struct_size = rc_u32(&c, &ok);
    out->schema_version = rc_u32(&c, &ok);
    rc_fixed(&c, out->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    out->window_seq = rc_u64(&c, &ok);
    out->broker_id = rc_u32(&c, &ok);
    out->epoch = rc_u64(&c, &ok);
    out->status = rc_u32(&c, &ok);
    out->payload_shape = rc_u32(&c, &ok);
    out->payload_count = rc_u32(&c, &ok);
    if (!ok || out->payload_count > OBICALL_MAX_PAYLOAD_DOUBLES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->payload_count; ++i) out->payload[i] = rc_f64(&c, &ok);
    out->covariance_count = rc_u32(&c, &ok);
    if (!ok || out->covariance_count > OBICALL_MAX_COV_DOUBLES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->covariance_count; ++i) out->covariance[i] = rc_f64(&c, &ok);
    rc_bytes(&c, out->input_digest, OBICALL_DIGEST_LEN, &ok);
    rc_bytes(&c, out->config_digest, OBICALL_DIGEST_LEN, &ok);
    out->timestamp_ns = rc_i64(&c, &ok);
    out->valid_until_ns = rc_i64(&c, &ok);
    out->source_count = rc_u32(&c, &ok);
    if (!ok || out->source_count > OBICALL_MAX_SOURCES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->source_count; ++i) rc_fixed(&c, out->sources[i], OBICALL_SENSOR_ID_LEN, &ok);
    out->dgt_action_id = rc_u32(&c, &ok);
    out->struct_size = sizeof(*out);
    if (!ok) return OBICALL_ERR_WIRE_MALFORMED;
    return OBICALL_OK;
}

/* ---- checkpoint --------------------------------------------------------*/

obicall_status_t OBICALL_CALL obicall_wire_encode_checkpoint(const obicall_checkpoint_t* k,
                                                               uint8_t* out, uint32_t out_cap,
                                                               uint32_t* out_len) {
    if (!k || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    if (k->state_dim > OBICALL_MAX_STATE_DIM || k->source_count > OBICALL_MAX_TRACKED_SOURCES) {
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, k->struct_size, &ok);
    wc_u32(&c, k->schema_version, &ok);
    wc_fixed(&c, k->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    wc_u64(&c, k->window_seq, &ok);
    wc_u32(&c, k->estimator_mode, &ok);
    wc_u32(&c, k->state_dim, &ok);
    for (uint32_t i = 0; i < k->state_dim; ++i) wc_f64(&c, k->state_mean[i], &ok);
    for (uint32_t i = 0; i < k->state_dim * k->state_dim; ++i) wc_f64(&c, k->state_covariance[i], &ok);
    wc_u32(&c, k->calibration_version, &ok);
    wc_u32(&c, k->policy_version, &ok);
    wc_u32(&c, k->dgt_action_id, &ok);
    wc_u32(&c, k->source_count, &ok);
    for (uint32_t i = 0; i < k->source_count; ++i) {
        wc_fixed(&c, k->sources[i].sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
        wc_u64(&c, k->sources[i].source_boot_id, &ok);
        wc_u64(&c, k->sources[i].last_sequence, &ok);
    }
    wc_i64(&c, k->created_at_ns, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_wire_decode_checkpoint(const uint8_t* in, uint32_t in_len,
                                                               obicall_checkpoint_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->struct_size = rc_u32(&c, &ok);
    out->schema_version = rc_u32(&c, &ok);
    rc_fixed(&c, out->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    out->window_seq = rc_u64(&c, &ok);
    out->estimator_mode = rc_u32(&c, &ok);
    out->state_dim = rc_u32(&c, &ok);
    if (!ok || out->state_dim > OBICALL_MAX_STATE_DIM) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->state_dim; ++i) out->state_mean[i] = rc_f64(&c, &ok);
    for (uint32_t i = 0; i < out->state_dim * out->state_dim; ++i) out->state_covariance[i] = rc_f64(&c, &ok);
    out->calibration_version = rc_u32(&c, &ok);
    out->policy_version = rc_u32(&c, &ok);
    out->dgt_action_id = rc_u32(&c, &ok);
    out->source_count = rc_u32(&c, &ok);
    if (!ok || out->source_count > OBICALL_MAX_TRACKED_SOURCES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->source_count; ++i) {
        rc_fixed(&c, out->sources[i].sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
        out->sources[i].source_boot_id = rc_u64(&c, &ok);
        out->sources[i].last_sequence = rc_u64(&c, &ok);
    }
    out->created_at_ns = rc_i64(&c, &ok);
    out->struct_size = sizeof(*out);
    if (!ok) return OBICALL_ERR_WIRE_MALFORMED;
    return OBICALL_OK;
}

/* ---- event (variable-length payload) ----------------------------------*/

obicall_status_t OBICALL_CALL obicall_wire_encode_event(const obicall_event_t* e, uint8_t* out,
                                                          uint32_t out_cap, uint32_t* out_len) {
    if (!e || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    if (e->payload.len > OBICALL_WIRE_MAX_PAYLOAD) return OBICALL_ERR_WIRE_TOO_LARGE;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, e->struct_size, &ok);
    wc_u32(&c, e->schema_version, &ok);
    wc_u32(&c, e->event_type, &ok);
    wc_u32(&c, e->reserved, &ok);
    wc_i64(&c, e->timestamp_ns, &ok);
    wc_u32(&c, e->payload.len, &ok);
    if (e->payload.len > 0) wc_bytes(&c, e->payload.data, e->payload.len, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_wire_decode_event(const uint8_t* in, uint32_t in_len,
                                                          uint8_t* payload_storage,
                                                          uint32_t payload_storage_cap,
                                                          obicall_event_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->struct_size = rc_u32(&c, &ok);
    out->schema_version = rc_u32(&c, &ok);
    out->event_type = rc_u32(&c, &ok);
    out->reserved = rc_u32(&c, &ok);
    out->timestamp_ns = rc_i64(&c, &ok);
    uint32_t payload_len = rc_u32(&c, &ok);
    if (!ok || payload_len > OBICALL_WIRE_MAX_PAYLOAD) return OBICALL_ERR_WIRE_MALFORMED;
    if (payload_len > 0) {
        if (!payload_storage || payload_storage_cap < payload_len) return OBICALL_ERR_BUFFER_TOO_SMALL;
        rc_bytes(&c, payload_storage, payload_len, &ok);
        out->payload.data = payload_storage;
        out->payload.len = payload_len;
    }
    out->struct_size = sizeof(*out);
    if (!ok) return OBICALL_ERR_WIRE_MALFORMED;
    return OBICALL_OK;
}

/* ---- control messages --------------------------------------------------*/

obicall_status_t OBICALL_CALL obicall_wire_encode_auth_hello(const obicall_msg_auth_hello_t* m,
                                                               uint8_t* out, uint32_t out_cap,
                                                               uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, m->protocol_version, &ok);
    wc_u32(&c, m->role, &ok);
    wc_bytes(&c, m->run_token, OBICALL_RUN_TOKEN_LEN, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_auth_hello(const uint8_t* in, uint32_t in_len,
                                                               obicall_msg_auth_hello_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->protocol_version = rc_u32(&c, &ok);
    out->role = rc_u32(&c, &ok);
    rc_bytes(&c, out->run_token, OBICALL_RUN_TOKEN_LEN, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_heartbeat(const obicall_msg_heartbeat_t* m,
                                                              uint8_t* out, uint32_t out_cap,
                                                              uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, m->broker_id, &ok);
    wc_u64(&c, m->epoch, &ok);
    wc_u64(&c, m->last_window_seq, &ok);
    wc_bytes(&c, m->config_digest, OBICALL_DIGEST_LEN, &ok);
    wc_u32(&c, m->checkpoint_schema_version, &ok);
    wc_u32(&c, m->healthy, &ok);
    wc_i64(&c, m->sent_at_ns, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_heartbeat(const uint8_t* in, uint32_t in_len,
                                                              obicall_msg_heartbeat_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->broker_id = rc_u32(&c, &ok);
    out->epoch = rc_u64(&c, &ok);
    out->last_window_seq = rc_u64(&c, &ok);
    rc_bytes(&c, out->config_digest, OBICALL_DIGEST_LEN, &ok);
    out->checkpoint_schema_version = rc_u32(&c, &ok);
    out->healthy = rc_u32(&c, &ok);
    out->sent_at_ns = rc_i64(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_epoch_grant(const obicall_msg_epoch_grant_t* m,
                                                                uint8_t* out, uint32_t out_cap,
                                                                uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u64(&c, m->new_epoch, &ok);
    wc_u32(&c, m->granted_to_broker_id, &ok);
    wc_i64(&c, m->granted_at_ns, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_epoch_grant(const uint8_t* in, uint32_t in_len,
                                                                obicall_msg_epoch_grant_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->new_epoch = rc_u64(&c, &ok);
    out->granted_to_broker_id = rc_u32(&c, &ok);
    out->granted_at_ns = rc_i64(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_epoch_revoke(const obicall_msg_epoch_revoke_t* m,
                                                                 uint8_t* out, uint32_t out_cap,
                                                                 uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u64(&c, m->revoked_epoch, &ok);
    wc_u32(&c, m->revoked_broker_id, &ok);
    wc_i64(&c, m->revoked_at_ns, &ok);
    wc_u32(&c, m->reason_code, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_epoch_revoke(const uint8_t* in, uint32_t in_len,
                                                                 obicall_msg_epoch_revoke_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->revoked_epoch = rc_u64(&c, &ok);
    out->revoked_broker_id = rc_u32(&c, &ok);
    out->revoked_at_ns = rc_i64(&c, &ok);
    out->reason_code = rc_u32(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_readiness_report(
    const obicall_msg_readiness_report_t* m, uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, m->broker_id, &ok);
    wc_bytes(&c, m->config_digest, OBICALL_DIGEST_LEN, &ok);
    wc_u32(&c, m->checkpoint_schema_version, &ok);
    wc_u64(&c, m->last_processed_window_seq, &ok);
    wc_u32(&c, m->healthy, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_readiness_report(
    const uint8_t* in, uint32_t in_len, obicall_msg_readiness_report_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->broker_id = rc_u32(&c, &ok);
    rc_bytes(&c, out->config_digest, OBICALL_DIGEST_LEN, &ok);
    out->checkpoint_schema_version = rc_u32(&c, &ok);
    out->last_processed_window_seq = rc_u64(&c, &ok);
    out->healthy = rc_u32(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_replay_request(
    const obicall_msg_replay_request_t* m, uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_fixed(&c, m->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    wc_u64(&c, m->from_window_seq, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_replay_request(const uint8_t* in, uint32_t in_len,
                                                                   obicall_msg_replay_request_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    rc_fixed(&c, out->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    out->from_window_seq = rc_u64(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_admission_decision(
    const obicall_msg_admission_decision_t* m, uint8_t* out, uint32_t out_cap, uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_fixed(&c, m->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    wc_u64(&c, m->window_seq, &ok);
    wc_fixed(&c, m->sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
    wc_u64(&c, m->sequence, &ok);
    wc_u32(&c, m->admitted, &ok);
    wc_u32(&c, m->reason_code, &ok);
    wc_i64(&c, m->decided_at_ns, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_admission_decision(
    const uint8_t* in, uint32_t in_len, obicall_msg_admission_decision_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    rc_fixed(&c, out->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    out->window_seq = rc_u64(&c, &ok);
    rc_fixed(&c, out->sensor_id, OBICALL_SENSOR_ID_LEN, &ok);
    out->sequence = rc_u64(&c, &ok);
    out->admitted = rc_u32(&c, &ok);
    out->reason_code = rc_u32(&c, &ok);
    out->decided_at_ns = rc_i64(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_result_ack(const obicall_msg_result_ack_t* m,
                                                               uint8_t* out, uint32_t out_cap,
                                                               uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_fixed(&c, m->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    wc_u64(&c, m->window_seq, &ok);
    wc_u32(&c, m->accepted, &ok);
    wc_i32(&c, m->reject_status, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_result_ack(const uint8_t* in, uint32_t in_len,
                                                               obicall_msg_result_ack_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    rc_fixed(&c, out->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
    out->window_seq = rc_u64(&c, &ok);
    out->accepted = rc_u32(&c, &ok);
    out->reject_status = rc_i32(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_wire_encode_status_reply(const obicall_msg_status_reply_t* m,
                                                                 uint8_t* out, uint32_t out_cap,
                                                                 uint32_t* out_len) {
    if (!m || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, m->running, &ok);
    wc_u64(&c, m->gate_epoch, &ok);
    wc_u32(&c, m->owner_broker_id, &ok);
    wc_u64(&c, m->last_committed_window_seq, &ok);
    wc_i64(&c, m->uptime_ns, &ok);
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}
obicall_status_t OBICALL_CALL obicall_wire_decode_status_reply(const uint8_t* in, uint32_t in_len,
                                                                 obicall_msg_status_reply_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->running = rc_u32(&c, &ok);
    out->gate_epoch = rc_u64(&c, &ok);
    out->owner_broker_id = rc_u32(&c, &ok);
    out->last_committed_window_seq = rc_u64(&c, &ok);
    out->uptime_ns = rc_i64(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}
