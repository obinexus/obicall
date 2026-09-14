#ifndef OBICALL_TYPES_H
#define OBICALL_TYPES_H

#include <stdint.h>
#include <stddef.h>

#include "obicall/platform.h"

OBICALL_BEGIN_DECLS

/* Bounds shared by every ABI-facing struct. Fixed-size arrays are used
 * instead of pointers inside these structs so they can be copied, memcmp'd
 * for digesting, and mirrored 1:1 by a ctypes.Structure without a separate
 * ownership/release protocol. Anything that can legitimately exceed these
 * bounds (checkpoints, provider events) uses obicall_buffer_t instead. */
#define OBICALL_MAX_PAYLOAD_DOUBLES 6u
#define OBICALL_MAX_COV_DOUBLES 36u /* 6x6 row-major, covers state dim <= 6 */
#define OBICALL_MAX_SOURCES 8u
#define OBICALL_SENSOR_ID_LEN 32u
#define OBICALL_PIPELINE_ID_LEN 32u
#define OBICALL_PROVIDER_NAME_LEN 64u
#define OBICALL_VERSION_STR_LEN 32u
#define OBICALL_DIGEST_LEN 32u /* SHA-256 */

/* Borrowed, read-only view. Valid only for the duration of the call that
 * received it; the callee must copy anything it needs to keep. */
typedef struct obicall_buffer {
    const uint8_t* data;
    uint32_t len;
} obicall_buffer_t;

/* Caller-owned buffer returned by the ABI. Must be released through the
 * release function documented alongside the call that produced it -
 * never through free()/the caller's own allocator. */
typedef struct obicall_owned_buffer {
    uint8_t* data;
    uint32_t len;
    uint32_t capacity;
} obicall_owned_buffer_t;

typedef enum obicall_clock_domain {
    OBICALL_CLOCK_DOMAIN_UNSPECIFIED = 0,
    OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC = 1, /* this host's CLOCK_MONOTONIC/QPC */
    OBICALL_CLOCK_DOMAIN_LOCAL_REALTIME = 2,  /* this host's wall clock */
    OBICALL_CLOCK_DOMAIN_SENSOR_DEVICE = 3    /* device clock, not comparable across hosts */
} obicall_clock_domain_t;

typedef enum obicall_coordinate_frame {
    OBICALL_FRAME_UNSPECIFIED = 0,
    OBICALL_FRAME_LOCAL_ENU = 1, /* East-North-Up, local tangent plane */
    OBICALL_FRAME_BODY = 2
} obicall_coordinate_frame_t;

typedef enum obicall_units {
    OBICALL_UNITS_UNSPECIFIED = 0,
    OBICALL_UNITS_METERS = 1,
    OBICALL_UNITS_METERS_PER_SECOND = 2,
    OBICALL_UNITS_RADIANS = 3
} obicall_units_t;

typedef enum obicall_payload_shape {
    OBICALL_SHAPE_UNSPECIFIED = 0,
    OBICALL_SHAPE_POSITION_1D = 1, /* [x] */
    OBICALL_SHAPE_POSITION_2D = 2, /* [x, y] */
    OBICALL_SHAPE_POSITION_2D_VELOCITY = 3, /* [x, y, vx, vy] */
    OBICALL_SHAPE_RANGE_1D = 4 /* [range] */
} obicall_payload_shape_t;

typedef enum obicall_result_status {
    OBICALL_RESULT_INVALID = 0,
    OBICALL_RESULT_DEGRADED = 1,
    OBICALL_RESULT_VALID = 2
} obicall_result_status_t;

typedef enum obicall_broker_id {
    OBICALL_BROKER_NONE = 0,
    OBICALL_BROKER_A = 1,
    OBICALL_BROKER_B = 2
} obicall_broker_id_t;

#define OBICALL_EPOCH_NONE 0ull

OBICALL_END_DECLS

#endif /* OBICALL_TYPES_H */
