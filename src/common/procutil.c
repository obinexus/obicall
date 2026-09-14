#include "procutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

void procutil_random_token(uint8_t out[OBICALL_RUN_TOKEN_LEN]) {
#if defined(_WIN32)
    if (BCryptGenRandom(NULL, out, OBICALL_RUN_TOKEN_LEN, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
        return;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < OBICALL_RUN_TOKEN_LEN) {
            ssize_t n = read(fd, out + got, OBICALL_RUN_TOKEN_LEN - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);
        if (got == OBICALL_RUN_TOKEN_LEN) return;
    }
#endif
    /* Fallback if the platform CSPRNG is unavailable - still process/time
     * seeded so distinct runs don't collide, but weaker than the above. */
    srand((unsigned)(osal_realtime_ns() ^ (int64_t)(intptr_t)out));
    for (uint32_t i = 0; i < OBICALL_RUN_TOKEN_LEN; ++i) out[i] = (uint8_t)rand();
}

int procutil_hex_encode(const uint8_t* data, uint32_t len, char* out, uint32_t out_cap) {
    static const char hexchars[] = "0123456789abcdef";
    if (out_cap < len * 2u + 1u) return -1;
    for (uint32_t i = 0; i < len; ++i) {
        out[i * 2] = hexchars[data[i] >> 4];
        out[i * 2 + 1] = hexchars[data[i] & 0xF];
    }
    out[len * 2] = '\0';
    return 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int procutil_hex_decode(const char* hex, uint32_t hex_len, uint8_t* out, uint32_t out_len) {
    if (hex_len != out_len * 2u) return -1;
    for (uint32_t i = 0; i < out_len; ++i) {
        int hi = hex_val(hex[i * 2]), lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int port_file_path(const char* runtime_dir, const char* name, char* out, size_t out_cap) {
    int n = snprintf(out, out_cap, "%s/%s.port", runtime_dir, name);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

int procutil_write_port_file(const char* runtime_dir, const char* name, uint16_t port) {
    char path[1024];
    if (port_file_path(runtime_dir, name, path, sizeof(path)) != 0) return -1;
    char tmp[1040];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "%u", (unsigned)port);
    fclose(f);
    return osal_file_atomic_replace(tmp, path);
}

void procutil_clear_port_file(const char* runtime_dir, const char* name) {
    char path[1024];
    if (port_file_path(runtime_dir, name, path, sizeof(path)) != 0) return;
    remove(path);
}

int procutil_read_port_file(const char* runtime_dir, const char* name, int timeout_ms,
                             uint16_t* out_port) {
    char path[1024];
    if (port_file_path(runtime_dir, name, path, sizeof(path)) != 0) return -1;
    int waited = 0;
    const int step = 20;
    for (;;) {
        FILE* f = fopen(path, "rb");
        if (f) {
            unsigned v = 0;
            int got = fscanf(f, "%u", &v);
            fclose(f);
            if (got == 1) {
                *out_port = (uint16_t)v;
                return 0;
            }
        }
        if (waited >= timeout_ms) return -1;
        osal_sleep_ms(step);
        waited += step;
    }
}

int procutil_write_pid_file(const char* runtime_dir, const char* name, long pid) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.pid", runtime_dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;
    char tmp[1040];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    fprintf(f, "%ld", pid);
    fclose(f);
    return osal_file_atomic_replace(tmp, path);
}

int procutil_write_token_file(const char* runtime_dir, uint8_t token[OBICALL_RUN_TOKEN_LEN]) {
    char hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    procutil_hex_encode(token, OBICALL_RUN_TOKEN_LEN, hex, sizeof(hex));
    char path[1024];
    snprintf(path, sizeof(path), "%s/run.token", runtime_dir);
    char tmp[1040];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    fputs(hex, f);
    fclose(f);
    return osal_file_atomic_replace(tmp, path);
}

int procutil_read_token_file(const char* runtime_dir, uint8_t out_token[OBICALL_RUN_TOKEN_LEN]) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/run.token", runtime_dir);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    size_t n = fread(hex, 1, OBICALL_RUN_TOKEN_LEN * 2, f);
    fclose(f);
    if (n != OBICALL_RUN_TOKEN_LEN * 2) return -1;
    hex[n] = '\0';
    return procutil_hex_decode(hex, (uint32_t)n, out_token, OBICALL_RUN_TOKEN_LEN);
}

int procutil_own_exe_dir(const char* argv0, char* out, size_t out_cap) {
#if defined(_WIN32)
    char path[1024];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (n > 0 && n < sizeof(path)) {
        char* slash = strrchr(path, '\\');
        if (!slash) slash = strrchr(path, '/');
        if (slash) {
            size_t len = (size_t)(slash - path);
            if (len < out_cap) { memcpy(out, path, len); out[len] = '\0'; return 0; }
        }
    }
#else
    char path[1024];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = '\0';
        char* slash = strrchr(path, '/');
        if (slash) {
            size_t len = (size_t)(slash - path);
            if (len < out_cap) { memcpy(out, path, len); out[len] = '\0'; return 0; }
        }
    }
#endif
    if (argv0) {
        const char* slash = strrchr(argv0, '/');
        const char* bslash = strrchr(argv0, '\\');
        const char* last = slash > bslash ? slash : bslash;
        if (last) {
            size_t len = (size_t)(last - argv0);
            if (len < out_cap) { memcpy(out, argv0, len); out[len] = '\0'; return 0; }
        }
    }
    if (out_cap > 1) { out[0] = '.'; out[1] = '\0'; return 0; }
    return -1;
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int procutil_find_on_path(const char* name, char* out, size_t out_cap) {
    const char* path_env = getenv("PATH");
    if (!path_env) return -1;
#if defined(_WIN32)
    const char* sep = ";";
#else
    const char* sep = ":";
#endif
    char dirs[8192];
    strncpy(dirs, path_env, sizeof(dirs) - 1);
    dirs[sizeof(dirs) - 1] = '\0';

    char* tok = strtok(dirs, sep);
    while (tok) {
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/%s", tok, name);
        if (file_exists(candidate)) {
            strncpy(out, candidate, out_cap - 1);
            out[out_cap - 1] = '\0';
            return 0;
        }
#if defined(_WIN32)
        snprintf(candidate, sizeof(candidate), "%s/%s.exe", tok, name);
        if (file_exists(candidate)) {
            strncpy(out, candidate, out_cap - 1);
            out[out_cap - 1] = '\0';
            return 0;
        }
#endif
        tok = strtok(NULL, sep);
    }
    return -1;
}

long procutil_current_pid(void) {
#if defined(_WIN32)
    return (long)GetCurrentProcessId();
#else
    return (long)getpid();
#endif
}

int procutil_client_connect(uint16_t port, const uint8_t token[OBICALL_RUN_TOKEN_LEN],
                             obicall_wire_role_t role, int timeout_ms, osal_socket_t** out_conn) {
    osal_socket_t* conn = NULL;
    if (osal_connect_loopback(port, timeout_ms, &conn) != 0) return -1;

    obicall_msg_auth_hello_t hello;
    hello.protocol_version = OBICALL_WIRE_VERSION;
    hello.role = (uint32_t)role;
    memcpy(hello.run_token, token, OBICALL_RUN_TOKEN_LEN);

    uint8_t buf[64];
    uint32_t len = 0;
    if (obicall_wire_encode_auth_hello(&hello, buf, sizeof(buf), &len) != OBICALL_OK) {
        osal_close(conn);
        return -1;
    }
    if (osal_send_frame(conn, OBICALL_MSG_AUTH_HELLO, buf, len, timeout_ms) != 0) {
        osal_close(conn);
        return -1;
    }
    *out_conn = conn;
    return 0;
}

int procutil_server_verify_hello(osal_socket_t* conn, const uint8_t token[OBICALL_RUN_TOKEN_LEN],
                                  int timeout_ms, obicall_wire_role_t* out_role) {
    uint8_t msg_type = 0;
    uint8_t payload[256];
    uint32_t payload_len = 0;
    if (osal_recv_frame(conn, &msg_type, payload, sizeof(payload), &payload_len, timeout_ms) != 0) {
        return -1;
    }
    if (msg_type != OBICALL_MSG_AUTH_HELLO) return -1;
    obicall_msg_auth_hello_t hello;
    if (obicall_wire_decode_auth_hello(payload, payload_len, &hello) != OBICALL_OK) return -1;
    if (hello.protocol_version != OBICALL_WIRE_VERSION) return -1;
    if (memcmp(hello.run_token, token, OBICALL_RUN_TOKEN_LEN) != 0) return -1;
    if (out_role) *out_role = (obicall_wire_role_t)hello.role;
    return 0;
}

static const char* find_arg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return NULL;
}

const char* procutil_arg_str(int argc, char** argv, const char* name, const char* def) {
    const char* v = find_arg(argc, argv, name);
    return v ? v : def;
}

int64_t procutil_arg_i64(int argc, char** argv, const char* name, int64_t def) {
    const char* v = find_arg(argc, argv, name);
    return v ? (int64_t)strtoll(v, NULL, 10) : def;
}

double procutil_arg_f64(int argc, char** argv, const char* name, double def) {
    const char* v = find_arg(argc, argv, name);
    return v ? strtod(v, NULL) : def;
}

int procutil_arg_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], name) == 0) return 1;
    }
    return 0;
}
