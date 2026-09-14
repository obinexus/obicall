# Validation

Every command and number in this document was actually run in this session,
on this machine, during development. Nothing here is projected, estimated,
or copied from a different run than the one described. Where a platform or
capability was not tested, that is stated as plainly as the things that
were.

## Environment this was built and tested on

- Windows 11 Home 10.0.26200 (x86_64)
- MSYS2 UCRT64: `gcc.exe (Rev3, Built by MSYS2 project) 16.2.0`,
  `cmake version 4.4.3`, `ninja 1.13.2`, `Python 3.14.7`
  (installed for this work via `pacman -S --needed
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python
  mingw-w64-ucrt-x86_64-pkgconf`)
- Also available and unused for the primary build: TDM-GCC 10.3.0
  (a different, non-UCRT64 MinGW distribution — deliberately not used, to
  match the brief's own UCRT64 instruction and avoid mixing toolchains).

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Result: **66/66 targets built, zero compiler warnings**, from a clean
checkout, using exactly the documented command (no extra flags — UCRT64's
`cc`/`gcc` was resolved from `PATH`). This includes the core shared library,
5 process binaries (`obicall`, `obicall-gated`, `obicall-journald`,
`obicall-brokerd`, `obicall-workerd`), 2 dynamically-loaded provider modules
(`provider_c_sim`, and the deliberately-broken `provider_bad_abi` test
fixture), 7 unit test binaries, 6 integration test binaries, and 2 fuzz
targets.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

Result: **17/17 tests passed, 100%**, total real time 13.37 s (this specific
run; individual timings vary run to run by a couple hundred ms — machine
noise, not flakiness). With `-DOBICALL_ENABLE_FUZZING=ON` added, both fuzz
targets bring the total to 19/19 passed.

```
Test project C:/Users/Nnamdi/Projects/obicall/build
 1/17 test_wire ................... Passed
 2/17 test_observation ............ Passed
 3/17 test_kalman .................. Passed
 4/17 test_dgt ..................... Passed
 5/17 test_journal ................. Passed
 6/17 test_manifest ................ Passed
 7/17 test_gate .................... Passed
 8/17 test_worker_abi_roundtrip .... Passed
 9/17 test_incompatible_abi ........ Passed
10/17 test_altered_artifact ........ Passed
11/17 test_gate_restart ............ Passed
12/17 test_worker_crash_hook ....... Passed
13/17 test_python_abi_roundtrip .... Passed
14/17 cli_doctor ................... Passed
15/17 cli_validate_example ......... Passed
16/17 cli_replay_fixture ........... Passed
17/17 cli_demo_broker_failover ..... Passed
100% tests passed, 0 tests failed out of 17
```

`test_python_abi_roundtrip` and `cli_demo_broker_failover` both require a
Python interpreter; `find_program(python3 python)` at CMake-configure time
located UCRT64's own `python.exe` automatically in this run and wired it into
each test's environment (`OBICALL_PYTHON`) — no manual step was needed. If
no interpreter is found at configure time, `test_python_abi_roundtrip` skips
itself (`SKIP_RETURN_CODE 125`) rather than failing, and a warning is printed
at configure time; `cli_demo_broker_failover` would fail at its
Python-worker spawn step in that case (see
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)).

Every integration test spawns **real** subprocesses and does **real**
dynamic loading — none of this is mocked:

- `test_worker_abi_roundtrip`: a real `obicall-workerd` dynamically loads the
  real `provider_c_sim` artifact and sends a real, wire-decoded observation
  to a fake journal listener.
- `test_incompatible_abi`: a real `obicall-workerd` is pointed at a real,
  separately-built module (`provider_bad_abi`, `tests/fixtures/`) that
  deliberately declares the wrong ABI major version, and is confirmed to
  exit nonzero without ever completing the journal handshake.
- `test_altered_artifact`: computes the real SHA-256 of the real
  `provider_c_sim` artifact (via `obicall_sha256`, not a hardcoded value —
  hardcoding would break across platforms/toolchains that produce different
  bytes), confirms an unaltered copy loads, then flips one byte and confirms
  the worker refuses to load it.
