#include "obicall/observation.h"

#include <math.h>
#include <string.h>

uint32_t OBICALL_CALL obicall_payload_shape_dimension(uint32_t shape) {
    switch (shape) {
        case OBICALL_SHAPE_POSITION_1D: return 1;
        case OBICALL_SHAPE_POSITION_2D: return 2;
        case OBICALL_SHAPE_POSITION_2D_VELOCITY: return 4;
        case OBICALL_SHAPE_RANGE_1D: return 1;
        default: return 0;
    }
}

static void report(obicall_validation_callback_fn cb, void* user_data,
                    obicall_validation_issue_t issue) {
    if (cb) cb(user_data, issue);
}

/* Semi-definite Cholesky attempt: a necessary condition for PSD (not a
 * full certificate via all principal minors), but a real numeric check
 * rather than a diagonal-only heuristic. dim is bounded by
 * OBICALL_MAX_STATE_DIM-scale callers (<= 6) so O(dim^3) is negligible. */
static int covariance_is_psd(const double* cov, uint32_t dim, double epsilon) {
    double l[6][6];
    memset(l, 0, sizeof(l));
    for (uint32_t i = 0; i < dim; ++i) {
        for (uint32_t j = 0; j <= i; ++j) {
            double sum = cov[i * dim + j];
            for (uint32_t k = 0; k < j; ++k) {
                sum -= l[i][k] * l[j][k];
            }
            if (i == j) {
                if (sum < -epsilon) return 0;
                l[i][j] = sum > 0.0 ? sqrt(sum) : 0.0;
            } else {
                l[i][j] = (l[j][j] > epsilon) ? (sum / l[j][j]) : 0.0;
            }
        }
    }
    return 1;
}

obicall_status_t OBICALL_CALL obicall_observation_validate(
    const obicall_observation_t* obs, obicall_validation_callback_fn on_issue, void* user_data) {
    if (obs == NULL) return OBICALL_ERR_NULL_POINTER;

    obicall_status_t first_error = OBICALL_OK;
#define NOTE(issue_code, err_code)                    \
    do {                                               \
        report(on_issue, user_data, (issue_code));     \
        if (first_error == OBICALL_OK) first_error = (err_code); \
    } while (0)

    if (obs->struct_size != sizeof(obicall_observation_t)) {
        NOTE(OBICALL_ISSUE_STRUCT_SIZE_MISMATCH, OBICALL_ERR_INVALID_ARGUMENT);
    }

    if (obs->sensor_id[0] == '\0') {
        NOTE(OBICALL_ISSUE_SENSOR_ID_EMPTY, OBICALL_ERR_INVALID_ARGUMENT);
    }
    {
        int has_terminator = 0;
        for (uint32_t i = 0; i < OBICALL_SENSOR_ID_LEN; ++i) {
            if (obs->sensor_id[i] == '\0') { has_terminator = 1; break; }
        }
        if (!has_terminator) NOTE(OBICALL_ISSUE_SENSOR_ID_EMPTY, OBICALL_ERR_INVALID_ARGUMENT);
    }

    if (obs->coordinate_frame == OBICALL_FRAME_UNSPECIFIED || obs->units == OBICALL_UNITS_UNSPECIFIED) {
        NOTE(OBICALL_ISSUE_UNSUPPORTED_FRAME_OR_UNITS, OBICALL_ERR_UNSUPPORTED);
    }

    if (!isfinite(obs->time_uncertainty_s) || obs->time_uncertainty_s < 0.0) {
        NOTE(OBICALL_ISSUE_TIME_UNCERTAINTY_NEGATIVE, OBICALL_ERR_VALIDATION_TIMING);
    }

    uint32_t expected_dim = obicall_payload_shape_dimension(obs->payload_shape);
    if (expected_dim == 0 || obs->payload_count != expected_dim ||
        obs->payload_count > OBICALL_MAX_PAYLOAD_DOUBLES) {
        NOTE(OBICALL_ISSUE_PAYLOAD_COUNT_OUT_OF_RANGE, OBICALL_ERR_VALIDATION_DIMENSION);
    } else {
        for (uint32_t i = 0; i < obs->payload_count; ++i) {
            if (!isfinite(obs->payload[i])) {
                NOTE(OBICALL_ISSUE_NON_FINITE_PAYLOAD, OBICALL_ERR_VALIDATION_NON_FINITE);
                break;
            }
        }
    }

    uint32_t expected_cov = expected_dim * expected_dim;
    if (expected_dim == 0 || obs->covariance_count != expected_cov ||
        obs->covariance_count > OBICALL_MAX_COV_DOUBLES) {
        NOTE(OBICALL_ISSUE_COVARIANCE_COUNT_OUT_OF_RANGE, OBICALL_ERR_VALIDATION_COVARIANCE);
    } else {
        int all_finite = 1;
        for (uint32_t i = 0; i < obs->covariance_count; ++i) {
            if (!isfinite(obs->covariance[i])) { all_finite = 0; break; }
        }
        if (!all_finite) {
            NOTE(OBICALL_ISSUE_NON_FINITE_COVARIANCE, OBICALL_ERR_VALIDATION_NON_FINITE);
        } else {
            int symmetric = 1;
            for (uint32_t i = 0; i < expected_dim && symmetric; ++i) {
                for (uint32_t j = 0; j < expected_dim; ++j) {
                    double a = obs->covariance[i * expected_dim + j];
                    double b = obs->covariance[j * expected_dim + i];
                    if (fabs(a - b) > 1e-9 * (1.0 + fabs(a) + fabs(b))) { symmetric = 0; break; }
                }
            }
            if (!symmetric) {
                NOTE(OBICALL_ISSUE_COVARIANCE_NOT_SYMMETRIC, OBICALL_ERR_VALIDATION_COVARIANCE);
            } else if (!covariance_is_psd(obs->covariance, expected_dim, 1e-9)) {
                NOTE(OBICALL_ISSUE_COVARIANCE_NOT_PSD, OBICALL_ERR_VALIDATION_COVARIANCE);
            }
        }
    }

#undef NOTE
    return first_error;
}
