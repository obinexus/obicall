#include "obicall_test.h"
#include "obicall/obicall.h"

/* The architecture document's worked example: independent unbiased
 * readings 10.0 m (var 0.04 m^2) and 10.4 m (var 0.16 m^2) fuse via
 * inverse-variance weighting to 10.08 m, 0.032 m^2 - a pure
 * static-fusion identity, not a Kalman update with a prior (see
 * docs/VALIDATION.md and obicall_static_fuse_1d's own doc comment). */
static void test_static_fuse_worked_example(void) {
    double means[2] = {10.0, 10.4};
    double variances[2] = {0.04, 0.16};
    double out_mean = 0, out_var = 0;
    OBICALL_CHECK(obicall_static_fuse_1d(means, variances, 2, &out_mean, &out_var) == OBICALL_OK);
    OBICALL_CHECK_NEAR(out_mean, 10.08, 1e-9);
    OBICALL_CHECK_NEAR(out_var, 0.032, 1e-9);
}

static void test_static_fuse_single_measurement_is_identity(void) {
    double means[1] = {7.5};
    double variances[1] = {0.5};
    double out_mean = 0, out_var = 0;
    OBICALL_CHECK(obicall_static_fuse_1d(means, variances, 1, &out_mean, &out_var) == OBICALL_OK);
    OBICALL_CHECK_NEAR(out_mean, 7.5, 1e-12);
    OBICALL_CHECK_NEAR(out_var, 0.5, 1e-12);
}

static void test_static_fuse_rejects_bad_input(void) {
    double means[2] = {1.0, 2.0};
    double bad_var[2] = {0.1, -1.0};
    double out_mean, out_var;
    OBICALL_CHECK(obicall_static_fuse_1d(means, bad_var, 2, &out_mean, &out_var) ==
                  OBICALL_ERR_VALIDATION_NON_FINITE);
    OBICALL_CHECK(obicall_static_fuse_1d(means, bad_var, 0, &out_mean, &out_var) == OBICALL_ERR_INVALID_ARGUMENT);
}

static obicall_kalman_params_t default_params(void) {
    obicall_kalman_params_t p;
    p.process_noise_position = 0.05;
    p.process_noise_velocity = 0.1;
    p.initial_position_variance = 10.0;
    p.initial_velocity_variance = 10.0;
    p.max_prediction_gap_s = 30.0;
    return p;
}

static void test_kalman_init_state(void) {
    obicall_kalman_params_t p = default_params();
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 3.0, 4.0, 1000000000LL);
    OBICALL_CHECK_EQ_INT(s.initialized, 1);
    OBICALL_CHECK_NEAR(s.mean[0], 3.0, 1e-12);
    OBICALL_CHECK_NEAR(s.mean[1], 4.0, 1e-12);
    OBICALL_CHECK_NEAR(s.mean[2], 0.0, 1e-12);
    OBICALL_CHECK_NEAR(s.mean[3], 0.0, 1e-12);
    OBICALL_CHECK_NEAR(s.covariance[0], p.initial_position_variance, 1e-12);
    OBICALL_CHECK_NEAR(s.covariance[5], p.initial_position_variance, 1e-12);
    OBICALL_CHECK_NEAR(s.covariance[10], p.initial_velocity_variance, 1e-12);
}

static void test_predict_only_grows_covariance_and_moves_by_velocity(void) {
    obicall_kalman_params_t p = default_params();
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 0.0, 0.0, 0);
    s.mean[2] = 2.0; /* vx */
    s.mean[3] = 1.0; /* vy */
    double cov_before = s.covariance[0];

    OBICALL_CHECK(obicall_kalman_predict(&s, &p, 1000000000LL) == OBICALL_OK); /* +1s */
    OBICALL_CHECK_NEAR(s.mean[0], 2.0, 1e-9); /* x += vx*dt */
    OBICALL_CHECK_NEAR(s.mean[1], 1.0, 1e-9);
    OBICALL_CHECK(s.covariance[0] > cov_before); /* uncertainty must grow without new evidence */

    /* Predicting to the same timestamp again is a no-op. */
    double mean0 = s.mean[0];
    OBICALL_CHECK(obicall_kalman_predict(&s, &p, 1000000000LL) == OBICALL_OK);
    OBICALL_CHECK_NEAR(s.mean[0], mean0, 1e-12);
}

static void test_predict_rejects_backwards_time(void) {
    obicall_kalman_params_t p = default_params();
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 0.0, 0.0, 1000000000LL);
    OBICALL_CHECK(obicall_kalman_predict(&s, &p, 500000000LL) == OBICALL_ERR_VALIDATION_TIMING);
}

static void test_update_reduces_covariance_and_pulls_toward_measurement(void) {
    obicall_kalman_params_t p = default_params();
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 0.0, 0.0, 0);
    double cov_before = s.covariance[0];

    double meas_cov[4] = {0.01, 0.0, 0.0, 0.01};
    OBICALL_CHECK(obicall_kalman_update_position(&s, &p, 100000000LL, 5.0, 5.0, meas_cov) == OBICALL_OK);

    OBICALL_CHECK(s.covariance[0] < cov_before); /* a real measurement must sharpen the estimate */
    OBICALL_CHECK(s.mean[0] > 0.0 && s.mean[0] < 5.0); /* pulled toward the measurement, not equal to prior or meas */
    /* Covariance must stay symmetric after the Joseph-form update. */
    OBICALL_CHECK_NEAR(s.covariance[1], s.covariance[4], 1e-9);
}

static void test_repeated_consistent_updates_converge(void) {
    obicall_kalman_params_t p = default_params();
    p.process_noise_position = 0.0001;
    p.process_noise_velocity = 0.0001;
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 0.0, 0.0, 0);
    double meas_cov[4] = {0.01, 0.0, 0.0, 0.01};

    int64_t t = 0;
    for (int i = 0; i < 200; ++i) {
        t += 50000000LL; /* 50ms steps */
        OBICALL_CHECK(obicall_kalman_update_position(&s, &p, t, 10.0, -3.0, meas_cov) == OBICALL_OK);
    }
    OBICALL_CHECK_NEAR(s.mean[0], 10.0, 0.05);
    OBICALL_CHECK_NEAR(s.mean[1], -3.0, 0.05);
    OBICALL_CHECK(s.covariance[0] < 0.01); /* should have converged well below the measurement variance */
}

static void test_update_rejects_non_finite_measurement(void) {
    obicall_kalman_params_t p = default_params();
    obicall_kalman_state_t s;
    obicall_kalman_init(&s, &p, 0.0, 0.0, 0);
    double meas_cov[4] = {0.01, 0.0, 0.0, 0.01};
    OBICALL_CHECK(obicall_kalman_update_position(&s, &p, 100, NAN, 1.0, meas_cov) ==
                  OBICALL_ERR_VALIDATION_NON_FINITE);
}

OBICALL_TEST_MAIN_BEGIN()
    test_static_fuse_worked_example();
    test_static_fuse_single_measurement_is_identity();
    test_static_fuse_rejects_bad_input();
    test_kalman_init_state();
    test_predict_only_grows_covariance_and_moves_by_velocity();
    test_predict_rejects_backwards_time();
    test_update_reduces_covariance_and_pulls_toward_measurement();
    test_repeated_consistent_updates_converge();
    test_update_rejects_non_finite_measurement();
OBICALL_TEST_MAIN_END()
