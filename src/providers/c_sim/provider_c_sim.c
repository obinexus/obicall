#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "obicall/obicall.h"
#include "json_min.h"

/*
 * Reference C provider: a simulated 2D position sensor. Dynamically
 * loaded by obicall-workerd via dlopen/LoadLibraryExW - never linked
 * against directly - so it depends on nothing but libc and the public
 * obicall headers (no osal, no internal core headers) to stay a
 * realistic example of a third-party provider artifact.
 *
 * Event delivery (poll_events) carries a wire-encoded obicall_observation_t
 * as event_type OBICALL_C_SIM_EVENT_OBSERVATION; that is the provider's
 * only output. submit() accepts ordinary observations as a no-op (this is
 * a sensor source, not a consumer) except for one documented control
 * sentinel - a submitted observation with sensor_id "$ctrl:fault" - used
 * by the demo/tests to switch on dropout/bias/delay fault simulation
 * (docs/DGT.md). That sentinel is this provider's only control surface;
 * it is not part of the core ABI.
 */

#define OBICALL_C_SIM_EVENT_OBSERVATION 1u
#define OBICALL_C_SIM_CHECKPOINT_VERSION 1u

enum sim_fault_mode { SIM_FAULT_NONE = 0, SIM_FAULT_DROPOUT = 1, SIM_FAULT_BIAS = 2, SIM_FAULT_DELAY = 3 };

typedef struct sim_provider {
    char sensor_id[OBICALL_SENSOR_ID_LEN];
    double noise_std;
    int64_t update_period_ns;
    double origin_x, origin_y, speed_x, speed_y;

    uint64_t source_boot_id;
    uint64_t next_sequence;
    int64_t start_time_ns;
    int64_t last_emit_time_ns;
    uint32_t rng_state;

    uint32_t fault_mode;
    double fault_bias;
    double fault_dropout_prob;
    int64_t fault_extra_delay_ns;
} sim_provider_t;

#pragma pack(push, 1)
typedef struct sim_checkpoint_blob {
    uint32_t version;
    uint64_t next_sequence;
    uint32_t rng_state;
    int64_t last_emit_time_ns;
} sim_checkpoint_blob_t;
#pragma pack(pop)

