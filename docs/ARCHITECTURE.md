# Architecture

Obicall is a dynamic dual-FFI polyglot runtime for sensor fusion: two
independent runtime brokers consume a shared, validated observation stream,
run a replicated Kalman estimator, and publish results through a single
authority (the gate) that enforces exactly the invariants a downstream
consumer needs to trust an estimate. This document describes the process
topology, the wire protocol that connects it, and the publication invariants
the gate enforces. For the loss model behind sensor-selection decisions, see
[DGT.md](DGT.md); for the failure model and recovery semantics, see
[FAULT_TOLERANCE.md](FAULT_TOLERANCE.md); for the C ABI itself, see
[ABI.md](ABI.md).

## Process topology

```
                         ┌─────────────────────┐
                         │   obicall (CLI)      │
                         │   acts as supervisor  │
                         │   for `run`/`demo`    │
                         └──────────┬───────────┘
                                    │ spawns, monitors, bounded-restarts
        ┌───────────┬───────────┬──┴────────┬────────────┬────────────┐
        ▼           ▼           ▼            ▼            ▼            ▼
 ┌─────────────┐┌──────────┐┌──────────┐┌──────────┐┌──────────┐┌──────────┐
 │obicall-      ││obicall-  ││obicall-  ││worker A×N││obicall-  ││worker B×N│
 │journald      ││gated     ││brokerd A ││(own procs││brokerd B ││(own procs│
 │(1 process)   ││(1 process)││          ││per sensor)││          ││per sensor)│
 └──────┬───────┘└────┬─────┘└────┬─────┘└────┬─────┘└────┬─────┘└────┬─────┘
        │  loopback TCP, framed, token-authenticated (see "Wire protocol")
        │◄──submit────────────────┤             │◄──submit───────────────┤
        │──replay+live tail──────►│             │──replay+live tail─────►│
        │                          │──heartbeat/publish, req/resp───────►│(to gate)
        └──────────────────────────┴─────────────────────────────────────┘
```

Every arrow is a real OS process boundary and a real loopback socket — not a
thread or an in-process call. A crash in one provider's native code, or in a
whole broker, cannot corrupt another process's memory.

### Why this shape

- **Two brokers, not one.** Broker A publishes first; broker B independently
  replays the same admitted stream and keeps its own live Kalman state, so it
  is ready to take over with a bounded catch-up gap rather than a cold start.
- **Each broker owns its own worker processes.** Broker A's position-sensor
  worker and broker B's position-sensor worker are two separate OS processes
  running the same provider — a crash in one cannot touch the other's address
  space, which is the actual isolation guarantee "process isolation" is
  supposed to buy you (two threads in one process, or two handles to one
  loaded library, would not).
- **The journal is shared and is a deliberate single point of failure for
  this prototype.** Every worker (regardless of which broker it belongs to)
  submits to the one journal process; both brokers read the identical
  admitted stream from it. This is what "both brokers consume identical
  window identifiers and admission decisions" means concretely. A journal
  failure stops the whole pipeline — this is documented, not hidden (see
  [FAULT_TOLERANCE.md](FAULT_TOLERANCE.md)).
- **The gate is an authority, not a third broker.** It never runs the
  estimator or the DGT policy. It only decides, for each candidate result
  offered to it, whether to commit it — see "Publication gate invariants"
  below.
- **The CLI is the supervisor.** `obicall run` and `obicall demo` spawn and
  monitor the whole tree from one process (`src/supervisor/supervisor.c`),
  restarting dead children with bounded retries and backoff. There is no
  separate `obicall-supervisord` binary — the supervisor is a library linked
  into the CLI, which keeps the process count and the argv-plumbing in one
  place instead of duplicating it across a `run` command and a standalone
  daemon.

### Windowing model

