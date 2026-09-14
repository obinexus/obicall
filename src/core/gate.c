#include "obicall/gate.h"
#include "obicall/digest.h"

#include <math.h>
#include <string.h>

#include "wire_cursor.h"

#if defined(_WIN32)
#include <io.h>
static int durable_sync(FILE* f) { return _commit(_fileno(f)) == 0; }
#else
#include <unistd.h>
static int durable_sync(FILE* f) { return fsync(fileno(f)) == 0; }
#endif

/* Host-raw-byte fingerprint, not a wire-format digest: only ever compared
 * within this process's own memory (equal-input disagreement checks,
 * logging), never sent across a process boundary or reloaded on a
 * different build, so it doesn't need obicall_wire_encode_result's
 * cross-compiler stability. */
static void compute_result_digest(const obicall_result_t* r, uint8_t out[OBICALL_DIGEST_LEN]) {
    obicall_sha256_ctx_t ctx;
    obicall_sha256_init(&ctx);
    obicall_sha256_update(&ctx, (const uint8_t*)&r->status, sizeof(r->status));
    obicall_sha256_update(&ctx, (const uint8_t*)&r->payload_count, sizeof(r->payload_count));
    obicall_sha256_update(&ctx, (const uint8_t*)r->payload, sizeof(double) * r->payload_count);
    obicall_sha256_update(&ctx, (const uint8_t*)&r->covariance_count, sizeof(r->covariance_count));
    obicall_sha256_update(&ctx, (const uint8_t*)r->covariance, sizeof(double) * r->covariance_count);
    obicall_sha256_final(&ctx, out);
}

obicall_gate_publish_outcome_t OBICALL_CALL obicall_gate_decide_publish(
    obicall_gate_state_t* state, const obicall_result_t* result, int64_t now_ns,
    uint32_t* out_pipeline_index) {
    if (!state || !result) return OBICALL_GATE_REJECT_INCOMPATIBLE;
    if (result->struct_size != sizeof(*result) || result->schema_version != OBICALL_RESULT_SCHEMA_VERSION) {
        return OBICALL_GATE_REJECT_INCOMPATIBLE;
    }
    if (result->valid_until_ns <= now_ns) return OBICALL_GATE_REJECT_EXPIRED;

    uint32_t pidx = OBICALL_MAX_PIPELINES;
    for (uint32_t i = 0; i < state->pipeline_count; ++i) {
        if (memcmp(state->pipelines[i].pipeline_id, result->pipeline_id, OBICALL_PIPELINE_ID_LEN) == 0) {
            pidx = i;
            break;
        }
    }
    if (pidx == OBICALL_MAX_PIPELINES) {
        if (state->pipeline_count >= OBICALL_MAX_PIPELINES) return OBICALL_GATE_REJECT_NO_CAPACITY;
        pidx = state->pipeline_count++;
        memset(&state->pipelines[pidx], 0, sizeof(state->pipelines[pidx]));
        memcpy(state->pipelines[pidx].pipeline_id, result->pipeline_id, OBICALL_PIPELINE_ID_LEN);
    }
    if (out_pipeline_index) *out_pipeline_index = pidx;
    obicall_gate_pipeline_state_t* ps = &state->pipelines[pidx];

    if (state->owner_broker_id == OBICALL_BROKER_NONE) {
        return OBICALL_GATE_REJECT_NOT_OWNER;
    }

    if (result->broker_id != state->owner_broker_id) {
        /* Not the current owner. A lower epoch identifies a revoked
         * former owner's leftover result, which must never be treated as
         * valid just because it happens to name the right pipeline. A
         * non-lower epoch from a non-owner is a legitimate shadow
         * submission - recorded for disagreement comparison, not
         * committed (only the gate's grant can make a broker the owner,
         * never a broker's own belief about the other's health). */
        if (result->epoch < state->epoch) return OBICALL_GATE_REJECT_STALE_EPOCH;
        return OBICALL_GATE_SHADOW_RECORDED;
    }

    if (result->epoch != state->epoch) {
        return OBICALL_GATE_REJECT_STALE_EPOCH;
    }

    if (ps->has_committed && result->window_seq <= ps->last_committed_window_seq) {
        return OBICALL_GATE_REJECT_DUPLICATE_WINDOW;
    }

    ps->has_committed = 1;
    ps->last_committed_window_seq = result->window_seq;
    compute_result_digest(result, ps->last_committed_digest);
    return OBICALL_GATE_COMMIT;
}