- `test_gate_restart`: spawns a real `obicall-gated`, drives it over the real
  wire protocol to bootstrap and commit a window, kills it, confirms the
  state file was written, restarts it and confirms it recovered the same
  epoch/owner/high-water-mark, confirms a replayed duplicate window is
  rejected post-restart, then corrupts the state file and confirms the gate
  refuses to start.
- `test_worker_crash_hook`: confirms the `--test-crash-after-ms` fault
  injection hook (compiled in only under `OBICALL_TEST_HOOKS`) actually
  terminates the process abnormally, on schedule.
- `test_python_abi_roundtrip`: spawns the real `python/obicall/provider_worker.py`,
  which loads the real compiled `obicall` shared library via `ctypes`,
  builds and validates an observation through the real C functions, and
  sends it over a real socket to a fake journal listener that decodes it
  with the same C decoder.
- `cli_demo_broker_failover`: runs `obicall demo --scenario broker-failover`
  end to end — see below.

## The end-to-end demonstration

```bash
obicall demo --scenario broker-failover --config examples/position-fusion.json --json
```

One real, reproducible run from this session:

```json
{"scenario":"broker-failover","bootstrapped_as_a":true,"initial_epoch":1,
 "promotion_detected":true,"new_owner":"B","new_epoch":2,
 "promotion_latency_ms":884,
 "stale_old_epoch_result_rejected":true,
 "gate_death_consumer_expiry_demonstrated":true,"overall":"pass"}
```

Exit code 0. The relevant stderr excerpt from the same run (8 real child
processes: journal, gate, 2 brokers, 4 workers — 2 native C, 2 Python):

```
obicall-gated: bootstrap grant epoch 1 to broker A
obicall-brokerd[A]: gate reports owner=1 epoch=1 (was owner=0)
supervisor: broker_A exited (code=9 / 0x00000009)
supervisor: broker_A restart suspended (fault-injection hold) - leaving it stopped
obicall-gated: promoted broker 2 to epoch 2 (owner 1 stale for 824ms)
obicall-brokerd[B]: gate reports owner=2 epoch=2 (was owner=1)
```

`884ms` end-to-end (from the kill call to the demo observing the new owner)
against a configured `confirm_timeout_ms=700` is a single measurement from
one unloaded machine, not a benchmark claim — expect it to vary with system
load and with the configured timeouts.

After the run, `tasklist` was checked and confirmed **zero** leftover
`obicall*` processes — the demo's `supervisor_stop_all` cleans up completely
even after the deliberate mid-run kills.

## Replay

`recordings/demo.obr` is a real journal segment captured from an actual
`obicall demo` run (copied from that run's `journal.segment` before its
temporary runtime directory was cleaned up) — not synthesized.

```bash
obicall replay --input recordings/demo.obr --json
```

```json
{"input":"recordings/demo.obr","records_total":102,"admitted":102,"rejected":0,
 "final_window_seq":102,
 "estimate":{"status":"valid","x":2.635114,"y":0.999785,"cov_xx":0.009473,"cov_yy":0.009473}}
```

Re-running this command against the same file is deterministic (same input
sequence, same estimator trajectory) up to floating-point associativity —
it was run twice during development with identical output both times.

## Install and the relocated-install smoke test

```bash
cmake --install build --prefix install
```

installs 5 executables, the core shared library + import library, 17 public
headers + the generated export header, the `provider_c_sim` module, the
example pipeline config, and the CMake package files
(`ObicallConfig.cmake`, `ObicallConfigVersion.cmake`, `ObicallTargets.cmake`).

The relocated-install smoke test actually relocates: the installed tree was
copied to `%TEMP%\obicall_relocated_smoketest`, a directory with no build
tree, no source tree, and no `PATH` entry pointing back at either, and run
from there:

