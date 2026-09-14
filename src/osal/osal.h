#ifndef OBICALL_OSAL_H
#define OBICALL_OSAL_H

/*
 * Internal, unexported OS abstraction used only by the process binaries
 * (gate/broker/worker/journal/cli+supervisor) - never by obicall_core,
 * which stays free of process/socket/dynload knowledge so it can be
 * embedded or unit-tested without spawning anything. Not part of the
 * public ABI; not installed.
 */

#include <stdint.h>

/* ---- clock -------------------------------------------------------------*/

int64_t osal_monotonic_ns(void);
int64_t osal_realtime_ns(void);

/* ---- process -------------------------------------------------------------*/

typedef struct osal_process osal_process_t;

typedef struct osal_process_spawn_opts {
    const char* exe_path;
    const char* const* argv; /* NULL-terminated; argv[0] conventionally exe_path */
    const char* const* envp; /* NULL-terminated "KEY=VALUE" list, or NULL to inherit */
} osal_process_spawn_opts_t;

int osal_process_spawn(const osal_process_spawn_opts_t* opts, osal_process_t** out_proc);
long osal_process_pid(const osal_process_t* proc);
int osal_process_is_alive(osal_process_t* proc);
/* timeout_ms < 0 blocks indefinitely. Returns 1 (exited, *out_exit_code
 * set), 0 (timed out, still running), or -1 (error). */
int osal_process_wait(osal_process_t* proc, int timeout_ms, int* out_exit_code);
int osal_process_terminate(osal_process_t* proc);
int osal_process_kill(osal_process_t* proc);
void osal_process_close(osal_process_t* proc);

/* ---- loopback IPC --------------------------------------------------------*/

typedef struct osal_socket osal_socket_t;

int osal_net_init(void);
void osal_net_shutdown(void);

/* *inout_port == 0 asks for an OS-assigned ephemeral port; the actual
 * bound port is written back. Always binds 127.0.0.1, never 0.0.0.0. */
int osal_listen_loopback(uint16_t* inout_port, osal_socket_t** out_listener);
/* Returns 1 (accepted, *out_conn set), 0 (timed out, nothing pending), or
 * -1 (error) - NOTE this is a different convention from osal_recv_frame
 * below; each function's convention is called out at its declaration. */
int osal_accept(osal_socket_t* listener, int timeout_ms, osal_socket_t** out_conn);
int osal_connect_loopback(uint16_t port, int timeout_ms, osal_socket_t** out_conn);

/* One wire frame (see obicall/wire.h) per call; payload_len is bounded by
 * OBICALL_WIRE_MAX_PAYLOAD and every partial-read/partial-write loop is
 * bounded by timeout_ms. Both return 0 on success, -1 on hard error
 * (caller must close the connection, never retry). */
int osal_send_frame(osal_socket_t* s, uint8_t msg_type, const uint8_t* payload, uint32_t payload_len,
                     int timeout_ms);
/* osal_recv_frame additionally returns 1 when timeout_ms elapsed with no
 * frame arriving and nothing was consumed from the stream - that case is
 * always safe to retry (the connection is still good, there was just
 * nothing to read yet), unlike -1. */
int osal_recv_frame(osal_socket_t* s, uint8_t* out_msg_type, uint8_t* payload_buf,
                     uint32_t payload_cap, uint32_t* out_payload_len, int timeout_ms);

void osal_close(osal_socket_t* s);

/* ---- dynamic loading -----------------------------------------------------*/

typedef struct osal_module osal_module_t;

/* path must already be an absolute, validated (integrity-checked) path -
 * this function does no policy enforcement of its own beyond restricting
 * the DLL search path on Windows to safe default directories. */
int osal_dynload_open(const char* path, osal_module_t** out_mod);
void* osal_dynload_symbol(osal_module_t* mod, const char* name);
void osal_dynload_close(osal_module_t* mod);
const char* osal_dynload_last_error(void);

/* ---- synchronization -------------------------------------------------------*/

typedef struct osal_thread osal_thread_t;
typedef void (*osal_thread_fn)(void* arg);

int osal_thread_start(osal_thread_fn fn, void* arg, osal_thread_t** out_thread);
void osal_thread_join(osal_thread_t* thread);
/* Fire-and-forget: the thread runs independently and frees its own OS
 * resources on exit. For per-connection handler threads in a server whose
 * count/lifetime isn't known up front, so there is nothing to join. */
int osal_thread_start_detached(osal_thread_fn fn, void* arg);
void osal_sleep_ms(int ms);

typedef struct osal_mutex osal_mutex_t;

int osal_mutex_init(osal_mutex_t** out);
void osal_mutex_lock(osal_mutex_t* m);
void osal_mutex_unlock(osal_mutex_t* m);
void osal_mutex_destroy(osal_mutex_t* m);

/* ---- durable file updates -------------------------------------------------*/

int osal_file_atomic_replace(const char* tmp_path, const char* final_path);
int osal_mkdir_p(const char* path);
/* Wall-clock modification time in nanoseconds (second resolution on most
 * filesystems - adequate for the multi-second staleness checks this is
 * used for), or -1 if the file does not exist. Not comparable with
 * osal_monotonic_ns(). */
int64_t osal_file_mtime_ns(const char* path);

#endif /* OBICALL_OSAL_H */
