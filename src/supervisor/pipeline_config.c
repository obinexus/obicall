#include "pipeline_config.h"
#include "json_min.h"
#include "obicall/digest.h"
#include "obicall/dgt.h"

#include <stdio.h>
#include <string.h>

static int get_str(const uint8_t* js, uint32_t jl, const char* key, char* out, uint32_t out_cap) {
    json_span_t sp;
    if (!json_object_find(js, jl, key, &sp) || !sp.is_string) return 0;
    return json_decode_string(js + sp.off, sp.len, out, out_cap) >= 0;
}
static int get_num(const uint8_t* js, uint32_t jl, const char* key, double* out) {
    json_span_t sp;
    if (!json_object_find(js, jl, key, &sp) || sp.is_string) return 0;
    return json_parse_number(js + sp.off, sp.len, out);
}
static int get_bool(const uint8_t* js, uint32_t jl, const char* key, int* out) {
    json_span_t sp;
    if (!json_object_find(js, jl, key, &sp) || sp.is_string) return 0;
    if (json_span_is_true(js + sp.off, sp.len)) { *out = 1; return 1; }
    if (json_span_is_false(js + sp.off, sp.len)) { *out = 0; return 1; }
    return 0;
}
static int get_obj(const uint8_t* js, uint32_t jl, const char* key, const uint8_t** oobj, uint32_t* olen) {
    json_span_t sp;
    if (!json_object_find(js, jl, key, &sp) || sp.is_string) return 0;
    *oobj = js + sp.off;
    *olen = sp.len;
    return 1;
}

int pipeline_config_load(const char* path, pipeline_config_t* out, char* err, size_t err_cap) {
    memset(out, 0, sizeof(*out));
    strncpy(out->pipeline_id, "default", sizeof(out->pipeline_id) - 1);
    strncpy(out->manifests_dir, "providers", sizeof(out->manifests_dir) - 1);
    out->journal_reorder_window = 8;
    out->journal_max_lateness_ms = 2000;
    out->journal_max_pending = 4096;
    out->gate_result_validity_ms = 500;
    out->gate_suspect_timeout_ms = 300;
    out->gate_confirm_timeout_ms = 700;
    out->gate_max_replay_gap = 50;
    out->est_process_noise_position = 0.05;
    out->est_process_noise_velocity = 0.1;
    out->est_initial_position_variance = 10.0;
    out->est_initial_velocity_variance = 10.0;
    out->est_max_prediction_gap_s = 30.0;
    out->dgt_hysteresis_margin = OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN;

    FILE* f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, err_cap, "cannot open %s", path);
        return -1;
    }
    static uint8_t buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), f);
    int truncated = (fgetc(f) != EOF);
    fclose(f);
    if (truncated) {
        if (err) snprintf(err, err_cap, "config file too large: %s", path);
        return -1;
    }
    obicall_sha256(buf, n, out->config_digest);

    const uint8_t* js = buf;
    uint32_t jl = (uint32_t)n;

    double num;
    if (get_num(js, jl, "schema_version", &num) && (uint32_t)num != 1) {
        if (err) snprintf(err, err_cap, "unsupported config schema_version");
        return -1;
    }
    get_str(js, jl, "pipeline_id", out->pipeline_id, sizeof(out->pipeline_id));
    get_str(js, jl, "manifests_dir", out->manifests_dir, sizeof(out->manifests_dir));

    const uint8_t* jo;
    uint32_t jol;
    if (get_obj(js, jl, "journal", &jo, &jol)) {
        if (get_num(jo, jol, "reorder_window", &num)) out->journal_reorder_window = (uint64_t)num;
        if (get_num(jo, jol, "max_lateness_ms", &num)) out->journal_max_lateness_ms = (int64_t)num;
        if (get_num(jo, jol, "max_pending", &num)) out->journal_max_pending = (uint32_t)num;
    }
    if (get_obj(js, jl, "gate", &jo, &jol)) {
        if (get_num(jo, jol, "result_validity_ms", &num)) out->gate_result_validity_ms = (int64_t)num;
        if (get_num(jo, jol, "suspect_timeout_ms", &num)) out->gate_suspect_timeout_ms = (int64_t)num;
        if (get_num(jo, jol, "confirm_timeout_ms", &num)) out->gate_confirm_timeout_ms = (int64_t)num;
        if (get_num(jo, jol, "max_replay_gap", &num)) out->gate_max_replay_gap = (uint64_t)num;
    }
    if (get_obj(js, jl, "estimator", &jo, &jol)) {
        if (get_num(jo, jol, "process_noise_position", &num)) out->est_process_noise_position = num;
        if (get_num(jo, jol, "process_noise_velocity", &num)) out->est_process_noise_velocity = num;
        if (get_num(jo, jol, "initial_position_variance", &num)) out->est_initial_position_variance = num;
        if (get_num(jo, jol, "initial_velocity_variance", &num)) out->est_initial_velocity_variance = num;
        if (get_num(jo, jol, "max_prediction_gap_s", &num)) out->est_max_prediction_gap_s = num;
    }
    if (get_obj(js, jl, "dgt", &jo, &jol)) {
        get_bool(jo, jol, "enabled", &out->dgt_enabled);
        if (get_num(jo, jol, "hysteresis_margin", &num)) out->dgt_hysteresis_margin = num;
    }

    {
        json_span_t sp;
        if (json_object_find(js, jl, "sensor_ids", &sp) && !sp.is_string) {
            int cnt = json_array_count(js + sp.off, sp.len);
            size_t off = 0;
            for (int i = 0; i < cnt; ++i) {
                json_span_t e;
                if (!json_array_get(js + sp.off, sp.len, (uint32_t)i, &e) || !e.is_string) continue;
                char name[OBICALL_SENSOR_ID_LEN];
                int dn = json_decode_string(js + sp.off + e.off, e.len, name, sizeof(name));
                if (dn < 0) continue;
                int written = snprintf(out->sensor_ids_csv + off, sizeof(out->sensor_ids_csv) - off, "%s%s",
                                        (off > 0 ? "," : ""), name);
                if (written > 0) off += (size_t)written;
            }
        }
    }

    {
        json_span_t sp;
        if (json_object_find(js, jl, "providers", &sp) && !sp.is_string) {
            int cnt = json_array_count(js + sp.off, sp.len);
            if (cnt > (int)PIPELINE_MAX_PROVIDERS) cnt = (int)PIPELINE_MAX_PROVIDERS;
            for (int i = 0; i < cnt; ++i) {
                json_span_t e;
                if (!json_array_get(js + sp.off, sp.len, (uint32_t)i, &e) || e.is_string) continue;
                const uint8_t* pobj = js + sp.off + e.off;
                uint32_t plen = e.len;
                provider_ref_t* pr = &out->providers[out->provider_count];
                get_str(pobj, plen, "manifest", pr->manifest, sizeof(pr->manifest));
                get_str(pobj, plen, "worker_name", pr->worker_name, sizeof(pr->worker_name));
                double seed = 1;
                get_num(pobj, plen, "instance_seed", &seed);
                pr->instance_seed = (int64_t)seed;
                if (pr->manifest[0] != '\0') out->provider_count++;
            }
        }
    }

    return 0;
}
