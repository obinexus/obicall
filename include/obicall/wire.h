#ifndef OBICALL_WIRE_H
#define OBICALL_WIRE_H

#include <stdint.h>
#include <stddef.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/observation.h"
#include "obicall/event.h"
#include "obicall/result.h"
#include "obicall/checkpoint.h"
#include "obicall/digest.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/*
 * Wire framing (see docs/ABI.md "Wire format" for the authoritative spec).
 * Every frame:
 *   bytes 0..3   magic       0x4F 0x42 0x57 0x46  ("OBWF")
 *   byte  4      wire_version
 *   byte  5      msg_type    (obicall_wire_msg_type_t)
 *   bytes 6..7   flags       little-endian u16, currently 0
 *   bytes 8..11  payload_len little-endian u32, <= OBICALL_WIRE_MAX_PAYLOAD
 *   bytes 12..15 crc32       little-endian u32, CRC-32/ISO-HDLC of payload
 *   bytes 16..   payload     payload_len bytes, msg_type-specific encoding
 *
 * All multi-byte payload fields are packed explicitly least-significant-
 * byte-first by the encode/decode functions below - never memcpy'd from a
 * native struct - so wire compatibility does not depend on the producing
 * or consuming compiler's struct layout, padding, or host endianness.
 * Fixed-size char[] fields are written verbatim, NUL-padded.
 */

#define OBICALL_WIRE_MAGIC0 0x4Fu
#define OBICALL_WIRE_MAGIC1 0x42u
#define OBICALL_WIRE_MAGIC2 0x57u
#define OBICALL_WIRE_MAGIC3 0x46u
#define OBICALL_WIRE_VERSION 1u
#define OBICALL_WIRE_HEADER_LEN 16u
#define OBICALL_WIRE_MAX_PAYLOAD (1u << 20) /* 1 MiB, bounds every queue/buffer sized off it */

typedef enum obicall_wire_msg_type {
    OBICALL_MSG_AUTH_HELLO = 1,
    OBICALL_MSG_HEARTBEAT = 2,
    OBICALL_MSG_OBSERVATION_SUBMIT = 3,
    OBICALL_MSG_EVENT = 4,
    OBICALL_MSG_ADMISSION_DECISION = 5,
    OBICALL_MSG_RESULT_PUBLISH = 6,
    OBICALL_MSG_RESULT_ACK = 7,
    OBICALL_MSG_EPOCH_GRANT = 8,
    OBICALL_MSG_EPOCH_REVOKE = 9,
    OBICALL_MSG_READINESS_REPORT = 10,
    OBICALL_MSG_CHECKPOINT_TRANSFER = 11,
    OBICALL_MSG_REPLAY_REQUEST = 12,
    OBICALL_MSG_REPLAY_DATA = 13,
    OBICALL_MSG_REPLAY_END = 14,
    OBICALL_MSG_STATUS_QUERY = 15,
    OBICALL_MSG_STATUS_REPLY = 16,
    OBICALL_MSG_SHUTDOWN = 17,
    OBICALL_MSG_ERROR = 18
} obicall_wire_msg_type_t;

typedef struct obicall_wire_header {
    uint8_t wire_version;
    uint8_t msg_type;
    uint16_t flags;
    uint32_t payload_len;
    uint32_t crc32;
} obicall_wire_header_t;

/* Frame integrity uses obicall_crc32 (digest.h); frame authenticity/
 * authorization uses the run-token carried in OBICALL_MSG_AUTH_HELLO,
 * checked by the transport layer (osal), not by these codecs. */

/* Encodes/decodes just the fixed OBICALL_WIRE_HEADER_LEN-byte header. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_header(
    const obicall_wire_header_t* header, uint8_t out[OBICALL_WIRE_HEADER_LEN]);
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_header(
    const uint8_t in[OBICALL_WIRE_HEADER_LEN], obicall_wire_header_t* out);

/* Payload codecs. Every _encode_ writes to out (capacity out_cap) and sets
 * *out_len; every _decode_ reads exactly in_len bytes. All reject
 * malformed/oversized input with OBICALL_ERR_WIRE_* rather than reading
 * out of bounds. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_observation(
    const obicall_observation_t* obs, uint8_t* out, uint32_t out_cap, uint32_t* out_len);
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_observation(
    const uint8_t* in, uint32_t in_len, obicall_observation_t* out);

OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_result(
    const obicall_result_t* result, uint8_t* out, uint32_t out_cap, uint32_t* out_len);
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_result(
    const uint8_t* in, uint32_t in_len, obicall_result_t* out);

OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_checkpoint(
    const obicall_checkpoint_t* ckpt, uint8_t* out, uint32_t out_cap, uint32_t* out_len);
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_checkpoint(
    const uint8_t* in, uint32_t in_len, obicall_checkpoint_t* out);

OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_event(
    const obicall_event_t* event, uint8_t* out, uint32_t out_cap, uint32_t* out_len);
OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_event(
    const uint8_t* in, uint32_t in_len, uint8_t* payload_storage, uint32_t payload_storage_cap,
    obicall_event_t* out);

#define OBICALL_RUN_TOKEN_LEN 32u

typedef struct obicall_msg_auth_hello {
    uint32_t protocol_version;
    uint32_t role; /* obicall_wire_role_t */
    uint8_t run_token[OBICALL_RUN_TOKEN_LEN];
} obicall_msg_auth_hello_t;

