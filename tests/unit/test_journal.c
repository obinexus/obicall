#include "obicall_test.h"
#include "obicall/obicall.h"

static obicall_observation_t make_obs(const char* sensor_id, uint64_t boot_id, uint64_t seq, int64_t sample_ns) {
    obicall_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.struct_size = sizeof(obs);
    obs.schema_version = OBICALL_OBSERVATION_SCHEMA_VERSION;
    strncpy(obs.sensor_id, sensor_id, sizeof(obs.sensor_id) - 1);
    obs.source_boot_id = boot_id;
    obs.sequence = seq;
    obs.sample_time_ns = sample_ns;
    obs.arrival_time_ns = sample_ns;
    obs.clock_domain = OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC;
    obs.coordinate_frame = OBICALL_FRAME_LOCAL_ENU;
    obs.units = OBICALL_UNITS_METERS;
    obs.calibration_version = 1;
    obs.payload_shape = OBICALL_SHAPE_POSITION_2D;
    obs.payload_count = 2;
    obs.payload[0] = 1.0;
    obs.payload[1] = 2.0;
    obs.covariance_count = 4;
    obs.covariance[0] = 0.1;
    obs.covariance[3] = 0.1;
    return obs;
}

static obicall_journal_config_t default_cfg(void) {
    obicall_journal_config_t c;
    c.reorder_window_n = 4;
    c.max_lateness_ns = 1000000000LL; /* 1s */
    c.max_pending_per_pipeline = 8;
    c.overflow_policy = OBICALL_OVERFLOW_REJECT_NEWEST;
    return c;
}

static void test_first_observation_admitted(void) {
    obicall_journal_config_t cfg = default_cfg();
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs = make_obs("cam_a", 1, 1, 1000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 0) == OBICALL_ADMIT_OK);
    OBICALL_CHECK_EQ_INT(track.highest_sequence_seen, 1);
}

static void test_increasing_sequence_admitted(void) {
    obicall_journal_config_t cfg = default_cfg();
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    for (uint64_t seq = 1; seq <= 5; ++seq) {
        obicall_observation_t obs = make_obs("cam_a", 1, seq, (int64_t)seq * 1000000000LL);
        OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 0) == OBICALL_ADMIT_OK);
    }
    OBICALL_CHECK_EQ_INT(track.highest_sequence_seen, 5);
}

static void test_exact_duplicate_rejected(void) {
    obicall_journal_config_t cfg = default_cfg();
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs = make_obs("cam_a", 1, 3, 3000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 0) == OBICALL_ADMIT_OK);
    obicall_observation_t dup = make_obs("cam_a", 1, 3, 3000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &dup, 0) == OBICALL_REJECT_DUPLICATE);
}

static void test_within_window_reorder_treated_as_duplicate(void) {
    obicall_journal_config_t cfg = default_cfg(); /* reorder_window_n = 4 */
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs10 = make_obs("cam_a", 1, 10, 10000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs10, 0) == OBICALL_ADMIT_OK);
    /* sequence 8 is 2 behind 10, within the window of 4: treated as a
     * duplicate/already-superseded resend, not a new admission. */
    obicall_observation_t obs8 = make_obs("cam_a", 1, 8, 8000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs8, 0) == OBICALL_REJECT_DUPLICATE);
}

static void test_far_behind_sequence_rejected_as_late(void) {
    obicall_journal_config_t cfg = default_cfg(); /* reorder_window_n = 4 */
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs100 = make_obs("cam_a", 1, 100, 100000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs100, 0) == OBICALL_ADMIT_OK);
    obicall_observation_t obs1 = make_obs("cam_a", 1, 1, 1000000000LL); /* 99 behind: outside window */
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs1, 0) == OBICALL_REJECT_LATE);
}

static void test_stale_sample_time_rejected_as_late(void) {
    obicall_journal_config_t cfg = default_cfg(); /* max_lateness_ns = 1s */
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs1 = make_obs("cam_a", 1, 1, 10000000000LL); /* watermark = 10s */
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs1, 0) == OBICALL_ADMIT_OK);
    /* A new (higher) sequence but a sample_time far behind the watermark
     * must still be rejected as late - lateness is about time, not just
     * sequence ordering. */
    obicall_observation_t obs2 = make_obs("cam_a", 1, 2, 5000000000LL); /* 5s: 5s behind watermark, > 1s bound */
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs2, 0) == OBICALL_REJECT_LATE);
}

static void test_overflow_rejected_when_pending_at_bound(void) {
    obicall_journal_config_t cfg = default_cfg(); /* max_pending_per_pipeline = 8 */
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs = make_obs("cam_a", 1, 1, 1000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 8) == OBICALL_REJECT_OVERFLOW);
    /* One below the bound must still be admitted. */
    obicall_observation_t obs2 = make_obs("cam_a", 1, 2, 2000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs2, 7) == OBICALL_ADMIT_OK);
}

static void test_boot_id_change_resets_tracking(void) {
    obicall_journal_config_t cfg = default_cfg();
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs = make_obs("cam_a", /*boot*/ 1, /*seq*/ 500, 500000000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 0) == OBICALL_ADMIT_OK);

    /* A restarted source (new boot id) starting again at a low sequence
     * must be admitted, not rejected as "far behind" against the old
     * boot's high-water mark. */
    obicall_observation_t restarted = make_obs("cam_a", /*boot*/ 2, /*seq*/ 1, 1000000LL);
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &restarted, 0) == OBICALL_ADMIT_OK);
    OBICALL_CHECK_EQ_INT(track.source_boot_id, 2);
    OBICALL_CHECK_EQ_INT(track.highest_sequence_seen, 1);
}