```
C:\...\obicall_relocated_smoketest> .\bin\obicall.exe doctor --json
{"checks":[{"name":"core_library","status":"ok","detail":"obicall 0.1.0 ABI 1.0"},
           {"name":"provider_c_sim","status":"ok",
            "detail":"C:\\...\\obicall_relocated_smoketest\\bin/../lib/obicall/providers/provider_c_sim.dll"}],
 "overall":"ok"}
Exit code: 0
```

This confirms the installed `obicall.exe` finds its own `obicall.dll`
(co-located in `bin/`) and the installed `provider_c_sim.dll` (under
`lib/obicall/providers/`, a different layout than the dev-tree's
`examples/providers/`) with no hardcoded build-tree path anywhere in the
resolution.

## Sanitizers

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOBICALL_ENABLE_SANITIZERS=ON
cmake --build build-asan
```

**Not run in this environment.** The build fails at link time:

```
ld.exe: cannot find -lasan: No such file or directory
ld.exe: cannot find -lubsan: No such file or directory
```

MSYS2 UCRT64 GCC does not ship `libasan`/`libubsan` — sanitizer runtime
support on Windows is primarily a Clang/MSVC feature, and no Clang toolchain
was set up in this environment. `-DOBICALL_ENABLE_SANITIZERS=ON` is wired
into the CMake build (`add_compile_options(-fsanitize=address,undefined ...)`
guarded to GCC/Clang) and is exercised in `.github/workflows/linux.yml`,
where Ubuntu's GCC does ship both. This is a genuine environment limitation,
not a skipped step.

## Fuzzing

**Real coverage-guided libFuzzer: not run in this environment** — same root
cause, no Clang toolchain available. What *was* run: both fuzz targets build
a real `LLVMFuzzerTestOneInput` (would build and run unmodified under real
libFuzzer with `-fsanitize=fuzzer` on Clang) plus a standalone random-input
driver (`tests/fuzz/fuzz_driver.h`) that was actually executed:

```bash
./build/bin/fuzz_wire_decode.exe 1 100000
./build/bin/fuzz_manifest_parse.exe 1 100000
```

```
fuzz-lite: 100000 iterations, seed=1, no crash
fuzz-lite: 100000 iterations, seed=1, no crash
```

100,000 random-byte inputs against every wire decoder and against the
manifest JSON parser, no crash. This is real evidence against crashes on
random input; it is explicitly **not** coverage-guided and should not be
read as equivalent to a real libFuzzer corpus-driven run.

## What was not tested

- **Linux, macOS builds.** No Linux or macOS host was available in this
  session. `.github/workflows/linux.yml` and `macos.yml` are configured
  (apt/brew toolchain install, the same `cmake`/`ctest`/`install` sequence
  documented above, plus the same relocated-install smoke test) but have
  never actually run. A cross-compile was not treated as a substitute for
  this and was not attempted.
- **Wrong-architecture rejection at the loader level.** The architecture
  filter in `obicall_manifest_rank_alternatives` is unit-tested directly
  (`tests/unit/test_manifest.c`), but there is no second-architecture build
  available in this single-host session to test end to end through the
  actual dynamic loader.
- **Hang detection's automated coverage.** The `--test-hang-after-ms` hook
  and the supervisor's heartbeat-file staleness detector
  (`src/supervisor/supervisor.c::supervisor_monitor_tick`) were both
  exercised *manually* during development — the staleness detector's grace
  period was specifically tuned after it produced false positives against a
  slow-starting Python worker during a real `obicall demo` run, which is
  how the current 4-second grace period and the "write a heartbeat before
  connecting, not after" fix in `provider_worker.py` were arrived at — but
  neither has a dedicated automated `ctest` case.
- **Equal-input replica disagreement, end to end.** The comparison primitive
  (`obicall_gate_results_agree`) is unit-tested; no scenario currently drives
  two brokers with deliberately-diverging identical-input data through a
  live run to observe the gate's disagreement bookkeeping.
- **Latency percentiles, memory, drop rates, estimation-error/uncertainty
  calibration under a defined workload.** No benchmarking harness was built.
  The only performance-shaped number in this document (884 ms promotion
  latency) is a single real measurement, reported as exactly that — not a
  substitute for a real benchmark suite, and not presented as one.
