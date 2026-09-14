# Fault tolerance

## What this prototype does and does not claim

**Does:** survive a crash of one broker (with a bounded promotion delay and
a documented gap in coverage for in-flight windows during that delay),
survive a worker crash or hang (supervisor-restarted, bounded retries), keep
a durable, checksummed record of gate ownership/commit state across a gate
restart, and give a consumer everything it needs to *know* an estimate has
gone stale even if nothing is left running to tell it so explicitly.

**Does not claim:** tolerance of a whole-host failure (everything here runs
on one machine), Byzantine fault tolerance, hard real-time scheduling
guarantees, or any kind of safety certification. The architecture document
this implements is explicit that a real multi-host deployment needs an
independently reliable authority (e.g. an actual Raft group) in place of a
single gate process — a two-voter Raft-like scheme loses its majority the
moment either voter is unavailable, so it would not actually add tolerance
over what a single, well-persisted gate already gives you on one host. This
prototype's redundancy is *within one host*: two broker processes, not two
hosts.

## Failure domains

| Component | Redundancy | What happens if it dies |
|---|---|---|
| Journal | None — single process | The whole pipeline stops admitting new observations. Both brokers' replay connections fail; the supervisor restarts it with bounded retries, but nothing already-connected reconnects automatically (see "Known gap" below). This is deliberate: shared input infrastructure needing its own redundancy is called out explicitly rather than silently assumed away. |
| Gate | None — single process, but durably persisted | No new commits are possible while it's down. On restart, it recovers epoch/owner/committed-window state from its last checksummed persist — or refuses to start at all if that record is corrupt (see "Persistence" below). Consumers holding a cached last result independently expire it via `valid_until_ns` without needing the gate to tell them (see "Consumer-side expiry"). |
| Broker A / B | Two, one active one hot-shadow | The gate detects the active owner's heartbeat going stale and, once the shadow passes its readiness check, promotes it — see "Promotion sequence" below. |
| Provider workers | Two per sensor (one under each broker) | Supervisor-restarted with bounded retries; the journal's per-sensor admission tracking resets cleanly on the new process's new `source_boot_id` (see `docs/ABI.md`... actually `ARCHITECTURE.md` "Windowing model" and `tests/unit/test_journal.c::test_boot_id_change_resets_tracking`). |

## Promotion sequence

Promotion authority lives **only** inside the gate process
(`src/gate/gate_main.c::evaluate_promotion_locked`), and is only ever
evaluated as a side effect of the gate receiving a heartbeat — from either
broker. This matters: a broker that independently notices its peer has gone
quiet (IPC failure, timeout on its own side) has **no code path** that lets
it promote itself. It can only report its own readiness and wait for the
gate to act.

1. Every broker heartbeat updates the gate's record of that broker's
   liveness, config digest, checkpoint schema version, and last-processed
   window — this is the readiness snapshot used in step 4.
2. On *every* heartbeat received (from either broker), the gate checks how
   long it has been since the *current owner's* last heartbeat, using the
   gate's own monotonic clock only — never a value taken from either
   broker.
3. Below `suspect_timeout_ms`: nothing happens. Between
   `suspect_timeout_ms` and `confirm_timeout_ms`: the owner is suspect, but
   suspicion alone authorizes nothing. Above `confirm_timeout_ms`: the gate
   evaluates whether to promote.
4. Promotion requires `obicall_gate_shadow_promotable` to return true for the
   other broker: healthy, matching config digest, matching checkpoint schema
   version, and last-processed window within `max_replay_gap` of the
   committed high-water mark. Any one of these failing blocks promotion —
   there is no partial credit.
5. If promotable, `obicall_gate_promote` revokes the old owner and grants the
   new one as one atomic state update, which is durably persisted
   **before** the gate's reply (carrying the new epoch/owner) leaves the
   process.
6. Any result the old owner sends after this point carries its old epoch and
   is rejected (`OBICALL_GATE_REJECT_STALE_EPOCH`) — this is fencing via
   epoch, not a best-effort race.

Measured example (`obicall demo --scenario broker-failover`, this
environment, `confirm_timeout_ms=700`): 884 ms from killing broker A's
process to the gate reporting broker B as owner at the new epoch. That
number is a single real measurement from one machine under no particular
load, not a benchmark claim — see [VALIDATION.md](VALIDATION.md) for exactly
how it was produced and how to reproduce it.

### Why the demo has to *suspend* the killed broker's auto-restart

