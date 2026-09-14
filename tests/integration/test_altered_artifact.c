#include "test_helpers.h"

/* Writes a manifest declaring the *real* provider_c_sim artifact's SHA-256
 * (computed here with the real obicall_sha256, not hand-copied - keeps
 * this test portable across platforms/toolchains where the built bytes
 * differ), points it at a working copy of that artifact, and confirms:
 * (1) an unaltered copy loads fine, (2) a copy with one flipped byte is
 * refused by obicall-workerd's integrity check rather than loaded. */

#ifndef OBICALL_WORKERD_PATH
#error "OBICALL_WORKERD_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_PROVIDER_ARTIFACT_PATH
#error "OBICALL_TEST_PROVIDER_ARTIFACT_PATH must be defined by the build"
#endif
#ifndef OBICALL_TEST_RUNTIME_DIR
#error "OBICALL_TEST_RUNTIME_DIR must be defined by the build"
#endif

static int read_whole_file(const char* path, uint8_t** out_data, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0) { fclose(f); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }
    *out_data = buf;
    *out_len = (size_t)size;
    return 0;
}

static int write_whole_file(const char* path, const uint8_t* data, size_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

/* Spawns obicall-workerd against manifest_path and expects it to exit
 * (nonzero) on its own within the timeout, without ever completing the
 * journal handshake - the shape a real integrity-check rejection takes. */
static int expect_worker_refuses(const char* manifest_path, uint16_t journal_port, const uint8_t* token) {
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%u", (unsigned)journal_port);
    char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    procutil_hex_encode(token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));

    const char* argv[] = {OBICALL_WORKERD_PATH,
                           "--manifest", manifest_path,
                           "--journal-port", port_s,
                           "--token", token_hex,
                           "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                           "--worker-name", "test_worker_artifact",
                           "--instance-seed", "1",
                           NULL};
    osal_process_spawn_opts_t opts;
    opts.exe_path = OBICALL_WORKERD_PATH;
    opts.argv = argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    if (osal_process_spawn(&opts, &proc) != 0) return -1;

    int exit_code = -1;
    int r = osal_process_wait(proc, 4000, &exit_code);
    int ok = (r == 1 && exit_code != 0) ? 0 : -1;
    if (r != 1) { osal_process_kill(proc); osal_process_wait(proc, 2000, NULL); }
    osal_process_close(proc);
    return ok;
}

int main(void) {
    osal_net_init();
    osal_mkdir_p(OBICALL_TEST_RUNTIME_DIR);

    uint8_t* artifact_data = NULL;
    size_t artifact_len = 0;
    if (read_whole_file(OBICALL_TEST_PROVIDER_ARTIFACT_PATH, &artifact_data, &artifact_len) != 0) {
        fprintf(stderr, "FAIL: could not read reference artifact at %s\n", OBICALL_TEST_PROVIDER_ARTIFACT_PATH);
        return 1;
    }

    uint8_t digest[OBICALL_DIGEST_LEN];
    obicall_sha256(artifact_data, artifact_len, digest);
    char digest_hex[OBICALL_DIGEST_LEN * 2 + 1];
    procutil_hex_encode(digest, OBICALL_DIGEST_LEN, digest_hex, sizeof(digest_hex));

    char artifact_copy_path[1024];
    snprintf(artifact_copy_path, sizeof(artifact_copy_path), "%s/provider_under_test.dll", OBICALL_TEST_RUNTIME_DIR);
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/artifact_test.manifest.json", OBICALL_TEST_RUNTIME_DIR);

    char manifest_json[2048];
    snprintf(manifest_json, sizeof(manifest_json),
             "{\"schema_version\":1,\"name\":\"provider_under_test\",\"version\":\"0.0.0\",\"language\":\"c\","
             "\"abi_version_major\":1,\"abi_version_minor\":0,\"capability_flags\":[\"position_sensor\"],"
             "\"artifact_path\":\"provider_under_test.dll\",\"artifact_sha256\":\"%s\","
             "\"config_schema_version\":1,\"config\":{},\"dependencies\":[]}",
             digest_hex);
    FILE* mf = fopen(manifest_path, "wb");
    if (!mf) { fprintf(stderr, "FAIL: could not write test manifest\n"); return 1; }
    fwrite(manifest_json, 1, strlen(manifest_json), mf);
    fclose(mf);

    int failures = 0;

    /* Phase 1: unaltered copy must load and connect. */
    if (write_whole_file(artifact_copy_path, artifact_data, artifact_len) != 0) {
        fprintf(stderr, "FAIL: could not write artifact copy\n");
        failures++;
    } else {
        fake_journal_t fj;
        fake_journal_start(&fj);
        char port_s[16];
        snprintf(port_s, sizeof(port_s), "%u", (unsigned)fj.port);
        char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
        procutil_hex_encode(fj.token, OBICALL_RUN_TOKEN_LEN, token_hex, sizeof(token_hex));
        const char* argv[] = {OBICALL_WORKERD_PATH,
                               "--manifest", manifest_path,
                               "--journal-port", port_s,
                               "--token", token_hex,
                               "--runtime-dir", OBICALL_TEST_RUNTIME_DIR,
                               "--worker-name", "test_worker_artifact_ok",
                               "--instance-seed", "1",
                               NULL};
        osal_process_spawn_opts_t opts;
        opts.exe_path = OBICALL_WORKERD_PATH;
        opts.argv = argv;
        opts.envp = NULL;
        osal_process_t* proc = NULL;
        if (osal_process_spawn(&opts, &proc) != 0) {
            fprintf(stderr, "FAIL: spawn failed for unaltered artifact\n");
            failures++;
        } else {
            if (fake_journal_accept_and_authenticate(&fj, 5000) != 0) {
                fprintf(stderr, "FAIL: unaltered artifact did not connect (hash check false positive?)\n");
                failures++;
            } else {
                fprintf(stderr, "ok: unaltered artifact loaded and connected\n");
            }
            osal_process_kill(proc);
            osal_process_wait(proc, 2000, NULL);
            osal_process_close(proc);
        }
        fake_journal_stop(&fj);
    }

    /* Phase 2: flip one byte, same declared hash - must be refused. */
    {
        uint8_t* corrupted = (uint8_t*)malloc(artifact_len);
        memcpy(corrupted, artifact_data, artifact_len);
        corrupted[artifact_len / 2] ^= 0xFF;
        if (write_whole_file(artifact_copy_path, corrupted, artifact_len) != 0) {
            fprintf(stderr, "FAIL: could not write corrupted artifact\n");
            failures++;
        } else {
            fake_journal_t fj;
            fake_journal_start(&fj);
            if (expect_worker_refuses(manifest_path, fj.port, fj.token) != 0) {
                fprintf(stderr, "FAIL: corrupted artifact was not rejected with a nonzero exit\n");
                failures++;
            } else {
                fprintf(stderr, "ok: corrupted artifact rejected (worker exited nonzero, never connected)\n");
            }
            fake_journal_stop(&fj);
        }
        free(corrupted);
    }

    free(artifact_data);
    fprintf(stderr, "%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
