# Obicall

Obicall is a dynamic dual-FFI polyglot runtime for sensor fusion: two
independent broker processes consume a shared, validated sensor stream, run
a replicated Kalman estimator, and publish results through a single
publication gate that enforces ownership, freshness, and duplicate-commit
invariants. An experimental Dimensional Game Theory (DGT) policy selects
which sensors actually feed the estimator, in C, live, based on eligibility
and a versioned loss table.

This is a **local desktop prototype**. It does not claim host-failure
tolerance, Byzantine consensus, hard real-time scheduling, or any safety
certification — see [docs/FAULT_TOLERANCE.md](docs/FAULT_TOLERANCE.md) for
exactly what it does and does not survive, and
[docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) for a
requirement-by-requirement account of what's implemented, tested, or
deliberately deferred.

## Quick start

Prerequisites: a C11 compiler, CMake ≥ 3.20, and Python 3 (used as a real
language adapter, not optional tooling — see [docs/ABI.md](docs/ABI.md)).
Ninja is used automatically when available but is not required — see
"Platform prerequisites" below.

```bash
make
make test
make install
```

`make` alone configures and builds; see "Building with `make`" below for
every target, or run the equivalent CMake commands directly if you'd rather
not use `make` at all:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
cmake --install build --config Debug --prefix install
```

Then run the end-to-end failover demonstration — it starts its own 8-process
tree (journal, gate, 2 brokers, 4 provider workers), kills the active
broker, observes the gate promote the other one, confirms a stale-epoch
result is rejected, and cleans up after itself:

```bash
./build/bin/obicall demo --scenario broker-failover --config examples/position-fusion.json --json
```

```json
{"scenario":"broker-failover","bootstrapped_as_a":true,"initial_epoch":1,
 "promotion_detected":true,"new_owner":"B","new_epoch":2,
 "promotion_latency_ms":884,"stale_old_epoch_result_rejected":true,
 "gate_death_consumer_expiry_demonstrated":true,"overall":"pass"}