int OBICALL_CALL obicall_gate_shadow_promotable(const obicall_gate_readiness_t* shadow_readiness,
                                                 const obicall_gate_promotion_requirements_t* requirements,
                                                 uint64_t committed_high_water_window_seq) {
    if (!shadow_readiness || !requirements) return 0;
    if (!shadow_readiness->healthy) return 0;
    if (memcmp(shadow_readiness->config_digest, requirements->required_config_digest, OBICALL_DIGEST_LEN) != 0) {
        return 0;
    }
    if (shadow_readiness->checkpoint_schema_version != requirements->required_checkpoint_schema_version) {
        return 0;
    }
    if (shadow_readiness->last_processed_window_seq + requirements->max_allowed_replay_gap <
        committed_high_water_window_seq) {
        return 0;
    }
    return 1;
}

obicall_status_t OBICALL_CALL obicall_gate_promote(obicall_gate_state_t* state,
                                                     uint32_t new_owner_broker_id, int64_t now_ns,
                                                     uint64_t* out_new_epoch) {
    if (!state || !out_new_epoch) return OBICALL_ERR_NULL_POINTER;
    (void)now_ns;
    uint64_t new_epoch = state->epoch + 1;
    state->owner_broker_id = OBICALL_BROKER_NONE; /* revoke */
    state->epoch = new_epoch;
    state->owner_broker_id = new_owner_broker_id; /* grant */
    *out_new_epoch = new_epoch;
    return OBICALL_OK;
}

int OBICALL_CALL obicall_gate_results_agree(const obicall_result_t* a, const obicall_result_t* b,
                                             double position_tolerance, double covariance_rel_tolerance) {
    if (!a || !b) return 0;
    if (a->payload_count != b->payload_count || a->covariance_count != b->covariance_count) return 0;
    for (uint32_t i = 0; i < a->payload_count; ++i) {
        if (fabs(a->payload[i] - b->payload[i]) > position_tolerance) return 0;
    }
    for (uint32_t i = 0; i < a->covariance_count; ++i) {
        double av = a->covariance[i], bv = b->covariance[i];
        double denom = fabs(av) > fabs(bv) ? fabs(av) : fabs(bv);
        double rel = denom > 0.0 ? fabs(av - bv) / denom : 0.0;
        if (rel > covariance_rel_tolerance) return 0;
    }
    return 1;
}

