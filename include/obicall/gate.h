#ifndef OBICALL_GATE_H
#define OBICALL_GATE_H

#include <stdint.h>
#include <stdio.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/result.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/*
 * Pure publication-gate decision logic (docs/ARCHITECTURE.md "Publication
 * gate invariants"). No I/O and no process/socket knowledge lives here -
 * src/gate/gate_main.c wraps this with IPC and durable persistence
 * (write-temp + fsync + atomic rename, via the osal). Keeping the
 * invariants in a pure, directly-testable module is what makes "stale
 * epoch rejected", "duplicate window rejected", etc. unit-testable without
 * spawning a process.
 */

#define OBICALL_MAX_PIPELINES 8u

typedef struct obicall_gate_pipeline_state {
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint32_t has_committed;
    uint64_t last_committed_window_seq;
    uint8_t last_committed_digest[OBICALL_DIGEST_LEN]; /* over payload+covariance+status */
} obicall_gate_pipeline_state_t;

typedef struct obicall_gate_state {
    uint32_t struct_size;
    uint32_t schema_version;
    uint64_t epoch; /* OBICALL_EPOCH_NONE until the first grant */
    uint32_t owner_broker_id; /* OBICALL_BROKER_NONE while revoked/unassigned */
    uint32_t pipeline_count;
    obicall_gate_pipeline_state_t pipelines[OBICALL_MAX_PIPELINES];
} obicall_gate_state_t;

typedef enum obicall_gate_publish_outcome {
    OBICALL_GATE_COMMIT = 0,
    OBICALL_GATE_SHADOW_RECORDED = 1,     /* valid submission from the non-owner; not committed */
    OBICALL_GATE_REJECT_NOT_OWNER = 2,
    OBICALL_GATE_REJECT_STALE_EPOCH = 3,
    OBICALL_GATE_REJECT_DUPLICATE_WINDOW = 4,
    OBICALL_GATE_REJECT_EXPIRED = 5,
    OBICALL_GATE_REJECT_INCOMPATIBLE = 6,
    OBICALL_GATE_REJECT_NO_CAPACITY = 7
} obicall_gate_publish_outcome_t;

/* now_ns must be the gate's own monotonic clock - never a value taken
 * from the submitting broker - since valid_until_ns/timestamp_ns are only
 * meaningful compared within one clock domain (docs/FAULT_TOLERANCE.md).
 * On OBICALL_GATE_COMMIT, state is updated in place (pipeline high-water
 * mark and digest) before returning. */
OBICALL_API obicall_gate_publish_outcome_t OBICALL_CALL obicall_gate_decide_publish(
    obicall_gate_state_t* state, const obicall_result_t* result, int64_t now_ns,
    uint32_t* out_pipeline_index);

typedef struct obicall_gate_readiness {
    uint32_t healthy;
    uint8_t config_digest[OBICALL_DIGEST_LEN];
    uint32_t checkpoint_schema_version;
    uint64_t last_processed_window_seq;
} obicall_gate_readiness_t;

typedef struct obicall_gate_promotion_requirements {
    uint8_t required_config_digest[OBICALL_DIGEST_LEN];
    uint32_t required_checkpoint_schema_version;
    uint64_t max_allowed_replay_gap;
} obicall_gate_promotion_requirements_t;

/* Non-zero iff capability/config/checkpoint-schema compatibility AND
 * replay progress (committed_high_water - last_processed_window_seq <=
 * max_allowed_replay_gap) all hold. This is the only function allowed to
 * say "yes" before obicall_gate_promote revokes the old epoch. */
OBICALL_API int OBICALL_CALL obicall_gate_shadow_promotable(
    const obicall_gate_readiness_t* shadow_readiness,
    const obicall_gate_promotion_requirements_t* requirements,
    uint64_t committed_high_water_window_seq);

/* Revokes the current owner's epoch and grants new_owner_broker_id
 * epoch+1 (or 1 if none was ever granted), atomically with respect to
 * this state struct. Caller must persist the returned state (see
 * obicall_gate_persist_state) before notifying anyone of the grant. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_gate_promote(obicall_gate_state_t* state,
                                                                 uint32_t new_owner_broker_id,
                                                                 int64_t now_ns,
                                                                 uint64_t* out_new_epoch);

/* Declared-tolerance comparison of two results for the same
 * pipeline/window, used to detect equal-input replica disagreement
 * (docs/FAULT_TOLERANCE.md) without picking a winner. */
OBICALL_API int OBICALL_CALL obicall_gate_results_agree(const obicall_result_t* a,
                                                          const obicall_result_t* b,
                                                          double position_tolerance,
                                                          double covariance_rel_tolerance);

OBICALL_API obicall_status_t OBICALL_CALL obicall_gate_encode_state(
    const obicall_gate_state_t* state, uint8_t* out, uint32_t out_cap, uint32_t* out_len);
OBICALL_API obicall_status_t OBICALL_CALL obicall_gate_decode_state(const uint8_t* in,
                                                                      uint32_t in_len,
                                                                      obicall_gate_state_t* out);

/* Writes one length-prefixed, checksummed record to f (assumed freshly
 * opened for writing at offset 0) and flushes it to durable storage
 * before returning OBICALL_OK. Does not itself perform the
 * write-temp+rename step - see obicall_file_atomic_replace in osal.h,
 * which src/gate/gate_main.c composes with this. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_gate_persist_state(
    FILE* f, const obicall_gate_state_t* state);

/* Reads the one record written by obicall_gate_persist_state. A checksum
 * or magic mismatch (torn write) returns OBICALL_ERR_WIRE_CHECKSUM /
 * OBICALL_ERR_WIRE_MALFORMED and leaves *out unmodified - the caller must
 * treat this as "no trustworthy state" (docs/FAULT_TOLERANCE.md), not
 * silently start from empty state. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_gate_load_state(FILE* f,
                                                                    obicall_gate_state_t* out);

OBICALL_END_DECLS

#endif /* OBICALL_GATE_H */