static void copy_bounded(char* dst, size_t dst_cap, const char* src) {
    size_t i = 0;
    for (; i + 1 < dst_cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

#if defined(_WIN32)
#include <windows.h>
static int64_t local_clock_ns(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * (1e9 / (double)freq.QuadPart));
}
#else
#include <time.h>
static int64_t local_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
#endif

static uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
static double uniform01(uint32_t* state) { return (double)(xorshift32(state) >> 8) / (double)(1u << 24); }
static double gaussian(uint32_t* state, double mean, double stddev) {
    double u1 = uniform01(state);
    if (u1 < 1e-12) u1 = 1e-12;
    double u2 = uniform01(state);
    const double two_pi = 6.283185307179586;
    double z = sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
    return mean + stddev * z;
}

static obicall_status_t sim_create(const obicall_provider_config_t* config,
                                    obicall_provider_handle_t* out_handle) {
    if (!config || !out_handle) return OBICALL_ERR_NULL_POINTER;
    sim_provider_t* p = (sim_provider_t*)calloc(1, sizeof(sim_provider_t));
    if (!p) return OBICALL_ERR_INTERNAL;

    copy_bounded(p->sensor_id, sizeof(p->sensor_id), "sim_sensor");
    p->noise_std = 0.2;
    p->update_period_ns = 100 * 1000000LL;
    p->origin_x = 0.0;
    p->origin_y = 0.0;
    p->speed_x = 1.0;
    p->speed_y = 0.5;

    const uint8_t* js = config->config_json.data;
    uint32_t jl = config->config_json.len;
    if (js && jl > 0) {
        json_span_t sp;
        double num;
        if (json_object_find(js, jl, "sensor_id", &sp) && sp.is_string) {
            json_decode_string(js + sp.off, sp.len, p->sensor_id, sizeof(p->sensor_id));
        }
        if (json_object_find(js, jl, "noise_std_m", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->noise_std = num;
        if (json_object_find(js, jl, "update_period_ms", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->update_period_ns = (int64_t)(num * 1e6);
        if (json_object_find(js, jl, "origin_x", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->origin_x = num;
        if (json_object_find(js, jl, "origin_y", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->origin_y = num;
        if (json_object_find(js, jl, "speed_x", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->speed_x = num;
        if (json_object_find(js, jl, "speed_y", &sp) && !sp.is_string && json_parse_number(js + sp.off, sp.len, &num)) p->speed_y = num;
    }

    p->rng_state = config->instance_seed ? (uint32_t)config->instance_seed : 0x9E3779B9u;
    if (p->rng_state == 0) p->rng_state = 0x9E3779B9u;
    p->source_boot_id = ((uint64_t)local_clock_ns()) ^ config->instance_seed;
    p->next_sequence = 1;
    p->start_time_ns = local_clock_ns();
    p->last_emit_time_ns = 0;
    p->fault_mode = SIM_FAULT_NONE;

    *out_handle = (obicall_provider_handle_t)p;
    return OBICALL_OK;
}

static obicall_status_t sim_submit(obicall_provider_handle_t handle, const obicall_observation_t* obs) {
    if (!handle || !obs) return OBICALL_ERR_NULL_POINTER;
    sim_provider_t* p = (sim_provider_t*)handle;
    if (strncmp(obs->sensor_id, "$ctrl:fault", OBICALL_SENSOR_ID_LEN) == 0 && obs->payload_count >= 1) {
        p->fault_mode = (uint32_t)obs->payload[0];
        if (obs->payload_count >= 2) p->fault_bias = obs->payload[1];
        if (obs->payload_count >= 3) p->fault_dropout_prob = obs->payload[2];
        if (obs->payload_count >= 4) p->fault_extra_delay_ns = (int64_t)(obs->payload[3] * 1e9);
    }
    return OBICALL_OK; /* not a control sentinel: this provider has no other submit semantics */
}

static obicall_status_t sim_poll_events(obicall_provider_handle_t handle,
                                         obicall_event_callback_fn on_event, void* user_data,
                                         uint32_t* out_count) {
    if (!handle || !out_count) return OBICALL_ERR_NULL_POINTER;
    sim_provider_t* p = (sim_provider_t*)handle;
    *out_count = 0;

    int64_t now = local_clock_ns();
    if (p->last_emit_time_ns != 0 && now - p->last_emit_time_ns < p->update_period_ns) return OBICALL_OK;
    p->last_emit_time_ns = now;

    if (p->fault_mode == SIM_FAULT_DROPOUT && uniform01(&p->rng_state) < p->fault_dropout_prob) {
        return OBICALL_OK; /* simulated dropped reading: no event this tick */
    }

    double t = (double)(now - p->start_time_ns) / 1e9;
    double bias = (p->fault_mode == SIM_FAULT_BIAS) ? p->fault_bias : 0.0;
    double zx = p->origin_x + p->speed_x * t + bias + gaussian(&p->rng_state, 0.0, p->noise_std);
    double zy = p->origin_y + p->speed_y * t + bias + gaussian(&p->rng_state, 0.0, p->noise_std);

    obicall_observation_t obs;
    memset(&obs, 0, sizeof(obs));
    obs.struct_size = sizeof(obs);
    obs.schema_version = OBICALL_OBSERVATION_SCHEMA_VERSION;
    copy_bounded(obs.sensor_id, sizeof(obs.sensor_id), p->sensor_id);
    obs.source_boot_id = p->source_boot_id;
    obs.sequence = p->next_sequence++;
    obs.sample_time_ns = (p->fault_mode == SIM_FAULT_DELAY) ? (now - p->fault_extra_delay_ns) : now;
    obs.arrival_time_ns = now;
    obs.clock_domain = OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC;
    obs.time_uncertainty_s = 0.01;
    obs.coordinate_frame = OBICALL_FRAME_LOCAL_ENU;
    obs.units = OBICALL_UNITS_METERS;
    obs.calibration_version = 1;
    obs.payload_shape = OBICALL_SHAPE_POSITION_2D;
    obs.payload_count = 2;
    obs.payload[0] = zx;
    obs.payload[1] = zy;
    obs.covariance_count = 4;
    double var = p->noise_std * p->noise_std;
    obs.covariance[0] = var;
    obs.covariance[1] = 0.0;
    obs.covariance[2] = 0.0;
    obs.covariance[3] = var;

    uint8_t wire_buf[1024];
    uint32_t wire_len = 0;
    if (obicall_wire_encode_observation(&obs, wire_buf, sizeof(wire_buf), &wire_len) != OBICALL_OK) {
        return OBICALL_ERR_INTERNAL;
    }

    obicall_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = sizeof(ev);
    ev.schema_version = OBICALL_EVENT_SCHEMA_VERSION;
    ev.event_type = OBICALL_C_SIM_EVENT_OBSERVATION;
    ev.timestamp_ns = now;
    ev.payload.data = wire_buf;
    ev.payload.len = wire_len;

    if (on_event) on_event(user_data, &ev);
    *out_count = 1;
    return OBICALL_OK;
}

static obicall_status_t sim_checkpoint(obicall_provider_handle_t handle, obicall_owned_buffer_t* out_blob) {
    if (!handle || !out_blob) return OBICALL_ERR_NULL_POINTER;
    sim_provider_t* p = (sim_provider_t*)handle;
    sim_checkpoint_blob_t blob;
    blob.version = OBICALL_C_SIM_CHECKPOINT_VERSION;
    blob.next_sequence = p->next_sequence;
    blob.rng_state = p->rng_state;
    blob.last_emit_time_ns = p->last_emit_time_ns;

    uint8_t* buf = (uint8_t*)malloc(sizeof(blob));
    if (!buf) return OBICALL_ERR_INTERNAL;
    memcpy(buf, &blob, sizeof(blob));
    out_blob->data = buf;
    out_blob->len = sizeof(blob);
    out_blob->capacity = sizeof(blob);
    return OBICALL_OK;
}

static obicall_status_t sim_restore(obicall_provider_handle_t handle, obicall_buffer_t blob) {
    if (!handle) return OBICALL_ERR_NULL_POINTER;
    if (blob.len != sizeof(sim_checkpoint_blob_t)) return OBICALL_ERR_INVALID_ARGUMENT;
    sim_checkpoint_blob_t b;
    memcpy(&b, blob.data, sizeof(b));
    if (b.version != OBICALL_C_SIM_CHECKPOINT_VERSION) return OBICALL_ERR_UNSUPPORTED;
    sim_provider_t* p = (sim_provider_t*)handle;
    p->next_sequence = b.next_sequence;
    p->rng_state = b.rng_state;
    p->last_emit_time_ns = b.last_emit_time_ns;
    return OBICALL_OK;
}

static void sim_release_buffer(obicall_owned_buffer_t* buf) {
    if (buf && buf->data) {
        free(buf->data);
        buf->data = NULL;
        buf->len = 0;
        buf->capacity = 0;
    }
}

static void sim_destroy(obicall_provider_handle_t handle) { free(handle); }

static const obicall_descriptor_t g_descriptor = {
    sizeof(obicall_descriptor_t),
    OBICALL_ABI_VERSION_MAJOR,
    OBICALL_ABI_VERSION_MINOR,
    OBICALL_OBSERVATION_SCHEMA_VERSION,
    OBICALL_C_SIM_CHECKPOINT_VERSION,
    1u,
    0u,
    OBICALL_CAP_POSITION_SENSOR | OBICALL_CAP_SUPPORTS_CHECKPOINT | OBICALL_CAP_SUPPORTS_FAULT_INJECTION,
    "provider_c_sim",
    "0.1.0"};

static const obicall_provider_vtable_t g_vtable = {
    sizeof(obicall_provider_vtable_t),
    0u,
    sim_create,
    sim_submit,
    sim_poll_events,
    sim_checkpoint,
    sim_restore,
    sim_release_buffer,
    sim_destroy};

OBICALL_EXPORT obicall_status_t OBICALL_CALL obicall_plugin_query_v1(
    const obicall_descriptor_t** out_descriptor, const obicall_provider_vtable_t** out_vtable) {
    if (!out_descriptor || !out_vtable) return OBICALL_ERR_NULL_POINTER;
    *out_descriptor = &g_descriptor;
    *out_vtable = &g_vtable;
    return OBICALL_OK;
}