obicall_status_t OBICALL_CALL obicall_gate_encode_state(const obicall_gate_state_t* state,
                                                          uint8_t* out, uint32_t out_cap,
                                                          uint32_t* out_len) {
    if (!state || !out || !out_len) return OBICALL_ERR_NULL_POINTER;
    if (state->pipeline_count > OBICALL_MAX_PIPELINES) return OBICALL_ERR_INVALID_ARGUMENT;
    wcursor_t c = {out, out_cap, 0};
    int ok = 1;
    wc_u32(&c, state->struct_size, &ok);
    wc_u32(&c, state->schema_version, &ok);
    wc_u64(&c, state->epoch, &ok);
    wc_u32(&c, state->owner_broker_id, &ok);
    wc_u32(&c, state->pipeline_count, &ok);
    for (uint32_t i = 0; i < state->pipeline_count; ++i) {
        const obicall_gate_pipeline_state_t* p = &state->pipelines[i];
        wc_fixed(&c, p->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
        wc_u32(&c, p->has_committed, &ok);
        wc_u64(&c, p->last_committed_window_seq, &ok);
        wc_bytes(&c, p->last_committed_digest, OBICALL_DIGEST_LEN, &ok);
    }
    if (!ok) return OBICALL_ERR_BUFFER_TOO_SMALL;
    *out_len = c.off;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_gate_decode_state(const uint8_t* in, uint32_t in_len,
                                                          obicall_gate_state_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    rcursor_t c = {in, in_len, 0};
    int ok = 1;
    out->struct_size = rc_u32(&c, &ok);
    out->schema_version = rc_u32(&c, &ok);
    out->epoch = rc_u64(&c, &ok);
    out->owner_broker_id = rc_u32(&c, &ok);
    out->pipeline_count = rc_u32(&c, &ok);
    if (!ok || out->pipeline_count > OBICALL_MAX_PIPELINES) return OBICALL_ERR_WIRE_MALFORMED;
    for (uint32_t i = 0; i < out->pipeline_count; ++i) {
        obicall_gate_pipeline_state_t* p = &out->pipelines[i];
        rc_fixed(&c, p->pipeline_id, OBICALL_PIPELINE_ID_LEN, &ok);
        p->has_committed = rc_u32(&c, &ok);
        p->last_committed_window_seq = rc_u64(&c, &ok);
        rc_bytes(&c, p->last_committed_digest, OBICALL_DIGEST_LEN, &ok);
    }
    out->struct_size = sizeof(*out);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

#define OBICALL_GATE_PERSIST_MAGIC 0x4F424754u /* "OBGT" */
#define OBICALL_GATE_PERSIST_HEADER_LEN 16u
#define OBICALL_GATE_PERSIST_MAX_PAYLOAD 8192u

obicall_status_t OBICALL_CALL obicall_gate_persist_state(FILE* f, const obicall_gate_state_t* state) {
    if (!f || !state) return OBICALL_ERR_NULL_POINTER;
    uint8_t payload[OBICALL_GATE_PERSIST_MAX_PAYLOAD];
    uint32_t payload_len = 0;
    obicall_status_t st = obicall_gate_encode_state(state, payload, sizeof(payload), &payload_len);
    if (st != OBICALL_OK) return st;
    uint32_t crc = obicall_crc32(payload, payload_len);

    uint8_t hdr[OBICALL_GATE_PERSIST_HEADER_LEN];
    wcursor_t c = {hdr, sizeof(hdr), 0};
    int ok = 1;
    wc_u32(&c, OBICALL_GATE_PERSIST_MAGIC, &ok);
    wc_u32(&c, payload_len, &ok);
    wc_u32(&c, crc, &ok);
    wc_u32(&c, 0, &ok);
    if (!ok) return OBICALL_ERR_INTERNAL;

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return OBICALL_ERR_GATE_PERSISTENCE;
    if (payload_len > 0 && fwrite(payload, 1, payload_len, f) != payload_len) return OBICALL_ERR_GATE_PERSISTENCE;
    if (fflush(f) != 0) return OBICALL_ERR_GATE_PERSISTENCE;
    if (!durable_sync(f)) return OBICALL_ERR_GATE_PERSISTENCE;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_gate_load_state(FILE* f, obicall_gate_state_t* out) {
    if (!f || !out) return OBICALL_ERR_NULL_POINTER;
    uint8_t hdr[OBICALL_GATE_PERSIST_HEADER_LEN];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) return OBICALL_ERR_WIRE_MALFORMED;
    rcursor_t c = {hdr, sizeof(hdr), 0};
    int ok = 1;
    uint32_t magic = rc_u32(&c, &ok);
    uint32_t payload_len = rc_u32(&c, &ok);
    uint32_t crc = rc_u32(&c, &ok);
    rc_u32(&c, &ok);
    if (!ok || magic != OBICALL_GATE_PERSIST_MAGIC || payload_len > OBICALL_GATE_PERSIST_MAX_PAYLOAD) {
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    uint8_t payload[OBICALL_GATE_PERSIST_MAX_PAYLOAD];
    if (fread(payload, 1, payload_len, f) != payload_len) return OBICALL_ERR_WIRE_MALFORMED;
    if (obicall_crc32(payload, payload_len) != crc) return OBICALL_ERR_WIRE_CHECKSUM;
    return obicall_gate_decode_state(payload, payload_len, out);
}
