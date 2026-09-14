#include "osal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)

#include <windows.h>

struct osal_process {
    HANDLE handle;
    DWORD pid;
};

/* All args are internally generated (paths, decimal ids, hex tokens) and
 * never contain '"' - unconditionally quoting each one is therefore
 * correct here without needing the full MSVCRT unescaping algorithm. */
static int build_command_line(char* out, size_t cap, const char* const* argv) {
    size_t pos = 0;
    for (int i = 0; argv[i] != NULL; ++i) {
        if (i > 0) {
            if (pos + 1 >= cap) return 0;
            out[pos++] = ' ';
        }
        if (pos + 1 >= cap) return 0;
        out[pos++] = '"';
        for (const char* s = argv[i]; *s; ++s) {
            if (pos + 1 >= cap) return 0;
            out[pos++] = *s;
        }
        if (pos + 1 >= cap) return 0;
        out[pos++] = '"';
    }
    if (pos >= cap) return 0;
    out[pos] = '\0';
    return 1;
}

static char* build_env_block(const char* const* envp) {
    if (!envp) return NULL;
    size_t total = 1; /* final extra NUL */
    for (int i = 0; envp[i]; ++i) total += strlen(envp[i]) + 1;
    char* block = (char*)malloc(total);
    if (!block) return NULL;
    size_t pos = 0;
    for (int i = 0; envp[i]; ++i) {
        size_t l = strlen(envp[i]) + 1;
        memcpy(block + pos, envp[i], l);
        pos += l;
    }
    block[pos] = '\0';
    return block;
}

int osal_process_spawn(const osal_process_spawn_opts_t* opts, osal_process_t** out_proc) {
    char cmdline[8192];
    if (!build_command_line(cmdline, sizeof(cmdline), opts->argv)) return -1;
    char* env_block = build_env_block(opts->envp);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessA(opts->exe_path, cmdline, NULL, NULL, FALSE, 0, env_block, NULL, &si, &pi);
    if (env_block) free(env_block);
    if (!ok) return -1;
    CloseHandle(pi.hThread);

    osal_process_t* p = (osal_process_t*)malloc(sizeof(*p));
    if (!p) { CloseHandle(pi.hProcess); return -1; }
    p->handle = pi.hProcess;
    p->pid = pi.dwProcessId;
    *out_proc = p;
    return 0;
}

long osal_process_pid(const osal_process_t* proc) { return (long)proc->pid; }

int osal_process_is_alive(osal_process_t* proc) {
    DWORD code = 0;
    if (!GetExitCodeProcess(proc->handle, &code)) return 0;
    return code == STILL_ACTIVE;
}

int osal_process_wait(osal_process_t* proc, int timeout_ms, int* out_exit_code) {
    DWORD r = WaitForSingleObject(proc->handle, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
    if (r == WAIT_TIMEOUT) return 0;
    if (r != WAIT_OBJECT_0) return -1;
    DWORD code = 0;
    GetExitCodeProcess(proc->handle, &code);
    if (out_exit_code) *out_exit_code = (int)code;
    return 1;
}

/* Windows has no portable graceful-stop signal equivalent to SIGTERM;
 * both terminate and kill are hard stops here. Graceful shutdown is
 * expected to go through OBICALL_MSG_SHUTDOWN over IPC first - see
 * src/supervisor. */
int osal_process_terminate(osal_process_t* proc) { return TerminateProcess(proc->handle, 1) ? 0 : -1; }
int osal_process_kill(osal_process_t* proc) { return TerminateProcess(proc->handle, 9) ? 0 : -1; }

void osal_process_close(osal_process_t* proc) {
    if (!proc) return;
    if (proc->handle) CloseHandle(proc->handle);
    free(proc);
}

#else /* POSIX */

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

extern char** environ;

struct osal_process {
    pid_t pid;
    int reaped;
    int exit_code;
};

int osal_process_spawn(const osal_process_spawn_opts_t* opts, osal_process_t** out_proc) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char* const* argv = (char* const*)(uintptr_t)opts->argv;
        char* const* envp = opts->envp ? (char* const*)(uintptr_t)opts->envp : environ;
        execve(opts->exe_path, argv, envp);
        _exit(127);
    }
    osal_process_t* p = (osal_process_t*)malloc(sizeof(*p));
    if (!p) return -1;
    p->pid = pid;
    p->reaped = 0;
    p->exit_code = -1;
    *out_proc = p;
    return 0;
}

long osal_process_pid(const osal_process_t* proc) { return (long)proc->pid; }

static int reap_nonblocking(osal_process_t* proc) {
    if (proc->reaped) return 1;
    int status = 0;
    pid_t r = waitpid(proc->pid, &status, WNOHANG);
    if (r == proc->pid) {
        proc->reaped = 1;
        proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (256 + WTERMSIG(status));
        return 1;
    }
    return 0;
}

int osal_process_is_alive(osal_process_t* proc) { return !reap_nonblocking(proc); }

int osal_process_wait(osal_process_t* proc, int timeout_ms, int* out_exit_code) {
    if (timeout_ms < 0) {
        int status = 0;
        pid_t r = waitpid(proc->pid, &status, proc->reaped ? WNOHANG : 0);
        if (proc->reaped || r == proc->pid) {
            if (!proc->reaped) {
                proc->reaped = 1;
                proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (256 + WTERMSIG(status));
            }
            if (out_exit_code) *out_exit_code = proc->exit_code;
            return 1;
        }
        return -1;
    }
    int waited_ms = 0;
    const int step_ms = 10;
    while (waited_ms <= timeout_ms) {
        if (reap_nonblocking(proc)) {
            if (out_exit_code) *out_exit_code = proc->exit_code;
            return 1;
        }
        struct timespec ts = {0, step_ms * 1000000L};
        nanosleep(&ts, NULL);
        waited_ms += step_ms;
    }
    return 0;
}

int osal_process_terminate(osal_process_t* proc) { return kill(proc->pid, SIGTERM) == 0 ? 0 : -1; }
int osal_process_kill(osal_process_t* proc) { return kill(proc->pid, SIGKILL) == 0 ? 0 : -1; }

void osal_process_close(osal_process_t* proc) {
    if (!proc) return;
    if (!proc->reaped) {
        int status;
        waitpid(proc->pid, &status, WNOHANG);
    }
    free(proc);
}

#endif
