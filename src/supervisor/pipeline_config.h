#ifndef OBICALL_PIPELINE_CONFIG_H
#define OBICALL_PIPELINE_CONFIG_H

#include <stdint.h>
#include "obicall/types.h"

#define PIPELINE_MAX_PROVIDERS 8u

typedef struct provider_ref {
    char manifest[512];
    char worker_name[64];
    int64_t instance_seed;
} provider_ref_t;

typedef struct pipeline_config {
    char pipeline_id[OBICALL_PIPELINE_ID_LEN];
    char manifests_dir[512];

    uint64_t journal_reorder_window;
    int64_t journal_max_lateness_ms;
    uint32_t journal_max_pending;

    int64_t gate_result_validity_ms;
    int64_t gate_suspect_timeout_ms;
    int64_t gate_confirm_timeout_ms;
    uint64_t gate_max_replay_gap;

    double est_process_noise_position;
    double est_process_noise_velocity;
    double est_initial_position_variance;
    double est_initial_velocity_variance;
    double est_max_prediction_gap_s;

    int dgt_enabled;
    double dgt_hysteresis_margin;

    char sensor_ids_csv[256];

    uint32_t provider_count;
    provider_ref_t providers[PIPELINE_MAX_PROVIDERS];

    uint8_t config_digest[32];
} pipeline_config_t;

/* err/err_cap receive a human-readable reason on failure. */
int pipeline_config_load(const char* path, pipeline_config_t* out, char* err, size_t err_cap);

#endif /* OBICALL_PIPELINE_CONFIG_H */