The supervisor's own bounded-retry restart is, by design, fast — usually
well under the gate's `confirm_timeout_ms`. Discovered during development:
naively killing broker A and letting the supervisor restart it immediately
brings the *same* `broker_id=A` back with fresh heartbeats before the gate
ever accumulates enough silence to promote B, which would make the failover
demonstration pass or fail depending on scheduling noise rather than by
actually exercising promotion. `obicall demo` uses
`supervisor_kill_child_and_suspend` to hold broker A down until promotion is
observed, then calls `supervisor_resume_child` — letting A rejoin afterward
as the new hot shadow, which is the recovery behavior worth demonstrating
anyway, on a schedule the demo actually controls.

## Persistence

The gate writes a single checksummed record on every commit and every
promotion (`src/core/gate.c::obicall_gate_persist_state`,
`obicall_gate_load_state`), via the standard durable-update pattern: write to
a temp file, `fsync`/`FlushFileBuffers`, then atomically rename over the real
state file (`src/osal/osal_file.c::osal_file_atomic_replace`). A crash
between the temp write and the rename leaves, at worst, an orphaned `.tmp`
file next to an *unmodified, still-valid* real state file — this was
observed directly during development (a `gate.state.tmp` left over after a
demo run that force-killed the gate) and is the expected, safe outcome of
this pattern, not a bug.

On startup, if a state file exists but fails its checksum, the gate **prints
an error and refuses to start** (exit code 3) rather than silently
continuing with empty state — see `tests/integration/test_gate_restart.c`
for a real test that corrupts a byte in a real persisted file and confirms
this. "If recovery cannot establish trustworthy state, retain invalid output
until reconciliation" is implemented here as "don't come up at all until a
human looks at it"; there is no automatic reconciliation procedure.

If no state file exists at all (the normal first-run case), the gate starts
fresh with no owner, and grants broker A the first epoch as soon as its
first heartbeat arrives (`evaluate_promotion_locked`'s bootstrap branch) —
this is the one case where the gate promotes without a competing shadow to
compare against, since there is nothing to compare against yet.

## Consumer-side expiry

Every published result carries `valid_until_ns`, measured against the gate's
own monotonic clock. A consumer that has cached the last result it saw can
determine locally, from its own clock, whether that result should still be
trusted — **no round trip to the gate is required**, which is the only way
this can possibly work once the gate itself is the thing that's gone.
`obicall demo`'s last phase demonstrates exactly this: it caches a last-known
result, kills the gate, waits past the result's declared validity window,
and confirms it can determine "no longer trustworthy" from local state alone
(`src/cli/main.c::cmd_demo`, `gate_death_consumer_expiry_demonstrated`).

## Equal-input disagreement

Two replicas cannot tell which of them is right by comparing only
themselves — if both compute the same wrong answer (a shared software bug,
or a shared sensor fault), agreement proves nothing. `obicall_gate_results_agree`
(`src/core/gate.c`) provides a declared-tolerance comparison between two
results for the same pipeline/window, intended to be used to flag
unexplained disagreement as degraded/invalid rather than to silently prefer
one side. The primitive is real and unit-tested
(`tests/unit/test_gate.c::test_results_agree_tolerance`); wiring it into a
live disagreement-logging path in `gate_main.c` (comparing the owner's
committed result against the shadow's `OBICALL_GATE_SHADOW_RECORDED`
candidate for the same window) is not yet done — see
[IMPLEMENTATION_STATUS.md](IMPLEMENTATION_STATUS.md).

## Known gap: journal/gate restart does not repropagate a new port

Each server process binds an OS-assigned ephemeral port and writes it to a
`<name>.port` file in the runtime directory at startup
(`src/common/procutil.c`). Children that need to reach it (brokers → journal
and gate; workers → journal) receive that port once, baked into their argv,
when the supervisor spawns them. If the journal or gate process is restarted
by the supervisor after a crash, the **new** process binds a **new**
ephemeral port — but nothing already running re-reads it. In practice this
means: a broker or worker that was already connected before a journal/gate
crash cannot reconnect after that specific process restarts; only children
started *after* the restart would pick up the new port (and even then, only
if something re-spawns them with fresh argv, which the supervisor does not
currently do for already-healthy siblings of a restarted server). This is
why the journal and gate are documented above as single points of failure
for this prototype rather than "restart-tolerant" — the supervisor does
restart them (bounded retries, as required), but full mesh reconnection
after that restart is not implemented. Discovered and worked around once
already in this codebase's own development: `obicall demo` reusing a fixed
runtime directory across repeated invocations hit exactly this class of bug
via stale leftover port files (fixed by clearing the port file immediately
before every spawn, and by giving `demo` a fresh, PID-suffixed runtime
directory per invocation — see `src/common/procutil.c::procutil_clear_port_file`
and `src/cli/main.c::cmd_demo`).
