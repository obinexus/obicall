#include "obicall_test.h"
#include "obicall/obicall.h"

static void make_sample_observation(obicall_observation_t* obs) {
    memset(obs, 0, sizeof(*obs));
    obs->struct_size = sizeof(*obs);
    obs->schema_version = OBICALL_OBSERVATION_SCHEMA_VERSION;
    strncpy(obs->sensor_id, "cam_a", sizeof(obs->sensor_id) - 1);
    obs->source_boot_id = 0x1122334455667788ull;
    obs->sequence = 42;
    obs->sample_time_ns = 1234567890123LL;
    obs->arrival_time_ns = 1234567891000LL;
    obs->clock_domain = OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC;
    obs->time_uncertainty_s = 0.01;
    obs->coordinate_frame = OBICALL_FRAME_LOCAL_ENU;
    obs->units = OBICALL_UNITS_METERS;
    obs->calibration_version = 3;
    obs->payload_shape = OBICALL_SHAPE_POSITION_2D;
    obs->payload_count = 2;
    obs->payload[0] = 10.5;
    obs->payload[1] = -3.25;
    obs->covariance_count = 4;
    obs->covariance[0] = 0.04;
    obs->covariance[1] = 0.0;
    obs->covariance[2] = 0.0;
    obs->covariance[3] = 0.09;
}

