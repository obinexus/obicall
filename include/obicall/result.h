#ifndef OBICALL_RESULT_H
#define OBICALL_RESULT_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"

OBICALL_BEGIN_DECLS

#define OBICALL_RESULT_SCHEMA_VERSION 1u

/* A candidate or committed fusion output. Only the gate may turn a
 * candidate into a commit (see gate.h); this struct is the same shape
 * either way so the wire encoder/decoder and digesting logic don't fork. */
typedef struct obicall_result {
    uint32_t struct_size;
    uint32_t schema_version;

    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint64_t window_seq;

    uint32_t broker_id;  /* obicall_broker_id_t: actual producer */
    uint64_t epoch;       /* ownership epoch this result was produced under */

    uint32_t status; /* obicall_result_status_t */

    uint32_t payload_shape;
    uint32_t payload_count;
    double payload[OBICALL_MAX_PAYLOAD_DOUBLES];

    uint32_t covariance_count;
    double covariance[OBICALL_MAX_COV_DOUBLES];

    uint8_t input_digest[OBICALL_DIGEST_LEN];  /* SHA-256 over admitted input window */
    uint8_t config_digest[OBICALL_DIGEST_LEN]; /* SHA-256 over active pipeline config */

    int64_t timestamp_ns;    /* local monotonic clock, production time */
    int64_t valid_until_ns;  /* local monotonic clock, same domain as timestamp_ns */

    uint32_t source_count;
    char sources[OBICALL_MAX_SOURCES][OBICALL_SENSOR_ID_LEN];

    uint32_t dgt_action_id; /* UINT32_MAX if DGT was not engaged for this window */
} obicall_result_t;

OBICALL_END_DECLS

#endif /* OBICALL_RESULT_H */