static void test_invalid_observation_rejected_before_sequence_logic(void) {
    obicall_journal_config_t cfg = default_cfg();
    obicall_sensor_track_t track;
    memset(&track, 0, sizeof(track));
    obicall_observation_t obs = make_obs("cam_a", 1, 1, 1000000000LL);
    obs.payload[0] = NAN; /* fails obicall_observation_validate */
    OBICALL_CHECK(obicall_journal_admit(&cfg, &track, &obs, 0) == OBICALL_REJECT_VALIDATION);
    /* Track must not have been advanced by a rejected observation. */
    OBICALL_CHECK_EQ_INT(track.has_data, 0);
}

static void test_record_header_roundtrip_and_append_read(void) {
    obicall_journal_record_header_t hdr;
    hdr.magic = OBICALL_JOURNAL_RECORD_MAGIC;
    hdr.record_len = OBICALL_JOURNAL_RECORD_HEADER_LEN + 100;
    hdr.observation_len = 100;
    hdr.crc32 = 0x12345678u;
    hdr.window_seq = 42;
    hdr.admitted_at_ns = 999;
    hdr.admission_reason = OBICALL_ADMIT_OK;
    hdr.reserved = 0;

    uint8_t buf[OBICALL_JOURNAL_RECORD_HEADER_LEN];
    OBICALL_CHECK(obicall_journal_encode_record_header(&hdr, buf) == OBICALL_OK);
    obicall_journal_record_header_t decoded;
    OBICALL_CHECK(obicall_journal_decode_record_header(buf, &decoded) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decoded.window_seq, 42);
    OBICALL_CHECK_EQ_INT(decoded.observation_len, 100);

    FILE* f = tmpfile();
    OBICALL_CHECK(f != NULL);
    if (f) {
        obicall_observation_t obs = make_obs("cam_a", 1, 1, 1000000000LL);
        OBICALL_CHECK(obicall_journal_append_record(f, 1, 555, OBICALL_ADMIT_OK, &obs) == OBICALL_OK);
        obicall_observation_t obs2 = make_obs("imu_a", 2, 1, 2000000000LL);
        OBICALL_CHECK(obicall_journal_append_record(f, 2, 666, OBICALL_REJECT_LATE, &obs2) == OBICALL_OK);

        rewind(f);
        obicall_journal_record_header_t h1;
        obicall_observation_t r1;
        OBICALL_CHECK(obicall_journal_read_record(f, &h1, &r1) == OBICALL_OK);
        OBICALL_CHECK_EQ_INT(h1.window_seq, 1);
        OBICALL_CHECK_EQ_INT(h1.admission_reason, OBICALL_ADMIT_OK);
        OBICALL_CHECK(strcmp(r1.sensor_id, "cam_a") == 0);

        obicall_journal_record_header_t h2;
        obicall_observation_t r2;
        OBICALL_CHECK(obicall_journal_read_record(f, &h2, &r2) == OBICALL_OK);
        OBICALL_CHECK_EQ_INT(h2.window_seq, 2);
        OBICALL_CHECK_EQ_INT(h2.admission_reason, OBICALL_REJECT_LATE);

        obicall_journal_record_header_t h3;
        obicall_observation_t r3;
        OBICALL_CHECK(obicall_journal_read_record(f, &h3, &r3) == OBICALL_ERR_NOT_FOUND);
        fclose(f);
    }
}

static void test_read_record_detects_torn_write(void) {
    FILE* f = tmpfile();
    OBICALL_CHECK(f != NULL);
    if (!f) return;
    obicall_observation_t obs = make_obs("cam_a", 1, 1, 1000000000LL);
    OBICALL_CHECK(obicall_journal_append_record(f, 1, 1, OBICALL_ADMIT_OK, &obs) == OBICALL_OK);
    long full_size = ftell(f);

    /* Simulate a crash mid-write: truncate to 3/4 of the record. */
    FILE* f2 = tmpfile();
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc((size_t)full_size);
    size_t n = fread(buf, 1, (size_t)full_size, f);
    fclose(f);
    OBICALL_CHECK_EQ_INT((long)n, full_size);
    size_t torn_size = (size_t)full_size * 3 / 4;
    fwrite(buf, 1, torn_size, f2);
    free(buf);
    rewind(f2);

    long pos_before = ftell(f2);
    obicall_journal_record_header_t h;
    obicall_observation_t r;
    obicall_status_t st = obicall_journal_read_record(f2, &h, &r);
    OBICALL_CHECK(st == OBICALL_ERR_WIRE_MALFORMED || st == OBICALL_ERR_WIRE_CHECKSUM);
    /* Position must be left at the start of the bad record, not
     * advanced, so the caller can truncate exactly there. */
    OBICALL_CHECK_EQ_INT(ftell(f2), pos_before);
    fclose(f2);
}

OBICALL_TEST_MAIN_BEGIN()
    test_first_observation_admitted();
    test_increasing_sequence_admitted();
    test_exact_duplicate_rejected();
    test_within_window_reorder_treated_as_duplicate();
    test_far_behind_sequence_rejected_as_late();
    test_stale_sample_time_rejected_as_late();
    test_overflow_rejected_when_pending_at_bound();
    test_boot_id_change_resets_tracking();
    test_invalid_observation_rejected_before_sequence_logic();
    test_record_header_roundtrip_and_append_read();
    test_read_record_detects_torn_write();
OBICALL_TEST_MAIN_END()