static void test_header_roundtrip(void) {
    obicall_wire_header_t h;
    h.wire_version = OBICALL_WIRE_VERSION;
    h.msg_type = OBICALL_MSG_OBSERVATION_SUBMIT;
    h.flags = 0;
    h.payload_len = 12345;
    h.crc32 = 0xDEADBEEFu;

    uint8_t buf[OBICALL_WIRE_HEADER_LEN];
    OBICALL_CHECK(obicall_wire_encode_header(&h, buf) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(buf[0], 'O');
    OBICALL_CHECK_EQ_INT(buf[1], 'B');
    OBICALL_CHECK_EQ_INT(buf[2], 'W');
    OBICALL_CHECK_EQ_INT(buf[3], 'F');

    obicall_wire_header_t decoded;
    OBICALL_CHECK(obicall_wire_decode_header(buf, &decoded) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decoded.wire_version, h.wire_version);
    OBICALL_CHECK_EQ_INT(decoded.msg_type, h.msg_type);
    OBICALL_CHECK_EQ_INT(decoded.payload_len, h.payload_len);
    OBICALL_CHECK_EQ_INT(decoded.crc32, h.crc32);

    /* Corrupt the magic: must be rejected, not silently parsed. */
    uint8_t bad[OBICALL_WIRE_HEADER_LEN];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 'X';
    obicall_wire_header_t junk;
    OBICALL_CHECK(obicall_wire_decode_header(bad, &junk) == OBICALL_ERR_WIRE_MALFORMED);

    /* Unsupported version must be rejected. */
    uint8_t badver[OBICALL_WIRE_HEADER_LEN];
    memcpy(badver, buf, sizeof(badver));
    badver[4] = 99;
    OBICALL_CHECK(obicall_wire_decode_header(badver, &junk) == OBICALL_ERR_WIRE_VERSION_UNSUPPORTED);

    /* Oversized payload_len must be rejected. */
    obicall_wire_header_t huge = h;
    huge.payload_len = OBICALL_WIRE_MAX_PAYLOAD + 1;
    uint8_t hugebuf[OBICALL_WIRE_HEADER_LEN];
    OBICALL_CHECK(obicall_wire_encode_header(&huge, hugebuf) == OBICALL_OK);
    OBICALL_CHECK(obicall_wire_decode_header(hugebuf, &junk) == OBICALL_ERR_WIRE_TOO_LARGE);
}

static void test_observation_roundtrip(void) {
    obicall_observation_t obs;
    make_sample_observation(&obs);

    uint8_t buf[512];
    uint32_t len = 0;
    OBICALL_CHECK(obicall_wire_encode_observation(&obs, buf, sizeof(buf), &len) == OBICALL_OK);
    OBICALL_CHECK(len > 0 && len < sizeof(buf));

    obicall_observation_t decoded;
    OBICALL_CHECK(obicall_wire_decode_observation(buf, len, &decoded) == OBICALL_OK);
    OBICALL_CHECK(strcmp(decoded.sensor_id, obs.sensor_id) == 0);
    OBICALL_CHECK_EQ_INT(decoded.source_boot_id, obs.source_boot_id);
    OBICALL_CHECK_EQ_INT(decoded.sequence, obs.sequence);
    OBICALL_CHECK_EQ_INT(decoded.sample_time_ns, obs.sample_time_ns);
    OBICALL_CHECK_NEAR(decoded.time_uncertainty_s, obs.time_uncertainty_s, 1e-12);
    OBICALL_CHECK_EQ_INT(decoded.payload_count, obs.payload_count);
    OBICALL_CHECK_NEAR(decoded.payload[0], obs.payload[0], 1e-12);
    OBICALL_CHECK_NEAR(decoded.payload[1], obs.payload[1], 1e-12);
    OBICALL_CHECK_EQ_INT(decoded.covariance_count, obs.covariance_count);
    OBICALL_CHECK_NEAR(decoded.covariance[3], obs.covariance[3], 1e-12);

    /* Buffer too small must fail cleanly, not overflow. */
    uint8_t tiny[4];
    uint32_t tiny_len = 0;
    OBICALL_CHECK(obicall_wire_encode_observation(&obs, tiny, sizeof(tiny), &tiny_len) ==
                  OBICALL_ERR_BUFFER_TOO_SMALL);

    /* Truncated input must be rejected, not read out of bounds. */
    obicall_observation_t junk;
    OBICALL_CHECK(obicall_wire_decode_observation(buf, 5, &junk) == OBICALL_ERR_WIRE_MALFORMED);

    /* A payload_count claiming more doubles than OBICALL_MAX_PAYLOAD_DOUBLES
     * must be rejected before it is used to drive further reads (offset
     * 100: struct_size(4) + schema_version(4) + sensor_id(32) +
     * source_boot_id(8) + sequence(8) + sample_time_ns(8) +
     * arrival_time_ns(8) + clock_domain(4) + time_uncertainty_s(8) +
     * coordinate_frame(4) + units(4) + calibration_version(4) +
     * payload_shape(4) = 100). */
    uint8_t forged[512];
    memcpy(forged, buf, len);
    forged[100] = 0xFF;
    forged[101] = 0xFF;
    forged[102] = 0xFF;
    forged[103] = 0xFF;
    OBICALL_CHECK(obicall_wire_decode_observation(forged, len, &junk) == OBICALL_ERR_WIRE_MALFORMED);
}

static void test_result_roundtrip(void) {
    obicall_result_t r;
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.schema_version = OBICALL_RESULT_SCHEMA_VERSION;
    strncpy(r.pipeline_id, "position-fusion", sizeof(r.pipeline_id) - 1);
    r.window_seq = 777;
    r.broker_id = OBICALL_BROKER_B;
    r.epoch = 5;
    r.status = OBICALL_RESULT_VALID;
    r.payload_shape = OBICALL_SHAPE_POSITION_2D;
    r.payload_count = 2;
    r.payload[0] = 1.5;
    r.payload[1] = 2.5;
    r.covariance_count = 4;
    r.covariance[0] = 0.1;
    r.covariance[3] = 0.2;
    for (uint32_t i = 0; i < OBICALL_DIGEST_LEN; ++i) r.input_digest[i] = (uint8_t)i;
    r.timestamp_ns = 1000;
    r.valid_until_ns = 2000;
    r.source_count = 2;
    strncpy(r.sources[0], "cam_a", OBICALL_SENSOR_ID_LEN - 1);
    strncpy(r.sources[1], "imu_a", OBICALL_SENSOR_ID_LEN - 1);
    r.dgt_action_id = 2;

    uint8_t buf[1024];
    uint32_t len = 0;
    OBICALL_CHECK(obicall_wire_encode_result(&r, buf, sizeof(buf), &len) == OBICALL_OK);

    obicall_result_t decoded;
    OBICALL_CHECK(obicall_wire_decode_result(buf, len, &decoded) == OBICALL_OK);
    OBICALL_CHECK(strcmp(decoded.pipeline_id, r.pipeline_id) == 0);
    OBICALL_CHECK_EQ_INT(decoded.window_seq, r.window_seq);
    OBICALL_CHECK_EQ_INT(decoded.broker_id, r.broker_id);
    OBICALL_CHECK_EQ_INT(decoded.epoch, r.epoch);
    OBICALL_CHECK_EQ_INT(decoded.source_count, r.source_count);
    OBICALL_CHECK(strcmp(decoded.sources[1], "imu_a") == 0);
    OBICALL_CHECK_EQ_INT(decoded.input_digest[7], 7);
    OBICALL_CHECK_EQ_INT(decoded.dgt_action_id, r.dgt_action_id);
}

static void test_checkpoint_roundtrip(void) {
    obicall_checkpoint_t k;
    memset(&k, 0, sizeof(k));
    k.struct_size = sizeof(k);
    k.schema_version = OBICALL_CHECKPOINT_SCHEMA_VERSION;
    strncpy(k.pipeline_id, "position-fusion", sizeof(k.pipeline_id) - 1);
    k.window_seq = 88;
    k.estimator_mode = OBICALL_ESTIMATOR_MODE_KALMAN_CV;
    k.state_dim = 4;
    for (int i = 0; i < 4; ++i) k.state_mean[i] = i * 1.5;
    for (int i = 0; i < 16; ++i) k.state_covariance[i] = i * 0.1;
    k.calibration_version = 1;
    k.policy_version = 1;
    k.dgt_action_id = 2;
    k.source_count = 2;
    strncpy(k.sources[0].sensor_id, "cam_a", OBICALL_SENSOR_ID_LEN - 1);
    k.sources[0].source_boot_id = 111;
    k.sources[0].last_sequence = 42;
    strncpy(k.sources[1].sensor_id, "imu_a", OBICALL_SENSOR_ID_LEN - 1);
    k.created_at_ns = 999;

    uint8_t buf[2048];
    uint32_t len = 0;
    OBICALL_CHECK(obicall_wire_encode_checkpoint(&k, buf, sizeof(buf), &len) == OBICALL_OK);

    obicall_checkpoint_t decoded;
    OBICALL_CHECK(obicall_wire_decode_checkpoint(buf, len, &decoded) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decoded.window_seq, k.window_seq);
    OBICALL_CHECK_EQ_INT(decoded.state_dim, k.state_dim);
    OBICALL_CHECK_NEAR(decoded.state_mean[2], k.state_mean[2], 1e-12);
    OBICALL_CHECK_NEAR(decoded.state_covariance[15], k.state_covariance[15], 1e-12);
    OBICALL_CHECK_EQ_INT(decoded.source_count, k.source_count);
    OBICALL_CHECK(strcmp(decoded.sources[1].sensor_id, "imu_a") == 0);
    OBICALL_CHECK_EQ_INT(decoded.sources[0].last_sequence, 42);
}

static void test_event_roundtrip(void) {
    obicall_observation_t inner;
    make_sample_observation(&inner);
    uint8_t inner_buf[512];
    uint32_t inner_len = 0;
    OBICALL_CHECK(obicall_wire_encode_observation(&inner, inner_buf, sizeof(inner_buf), &inner_len) == OBICALL_OK);

    obicall_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.schema_version = OBICALL_EVENT_SCHEMA_VERSION;
    ev.event_type = 1;
    ev.timestamp_ns = 555;
    ev.payload.data = inner_buf;
    ev.payload.len = inner_len;

    uint8_t buf[1024];
    uint32_t len = 0;
    OBICALL_CHECK(obicall_wire_encode_event(&ev, buf, sizeof(buf), &len) == OBICALL_OK);

    uint8_t storage[512];
    obicall_event_t decoded;
    OBICALL_CHECK(obicall_wire_decode_event(buf, len, storage, sizeof(storage), &decoded) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decoded.event_type, ev.event_type);
    OBICALL_CHECK_EQ_INT(decoded.timestamp_ns, ev.timestamp_ns);
    OBICALL_CHECK_EQ_INT(decoded.payload.len, inner_len);
    OBICALL_CHECK(memcmp(decoded.payload.data, inner_buf, inner_len) == 0);

    /* Undersized storage must fail rather than truncate silently. */
    uint8_t small_storage[4];
    obicall_event_t junk;
    OBICALL_CHECK(obicall_wire_decode_event(buf, len, small_storage, sizeof(small_storage), &junk) ==
                  OBICALL_ERR_BUFFER_TOO_SMALL);
}

static void test_control_messages(void) {
    obicall_msg_heartbeat_t hb;
    memset(&hb, 0, sizeof(hb));
    hb.broker_id = OBICALL_BROKER_A;
    hb.epoch = 3;
    hb.last_window_seq = 100;
    hb.checkpoint_schema_version = 1;
    hb.healthy = 1;
    hb.sent_at_ns = 42;
    uint8_t buf[256];
    uint32_t len = 0;
    OBICALL_CHECK(obicall_wire_encode_heartbeat(&hb, buf, sizeof(buf), &len) == OBICALL_OK);
    obicall_msg_heartbeat_t hb2;
    OBICALL_CHECK(obicall_wire_decode_heartbeat(buf, len, &hb2) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(hb2.epoch, hb.epoch);
    OBICALL_CHECK_EQ_INT(hb2.last_window_seq, hb.last_window_seq);

    obicall_msg_epoch_grant_t g;
    g.new_epoch = 9;
    g.granted_to_broker_id = OBICALL_BROKER_B;
    g.granted_at_ns = 123;
    OBICALL_CHECK(obicall_wire_encode_epoch_grant(&g, buf, sizeof(buf), &len) == OBICALL_OK);
    obicall_msg_epoch_grant_t g2;
    OBICALL_CHECK(obicall_wire_decode_epoch_grant(buf, len, &g2) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(g2.new_epoch, 9);
    OBICALL_CHECK_EQ_INT(g2.granted_to_broker_id, OBICALL_BROKER_B);

    obicall_msg_result_ack_t ack;
    memset(&ack, 0, sizeof(ack));
    strncpy(ack.pipeline_id, "p", sizeof(ack.pipeline_id) - 1);
    ack.window_seq = 55;
    ack.accepted = 0;
    ack.reject_status = OBICALL_ERR_GATE_STALE_EPOCH;
    OBICALL_CHECK(obicall_wire_encode_result_ack(&ack, buf, sizeof(buf), &len) == OBICALL_OK);
    obicall_msg_result_ack_t ack2;
    OBICALL_CHECK(obicall_wire_decode_result_ack(buf, len, &ack2) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(ack2.accepted, 0);
    OBICALL_CHECK_EQ_INT(ack2.reject_status, OBICALL_ERR_GATE_STALE_EPOCH);
}

static void test_crc32_known_vector(void) {
    /* CRC-32/ISO-HDLC of ASCII "123456789" is the standard 0xCBF43926 check value. */
    const uint8_t input[] = "123456789";
    OBICALL_CHECK_EQ_INT(obicall_crc32(input, 9), 0xCBF43926u);
}

OBICALL_TEST_MAIN_BEGIN()
    test_crc32_known_vector();
    test_header_roundtrip();
    test_observation_roundtrip();
    test_result_roundtrip();
    test_checkpoint_roundtrip();
    test_event_roundtrip();
    test_control_messages();
OBICALL_TEST_MAIN_END()