```

That exact output (except the latency number, which will vary) is what this
command produces — see [docs/VALIDATION.md](docs/VALIDATION.md) for the full
session log this came from.

### Other commands

```bash
obicall doctor --json                                       # environment/ABI/provider sanity check
obicall validate --config examples/position-fusion.json --json   # config + manifest + dependency-graph check
obicall run --config examples/position-fusion.json --duration-seconds 30 --json
obicall status --json                                        # query a running instance
obicall replay --input recordings/demo.obr --json            # deterministic offline replay of a real recording
```

`obicall status` locates a running instance via a runtime directory (default
`./obicall-run`, override with `--runtime-dir`) containing the gate's port
and an authentication token written at startup — see
[docs/ABI.md](docs/ABI.md) "Trust model".

## Building with `make`

The root `Makefile` is a thin wrapper around the CMake build above — it
does not duplicate the CMake target graph, it just calls `cmake`/`ctest`
for you. It works the same way from Windows PowerShell, an MSYS2 UCRT64
shell, Linux, or macOS: every recipe is a plain `cmake`/`ctest` invocation
with no shell-specific syntax, so it doesn't matter whether GNU Make ends
up running its recipes under `sh` or under `cmd.exe` (which one that is
depends on what GNU Make finds on `PATH`, **not** on which shell you typed
`make` into — see the comment at the top of the `Makefile` for what was
actually observed on this project's own dev machine).

```bash
make            # configure and build (default target)
make debug      # build the Debug configuration, into BUILD_DIR
make release    # build the Release configuration, into a separate directory
make test       # build, then run ctest with failure output shown
make install    # build, then install under an overridable local prefix
make clean      # remove compiled outputs; keeps the CMake cache
make help       # full target/variable reference with examples
```

Every target CMake defines gets built — the core shared library, every
process binary (`obicall-gated`/`-journald`/`-brokerd`/`-workerd`), the CLI,
the provider modules, and the test binaries — because `make build` runs
plain `cmake --build`, CMake's own default "build everything" target, not
an invented, separately-maintained list of names.

**Variables** (override as `make VAR=value ...`):

| Variable | Default | Meaning |
|---|---|---|
| `CMAKE` | `cmake` | The CMake executable to use |
| `CTEST` | `ctest` | The CTest executable to use |
| `BUILD_DIR` | `build` | Where `all`/`build`/`configure`/`test`/`install`/`clean`/`rebuild` operate |
| `BUILD_TYPE` | `Debug` | Passed as both `-DCMAKE_BUILD_TYPE` (configure time) and `--config`/`-C` (build/test/install time), so it does the right thing for both single- and multi-configuration generators |
| `GENERATOR` | *(empty)* | Passed as `-G` only if set; empty lets CMake pick (see below) |
| `INSTALL_PREFIX` | `<repo>/install` | A local directory by default — `make install` never needs Administrator/root |
| `JOBS` | *(empty)* | Passed as `--parallel`/`-j` only if set; empty lets the build tool use its own default |
| `BUILD_DIR_RELEASE` | `BUILD_DIR-release` | Used only by `make release`, so it never collides with a Debug-configured `BUILD_DIR` |

```bash
make JOBS=8
make BUILD_DIR=build-ucrt64 GENERATOR=Ninja test
make INSTALL_PREFIX=C:/opt/obicall install
```

**Safety notes**, both directly informed by testing this Makefile against
this repository's own pre-existing `build/` directory:

- `make configure` never deletes an existing `CMakeCache.txt`. Reconfiguring
  with a `GENERATOR` that conflicts with what a build directory already
  used fails with CMake's own actionable error (it names the previous
  generator and tells you to remove the cache or pick a different
  directory) rather than silently doing something to it.
- Reusing a build directory that was configured by one MSYS2 environment
  (say, UCRT64) from a shell that doesn't have that environment active is
  a real, reproducible failure mode, not a hypothetical one: the cached
  compiler's *driver* can still run (e.g. answering `--version`) while
  actual compilation fails deep in the build with almost no useful output,
  because the compiler's back end can't find its own runtime DLLs outside
  that environment. `make configure` checks for this specific situation
  up front and prints a clear warning naming the fix, instead of leaving
  you to debug a confusing mid-build failure.
- `make rebuild` runs `clean` and `build` as two lines of one target's
  recipe, not as a prerequisite list — GNU Make only guarantees
  prerequisites finish before their target's own recipe starts, not that
  they run in the order listed relative to each other, so under `make -j`
  a prerequisite-based `clean build` could start building while cleaning
  was still in flight. Two recipe lines never run concurrently with each
  other, regardless of `-j`.
- `test` and `install` both depend on `build` as a real prerequisite, so
  `make -j test` (or any other parallel invocation) still waits for a
  successful build before either runs.

Run `make help` for the complete, current reference — it's generated from
the same `Makefile`, so it can't drift out of sync with what the targets
actually do.

## Platform prerequisites

Built and tested in this repository on **Windows via MSYS2 UCRT64**:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python make
```

(`make` there is the plain MSYS `make` package, which installs as
`make.exe` so `make` works directly. `mingw-w64-ucrt-x86_64-make` also
exists, but — as of this writing — it installs as `mingw32-make.exe`, so
you'd have to type `mingw32-make` instead; the plain `make` package is
simpler unless you specifically want to avoid any MSYS-layer POSIX
components.)

