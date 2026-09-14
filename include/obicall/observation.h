#ifndef OBICALL_OBSERVATION_H
#define OBICALL_OBSERVATION_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

#define OBICALL_OBSERVATION_SCHEMA_VERSION 1u

/* One sensor reading. Crosses two boundaries in its lifetime: the in-process
 * plugin vtable call (native struct, this definition) and the inter-process
 * wire (obicall_wire_encode_observation/_decode_observation in wire.h,
 * which serialize the same fields byte-by-byte - never this struct's raw
 * memory). struct_size lets a future ABI minor version append fields while
 * older callers keep working; struct_size is checked before schema_version. */
typedef struct obicall_observation {
    uint32_t struct_size;
    uint32_t schema_version;

    char sensor_id[OBICALL_SENSOR_ID_LEN];
    uint64_t source_boot_id;  /* random at provider start; changes across restarts */
    uint64_t sequence;        /* monotonic per (sensor_id, source_boot_id) */

    int64_t sample_time_ns;   /* in clock_domain */
    int64_t arrival_time_ns;  /* OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC, journal-assigned */
    uint32_t clock_domain;    /* obicall_clock_domain_t */
    double time_uncertainty_s;

    uint32_t coordinate_frame; /* obicall_coordinate_frame_t */
    uint32_t units;            /* obicall_units_t */
    uint32_t calibration_version;

    uint32_t payload_shape; /* obicall_payload_shape_t */
    uint32_t payload_count; /* valid doubles in payload[] */
    double payload[OBICALL_MAX_PAYLOAD_DOUBLES];

    uint32_t covariance_count; /* valid doubles in covariance[], row-major */
    double covariance[OBICALL_MAX_COV_DOUBLES];
} obicall_observation_t;

typedef enum obicall_validation_issue {
    OBICALL_ISSUE_NONE = 0,
    OBICALL_ISSUE_NON_FINITE_PAYLOAD = 1,
    OBICALL_ISSUE_NON_FINITE_COVARIANCE = 2,
    OBICALL_ISSUE_DIMENSION_MISMATCH = 3,
    OBICALL_ISSUE_PAYLOAD_COUNT_OUT_OF_RANGE = 4,
    OBICALL_ISSUE_COVARIANCE_COUNT_OUT_OF_RANGE = 5,
    OBICALL_ISSUE_COVARIANCE_NOT_SYMMETRIC = 6,
    OBICALL_ISSUE_COVARIANCE_NOT_PSD = 7,
    OBICALL_ISSUE_SENSOR_ID_EMPTY = 8,
    OBICALL_ISSUE_UNSUPPORTED_FRAME_OR_UNITS = 9,
    OBICALL_ISSUE_TIME_UNCERTAINTY_NEGATIVE = 10,
    OBICALL_ISSUE_STRUCT_SIZE_MISMATCH = 11
} obicall_validation_issue_t;

/* Invoked synchronously, zero or more times (bounded, see wire.h's
 * OBICALL_MAX_VALIDATION_ISSUES), by obicall_observation_validate. The
 * callback pointer only needs to stay valid for the duration of that one
 * call - this is what makes it safe to bind from ctypes without a
 * separately-managed callback lifetime. */
typedef void(OBICALL_CALL* obicall_validation_callback_fn)(
    void* user_data, obicall_validation_issue_t issue);

/* Pure/stateless: checks finiteness, declared dimensions vs payload_count/
 * covariance_count, covariance symmetry and (cheaply, via leading-minor
 * determinants) positive-semi-definiteness, and required identity fields.
 * Returns OBICALL_OK if no issues were reported, OBICALL_ERR_VALIDATION_*
 * for the first category of issue found otherwise - the callback still
 * receives every issue found, not just the first. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_observation_validate(
    const obicall_observation_t* obs,
    obicall_validation_callback_fn on_issue,
    void* user_data);

OBICALL_API uint32_t OBICALL_CALL obicall_payload_shape_dimension(uint32_t shape);

OBICALL_END_DECLS

#endif /* OBICALL_OBSERVATION_H */