typedef enum obicall_wire_role {
    OBICALL_ROLE_UNKNOWN = 0,
    OBICALL_ROLE_WORKER = 1,
    OBICALL_ROLE_BROKER = 2,
    OBICALL_ROLE_GATE = 3,
    OBICALL_ROLE_JOURNAL = 4,
    OBICALL_ROLE_CLI = 5,
    OBICALL_ROLE_SUPERVISOR = 6
} obicall_wire_role_t;

typedef struct obicall_msg_heartbeat {
    uint32_t broker_id;
    uint64_t epoch;
    uint64_t last_window_seq;
    uint8_t config_digest[OBICALL_DIGEST_LEN];
    uint32_t checkpoint_schema_version;
    uint32_t healthy;
    int64_t sent_at_ns;
} obicall_msg_heartbeat_t;

typedef struct obicall_msg_epoch_grant {
    uint64_t new_epoch;
    uint32_t granted_to_broker_id;
    int64_t granted_at_ns;
} obicall_msg_epoch_grant_t;

typedef struct obicall_msg_epoch_revoke {
    uint64_t revoked_epoch;
    uint32_t revoked_broker_id;
    int64_t revoked_at_ns;
    uint32_t reason_code;
} obicall_msg_epoch_revoke_t;

typedef struct obicall_msg_readiness_report {
    uint32_t broker_id;
    uint8_t config_digest[OBICALL_DIGEST_LEN];
    uint32_t checkpoint_schema_version;
    uint64_t last_processed_window_seq;
    uint32_t healthy;
} obicall_msg_readiness_report_t;

typedef struct obicall_msg_replay_request {
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint64_t from_window_seq;
} obicall_msg_replay_request_t;

typedef struct obicall_msg_admission_decision {
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint64_t window_seq;
    char sensor_id[OBICALL_SENSOR_ID_LEN];
    uint64_t sequence;
    uint32_t admitted;
    uint32_t reason_code;
    int64_t decided_at_ns;
} obicall_msg_admission_decision_t;

typedef struct obicall_msg_result_ack {
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    uint64_t window_seq;
    uint32_t accepted;
    int32_t reject_status; /* obicall_status_t if accepted == 0 */
} obicall_msg_result_ack_t;

typedef struct obicall_msg_status_reply {
    uint32_t running;
    uint64_t gate_epoch;
    uint32_t owner_broker_id;
    uint64_t last_committed_window_seq;
    int64_t uptime_ns;
} obicall_msg_status_reply_t;

#define OBICALL_WIRE_STRUCT_CODEC_DECL(name, type)                                            \
    OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_encode_##name(                      \
        const type* msg, uint8_t* out, uint32_t out_cap, uint32_t* out_len);                   \
    OBICALL_API obicall_status_t OBICALL_CALL obicall_wire_decode_##name(                      \
        const uint8_t* in, uint32_t in_len, type* out)

OBICALL_WIRE_STRUCT_CODEC_DECL(auth_hello, obicall_msg_auth_hello_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(heartbeat, obicall_msg_heartbeat_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(epoch_grant, obicall_msg_epoch_grant_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(epoch_revoke, obicall_msg_epoch_revoke_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(readiness_report, obicall_msg_readiness_report_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(replay_request, obicall_msg_replay_request_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(admission_decision, obicall_msg_admission_decision_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(result_ack, obicall_msg_result_ack_t);
OBICALL_WIRE_STRUCT_CODEC_DECL(status_reply, obicall_msg_status_reply_t);

#undef OBICALL_WIRE_STRUCT_CODEC_DECL

OBICALL_END_DECLS

#endif /* OBICALL_WIRE_H */
