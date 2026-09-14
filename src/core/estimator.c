#include "obicall/estimator.h"

#include <math.h>
#include <string.h>

#define SD OBICALL_ESTIMATOR_STATE_DIM /* 4 */

void OBICALL_CALL obicall_kalman_init(obicall_kalman_state_t* state,
                                       const obicall_kalman_params_t* params, double x0, double y0,
                                       int64_t time_ns) {
    memset(state, 0, sizeof(*state));
    state->mean[0] = x0;
    state->mean[1] = y0;
    state->mean[2] = 0.0;
    state->mean[3] = 0.0;
    for (uint32_t i = 0; i < SD; ++i) {
        for (uint32_t j = 0; j < SD; ++j) {
            state->covariance[i * SD + j] =
                (i != j) ? 0.0 : (i < 2 ? params->initial_position_variance : params->initial_velocity_variance);
        }
    }
    state->last_update_time_ns = time_ns;
    state->initialized = 1;
}

obicall_status_t OBICALL_CALL obicall_kalman_predict(obicall_kalman_state_t* state,
                                                       const obicall_kalman_params_t* params,
                                                       int64_t time_ns) {
    if (!state || !params) return OBICALL_ERR_NULL_POINTER;
    if (!state->initialized) return OBICALL_ERR_INVALID_ARGUMENT;
    if (time_ns == state->last_update_time_ns) return OBICALL_OK;

    double dt = (double)(time_ns - state->last_update_time_ns) / 1e9;
    if (dt < 0.0) return OBICALL_ERR_VALIDATION_TIMING;
    if (params->max_prediction_gap_s > 0.0 && dt > params->max_prediction_gap_s) {
        return OBICALL_ERR_VALIDATION_TIMING;
    }

    /* Constant-velocity transition; process noise is a simplified diagonal
     * model (independent position/velocity spectral densities scaled by
     * dt), not the full Van Loan discretization - see docs/ARCHITECTURE.md. */
    double f[SD][SD] = {{1, 0, dt, 0}, {0, 1, 0, dt}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    double p[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) p[i][j] = state->covariance[i * SD + j];

    double x_new[SD];
    for (uint32_t i = 0; i < SD; ++i) {
        x_new[i] = 0.0;
        for (uint32_t j = 0; j < SD; ++j) x_new[i] += f[i][j] * state->mean[j];
    }

    double fp[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) {
            fp[i][j] = 0.0;
            for (uint32_t k = 0; k < SD; ++k) fp[i][j] += f[i][k] * p[k][j];
        }

    double fpft[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) {
            fpft[i][j] = 0.0;
            for (uint32_t k = 0; k < SD; ++k) fpft[i][j] += fp[i][k] * f[j][k];
        }

    double q_pos = params->process_noise_position * dt;
    double q_vel = params->process_noise_velocity * dt;

    for (uint32_t i = 0; i < SD; ++i) {
        for (uint32_t j = 0; j < SD; ++j) {
            double q = 0.0;
            if (i == j) q = (i < 2) ? q_pos : q_vel;
            state->covariance[i * SD + j] = fpft[i][j] + q;
        }
    }
    for (uint32_t i = 0; i < SD; ++i) state->mean[i] = x_new[i];
    state->last_update_time_ns = time_ns;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_kalman_update_position(obicall_kalman_state_t* state,
                                                               const obicall_kalman_params_t* params,
                                                               int64_t time_ns, double zx, double zy,
                                                               const double meas_cov_2x2[4]) {
    if (!state || !params || !meas_cov_2x2) return OBICALL_ERR_NULL_POINTER;
    if (!isfinite(zx) || !isfinite(zy)) return OBICALL_ERR_VALIDATION_NON_FINITE;
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(meas_cov_2x2[i])) return OBICALL_ERR_VALIDATION_NON_FINITE;
    }

    obicall_status_t st = obicall_kalman_predict(state, params, time_ns);
    if (st != OBICALL_OK) return st;

    double p[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) p[i][j] = state->covariance[i * SD + j];

    double y[2] = {zx - state->mean[0], zy - state->mean[1]};

    double s[2][2] = {{p[0][0] + meas_cov_2x2[0], p[0][1] + meas_cov_2x2[1]},
                       {p[1][0] + meas_cov_2x2[2], p[1][1] + meas_cov_2x2[3]}};
    double det = s[0][0] * s[1][1] - s[0][1] * s[1][0];
    if (!isfinite(det) || fabs(det) < 1e-15) return OBICALL_ERR_VALIDATION_COVARIANCE;
    double sinv[2][2] = {{s[1][1] / det, -s[0][1] / det}, {-s[1][0] / det, s[0][0] / det}};

    double k[SD][2];
    for (uint32_t i = 0; i < SD; ++i) {
        k[i][0] = p[i][0] * sinv[0][0] + p[i][1] * sinv[1][0];
        k[i][1] = p[i][0] * sinv[0][1] + p[i][1] * sinv[1][1];
    }

    for (uint32_t i = 0; i < SD; ++i) state->mean[i] += k[i][0] * y[0] + k[i][1] * y[1];

    /* Joseph form P = (I-KH) P (I-KH)^T + K R K^T for numerical robustness
     * (guarantees a symmetric, non-negative result even with an
     * imperfect gain) over the simpler P = (I-KH) P. */
    double ikh[SD][SD];
    for (uint32_t i = 0; i < SD; ++i) {
        for (uint32_t j = 0; j < SD; ++j) {
            double kh = (j == 0) ? k[i][0] : (j == 1 ? k[i][1] : 0.0);
            ikh[i][j] = (i == j ? 1.0 : 0.0) - kh;
        }
    }
    double tmp[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) {
            tmp[i][j] = 0.0;
            for (uint32_t kk = 0; kk < SD; ++kk) tmp[i][j] += ikh[i][kk] * p[kk][j];
        }
    double p_new[SD][SD];
    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) {
            p_new[i][j] = 0.0;
            for (uint32_t kk = 0; kk < SD; ++kk) p_new[i][j] += tmp[i][kk] * ikh[j][kk];
        }
    double r[2][2] = {{meas_cov_2x2[0], meas_cov_2x2[1]}, {meas_cov_2x2[2], meas_cov_2x2[3]}};
    for (uint32_t i = 0; i < SD; ++i) {
        double kr0 = k[i][0] * r[0][0] + k[i][1] * r[1][0];
        double kr1 = k[i][0] * r[0][1] + k[i][1] * r[1][1];
        for (uint32_t j = 0; j < SD; ++j) p_new[i][j] += kr0 * k[j][0] + kr1 * k[j][1];
    }

    for (uint32_t i = 0; i < SD; ++i)
        for (uint32_t j = 0; j < SD; ++j) state->covariance[i * SD + j] = p_new[i][j];

    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_static_fuse_1d(const double* means, const double* variances,
                                                       uint32_t count, double* out_mean,
                                                       double* out_variance) {
    if (!means || !variances || !out_mean || !out_variance) return OBICALL_ERR_NULL_POINTER;
    if (count == 0) return OBICALL_ERR_INVALID_ARGUMENT;

    double sum_inv_var = 0.0, sum_mean_over_var = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!isfinite(means[i]) || !isfinite(variances[i]) || variances[i] <= 0.0) {
            return OBICALL_ERR_VALIDATION_NON_FINITE;
        }
        double w = 1.0 / variances[i];
        sum_inv_var += w;
        sum_mean_over_var += means[i] * w;
    }
    *out_variance = 1.0 / sum_inv_var;
    *out_mean = sum_mean_over_var / sum_inv_var;
    return OBICALL_OK;
}

#undef SD
