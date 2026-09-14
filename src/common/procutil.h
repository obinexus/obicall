#ifndef OBICALL_PROCUTIL_H
#define OBICALL_PROCUTIL_H

/* Shared helpers for the process binaries (gate/journal/broker/worker/
 * supervisor/cli): argv parsing, the run-token handshake every IPC
 * connection performs before anything else, and the runtime-dir port
 * files children use to discover each other's listening port. Not used
 * by obicall_core. */

#include <stdint.h>
#include "osal.h"
#include "obicall/wire.h"

void procutil_random_token(uint8_t out[OBICALL_RUN_TOKEN_LEN]);
int procutil_hex_encode(const uint8_t* data, uint32_t len, char* out, uint32_t out_cap);
int procutil_hex_decode(const char* hex, uint32_t hex_len, uint8_t* out, uint32_t out_len);

int procutil_write_port_file(const char* runtime_dir, const char* name, uint16_t port);
/* Polls (bounded by timeout_ms) for the file to appear - the writer and
 * reader are separate, racing processes. */
int procutil_read_port_file(const char* runtime_dir, const char* name, int timeout_ms,
                             uint16_t* out_port);

int procutil_write_pid_file(const char* runtime_dir, const char* name, long pid);
long procutil_current_pid(void);
/* Deletes runtime_dir/name.port if present - call before spawning a
 * server child so procutil_read_port_file can never read a port left
 * over from an earlier run using the same runtime directory. */
void procutil_clear_port_file(const char* runtime_dir, const char* name);

/* Persists the run token so a later, separate CLI invocation (status,
 * demo cleanup, etc.) can authenticate to an already-running instance's
 * sockets without the caller having to pass it on the command line. Only
 * as protected as the runtime directory itself - see docs/ABI.md "Trust
 * model" for why that is an adequate boundary for a local-user prototype. */
int procutil_write_token_file(const char* runtime_dir, uint8_t token[OBICALL_RUN_TOKEN_LEN]);
int procutil_read_token_file(const char* runtime_dir, uint8_t out_token[OBICALL_RUN_TOKEN_LEN]);

/* Directory containing the currently-running executable, so child
 * binaries (co-located per the build's single output directory) can be
 * found without relying on PATH or CWD. Falls back to argv0's own
 * directory (or ".") if the platform-specific self-path query fails. */
int procutil_own_exe_dir(const char* argv0, char* out, size_t out_cap);

/* Searches the PATH environment variable's directories for name (trying
 * name and, on Windows, name.exe in each), since osal_process_spawn's
 * exe_path is passed directly to CreateProcess/execve and neither of
 * those searches PATH the way a shell does. Returns 0 and fills out with
 * a full path on the first match, -1 if not found anywhere on PATH. */
int procutil_find_on_path(const char* name, char* out, size_t out_cap);

/* Connects, sends AUTH_HELLO, and returns the live connection. Does not
 * wait for a reply - the server either proceeds or silently closes the
 * socket on mismatch (see procutil_server_verify_hello). */
int procutil_client_connect(uint16_t port, const uint8_t token[OBICALL_RUN_TOKEN_LEN],
                             obicall_wire_role_t role, int timeout_ms, osal_socket_t** out_conn);

/* Server side of the handshake: reads one frame, requires it to be
 * AUTH_HELLO with a matching token, and reports the peer's declared role.
 * Returns 0 on success, -1 on any mismatch/timeout/error (caller should
 * close the connection either way without further diagnostics, since this
 * is a local control-plane authentication check, not a place to leak
 * detail to an unauthenticated peer). */
int procutil_server_verify_hello(osal_socket_t* conn, const uint8_t token[OBICALL_RUN_TOKEN_LEN],
                                  int timeout_ms, obicall_wire_role_t* out_role);

const char* procutil_arg_str(int argc, char** argv, const char* name, const char* def);
int64_t procutil_arg_i64(int argc, char** argv, const char* name, int64_t def);
double procutil_arg_f64(int argc, char** argv, const char* name, double def);
int procutil_arg_flag(int argc, char** argv, const char* name);

#endif /* OBICALL_PROCUTIL_H */
