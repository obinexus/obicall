#ifndef OBICALL_JOURNAL_H
#define OBICALL_JOURNAL_H

#include <stdint.h>
#include <stdio.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/observation.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

typedef enum obicall_admission_reason {
    OBICALL_ADMIT_OK = 0,
    OBICALL_REJECT_VALIDATION = 1,
    OBICALL_REJECT_DUPLICATE = 2,
    OBICALL_REJECT_LATE = 3,
    OBICALL_REJECT_OVERFLOW = 4,
    OBICALL_REJECT_SEQUENCE_REGRESSION = 5
} obicall_admission_reason_t;

typedef enum obicall_overflow_policy {
    OBICALL_OVERFLOW_REJECT_NEWEST = 0,
    OBICALL_OVERFLOW_REJECT_OLDEST = 1
} obicall_overflow_policy_t;

typedef struct obicall_journal_config {
    uint64_t reorder_window_n;     /* sequence may arrive this far behind highest_sequence_seen */
    int64_t max_lateness_ns;       /* sample_time_ns may trail the per-sensor watermark by this much */
    uint32_t max_pending_per_pipeline;
    uint32_t overflow_policy;      /* obicall_overflow_policy_t */
} obicall_journal_config_t;

/* Per (sensor_id, source_boot_id) admission state. A boot id change resets
 * tracking for that sensor (the source restarted; its sequence numbering
 * restarts too), it never carries stale high-water marks across restarts. */
typedef struct obicall_sensor_track {
    char sensor_id[OBICALL_SENSOR_ID_LEN];
    uint64_t source_boot_id;
    uint32_t has_data;
    uint64_t highest_sequence_seen;
    int64_t watermark_sample_time_ns;
} obicall_sensor_track_t;

/* Pure decision function: no I/O, no window-id assignment. track is
 * updated in place only when the decision is OBICALL_ADMIT_OK. Missing
 * readings are never synthesized here or anywhere else in the pipeline -
 * absence just means this function is not called for that tick. */
OBICALL_API obicall_admission_reason_t OBICALL_CALL obicall_journal_admit(
    const obicall_journal_config_t* cfg, obicall_sensor_track_t* track,
    const obicall_observation_t* obs, uint32_t current_pending_count);

#define OBICALL_JOURNAL_RECORD_MAGIC 0x4A4F5242u /* "JORB" */

/* On-disk record: header followed immediately by
 * obicall_wire_encode_observation's output, observation_len bytes.
 * record_len = OBICALL_JOURNAL_RECORD_HEADER_LEN + observation_len, so a
 * reader can skip a record without decoding it. crc32 covers exactly the
 * trailing observation_len bytes; a mismatch means a torn/partial write
 * and the reader must stop, not skip past it (see docs/FAULT_TOLERANCE.md). */
typedef struct obicall_journal_record_header {
    uint32_t magic;
    uint32_t record_len;
    uint32_t observation_len;
    uint32_t crc32;
    uint64_t window_seq;
    int64_t admitted_at_ns;
    uint32_t admission_reason; /* obicall_admission_reason_t */
    uint32_t reserved;
} obicall_journal_record_header_t;

#define OBICALL_JOURNAL_RECORD_HEADER_LEN 40u

OBICALL_API obicall_status_t OBICALL_CALL obicall_journal_encode_record_header(
    const obicall_journal_record_header_t* hdr, uint8_t out[OBICALL_JOURNAL_RECORD_HEADER_LEN]);
OBICALL_API obicall_status_t OBICALL_CALL obicall_journal_decode_record_header(
    const uint8_t in[OBICALL_JOURNAL_RECORD_HEADER_LEN], obicall_journal_record_header_t* out);

/* Appends one record (header + wire-encoded observation) to an
 * already-open, append-positioned file, and fflush+fsync/FlushFileBuffers
 * before returning - the record is durable once this returns OBICALL_OK. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_journal_append_record(
    FILE* f, uint64_t window_seq, int64_t admitted_at_ns, obicall_admission_reason_t reason,
    const obicall_observation_t* obs);

/* Reads one record starting at the file's current position. Returns
 * OBICALL_ERR_NOT_FOUND at a clean EOF, OBICALL_ERR_WIRE_CHECKSUM or
 * OBICALL_ERR_WIRE_MALFORMED at a torn trailing record (the file position
 * is left at the start of that bad record so the caller can truncate it). */
OBICALL_API obicall_status_t OBICALL_CALL obicall_journal_read_record(
    FILE* f, obicall_journal_record_header_t* out_header, obicall_observation_t* out_obs);

OBICALL_END_DECLS

#endif /* OBICALL_JOURNAL_H */