A "window" in this codebase is **one admitted observation**, not a
time-bucket aggregating several sensors. The journal assigns a single
monotonically increasing `window_seq` per pipeline to each observation it
admits (or rejects — the sequence advances either way, so both brokers agree
on numbering regardless of admission outcome). A broker processes admitted
windows one at a time: predict the Kalman state to the observation's
`sample_time_ns`, then (if the observation's sensor is in the currently
DGT-selected sensor subset) update with it, then publish a result for that
window. This is a simpler, fully deterministic alternative to time-bucketed
fan-in, and it is what makes journal replay exactly reproducible (same input
sequence in, same estimator trajectory out, modulo floating-point
associativity — see [VALIDATION.md](VALIDATION.md) for the numeric tolerance
this is actually checked at).

## Wire protocol

All inter-process communication uses one transport: a loopback (`127.0.0.1`
only, never `0.0.0.0`) TCP connection carrying length-prefixed, checksummed
frames (`include/obicall/wire.h`, `src/core/wire.c`):

```
bytes 0..3   magic       'O' 'B' 'W' 'F'
byte  4      wire_version
byte  5      msg_type
bytes 6..7   flags       u16 LE, currently 0
bytes 8..11  payload_len u32 LE, <= OBICALL_WIRE_MAX_PAYLOAD (1 MiB)
bytes 12..15 crc32       u32 LE, CRC-32/ISO-HDLC over the payload
bytes 16..   payload     payload_len bytes, msg_type-specific
```

Every multi-byte payload field is packed explicitly least-significant-byte
first by hand-written encode/decode functions — never a `memcpy` of a native
struct — so wire compatibility never depends on a compiler's padding,
alignment, or host endianness. The full payload encodings (observation,
result, checkpoint, event, and each control message) are documented in
[ABI.md](ABI.md).

**Authentication.** Every connection's first frame must be
`OBICALL_MSG_AUTH_HELLO` carrying a 256-bit token generated fresh per run
(`src/common/procutil.c::procutil_random_token`, sourced from
`BCryptGenRandom`/`/dev/urandom`) and passed to every child process at spawn
time. A server that doesn't see a matching token on the first frame closes
the connection without further diagnostics. Combined with binding only to
loopback, this is "authenticate or restrict access to local control
endpoints" — it is not intended to resist an attacker who already has
arbitrary code execution as the same local user.

**Why TCP-over-loopback and not Unix domain sockets / named pipes.** A single
transport implementation that behaves identically on Linux, macOS, and
Windows was worth more for this prototype than the marginal efficiency of a
platform-native IPC primitive, and it avoids maintaining two codepaths for
framing logic that has to be bit-for-bit correct either way.

**Broker↔gate is request/response only.** The gate never pushes a message
unsolicited. A broker's periodic `HEARTBEAT` doubles as its promotion
readiness report, and the gate's `STATUS_REPLY` to it carries the gate's
current epoch and owner — that is how a broker learns it has been granted or
has lost ownership. This trades a little latency (bounded by the heartbeat
interval, ~150 ms in the example config) for not needing a second,
gate-initiated push channel per broker. See
[FAULT_TOLERANCE.md](FAULT_TOLERANCE.md) for why this still keeps promotion
authority entirely inside the gate.

## Publication gate invariants

Implemented as pure, dependency-free logic in `src/core/gate.c`
(`obicall_gate_decide_publish`, `obicall_gate_promote`,
`obicall_gate_shadow_promotable`) with no I/O or socket knowledge, wrapped by
`src/gate/gate_main.c` for the actual process. Keeping the invariants in a
pure module is what makes them directly unit-testable
(`tests/unit/test_gate.c`) without spawning anything.

1. **Only the current owner, at the current epoch, may commit.** Every
   result carries the broker's belief about its own epoch. A result from a
   broker that is not `state.owner_broker_id` is recorded for disagreement
   comparison (`OBICALL_GATE_SHADOW_RECORDED`) but never committed. A result
   whose epoch is behind the gate's current epoch is rejected
   (`OBICALL_GATE_REJECT_STALE_EPOCH`) — this is the fencing-token pattern
   (the epoch is the sequencer), the same mechanism Chubby's lock sequencers
   use to stop a delayed message from a since-deposed holder acting as if it
   still held the lock.
