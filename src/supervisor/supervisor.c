#include "supervisor.h"
#include "procutil.h"
#include "obicall/manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define EXE_SUFFIX ".exe"
#else
#define EXE_SUFFIX ""
#endif

static int file_exists_quick(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static char* dup_str(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void set_argv(child_proc_t* c, const char** argv, int argc) {
    c->argc = argc < (int)SUPERVISOR_MAX_ARGV - 1 ? argc : (int)SUPERVISOR_MAX_ARGV - 1;
    for (int i = 0; i < c->argc; ++i) c->argv[i] = dup_str(argv[i]);
    c->argv[c->argc] = NULL;
}

static int spawn_child(supervisor_t* sv, child_proc_t* c) {
    (void)sv;
    osal_process_spawn_opts_t opts;
    opts.exe_path = c->exe_path;
    opts.argv = (const char* const*)c->argv;
    opts.envp = NULL;
    osal_process_t* proc = NULL;
    if (osal_process_spawn(&opts, &proc) != 0) {
        fprintf(stderr, "supervisor: failed to spawn %s (%s)\n", c->name, c->exe_path);
        return -1;
    }
    c->proc = proc;
    c->window_start_ns = osal_monotonic_ns();
    fprintf(stderr, "supervisor: started %s pid=%ld\n", c->name, osal_process_pid(proc));
    return 0;
}

static child_proc_t* add_child(supervisor_t* sv, const char* name, const char* exe_path, const char** argv,
                                int argc, int is_worker) {
    if (sv->child_count >= SUPERVISOR_MAX_CHILDREN) return NULL;
    child_proc_t* c = &sv->children[sv->child_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name) - 1);
    strncpy(c->exe_path, exe_path, sizeof(c->exe_path) - 1);
    c->is_worker = is_worker;
    set_argv(c, argv, argc);
    return c;
}

static void num_to_buf(char* buf, size_t cap, long long v) { snprintf(buf, cap, "%lld", v); }
static void f64_to_buf(char* buf, size_t cap, double v) { snprintf(buf, cap, "%.9g", v); }

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

/* Returns the manifest's obicall_provider_language_t value (0 if the
 * manifest can't be read/parsed - callers treat that as "native", which
 * then fails loudly later at worker load time with a clear error rather
 * than silently here). */
static uint32_t provider_manifest_language(const char* manifest_path) {
    FILE* f = fopen(manifest_path, "rb");
    if (!f) return 0;
    uint8_t buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    obicall_manifest_t m;
    char detail[128];
    if (obicall_manifest_parse_json(buf, (uint32_t)n, &m, detail, sizeof(detail)) != OBICALL_OK) return 0;
    return m.language;
}

static int resolve_python_exe(char* out, size_t cap) {
    const char* env = getenv("OBICALL_PYTHON");
    if (env && file_exists_quick(env)) { strncpy(out, env, cap - 1); out[cap - 1] = '\0'; return 0; }
    if (procutil_find_on_path("python3", out, cap) == 0) return 0;
    if (procutil_find_on_path("python", out, cap) == 0) return 0;
    return -1;
}

int supervisor_start(supervisor_t* sv, const char* config_path, const char* runtime_dir, const char* bin_dir) {
    memset(sv, 0, sizeof(*sv));
    strncpy(sv->runtime_dir, runtime_dir, sizeof(sv->runtime_dir) - 1);
    strncpy(sv->bin_dir, bin_dir, sizeof(sv->bin_dir) - 1);
    osal_mkdir_p(runtime_dir);

    char err[256];
    if (pipeline_config_load(config_path, &sv->cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "supervisor: %s\n", err);
        return -1;
    }

    /* manifests_dir in the config is relative to the config file's own
     * directory, matching `obicall validate` (docs/ABI.md "Manifest
     * schema") - resolve it to an absolute-enough path once here so
     * every worker spawn below (and any CWD the supervisor itself was
     * launched from) agrees on the same location. */
    {
        char config_dir[512];
        dirname_of(config_path, config_dir, sizeof(config_dir));
        char resolved[1024];
        snprintf(resolved, sizeof(resolved), "%s/%s", config_dir, sv->cfg.manifests_dir);
        strncpy(sv->cfg.manifests_dir, resolved, sizeof(sv->cfg.manifests_dir) - 1);
        sv->cfg.manifests_dir[sizeof(sv->cfg.manifests_dir) - 1] = '\0';
    }

    procutil_random_token(sv->run_token);
    procutil_hex_encode(sv->run_token, OBICALL_RUN_TOKEN_LEN, sv->token_hex, sizeof(sv->token_hex));
    procutil_write_token_file(runtime_dir, sv->run_token);
    char config_digest_hex[65];
    procutil_hex_encode(sv->cfg.config_digest, 32, config_digest_hex, sizeof(config_digest_hex));

    snprintf(sv->state_file, sizeof(sv->state_file), "%s/gate.state", runtime_dir);

    char journal_exe[560], gate_exe[560], broker_exe[560], worker_exe[560];
    snprintf(journal_exe, sizeof(journal_exe), "%s/obicall-journald%s", bin_dir, EXE_SUFFIX);
    snprintf(gate_exe, sizeof(gate_exe), "%s/obicall-gated%s", bin_dir, EXE_SUFFIX);
    snprintf(broker_exe, sizeof(broker_exe), "%s/obicall-brokerd%s", bin_dir, EXE_SUFFIX);
    snprintf(worker_exe, sizeof(worker_exe), "%s/obicall-workerd%s", bin_dir, EXE_SUFFIX);

    char segment_file[600];
    snprintf(segment_file, sizeof(segment_file), "%s/journal.segment", runtime_dir);

    char b_reorder[32], b_lateness[32], b_pending[32];
    num_to_buf(b_reorder, sizeof(b_reorder), (long long)sv->cfg.journal_reorder_window);
    num_to_buf(b_lateness, sizeof(b_lateness), (long long)sv->cfg.journal_max_lateness_ms);
    num_to_buf(b_pending, sizeof(b_pending), (long long)sv->cfg.journal_max_pending);
    const char* jargv[] = {journal_exe,
                            "--runtime-dir", runtime_dir,
                            "--token", sv->token_hex,
                            "--pipeline-id", sv->cfg.pipeline_id,
                            "--segment-file", segment_file,
                            "--reorder-window", b_reorder,
                            "--max-lateness-ms", b_lateness,
                            "--max-pending", b_pending};
    procutil_clear_port_file(runtime_dir, "journal");
    child_proc_t* jc = add_child(sv, "journal", journal_exe, jargv, (int)(sizeof(jargv) / sizeof(jargv[0])), 0);
    if (!jc || spawn_child(sv, jc) != 0) return -1;
    if (procutil_read_port_file(runtime_dir, "journal", 5000, &sv->journal_port) != 0) {
        fprintf(stderr, "supervisor: journal did not report a port in time\n");
        return -1;
    }

    char b_validity[32], b_suspect[32], b_confirm[32], b_gap[32];
    num_to_buf(b_validity, sizeof(b_validity), (long long)sv->cfg.gate_result_validity_ms);
    num_to_buf(b_suspect, sizeof(b_suspect), (long long)sv->cfg.gate_suspect_timeout_ms);
    num_to_buf(b_confirm, sizeof(b_confirm), (long long)sv->cfg.gate_confirm_timeout_ms);
    num_to_buf(b_gap, sizeof(b_gap), (long long)sv->cfg.gate_max_replay_gap);
    const char* gargv[] = {gate_exe,
                            "--runtime-dir", runtime_dir,
                            "--token", sv->token_hex,
                            "--config-digest", config_digest_hex,
                            "--state-file", sv->state_file,
                            "--result-validity-ms", b_validity,
                            "--suspect-timeout-ms", b_suspect,
                            "--confirm-timeout-ms", b_confirm,
                            "--max-replay-gap", b_gap};
    procutil_clear_port_file(runtime_dir, "gate");
    child_proc_t* gc = add_child(sv, "gate", gate_exe, gargv, (int)(sizeof(gargv) / sizeof(gargv[0])), 0);
    if (!gc || spawn_child(sv, gc) != 0) return -1;
    if (procutil_read_port_file(runtime_dir, "gate", 5000, &sv->gate_port) != 0) {
        fprintf(stderr, "supervisor: gate did not report a port in time\n");
        return -1;
    }

    char jport_s[16], gport_s[16];
    num_to_buf(jport_s, sizeof(jport_s), sv->journal_port);
    num_to_buf(gport_s, sizeof(gport_s), sv->gate_port);

    char b_pnp[32], b_pnv[32], b_ipv[32], b_ivv[32], b_mpg[32], b_hyst[32];
    f64_to_buf(b_pnp, sizeof(b_pnp), sv->cfg.est_process_noise_position);
    f64_to_buf(b_pnv, sizeof(b_pnv), sv->cfg.est_process_noise_velocity);
    f64_to_buf(b_ipv, sizeof(b_ipv), sv->cfg.est_initial_position_variance);
    f64_to_buf(b_ivv, sizeof(b_ivv), sv->cfg.est_initial_velocity_variance);
    f64_to_buf(b_mpg, sizeof(b_mpg), sv->cfg.est_max_prediction_gap_s);
    f64_to_buf(b_hyst, sizeof(b_hyst), sv->cfg.dgt_hysteresis_margin);

    for (int bi = 0; bi < 2; ++bi) {
        const char* broker_letter = bi == 0 ? "A" : "B";
        char broker_name[32];
        snprintf(broker_name, sizeof(broker_name), "broker_%s", broker_letter);

        const char* bargv_base[] = {broker_exe,
                                     "--broker-id", broker_letter,
                                     "--journal-port", jport_s,
                                     "--gate-port", gport_s,
                                     "--token", sv->token_hex,
                                     "--pipeline-id", sv->cfg.pipeline_id,
                                     "--runtime-dir", runtime_dir,
                                     "--config-digest", config_digest_hex,
                                     "--sensor-ids", sv->cfg.sensor_ids_csv,
                                     "--process-noise-position", b_pnp,
                                     "--process-noise-velocity", b_pnv,
                                     "--initial-position-variance", b_ipv,
                                     "--initial-velocity-variance", b_ivv,
                                     "--max-prediction-gap-s", b_mpg,
                                     "--dgt-hysteresis-margin", b_hyst};
        int base_argc = (int)(sizeof(bargv_base) / sizeof(bargv_base[0]));
        const char* bargv[SUPERVISOR_MAX_ARGV];
        int bargc = 0;
        for (; bargc < base_argc; ++bargc) bargv[bargc] = bargv_base[bargc];
        if (sv->cfg.dgt_enabled) bargv[bargc++] = "--dgt-enabled";

        child_proc_t* bc = add_child(sv, broker_name, broker_exe, bargv, bargc, 0);
        if (!bc || spawn_child(sv, bc) != 0) return -1;

        for (uint32_t p = 0; p < sv->cfg.provider_count; ++p) {
            provider_ref_t* pr = &sv->cfg.providers[p];
            char manifest_path[1200];
            int mp_written = snprintf(manifest_path, sizeof(manifest_path), "%s/%s", sv->cfg.manifests_dir, pr->manifest);
            if (mp_written < 0 || (size_t)mp_written >= sizeof(manifest_path)) {
                fprintf(stderr, "supervisor: manifest path too long for %s\n", pr->manifest);
                return -1;
            }
            char worker_name[96];
            snprintf(worker_name, sizeof(worker_name), "%s_%s", pr->worker_name, broker_letter);
            char seed_s[32];
            num_to_buf(seed_s, sizeof(seed_s), pr->instance_seed + (bi == 1 ? 1000000LL : 0));

            uint32_t language = provider_manifest_language(manifest_path);

            child_proc_t* wc;
            if (language == 2 /* OBICALL_LANG_PYTHON */) {
                char python_exe[600];
                if (resolve_python_exe(python_exe, sizeof(python_exe)) != 0) {
                    fprintf(stderr, "supervisor: provider %s needs Python but no interpreter was found"
                                     " (set OBICALL_PYTHON or put python3 on PATH)\n",
                            pr->manifest);
                    return -1;
                }
                char script_path[900];
#if defined(OBICALL_SOURCE_DIR)
                snprintf(script_path, sizeof(script_path), "%s/python/obicall/provider_worker.py", OBICALL_SOURCE_DIR);
#else
                snprintf(script_path, sizeof(script_path), "%s/../python/obicall/provider_worker.py", bin_dir);
#endif
                char core_lib_path[700];
#if defined(_WIN32)
                snprintf(core_lib_path, sizeof(core_lib_path), "%s/obicall.dll", bin_dir);
#elif defined(__APPLE__)
                snprintf(core_lib_path, sizeof(core_lib_path), "%s/libobicall.dylib", bin_dir);
#else
                snprintf(core_lib_path, sizeof(core_lib_path), "%s/libobicall.so", bin_dir);
#endif
                const char* wargv[] = {python_exe,
                                        script_path,
                                        "--manifest", manifest_path,
                                        "--journal-port", jport_s,
                                        "--token", sv->token_hex,
                                        "--runtime-dir", runtime_dir,
                                        "--worker-name", worker_name,
                                        "--instance-seed", seed_s,
                                        "--core-lib", core_lib_path};
                wc = add_child(sv, worker_name, python_exe, wargv, (int)(sizeof(wargv) / sizeof(wargv[0])), 1);
            } else {
                const char* wargv[] = {worker_exe,
                                        "--manifest", manifest_path,
                                        "--journal-port", jport_s,
                                        "--token", sv->token_hex,
                                        "--runtime-dir", runtime_dir,
                                        "--worker-name", worker_name,
                                        "--instance-seed", seed_s};
                wc = add_child(sv, worker_name, worker_exe, wargv, (int)(sizeof(wargv) / sizeof(wargv[0])), 1);
            }
            if (!wc || spawn_child(sv, wc) != 0) return -1;
        }
    }

    sv->started_at_ns = osal_monotonic_ns();
    return 0;
}

void supervisor_monitor_tick(supervisor_t* sv) {
    int64_t now = osal_monotonic_ns();
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (!c->proc) continue;

        int alive = osal_process_is_alive(c->proc);
        int hung = 0;
        /* Grace period: a worker's process-startup + journal-connect can
         * legitimately take a couple of seconds (longer for a Python
         * interpreter), during which it has not written its first
         * heartbeat yet. Only judge staleness once it has had time to. */
        int past_grace = (now - c->window_start_ns) > 4000000000LL;
        if (alive && c->is_worker && past_grace) {
            char path[700];
            snprintf(path, sizeof(path), "%s/%s.pid", sv->runtime_dir, c->name);
            int64_t mtime = osal_file_mtime_ns(path);
            /* Wall-clock mtime vs monotonic "now" are different clocks;
             * this only needs a coarse "has it been touched recently"
             * signal, and both advance at the same rate on a live
             * system, so comparing their deltas since start is adequate
             * here (see docs/FAULT_TOLERANCE.md). mtime < 0 (file still
             * doesn't exist even past the grace period) counts as hung
             * too - a worker that never got as far as its first
             * heartbeat is exactly what this check exists to catch. */
            if (mtime < 0 || (osal_realtime_ns() - mtime) > 5000000000LL) hung = 1;
        }

        if (alive && !hung) continue;

        if (hung) {
            fprintf(stderr, "supervisor: %s appears hung (stale heartbeat) - killing\n", c->name);
            osal_process_kill(c->proc);
            osal_process_wait(c->proc, 2000, NULL);
        } else {
            int exit_code = -1;
            int wr = osal_process_wait(c->proc, 500, &exit_code);
            if (wr == 1) fprintf(stderr, "supervisor: %s exited (code=%d / 0x%08X)\n", c->name, exit_code,
                                  (unsigned)exit_code);
            else fprintf(stderr, "supervisor: %s exited (exit code unavailable, wait rc=%d)\n", c->name, wr);
        }
        osal_process_close(c->proc);
        c->proc = NULL;

        if (c->restart_suspended) {
            fprintf(stderr, "supervisor: %s restart suspended (fault-injection hold) - leaving it stopped\n", c->name);
            continue;
        }

        if (now - c->window_start_ns > 60000000000LL) {
            c->restart_count = 0;
            c->window_start_ns = now;
        }
        if (c->restart_count >= 5) {
            fprintf(stderr, "supervisor: %s exceeded restart bound (5/60s) - leaving it stopped\n", c->name);
            continue;
        }
        c->restart_count++;
        fprintf(stderr, "supervisor: restarting %s (attempt %d)\n", c->name, c->restart_count);
        spawn_child(sv, c);
    }
}

void supervisor_stop_all(supervisor_t* sv) {
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (c->proc) {
            osal_process_terminate(c->proc);
        }
    }
    osal_sleep_ms(200);
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (c->proc) {
            osal_process_kill(c->proc);
            osal_process_wait(c->proc, 2000, NULL);
            osal_process_close(c->proc);
            c->proc = NULL;
        }
    }
}

int supervisor_kill_child(supervisor_t* sv, const char* name) {
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (strcmp(c->name, name) == 0 && c->proc) {
            osal_process_kill(c->proc);
            return 0;
        }
    }
    return -1;
}

int supervisor_kill_child_and_suspend(supervisor_t* sv, const char* name) {
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (strcmp(c->name, name) == 0 && c->proc) {
            c->restart_suspended = 1;
            osal_process_kill(c->proc);
            return 0;
        }
    }
    return -1;
}

void supervisor_resume_child(supervisor_t* sv, const char* name) {
    for (uint32_t i = 0; i < sv->child_count; ++i) {
        child_proc_t* c = &sv->children[i];
        if (strcmp(c->name, name) == 0) {
            c->restart_suspended = 0;
            c->restart_count = 0;
            c->window_start_ns = osal_monotonic_ns();
            return;
        }
    }
}
