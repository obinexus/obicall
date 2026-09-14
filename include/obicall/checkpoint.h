#ifndef OBICALL_CHECKPOINT_H
#define OBICALL_CHECKPOINT_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"

OBICALL_BEGIN_DECLS

#define OBICALL_CHECKPOINT_SCHEMA_VERSION 1u
#define OBICALL_MAX_TRACKED_SOURCES 16u
#define OBICALL_MAX_STATE_DIM 6u

/* Per-sensor replay position, so a restored broker/worker knows which
 * sequence numbers it has already consumed and can reject/skip re-delivery
 * instead of double-admitting. */
typedef struct obicall_source_position {
    char sensor_id[OBICALL_SENSOR_ID_LEN];
    uint64_t source_boot_id;
    uint64_t last_sequence;
} obicall_source_position_t;

/* Broker/estimator-level checkpoint - distinct from a provider's opaque
 * checkpoint blob (plugin.h). This one is read and written directly by
 * core code (journal replay, shadow catch-up, worker replacement) so its
 * layout is documented and versioned rather than opaque. Contains no
 * native pointers; state_dim/state_mean/state_covariance fully describe
 * the Kalman estimator (see docs/ABI.md and docs/ARCHITECTURE.md). */
typedef struct obicall_checkpoint {
    uint32_t struct_size;
    uint32_t schema_version;

    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint64_t window_seq; /* last window this checkpoint reflects */

    uint32_t estimator_mode;
    uint32_t state_dim;
    double state_mean[OBICALL_MAX_STATE_DIM];
    double state_covariance[OBICALL_MAX_STATE_DIM * OBICALL_MAX_STATE_DIM];

    uint32_t calibration_version;
    uint32_t policy_version; /* DGT scenario/loss table version active at checkpoint time */
    uint32_t dgt_action_id;

    uint32_t source_count;
    obicall_source_position_t sources[OBICALL_MAX_TRACKED_SOURCES];

    int64_t created_at_ns; /* local monotonic clock */
} obicall_checkpoint_t;

OBICALL_END_DECLS

#endif /* OBICALL_CHECKPOINT_H */
