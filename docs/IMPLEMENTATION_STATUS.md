# Implementation status

This is the authoritative requirement-to-code/test mapping for Obicall. Status
values: **Implemented** (code exists and is exercised by at least one
automated test), **Partial** (code exists but coverage or scope is narrower
than the full requirement), **Deferred** (explicitly out of scope for this
prototype, per the brief's own "later extensions" carve-out), **Not tested**
(code exists, believed correct, but no automated test in this environment).

Nothing in this document is aspirational — every "Implemented" row has a
corresponding source file and a corresponding test that passes in this
repository's own `ctest` run (see [VALIDATION.md](VALIDATION.md) for the exact
commands and output).

## Core and C ABI

| Requirement | Status | Where |
|---|---|---|
| C11 core, public C ABI headers | Implemented | `include/obicall/*.h` |
| Opaque handles, fixed-width status codes | Implemented | `include/obicall/types.h`, `status.h` |
| Versioned descriptors (ABI version, struct_size, capabilities, schema versions) | Implemented | `include/obicall/plugin.h` (`obicall_descriptor_t`) |
| `obicall_plugin_query_v1` + versioned function table | Implemented | `include/obicall/plugin.h`; loaded in `src/worker/worker_main.c` |
| Explicit calling convention, export macros | Implemented | `include/obicall/platform.h` (`OBICALL_CALL`, `OBICALL_EXPORT`), generated `obicall_export.h` |
| Pointer+length buffers with documented ownership/release | Implemented | `obicall_buffer_t`/`obicall_owned_buffer_t` (types.h); `release_buffer` in plugin vtable |
| Borrowed vs. copied/retained inputs | Implemented | Observation struct is borrowed for one call; provider events (`obicall_event_t`) are read only during the `poll_events` call |
| Bounded reentrancy | Implemented by construction | `poll_events` is synchronous and pull-based (drains once per call), not an async push callback — see `docs/ABI.md` |
| No pointers/native struct layout over IPC; versioned wire encoding | Implemented | `include/obicall/wire.h`, `src/core/wire.c` — explicit little-endian field packing via `src/core/wire_cursor.h`, never `memcpy` of a struct |
| Byte order, framing, size limits, integer-overflow checks | Implemented | `OBICALL_WIRE_MAX_PAYLOAD` bound checked in every decoder; `tests/unit/test_wire.c`, `tests/fuzz/fuzz_wire_decode.c` |
| Separate ABI / wire / checkpoint / config versions | Implemented | `OBICALL_ABI_VERSION_MAJOR/MINOR`, `OBICALL_WIRE_VERSION`, `OBICALL_CHECKPOINT_SCHEMA_VERSION`, per-manifest `config_schema_version` |
| Python: explicit ctypes argtypes/restype, real invocation | Implemented | `python/obicall/abi.py`, exercised by `python/obicall/provider_worker.py`; `tests/integration/test_python_abi_roundtrip.c` |
| Python: preserved callback lifetime | Implemented | `abi.ObservationValidator` holds its `CFUNCTYPE` for the object's lifetime, not a call-site temporary |
| libffi / arbitrary dynamic signatures | Deferred | Fixed function tables only, as the brief specifies for this phase |
| Node-API adapter | Deferred | Explicitly a later extension in the brief |

## Dynamic discovery and loading

