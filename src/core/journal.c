#include "obicall/journal.h"
#include "obicall/observation.h"
#include "obicall/wire.h"
#include "obicall/digest.h"

#include <string.h>

#include "wire_cursor.h"

#if defined(_WIN32)
#include <io.h>
static int durable_sync(FILE* f) { return _commit(_fileno(f)) == 0; }
#else
#include <unistd.h>
static int durable_sync(FILE* f) { return fsync(fileno(f)) == 0; }
#endif

obicall_admission_reason_t OBICALL_CALL obicall_journal_admit(const obicall_journal_config_t* cfg,
                                                                obicall_sensor_track_t* track,
                                                                const obicall_observation_t* obs,
                                                                uint32_t current_pending_count) {
    if (!cfg || !track || !obs) return OBICALL_REJECT_VALIDATION;
    if (obicall_observation_validate(obs, NULL, NULL) != OBICALL_OK) return OBICALL_REJECT_VALIDATION;

    int is_new_session = !track->has_data || track->source_boot_id != obs->source_boot_id;

    if (!is_new_session) {
        if (obs->sequence <= track->highest_sequence_seen) {
            uint64_t behind = track->highest_sequence_seen - obs->sequence;
            return (behind > cfg->reorder_window_n) ? OBICALL_REJECT_LATE : OBICALL_REJECT_DUPLICATE;
        }
        if (obs->sample_time_ns < track->watermark_sample_time_ns - cfg->max_lateness_ns) {
            return OBICALL_REJECT_LATE;
        }
    }

    if (current_pending_count >= cfg->max_pending_per_pipeline) return OBICALL_REJECT_OVERFLOW;

    memcpy(track->sensor_id, obs->sensor_id, OBICALL_SENSOR_ID_LEN);
    track->source_boot_id = obs->source_boot_id;
    track->has_data = 1;
    track->highest_sequence_seen = obs->sequence;
    if (is_new_session || obs->sample_time_ns > track->watermark_sample_time_ns) {
        track->watermark_sample_time_ns = obs->sample_time_ns;
    }
    return OBICALL_ADMIT_OK;
}

