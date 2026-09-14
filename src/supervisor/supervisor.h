#ifndef OBICALL_SUPERVISOR_H
#define OBICALL_SUPERVISOR_H

#include <stdint.h>
#include "osal.h"
#include "pipeline_config.h"
#include "obicall/wire.h"

#define SUPERVISOR_MAX_CHILDREN 32u
#define SUPERVISOR_MAX_ARGV 40u

typedef struct child_proc {
    char name[64];
    char exe_path[512];
    char* argv[SUPERVISOR_MAX_ARGV];
    int argc;
    osal_process_t* proc;
    int restart_count;
    int64_t window_start_ns;
    int is_worker; /* workers are heartbeat-file-monitored for hangs too */
    int restart_suspended; /* set by supervisor_kill_child_and_suspend */
} child_proc_t;

typedef struct supervisor {
    char runtime_dir[512];
    char bin_dir[512];
    uint8_t run_token[OBICALL_RUN_TOKEN_LEN];
    char token_hex[OBICALL_RUN_TOKEN_LEN * 2 + 1];
    pipeline_config_t cfg;
    uint16_t journal_port;
    uint16_t gate_port;
    char state_file[600];
    child_proc_t children[SUPERVISOR_MAX_CHILDREN];
    uint32_t child_count;
    int64_t started_at_ns;
} supervisor_t;

int supervisor_start(supervisor_t* sv, const char* config_path, const char* runtime_dir, const char* bin_dir);

/* Restarts any dead child within a bounded retry rate (5 restarts per 60s
 * window per child); a child that exceeds that is left dead and logged -
 * see docs/FAULT_TOLERANCE.md. Call periodically from the owning CLI
 * command's loop. */
void supervisor_monitor_tick(supervisor_t* sv);

void supervisor_stop_all(supervisor_t* sv);

/* Test/demo hook: forcibly kill a named child (e.g. "broker_A") so its
 * disappearance can be observed by supervisor_monitor_tick and by the
 * gate's promotion logic. Returns 0 if found and killed. */
int supervisor_kill_child(supervisor_t* sv, const char* name);

/* Like supervisor_kill_child, but also marks the child so
 * supervisor_monitor_tick will not auto-restart it - without this, the
 * bounded-retry restart (by design, fast: well under a gate's
 * confirm-timeout) can bring the same broker_id back before the gate
 * ever sees a heartbeat gap long enough to promote the other broker,
 * which defeats the point of injecting the failure. Call
 * supervisor_resume_child once the fault scenario being demonstrated no
 * longer needs the child to stay down. */
int supervisor_kill_child_and_suspend(supervisor_t* sv, const char* name);
void supervisor_resume_child(supervisor_t* sv, const char* name);

#endif /* OBICALL_SUPERVISOR_H */
