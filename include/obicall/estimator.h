#ifndef OBICALL_ESTIMATOR_H
#define OBICALL_ESTIMATOR_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/* Constant-velocity state [px, py, vx, vy]. Documented in docs/ARCHITECTURE.md;
 * OBICALL_MAX_STATE_DIM (checkpoint.h) bounds larger future models. */
#define OBICALL_ESTIMATOR_STATE_DIM 4u

typedef enum obicall_estimator_mode {
    OBICALL_ESTIMATOR_MODE_KALMAN_CV = 0,     /* constant-velocity Kalman filter */
    OBICALL_ESTIMATOR_MODE_LAST_VALUE_HOLD = 1 /* trivial baseline: holds last admitted position */
} obicall_estimator_mode_t;

typedef struct obicall_kalman_params {
    double process_noise_position; /* Q diag entry for px,py, per second */
    double process_noise_velocity; /* Q diag entry for vx,vy, per second */
    double initial_position_variance;
    double initial_velocity_variance;
    double max_prediction_gap_s; /* reject predict() spanning more than this */
} obicall_kalman_params_t;

typedef struct obicall_kalman_state {
    uint32_t initialized;
    uint32_t reserved;
    double mean[OBICALL_ESTIMATOR_STATE_DIM];
    double covariance[OBICALL_ESTIMATOR_STATE_DIM * OBICALL_ESTIMATOR_STATE_DIM];
    int64_t last_update_time_ns;
} obicall_kalman_state_t;

OBICALL_API void OBICALL_CALL obicall_kalman_init(obicall_kalman_state_t* state,
                                                    const obicall_kalman_params_t* params, double x0,
                                                    double y0, int64_t time_ns);

/* Advances state to time_ns under the constant-velocity process model.
 * A no-op (OBICALL_OK) if time_ns == last_update_time_ns. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_kalman_predict(
    obicall_kalman_state_t* state, const obicall_kalman_params_t* params, int64_t time_ns);

/* Measurement model z = [px, py] + noise(meas_cov_2x2). Predicts to
 * time_ns first, then applies the update. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_kalman_update_position(
    obicall_kalman_state_t* state, const obicall_kalman_params_t* params, int64_t time_ns,
    double zx, double zy, const double meas_cov_2x2[4]);

/* Inverse-variance weighted fusion of independent unbiased scalar
 * measurements - NOT a Kalman recursion and carries no prior. This is the
 * function docs/VALIDATION.md's 10.0/10.4 m, 0.04/0.16 m^2 -> 10.08 m,
 * 0.032 m^2 example exercises; obicall_kalman_update_position will not
 * reproduce that exact number because it also incorporates the filter's
 * prior. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_static_fuse_1d(const double* means,
                                                                   const double* variances,
                                                                   uint32_t count, double* out_mean,
                                                                   double* out_variance);

OBICALL_END_DECLS

#endif /* OBICALL_ESTIMATOR_H */
