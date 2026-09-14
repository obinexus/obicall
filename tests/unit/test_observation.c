#include "obicall_test.h"
#include "obicall/obicall.h"

static void make_valid(obicall_observation_t* obs) {
    memset(obs, 0, sizeof(*obs));
    obs->struct_size = sizeof(*obs);
    obs->schema_version = OBICALL_OBSERVATION_SCHEMA_VERSION;
    strncpy(obs->sensor_id, "cam_a", sizeof(obs->sensor_id) - 1);
    obs->source_boot_id = 1;
    obs->sequence = 1;
    obs->sample_time_ns = 1000;
    obs->arrival_time_ns = 1001;
    obs->clock_domain = OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC;
    obs->time_uncertainty_s = 0.01;
    obs->coordinate_frame = OBICALL_FRAME_LOCAL_ENU;
    obs->units = OBICALL_UNITS_METERS;
    obs->calibration_version = 1;
    obs->payload_shape = OBICALL_SHAPE_POSITION_2D;
    obs->payload_count = 2;
    obs->payload[0] = 1.0;
    obs->payload[1] = 2.0;
    obs->covariance_count = 4;
    obs->covariance[0] = 0.04;
    obs->covariance[1] = 0.0;
    obs->covariance[2] = 0.0;
    obs->covariance[3] = 0.09;
}

static int g_last_issue_count;
static obicall_validation_issue_t g_last_issues[16];

static void OBICALL_CALL collect(void* user_data, obicall_validation_issue_t issue) {
    (void)user_data;
    if (g_last_issue_count < 16) g_last_issues[g_last_issue_count++] = issue;
}

static int has_issue(obicall_validation_issue_t want) {
    for (int i = 0; i < g_last_issue_count; ++i) {
        if (g_last_issues[i] == want) return 1;
    }
    return 0;
}

static obicall_status_t validate_and_collect(const obicall_observation_t* obs) {
    g_last_issue_count = 0;
    return obicall_observation_validate(obs, collect, NULL);
}

static void test_valid_observation_passes(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(g_last_issue_count, 0);
}

static void test_nan_payload_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.payload[0] = NAN;
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_NON_FINITE);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_NON_FINITE_PAYLOAD));
}

static void test_infinite_covariance_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.covariance[0] = INFINITY;
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_NON_FINITE);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_NON_FINITE_COVARIANCE));
}

static void test_dimension_mismatch_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.payload_count = 3; /* POSITION_2D expects exactly 2 */
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_DIMENSION);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_PAYLOAD_COUNT_OUT_OF_RANGE));
}

static void test_asymmetric_covariance_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.covariance[1] = 5.0;
    obs.covariance[2] = -5.0; /* off-diagonals disagree */
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_COVARIANCE);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_COVARIANCE_NOT_SYMMETRIC));
}

static void test_non_psd_covariance_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    /* Symmetric but not PSD: negative eigenvalue. */
    obs.covariance[0] = 1.0;
    obs.covariance[1] = 2.0;
    obs.covariance[2] = 2.0;
    obs.covariance[3] = 1.0; /* det = 1 - 4 = -3 < 0 */
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_COVARIANCE);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_COVARIANCE_NOT_PSD));
}

static void test_empty_sensor_id_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.sensor_id[0] = '\0';
    OBICALL_CHECK(validate_and_collect(&obs) != OBICALL_OK);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_SENSOR_ID_EMPTY));
}

static void test_unspecified_frame_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.coordinate_frame = OBICALL_FRAME_UNSPECIFIED;
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_UNSUPPORTED);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_UNSUPPORTED_FRAME_OR_UNITS));
}

static void test_negative_time_uncertainty_rejected(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.time_uncertainty_s = -0.1;
    OBICALL_CHECK(validate_and_collect(&obs) == OBICALL_ERR_VALIDATION_TIMING);
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_TIME_UNCERTAINTY_NEGATIVE));
}

static void test_multiple_issues_all_reported(void) {
    obicall_observation_t obs;
    make_valid(&obs);
    obs.sensor_id[0] = '\0';
    obs.coordinate_frame = OBICALL_FRAME_UNSPECIFIED;
    obs.payload[0] = NAN;
    validate_and_collect(&obs);
    /* The callback must fire for every distinct category found, not stop
     * at the first (docs/ABI.md: obicall_observation_validate). */
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_SENSOR_ID_EMPTY));
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_UNSUPPORTED_FRAME_OR_UNITS));
    OBICALL_CHECK(has_issue(OBICALL_ISSUE_NON_FINITE_PAYLOAD));
}

static void test_payload_shape_dimension(void) {
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(OBICALL_SHAPE_POSITION_1D), 1);
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(OBICALL_SHAPE_POSITION_2D), 2);
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(OBICALL_SHAPE_POSITION_2D_VELOCITY), 4);
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(OBICALL_SHAPE_RANGE_1D), 1);
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(OBICALL_SHAPE_UNSPECIFIED), 0);
    OBICALL_CHECK_EQ_INT(obicall_payload_shape_dimension(9999), 0);
}

OBICALL_TEST_MAIN_BEGIN()
    test_valid_observation_passes();
    test_nan_payload_rejected();
    test_infinite_covariance_rejected();
    test_dimension_mismatch_rejected();
    test_asymmetric_covariance_rejected();
    test_non_psd_covariance_rejected();
    test_empty_sensor_id_rejected();
    test_unspecified_frame_rejected();
    test_negative_time_uncertainty_rejected();
    test_multiple_issues_all_reported();
    test_payload_shape_dimension();
OBICALL_TEST_MAIN_END()