Always launch a **real** MSYS2 UCRT64 shell for this (the `MSYS2 MinGW
UCRT64` Start Menu shortcut, or `C:\msys64\ucrt64.exe`) rather than trying
to reproduce that environment by hand in another shell (e.g. by manually
adding `C:\msys64\ucrt64\bin` to PATH) — the real shell also sets other
variables the toolchain depends on (notably `TMP`/`TEMP`, in MSYS path
form) that a partial manual setup easily misses, in ways that produce the
same confusing "compiler driver runs, real compilation fails" symptom
described above. Run from the matching **UCRT64** environment specifically
(not `MSYS`, `MINGW64`, or `CLANG64` — mixing those toolchains' objects
doesn't link correctly against each other).

If you don't have MSYS2 at all, `make`/`cmake` still work from plain
Windows PowerShell or Command Prompt with any CMake-supported toolchain
already on `PATH` (for example, Visual Studio's Build Tools — CMake
auto-selects a Visual Studio generator when it detects one and no other
generator is requested; this was one of the two full toolchains actually
used to validate this Makefile, see
[docs/VALIDATION.md](docs/VALIDATION.md)). Ninja is not required in that
case.

### If `make` warns about a toolchain/shell mismatch

If `BUILD_DIR` (`build` by default) was configured from a different shell
than the one you're running `make` in now — the most common case being an
MSYS2 UCRT64-configured `build/` reused from plain PowerShell or Command
Prompt — `make configure` prints a warning naming the problem and then the
real compile still fails, because that compiler needs its own environment
active to run. This is expected, not a bug in the check: it warned
correctly and the predicted failure happened. Pick one:

- **Stay in your current shell, build with whatever toolchain it can see
  on its own** (e.g. Visual Studio, from PowerShell) **by using a separate
  build directory**, so the existing `build/` is left untouched:

  ```powershell
  make BUILD_DIR=build-msvc
  make BUILD_DIR=build-msvc test
  ```

  Use `BUILD_DIR=...` on every invocation for that directory (or `export`/
  `$env:` the variable for the session), since `BUILD_DIR` has no memory of
  which shell last used it — the toolchain/shell check exists precisely
  because that pairing isn't tracked anywhere else.

- **Or open the shell that matches the existing `build/`** — for a UCRT64
  cache, a real MSYS2 UCRT64 shell (`C:\msys64\ucrt64.exe`, or the `MSYS2
  MinGW UCRT64` Start Menu shortcut) — `cd` to the repository, and run
  `make`/`make test` there instead. That reuses `build/` as-is.

Either is correct; they just produce artifacts under different directories
(`build/` vs `build-msvc/`, in the example above). There's nothing to
clean up or undo from seeing the warning.

Linux and macOS are configured (`.github/workflows/linux.yml`, `macos.yml` —
standard `apt`/`brew` toolchain install, the same `make`/`make test`/
`make install` commands above, plus the same relocated-install check
described below) but have not been run in this development environment,
which had no Linux or macOS host available.
See [docs/VALIDATION.md](docs/VALIDATION.md) for exactly what was and wasn't
executed, on which platform.

## What's real here

Every claim below has a passing, currently-runnable test backing it —
`ctest --test-dir build --output-on-failure` reproduces all of them:

- **Real dynamic loading.** `obicall-workerd` uses `dlopen`/`LoadLibraryExW`
  to load an actual compiled provider artifact at runtime, validates its
  declared ABI version and integrity hash before touching it, and rejects
  (with a distinct exit code) a provider that fails either check —
  `tests/integration/test_incompatible_abi.c`,
  `test_altered_artifact.c`.
- **A real Python FFI adapter**, not a subprocess emitting canned JSON: the
  Python worker builds an observation via `ctypes`, validates and
  wire-encodes it by calling the *actual compiled* `obicall` shared
  library's C functions, and sends the result over a real socket —
  `tests/integration/test_python_abi_roundtrip.c`,
  `python/obicall/abi.py`.
- **A dependency resolver that actually resolves the full graph**, not one
  shortest path — deliberately correcting a real flaw found in one of this
  project's own architectural references (see
  [docs/ABI.md](docs/ABI.md) "Manifest schema") —
  `tests/unit/test_manifest.c`.
- **A publication gate with real fencing**: promotion authority lives only
  in the gate process, a promoted epoch strictly increases, and a delayed
  result from a since-deposed owner is provably rejected — exercised both as
  pure logic (`tests/unit/test_gate.c`) and as real subprocesses
  (`cli_demo_broker_failover`, `test_gate_restart`).