obicall_status_t OBICALL_CALL obicall_journal_encode_record_header(
    const obicall_journal_record_header_t* hdr, uint8_t out[OBICALL_JOURNAL_RECORD_HEADER_LEN]) {
    if (!hdr || !out) return OBICALL_ERR_NULL_POINTER;
    wcursor_t c = {out, OBICALL_JOURNAL_RECORD_HEADER_LEN, 0};
    int ok = 1;
    wc_u32(&c, hdr->magic, &ok);
    wc_u32(&c, hdr->record_len, &ok);
    wc_u32(&c, hdr->observation_len, &ok);
    wc_u32(&c, hdr->crc32, &ok);
    wc_u64(&c, hdr->window_seq, &ok);
    wc_i64(&c, hdr->admitted_at_ns, &ok);
    wc_u32(&c, hdr->admission_reason, &ok);
    wc_u32(&c, hdr->reserved, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

obicall_status_t OBICALL_CALL obicall_journal_decode_record_header(
    const uint8_t in[OBICALL_JOURNAL_RECORD_HEADER_LEN], obicall_journal_record_header_t* out) {
    if (!in || !out) return OBICALL_ERR_NULL_POINTER;
    rcursor_t c = {in, OBICALL_JOURNAL_RECORD_HEADER_LEN, 0};
    int ok = 1;
    out->magic = rc_u32(&c, &ok);
    out->record_len = rc_u32(&c, &ok);
    out->observation_len = rc_u32(&c, &ok);
    out->crc32 = rc_u32(&c, &ok);
    out->window_seq = rc_u64(&c, &ok);
    out->admitted_at_ns = rc_i64(&c, &ok);
    out->admission_reason = rc_u32(&c, &ok);
    out->reserved = rc_u32(&c, &ok);
    return ok ? OBICALL_OK : OBICALL_ERR_WIRE_MALFORMED;
}

#define OBICALL_JOURNAL_MAX_RECORD_OBS_LEN 4096u

obicall_status_t OBICALL_CALL obicall_journal_append_record(FILE* f, uint64_t window_seq,
                                                              int64_t admitted_at_ns,
                                                              obicall_admission_reason_t reason,
                                                              const obicall_observation_t* obs) {
    if (!f || !obs) return OBICALL_ERR_NULL_POINTER;

    uint8_t obs_buf[OBICALL_JOURNAL_MAX_RECORD_OBS_LEN];
    uint32_t obs_len = 0;
    obicall_status_t st = obicall_wire_encode_observation(obs, obs_buf, sizeof(obs_buf), &obs_len);
    if (st != OBICALL_OK) return st;

    obicall_journal_record_header_t hdr;
    hdr.magic = OBICALL_JOURNAL_RECORD_MAGIC;
    hdr.record_len = OBICALL_JOURNAL_RECORD_HEADER_LEN + obs_len;
    hdr.observation_len = obs_len;
    hdr.crc32 = obicall_crc32(obs_buf, obs_len);
    hdr.window_seq = window_seq;
    hdr.admitted_at_ns = admitted_at_ns;
    hdr.admission_reason = (uint32_t)reason;
    hdr.reserved = 0;

    uint8_t hdr_buf[OBICALL_JOURNAL_RECORD_HEADER_LEN];
    st = obicall_journal_encode_record_header(&hdr, hdr_buf);
    if (st != OBICALL_OK) return st;

    if (fwrite(hdr_buf, 1, sizeof(hdr_buf), f) != sizeof(hdr_buf)) return OBICALL_ERR_IO;
    if (obs_len > 0 && fwrite(obs_buf, 1, obs_len, f) != obs_len) return OBICALL_ERR_IO;
    if (fflush(f) != 0) return OBICALL_ERR_IO;
    if (!durable_sync(f)) return OBICALL_ERR_IO;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_journal_read_record(FILE* f,
                                                            obicall_journal_record_header_t* out_header,
                                                            obicall_observation_t* out_obs) {
    if (!f || !out_header || !out_obs) return OBICALL_ERR_NULL_POINTER;
    long start_pos = ftell(f);

    uint8_t hdr_buf[OBICALL_JOURNAL_RECORD_HEADER_LEN];
    size_t n = fread(hdr_buf, 1, sizeof(hdr_buf), f);
    if (n == 0 && feof(f)) return OBICALL_ERR_NOT_FOUND;
    if (n != sizeof(hdr_buf)) {
        fseek(f, start_pos, SEEK_SET);
        return OBICALL_ERR_WIRE_MALFORMED;
    }

    obicall_journal_record_header_t hdr;
    if (obicall_journal_decode_record_header(hdr_buf, &hdr) != OBICALL_OK ||
        hdr.magic != OBICALL_JOURNAL_RECORD_MAGIC) {
        fseek(f, start_pos, SEEK_SET);
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    if (hdr.observation_len > OBICALL_JOURNAL_MAX_RECORD_OBS_LEN ||
        hdr.record_len != OBICALL_JOURNAL_RECORD_HEADER_LEN + hdr.observation_len) {
        fseek(f, start_pos, SEEK_SET);
        return OBICALL_ERR_WIRE_TOO_LARGE;
    }

    uint8_t obs_buf[OBICALL_JOURNAL_MAX_RECORD_OBS_LEN];
    n = fread(obs_buf, 1, hdr.observation_len, f);
    if (n != hdr.observation_len) {
        fseek(f, start_pos, SEEK_SET);
        return OBICALL_ERR_WIRE_MALFORMED;
    }
    if (obicall_crc32(obs_buf, hdr.observation_len) != hdr.crc32) {
        fseek(f, start_pos, SEEK_SET);
        return OBICALL_ERR_WIRE_CHECKSUM;
    }

    obicall_status_t st = obicall_wire_decode_observation(obs_buf, hdr.observation_len, out_obs);
    if (st != OBICALL_OK) {
        fseek(f, start_pos, SEEK_SET);
        return st;
    }

    *out_header = hdr;
    return OBICALL_OK;
}