| Requirement | Status | Where |
|---|---|---|
| Provider manifest (capabilities, ABI, arch, digests, config schema) | Implemented | `include/obicall/manifest.h`, `src/core/manifest.c`; examples in `examples/providers/*.manifest.json` |
| Full mandatory-dependency graph resolution (topological, not shortest-path) | Implemented | `obicall_manifest_resolve` (Kahn's algorithm) — deliberately fixes the shortest-path flaw present in the `dynamic-cabi-loader` reference; `tests/unit/test_manifest.c::test_resolve_requires_all_mandatory_dependencies_not_just_one_path` |
| Cycle rejection | Implemented | `tests/unit/test_manifest.c::test_resolve_detects_cycle` |
| Alternatives ranked only after eligibility (arch/ABI/dependency resolution) passes | Implemented | `obicall_manifest_rank_alternatives`; `tests/unit/test_manifest.c` (architecture/ABI/dependency exclusion tests) |
| `dlopen`/`dlsym` (POSIX), `LoadLibraryExW`/`GetProcAddress` (Windows) | Implemented | `src/osal/osal_dynload.c` |
| Restricted search path | Implemented | `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS \| LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` (never CWD/PATH) on Windows; `RTLD_NOW \| RTLD_LOCAL` on POSIX |
| Artifact integrity check (SHA-256) before load | Implemented | `src/worker/worker_main.c` (hash check before `osal_dynload_open`); `tests/integration/test_altered_artifact.c` |
| Hash is not publisher authentication (documented limitation) | Implemented (as documentation) | `docs/ABI.md` "Trust model" |
| ABI/struct_size/architecture validation, not just symbol presence | Implemented | `src/worker/worker_main.c` descriptor checks; `tests/integration/test_incompatible_abi.c` |
| Worker replacement (start, validate, restore, switch, drain, terminate) | Partial | Supervisor can start a fresh worker process for a manifest (`src/supervisor/supervisor.c`); journal-level session continuity across a worker restart is real and tested indirectly, but there is no live checkpoint hand-off between an old and a new worker process — see "Deferred / narrower than the brief" below |

## Publication gate and fault tolerance

| Requirement | Status | Where |
|---|---|---|
| Gate is sole commit authority, not a third broker, doesn't run fusion | Implemented | `src/core/gate.c` (pure decision logic), `src/gate/gate_main.c` (process) |
| Only current owner+epoch may publish | Implemented | `obicall_gate_decide_publish`; `tests/unit/test_gate.c` |
| At most one commit per pipeline/window | Implemented | duplicate/stale window rejection; `test_duplicate_window_rejected`, and process-level in `tests/integration/test_gate_restart.c` (duplicate rejected **after a real restart**) |
| Stale/expired/incompatible/duplicate rejected | Implemented | `OBICALL_GATE_REJECT_*` outcomes, all unit-tested |
| Heartbeat timeout marks suspicion only, never self-promotion | Implemented | All promotion decisions happen inside `gate_main.c`'s `evaluate_promotion_locked`, triggered only by the gate's own receipt of a heartbeat — brokers have no code path to assume ownership themselves |
| Promotion revokes old epoch before granting new | Implemented | `obicall_gate_promote`; `test_promote_revokes_before_granting` |
| Shadow promotable only after capability/config/checkpoint/replay checks | Implemented | `obicall_gate_shadow_promotable`; `test_shadow_promotable_requires_matching_readiness` |
| Equal-input disagreement → documented degraded/invalid, not arbitrary winner | Implemented | `obicall_gate_results_agree` (tolerance-based comparison primitive); the gate records but never commits a non-owner's result (`OBICALL_GATE_SHADOW_RECORDED`) |
| Results carry broker identity, epoch, window id, input/config digest, uncertainty, provenance, timestamp, deadline | Implemented | `include/obicall/result.h` |
| Gate's own monotonic clock for lease/deadline enforcement | Implemented | `gate_main.c` uses `osal_monotonic_ns()` exclusively for staleness/expiry, never a submitting broker's clock |
| Persist ownership/commit state before acknowledging | Implemented | `handle_result_publish`/`evaluate_promotion_locked` call `persist_locked()` (write-temp+fsync+atomic-rename) before replying |
| Handle partial writes/corrupt records | Implemented | Checksummed persistence record; corrupt state → refuse to start rather than guess; `tests/integration/test_gate_restart.c` (corrupted-state refusal), `tests/unit/test_gate.c::test_load_detects_corruption` |
| No exactly-once claim for external side effects | Implemented (as documentation) | `docs/FAULT_TOLERANCE.md` |
| Checkpoints: estimator state+covariance, source boot IDs/sequences, pending windows, calibration/model/policy versions, no native pointers | Implemented (type + codec); Not wired into a live broker-to-broker handoff | `include/obicall/checkpoint.h`, `src/core/wire.c` codec, `tests/unit/test_wire.c::test_checkpoint_roundtrip`. Brokers currently recover via independent journal replay, not literal checkpoint transfer — see "Deferred" below |
| Supervisor restarts failed processes, bounded retries+backoff | Implemented | `src/supervisor/supervisor.c::supervisor_monitor_tick` (5 restarts / 60s window per child); observed directly in `docs/VALIDATION.md` |
| Consumer must expire last result even with no process left to notify | Implemented | `valid_until_ns` + local clock comparison, no round trip required; demonstrated in `obicall demo`'s "gate_death_consumer_expiry_demonstrated" phase (`src/cli/main.c::cmd_demo`) |

## Sensor data and estimation

| Requirement | Status | Where |
|---|---|---|
| Observation envelope (identity, sequence, sampling/arrival time, clock domain+uncertainty, frame, units, calibration, shape, covariance) | Implemented | `include/obicall/observation.h` |
| Validate finiteness, dimensions, lengths, covariance suitability | Implemented | `src/core/observation.c` (symmetry + semi-definite Cholesky check); `tests/unit/test_observation.c` |
| Bounded reordering, explicit late-data/overflow policy, duplicate suppression | Implemented | `src/core/journal.c::obicall_journal_admit`; `tests/unit/test_journal.c` |
| Missing readings never become zero | Implemented by construction | The estimator is only ever called with an admitted observation; there is no code path that synthesizes a zero-valued reading |
| Linear Kalman estimator, explicit models/noise/tolerances | Implemented | `include/obicall/estimator.h`, `src/core/estimator.c` (constant-velocity, Joseph-form update); `tests/unit/test_kalman.c` |
| Static-fusion worked example (10.0/10.4 → 10.08/0.032) | Implemented, exact | `obicall_static_fuse_1d`; `tests/unit/test_kalman.c::test_static_fuse_worked_example` (checked to 1e-9) |
| Kalman update not conflated with the static example | Implemented (documented) | Separate function, separate doc comment in `estimator.h` |
| Correlation assumptions documented; no silent duplicate-evidence assumption | Implemented (as documentation + scope) | `docs/ARCHITECTURE.md` "Estimation model"; covariance intersection itself is **not implemented** (see Deferred) |
| Deterministic recordings + replay within numeric tolerance | Implemented | `.obr` journal record format (`include/obicall/journal.h`), `obicall replay`; `recordings/demo.obr`, `tests/CMakeLists.txt::cli_replay_fixture` |
| Published estimate includes uncertainty, sources, timestamp, valid/degraded/invalid status | Implemented | `include/obicall/result.h`; populated in `src/broker/broker_main.c` |
| Covariance intersection | Deferred | Explicitly a later extension in the brief |

## Dimensional Game Theory

| Requirement | Status | Where |
|---|---|---|
| Versioned, experimental, constrained policy in C | Implemented | `include/obicall/dgt.h`, `src/core/dgt.c` |
| Actions = approved (sensor subset, estimator mode, provider) combos | Implemented | `obicall_dgt_action_t`; live table in `src/broker/broker_main.c::init_default_dgt_table` |
| Scenarios = fault catalog, not strategic sensors | Implemented | `obicall_dgt_scenario_t` (dropout/bias/delay) |
| Dimensionless weighted loss, documented scales, finite/nonnegative | Implemented | `obicall_dgt_validate_table`; `tests/unit/test_dgt.c` |
| Eligibility filter before minimax selection | Implemented | `obicall_dgt_select_action` |
| Deterministic tie-break | Implemented | Lowest `action_id` wins; `test_deterministic_tie_break_by_action_id` |
| Switching hysteresis; ineligible action never retained | Implemented | `test_hysteresis_holds_previous_within_margin`, `test_hysteresis_never_retains_now_ineligible_action` |
| Fallback to validated baseline / invalid on policy failure | Implemented | `test_invalid_table_falls_back_to_baseline`, `test_no_eligible_action_and_ineligible_baseline_fails` |
| Required synthetic fixture (0.85/0.70/0.35, action 3 wins when eligible; loses eligibility test) | Implemented, exact | `tests/unit/test_dgt.c::test_fixture_losses_are_exact`, `test_balanced_action_wins_when_eligible`, `test_balanced_action_excluded_when_ineligible` |
| DGT changes a real eligible action in the running demo, not just a label | Implemented | `src/broker/broker_main.c` main loop: the selected action's `sensor_subset_mask` gates which admitted observation actually reaches `obicall_kalman_update_position` |
| DGT cannot grant ownership, weaken validation, invent covariance | Implemented by construction | DGT code has no access to gate state, no path to bypass `obicall_observation_validate`, and never writes into `obicall_result_t.covariance` itself (the estimator does) |
| Not a general Nash-equilibrium solver; manuscript's "perfect play ⇒ tie" claim not relied on | Implemented (as documentation) | `docs/DGT.md` |

## CMake, CLI, platforms

| Requirement | Status | Where |
|---|---|---|
| Distinct CMake targets per component | Implemented | `src/*/CMakeLists.txt` (core, osal, common, cli, gate, journal, broker, worker, supervisor, provider, tests) |
| C11 explicit, target-based includes/definitions/linkage | Implemented | root `CMakeLists.txt`, all subdirectory lists |
| Generated export header, hidden internal symbols | Implemented | `generate_export_header(obicall_core ...)`, `C_VISIBILITY_PRESET hidden` everywhere |
| `${CMAKE_DL_LIBS}` where applicable | Implemented | `src/osal/CMakeLists.txt` |
| OS abstractions: process, clock, IPC, synchronization, durable file | Implemented | `src/osal/osal_process.c`, `osal_clock.c`, `osal_ipc.c`, `osal_thread.c`, `osal_file.c` |
| Install rules, public headers, CMake package exports | Implemented | root `CMakeLists.txt` (`ObicallConfig.cmake` etc.), per-target `install()` |
| Relocated-install smoke test | Implemented, real | See `docs/VALIDATION.md` — install tree copied to an unrelated temp directory, `obicall doctor --json` run from there, `"overall":"ok"` |
| Windows import lib separate from DLL; core output name `obicall`, no `lib` prefix | Implemented | `src/core/CMakeLists.txt` (`OUTPUT_NAME obicall`, `PREFIX ""`) |
| MSYS2 UCRT64 build, matching toolchain (not mixed) | Implemented, real | Built and tested in this environment via `mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,python}` — see `docs/VALIDATION.md` |
| Linux / macOS builds | Configured, not executed here | `.github/workflows/linux.yml`, `macos.yml`; this session has no Linux/macOS host — see `docs/VALIDATION.md` "Not tested" |
| `obicall doctor/validate/run/status/replay/demo`, JSON on stdout, diagnostics on stderr, documented exit codes | Implemented | `src/cli/main.c`; every command exercised by `tests/CMakeLists.txt` CLI smoke tests |
| `status` locates the running instance | Implemented | Runtime-dir `gate.port` + `run.token` files (`src/common/procutil.c`); documented in `docs/ABI.md` |
| Demo starts/cleans up its own children, no separate terminals | Implemented, real | `src/cli/main.c::cmd_demo` (own `supervisor_t`, own teardown) |
| Fault injection restricted to test-enabled builds | Implemented | `--test-crash-after-ms`/`--test-hang-after-ms` compiled only under `OBICALL_TEST_HOOKS_ENABLED` (`OBICALL_TEST_HOOKS` CMake option, default ON — set `-DOBICALL_TEST_HOOKS=OFF` for a build that must not carry these hooks) |

## Required acceptance evidence

All of these have a real, currently-passing automated test unless marked otherwise. See `docs/VALIDATION.md` for the exact `ctest` run.

| Scenario | Status | Test |
|---|---|---|
| C ABI round trip, real dynamic loading | Implemented | `test_worker_abi_roundtrip` |
| Python ABI round trip, event delivery | Implemented | `test_python_abi_roundtrip` |
| Ownership/shutdown (worker) | Implemented | `test_worker_abi_roundtrip` (process kill+close), `test_gate_restart` (gate) |
| Incompatible ABI, missing symbol shape | Implemented | `test_incompatible_abi` |
| Wrong architecture | Not tested | `obicall_manifest_rank_alternatives` architecture filter is unit-tested (`test_manifest.c`); no cross-arch **build** exists in this single-host session to test at the loader level |
| Altered artifact (integrity) | Implemented | `test_altered_artifact` |
| Missing transitive dependency, cycle | Implemented | `test_manifest.c` |
| Truncated/oversized frames, integer bounds, malformed input, NaN, invalid covariance | Implemented | `test_wire.c`, `test_observation.c`, `fuzz_wire_decode` |
| Sensor dropout, reordering, duplication, clock reset (stale sample time), stale input, queue saturation | Implemented | `test_journal.c` (all admission-policy branches) |
| Kalman numerical behavior, static fusion example | Implemented, exact | `test_kalman.c` |
| DGT scoring, ties, eligibility, hysteresis, invalid scores, fallback | Implemented | `test_dgt.c` |
| Worker crash/hang | Implemented (crash); Partial (hang) | `test_worker_crash_hook` (`--test-crash-after-ms`); the `--test-hang-after-ms` hook and the supervisor's heartbeat-file staleness detector both exist and were exercised manually during development (see VALIDATION.md) but do not yet have a dedicated automated test |
| Active broker death, shadow catch-up, controlled promotion | Implemented, real | `cli_demo_broker_failover` (kills a real broker subprocess, observes a real gate-issued promotion) |
| Delayed old-epoch output rejected after promotion | Implemented, real | `cli_demo_broker_failover` ("stale_old_epoch_result_rejected"); also `test_gate.c::test_stale_epoch_after_promotion_rejected` |
| Replayed duplicate windows: no second commit | Implemented, real | `test_gate_restart` (real gate process, real restart, real duplicate resubmission) |
| Gate restart, corrupt persistence, consumer expiry when gate disappears | Implemented, real | `test_gate_restart`; `cli_demo_broker_failover` |
| Equal-input replica disagreement, no arbitrary winner | Implemented (primitive); Not tested end-to-end | `obicall_gate_results_agree` exists and is unit-tested (`test_gate.c::test_results_agree_tolerance`); no end-to-end scenario currently feeds two brokers deliberately-diverging identical-input data to observe the gate's disagreement bookkeeping in a live run |
| Worker replacement + replay using a compatible checkpoint | Partial | Process replacement and journal-level continuity are real and covered by the broker-failover demo's recovery phase; a literal provider-checkpoint transfer between two OS processes is not implemented — see below |
| Sanitizers | Not run here | GCC on MSYS2 UCRT64 does not ship `libasan`/`libubsan` (linker: `cannot find -lasan`); `-DOBICALL_ENABLE_SANITIZERS=ON` is wired into the build for Linux/macOS CI (GCC/Clang there do ship them) but was not exercised in this environment |
| Fuzzing | Implemented, not coverage-guided here | No Clang/libFuzzer available in this environment; `tests/fuzz/` builds a real `LLVMFuzzerTestOneInput` per target that runs unmodified under real libFuzzer where available, plus a standalone random-input driver that ran 100,000 iterations against each target here with no crash (`fuzz_wire_decode`, `fuzz_manifest_parse`) |
| Latency/memory/drop/estimation-error/uncertainty-calibration measurement | Not implemented | No benchmarking harness exists; the brief's own instruction not to fabricate benchmark numbers is followed by simply not reporting any |

## Deferred / narrower than the brief (by design, not oversight)

- **Node-API adapter and arbitrary libffi signatures** — explicitly later extensions in the brief; not started.
- **Covariance intersection** — not implemented; the estimator fuses one position stream at a time through the Kalman update, and the static-fusion identity is a separate, non-recursive function. Cross-correlated-input handling is limited to "not silently assumed independent" at the design level, not a CI algorithm.
- **Literal broker-to-broker / worker-to-worker checkpoint transfer** — the checkpoint type and wire codec are real and tested, but the running system's actual recovery mechanism is: both brokers independently replay the same journal (see `docs/ARCHITECTURE.md` "Windowing model"), and a replaced worker's provider starts a fresh (new `source_boot_id`) session that the journal's admission policy already handles correctly (a boot-id change resets duplicate/reorder tracking for that sensor — `test_journal.c::test_boot_id_change_resets_tracking`). This is a real, tested, but different recovery mechanism than "serialize old worker's state, deserialize into new worker."
- **Multi-host coordination** — out of scope by design; the architecture document itself says a two-host deployment needs an independently reliable authority (e.g. a real Raft group), which this single-gate-process prototype is not.
- **Host-failure tolerance, Byzantine consensus, hard real-time scheduling, automotive safety certification** — explicitly disclaimed, never implemented, never claimed.
- **Latency percentile / throughput benchmarking** — no workload generator or measurement harness was built; see `docs/VALIDATION.md` for what was actually measured (a handful of concrete, reproducible numbers from real runs, not a benchmark suite).
