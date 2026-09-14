#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obicall/obicall.h"
#include "osal.h"
#include "procutil.h"

/*
 * obicall-workerd: hosts exactly one provider plugin, loaded dynamically
 * (dlopen/LoadLibraryExW) from a manifest-declared, integrity-checked
 * artifact. Its only IPC peer is the journal - see
 * docs/ARCHITECTURE.md "Worker/broker topology" for why this prototype
 * does not also give workers a direct connection to their broker.
 */

static void dirname_of(const char* path, char* out, size_t out_cap) {
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    const char* last = slash > bslash ? slash : bslash;
    if (!last) {
        if (out_cap > 1) { out[0] = '.'; out[1] = '\0'; }
        return;
    }
    size_t len = (size_t)(last - path);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int read_whole_file(const char* path, uint8_t* buf, uint32_t cap, uint32_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    int truncated = (fgetc(f) != EOF);
    fclose(f);
    if (truncated) return -1;
    *out_len = (uint32_t)n;
    return 0;
}

typedef struct forward_ctx {
    osal_socket_t* journal;
    uint32_t forwarded;
} forward_ctx_t;

static void on_provider_event(void* user_data, const obicall_event_t* event) {
    forward_ctx_t* ctx = (forward_ctx_t*)user_data;
    if (event->event_type != 1u /* observation, by this reference provider's convention */) return;
    obicall_observation_t obs;
    if (obicall_wire_decode_observation(event->payload.data, event->payload.len, &obs) != OBICALL_OK) return;
    if (obicall_observation_validate(&obs, NULL, NULL) != OBICALL_OK) return;

    uint8_t buf[1200];
    uint32_t len = 0;
    if (obicall_wire_encode_observation(&obs, buf, sizeof(buf), &len) != OBICALL_OK) return;
    if (osal_send_frame(ctx->journal, OBICALL_MSG_OBSERVATION_SUBMIT, buf, len, 2000) == 0) {
        ctx->forwarded++;
    }
}

static void apply_fault_file(const obicall_provider_vtable_t* vtable, obicall_provider_handle_t handle,
                              const char* fault_file, char* last_content, size_t last_content_cap) {
    FILE* f = fopen(fault_file, "rb");
    if (!f) return;
    char buf[128];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (strcmp(buf, last_content) == 0) return;
    strncpy(last_content, buf, last_content_cap - 1);

    double mode = 0, bias = 0, dropout = 0, delay = 0;
    sscanf(buf, "%lf %lf %lf %lf", &mode, &bias, &dropout, &delay);

    obicall_observation_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.struct_size = sizeof(ctrl);
    ctrl.schema_version = OBICALL_OBSERVATION_SCHEMA_VERSION;
    strncpy(ctrl.sensor_id, "$ctrl:fault", OBICALL_SENSOR_ID_LEN - 1);
    ctrl.coordinate_frame = OBICALL_FRAME_LOCAL_ENU;
    ctrl.units = OBICALL_UNITS_METERS;
    ctrl.payload_shape = OBICALL_SHAPE_POSITION_2D_VELOCITY;
    ctrl.payload_count = 4;
    ctrl.payload[0] = mode;
    ctrl.payload[1] = bias;
    ctrl.payload[2] = dropout;
    ctrl.payload[3] = delay;
    vtable->submit(handle, &ctrl);
}

int main(int argc, char** argv) {
    osal_net_init();

    const char* manifest_path = procutil_arg_str(argc, argv, "--manifest", NULL);
    const char* journal_port_s = procutil_arg_str(argc, argv, "--journal-port", NULL);
    const char* token_hex = procutil_arg_str(argc, argv, "--token", NULL);
    const char* runtime_dir = procutil_arg_str(argc, argv, "--runtime-dir", ".");
    const char* worker_name = procutil_arg_str(argc, argv, "--worker-name", "worker");
    const char* fault_file = procutil_arg_str(argc, argv, "--fault-file", NULL);
    int64_t instance_seed = procutil_arg_i64(argc, argv, "--instance-seed", 1);
    int64_t tick_ms = procutil_arg_i64(argc, argv, "--tick-ms", 20);
#if defined(OBICALL_TEST_HOOKS_ENABLED)
    int64_t test_crash_after_ms = procutil_arg_i64(argc, argv, "--test-crash-after-ms", -1);
    int64_t test_hang_after_ms = procutil_arg_i64(argc, argv, "--test-hang-after-ms", -1);
#endif

    if (!manifest_path || !journal_port_s || !token_hex) {
        fprintf(stderr, "obicall-workerd: --manifest, --journal-port, --token are required\n");
        return 2;
    }
    /* Written before dynload/create/connect: that whole sequence can
     * legitimately take a little while, during which the supervisor's
     * heartbeat-file staleness check would otherwise see nothing yet and
     * could mistake startup for a hang. */
    procutil_write_pid_file(runtime_dir, worker_name, procutil_current_pid());

    uint8_t token[OBICALL_RUN_TOKEN_LEN];
    if (strlen(token_hex) != OBICALL_RUN_TOKEN_LEN * 2 ||
        procutil_hex_decode(token_hex, (uint32_t)strlen(token_hex), token, OBICALL_RUN_TOKEN_LEN) != 0) {
        fprintf(stderr, "obicall-workerd: malformed --token\n");
        return 2;
    }

    uint8_t manifest_buf[8192];
    uint32_t manifest_len = 0;
    if (read_whole_file(manifest_path, manifest_buf, sizeof(manifest_buf), &manifest_len) != 0) {
        fprintf(stderr, "obicall-workerd: cannot read manifest %s\n", manifest_path);
        return 2;
    }
    obicall_manifest_t manifest;
    char issue[256];
    if (obicall_manifest_parse_json(manifest_buf, manifest_len, &manifest, issue, sizeof(issue)) != OBICALL_OK) {
        fprintf(stderr, "obicall-workerd: manifest parse failed: %s\n", issue);
        return 2;
    }

    char dir[512];
    dirname_of(manifest_path, dir, sizeof(dir));
    char artifact_path[1024];
    /* A manifest's artifact_path may omit the platform's shared-library
     * extension (the common, portable case - one manifest works on every
     * OS); if it already has one, it is used verbatim. */
    size_t apath_len = strlen(manifest.artifact_path);
    int has_known_ext = (apath_len >= 3 && strcmp(manifest.artifact_path + apath_len - 3, ".so") == 0) ||
                         (apath_len >= 4 && strcmp(manifest.artifact_path + apath_len - 4, ".dll") == 0) ||
                         (apath_len >= 6 && strcmp(manifest.artifact_path + apath_len - 6, ".dylib") == 0);
#if defined(_WIN32)
    const char* platform_ext = has_known_ext ? "" : ".dll";
#elif defined(__APPLE__)
    const char* platform_ext = has_known_ext ? "" : ".dylib";
#else
    const char* platform_ext = has_known_ext ? "" : ".so";
#endif
    snprintf(artifact_path, sizeof(artifact_path), "%s/%s%s", dir, manifest.artifact_path, platform_ext);

    if (manifest.has_artifact_sha256) {
        FILE* af = fopen(artifact_path, "rb");
        if (!af) {
            fprintf(stderr, "obicall-workerd: cannot open artifact %s\n", artifact_path);
            return 2;
        }
        obicall_sha256_ctx_t ctx;
        obicall_sha256_init(&ctx);
        uint8_t chunk[65536];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), af)) > 0) obicall_sha256_update(&ctx, chunk, n);
        fclose(af);
        uint8_t digest[OBICALL_DIGEST_LEN];
        obicall_sha256_final(&ctx, digest);
        if (memcmp(digest, manifest.artifact_sha256, OBICALL_DIGEST_LEN) != 0) {
            fprintf(stderr, "obicall-workerd: artifact SHA-256 mismatch for %s - refusing to load\n", artifact_path);
            return 3;
        }
    }

    osal_module_t* module = NULL;
    if (osal_dynload_open(artifact_path, &module) != 0) {
        fprintf(stderr, "obicall-workerd: failed to load %s: %s\n", artifact_path, osal_dynload_last_error());
        return 4;
    }
    obicall_plugin_query_v1_fn query =
        (obicall_plugin_query_v1_fn)osal_dynload_symbol(module, OBICALL_PLUGIN_QUERY_SYMBOL);
    if (!query) {
        fprintf(stderr, "obicall-workerd: symbol %s not found in %s\n", OBICALL_PLUGIN_QUERY_SYMBOL, artifact_path);
        return 5;
    }

    const obicall_descriptor_t* descriptor = NULL;
    const obicall_provider_vtable_t* vtable = NULL;
    if (query(&descriptor, &vtable) != OBICALL_OK || !descriptor || !vtable) {
        fprintf(stderr, "obicall-workerd: plugin query failed\n");
        return 6;
    }
    if (descriptor->struct_size < sizeof(obicall_descriptor_t) ||
        descriptor->abi_version_major != OBICALL_ABI_VERSION_MAJOR ||
        descriptor->abi_version_minor > OBICALL_ABI_VERSION_MINOR) {
        fprintf(stderr, "obicall-workerd: incompatible ABI %u.%u (runtime supports %u.%u)\n",
                descriptor->abi_version_major, descriptor->abi_version_minor, OBICALL_ABI_VERSION_MAJOR,
                OBICALL_ABI_VERSION_MINOR);
        return 7;
    }
    if (vtable->struct_size < sizeof(obicall_provider_vtable_t) || !vtable->create || !vtable->submit ||
        !vtable->poll_events || !vtable->destroy || !vtable->release_buffer) {
        fprintf(stderr, "obicall-workerd: provider vtable missing required entries\n");
        return 7;
    }

    obicall_provider_config_t config;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.config_schema_version = manifest.config_schema_version;
    config.config_json.data = (const uint8_t*)manifest.config_json;
    config.config_json.len = (uint32_t)strlen(manifest.config_json);
    config.instance_seed = (uint64_t)instance_seed;

    obicall_provider_handle_t handle = NULL;
    if (vtable->create(&config, &handle) != OBICALL_OK || !handle) {
        fprintf(stderr, "obicall-workerd: provider create() failed\n");
        return 8;
    }

    uint16_t journal_port = (uint16_t)strtol(journal_port_s, NULL, 10);
    osal_socket_t* journal_conn = NULL;
    if (procutil_client_connect(journal_port, token, OBICALL_ROLE_WORKER, 3000, &journal_conn) != 0) {
        fprintf(stderr, "obicall-workerd: failed to connect to journal on port %u\n", (unsigned)journal_port);
        return 9;
    }

    fprintf(stderr, "obicall-workerd[%s]: loaded %s %s, connected to journal\n", worker_name,
            descriptor->provider_name, descriptor->provider_version);

    forward_ctx_t fctx = {journal_conn, 0};
    char last_fault_content[128] = "";
    int64_t start = osal_monotonic_ns();
    int64_t last_heartbeat_write = 0;
    const int64_t heartbeat_period_ns = 200 * 1000000LL;

    for (;;) {
#if defined(OBICALL_TEST_HOOKS_ENABLED)
        int64_t elapsed_ms = (osal_monotonic_ns() - start) / 1000000;
        if (test_crash_after_ms >= 0 && elapsed_ms >= test_crash_after_ms) {
            fprintf(stderr, "obicall-workerd[%s]: test hook: simulating crash\n", worker_name);
            abort();
        }
        if (test_hang_after_ms >= 0 && elapsed_ms >= test_hang_after_ms) {
            fprintf(stderr, "obicall-workerd[%s]: test hook: simulating hang\n", worker_name);
            for (;;) osal_sleep_ms(60000);
        }
#endif
        if (fault_file) apply_fault_file(vtable, handle, fault_file, last_fault_content, sizeof(last_fault_content));

        uint32_t count = 0;
        vtable->poll_events(handle, on_provider_event, &fctx, &count);

        int64_t now = osal_monotonic_ns();
        if (now - last_heartbeat_write >= heartbeat_period_ns) {
            procutil_write_pid_file(runtime_dir, worker_name, procutil_current_pid());
            last_heartbeat_write = now;
        }
        osal_sleep_ms((int)tick_ms);
    }
}