2. **At most one commit per (pipeline, window).** The gate tracks a
   high-water mark per pipeline; anything at or below it is rejected as a
   duplicate. Windows are processed in increasing order by construction (see
   "Windowing model"), so a scalar high-water mark is sufficient — no need to
   remember every individual committed window ID.
3. **Promotion is two-phase and centralized.** `obicall_gate_promote` revokes
   the old owner (sets `owner_broker_id = NONE`) and only then grants the new
   one, as one atomic update to the in-memory state, which the caller must
   persist durably before telling anyone about it. Because *every* promotion
   decision is made inside the gate process, in response to a heartbeat the
   gate itself received, a broker has no code path that lets it decide for
   itself that it is now the owner — see
   [FAULT_TOLERANCE.md](FAULT_TOLERANCE.md) for the exact suspicion → confirm
   → promote sequence.
4. **A shadow must pass a readiness check before promotion.**
   `obicall_gate_shadow_promotable` requires the shadow's last reported
   config digest and checkpoint schema version to match what the gate
   expects, and its last-processed window to be within a configured replay
   gap of the committed high-water mark. Failing any of these blocks
   promotion outright — there is no partial-credit path.
5. **Persistence precedes acknowledgment.** Both a commit and a promotion are
   durably persisted (write-temp, fsync, atomic rename —
   `osal_file_atomic_replace`) before the gate's reply leaves the process.
   Recovery reads a checksummed record; a checksum mismatch is treated as "no
   trustworthy state," and the gate refuses to start rather than guess (see
   `tests/integration/test_gate_restart.c`).

## Estimation model

The estimator is a constant-velocity linear Kalman filter over state
`[px, py, vx, vy]` (`src/core/estimator.c`), using a Joseph-form covariance
update for numerical robustness. Process noise is a simplified diagonal model
(independent position/velocity spectral densities scaled by the prediction
interval) rather than the full Van Loan discretization of a white-noise-
acceleration model — a documented simplification, not an oversight. The
separate static-fusion identity (`obicall_static_fuse_1d`, pure
inverse-variance weighting with no prior) exists specifically to reproduce
the architecture document's worked numeric example and must never be
confused with a Kalman update, which necessarily also incorporates a prior.

Correlation between sensors is not modeled: the estimator processes one
scalar-position update at a time, in window order, and never fuses two
readings as if they were independent when they might share information (the
two brokers' replicated outputs are the clearest case of this — they are
never treated as independent evidence of anything). Covariance intersection,
which would let a future version handle genuinely-unknown-correlation inputs
without this restriction, is not implemented (see
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md)).

## Build and toolchain notes

- The core library's CMake target is `obicall_core`, producing `obicall.dll`
  / `libobicall.so` / `libobicall.dylib` (`OUTPUT_NAME obicall`, empty
  `PREFIX` so Windows doesn't get a `lib` prefix it shouldn't have).
- All executables, the core shared library, and dynamically-loaded provider
  modules are built into **one output directory**
  (`CMAKE_RUNTIME_OUTPUT_DIRECTORY` = `CMAKE_LIBRARY_OUTPUT_DIRECTORY`). This
  is load-bearing on Windows, not cosmetic: `obicall-workerd` loads a
  provider via `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)`, which never searches the process's
  current directory or `PATH` — only the application directory, the loaded
  DLL's own directory, and system directories — so `obicall.dll` has to be
  reachable from one of those for the provider's own dependency on it to
  resolve.
- MSYS2's **UCRT64** environment specifically (`mingw-w64-ucrt-x86_64-gcc`
  and friends) — not `mingw64`, `clang64`, or plain MSYS — because mixing
  MSYS's POSIX-emulation objects with native UCRT64 objects produces
  binaries that don't link correctly against each other. This is exactly
  what was used to build and test this repository; see
  [VALIDATION.md](VALIDATION.md).