- **Durable, checksummed gate persistence** that survives a real process
  restart and refuses to start on a corrupted state file rather than
  guessing — `tests/integration/test_gate_restart.c`.
- **The architecture document's exact worked numeric example**
  (independent readings 10.0 m / 10.4 m, variances 0.04 / 0.16 m² → 10.08 m,
  0.032 m²) reproduced to `1e-9` —
  `tests/unit/test_kalman.c::test_static_fuse_worked_example`.
- **The brief's exact DGT fixture** (worst-case losses 0.85 / 0.70 / 0.35,
  balanced action wins when eligible, loses when it isn't) reproduced
  exactly — `tests/unit/test_dgt.c`.
- **DGT that changes what the estimator actually does**, not a logged label
  — see [docs/DGT.md](docs/DGT.md) "The action actually changes".
- **A relocated install**: the installed tree, copied to an unrelated
  directory with no build tree in sight, runs correctly —
  [docs/VALIDATION.md](docs/VALIDATION.md) "Install and the
  relocated-install smoke test".

## What's honestly not there

- No Node-API adapter, no arbitrary-signature libffi calls, no covariance
  intersection, no multi-host coordination — these are the brief's own
  named later extensions, not started.
- No literal broker-to-broker checkpoint hand-off (the checkpoint type and
  wire codec exist and are tested; the running system's actual recovery
  mechanism is independent journal replay — see
  [docs/IMPLEMENTATION_STATUS.md](docs/IMPLEMENTATION_STATUS.md) for the
  distinction).
- No sanitizer or coverage-guided-fuzzing run in *this* environment
  (MSYS2 UCRT64 GCC has no `libasan`/`libubsan`/libFuzzer) — both are wired
  into the build for a toolchain that has them; see
  [docs/VALIDATION.md](docs/VALIDATION.md).
- No benchmarking harness, so no latency-percentile/throughput/memory
  numbers are reported — the one latency number in this README is a single
  real measurement, labeled as exactly that.

## Repository layout

```
include/obicall/     public C ABI headers
src/core/             obicall_core: pure logic - wire codec, validation,
                      estimator, DGT, journal admission policy, gate
                      invariants - no process/socket/dlopen knowledge
src/osal/             OS abstraction: process, clock, loopback IPC,
                      threading, durable file updates, dynamic loading
src/common/           shared process-binary helpers (argv parsing, the
                      run-token handshake, port-file discovery)
src/gate/             obicall-gated
src/journal/          obicall-journald
src/broker/           obicall-brokerd
src/worker/           obicall-workerd
src/supervisor/       process tree spawn/monitor/restart, used by the CLI
src/cli/              obicall (doctor/validate/run/status/replay/demo)
src/providers/c_sim/  reference C provider (simulated position sensor)
python/obicall/       Python provider adapter (ctypes bindings + worker)
examples/             a working pipeline config + provider manifests
recordings/           a real recorded journal segment for `obicall replay`
tests/unit/           pure-logic tests, no subprocesses
tests/integration/    real-subprocess tests (dynamic loading, IPC, restart)
tests/fixtures/       a deliberately ABI-incompatible provider, for testing
tests/fuzz/           LLVMFuzzerTestOneInput targets + a standalone driver
docs/                 architecture, ABI, fault tolerance, DGT, validation,
                      implementation status
```

## Reference architectures

Design ideas, not code, were drawn from four supplied projects — see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/ABI.md](docs/ABI.md) for where and why:

| Reference | What it contributed to this design |
|---|---|
| LibPolyCall | Opaque C contexts; keeping estimation logic out of language adapters |
| NSIGII dual FFI | Native calls plus a separate message transport for the cross-process case |
| Dynamic C ABI Loader | Manifest-driven discovery — and, via its shortest-path flaw, the reason this project's resolver computes the *full* dependency closure instead |
| OBIAI Tensor TTS v0.2 | Explicit metadata and replayable diagnostic state as a model for the observation/checkpoint schemas |

## License

[MIT](LICENSE).
