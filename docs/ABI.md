# C ABI reference

This document is the contract for `include/obicall/*.h`. If code and this
document disagree, the header is authoritative and this document has a bug —
please report it as such.

## Versioning

Four independent version numbers, deliberately not conflated:

| Version | Constant | Governs |
|---|---|---|
| ABI | `OBICALL_ABI_VERSION_MAJOR`/`MINOR` (`plugin.h`) | The shape of `obicall_descriptor_t`/`obicall_provider_vtable_t` and the meaning of `obicall_plugin_query_v1` |
| Wire schema | `OBICALL_WIRE_VERSION` (`wire.h`), plus a per-struct `*_SCHEMA_VERSION` (`OBICALL_OBSERVATION_SCHEMA_VERSION`, `OBICALL_RESULT_SCHEMA_VERSION`, `OBICALL_EVENT_SCHEMA_VERSION`) | The byte layout of frames on the loopback wire |
| Checkpoint schema | `OBICALL_CHECKPOINT_SCHEMA_VERSION` (`checkpoint.h`) | The layout of `obicall_checkpoint_t` |
| Config schema | Per-manifest `config_schema_version` | The provider-defined shape of the manifest's `"config"` JSON object |

A major ABI version bump means an incompatible change to the plugin contract
itself; a minor bump means an additive, backward-compatible change (a loader
built against an older minor version must still work against a newer
plugin — this is exactly what `struct_size` exists to make safe).

## Loading sequence

1. Resolve and validate the manifest (`obicall_manifest_parse_json`,
   `obicall_manifest_resolve` for its dependency closure).
2. If the manifest declares `artifact_sha256`, hash the actual artifact file
   with `obicall_sha256` and compare before going anywhere near
   `dlopen`/`LoadLibraryExW`. **A hash proves the artifact matches what a
   trusted manifest declared — it does not authenticate who published
   the manifest.** Manifests and their artifact directories are assumed to
   already be in a trusted, protected location; nothing in this codebase
   establishes that trust itself (no signature verification is implemented).
3. `dlopen`/`LoadLibraryExW` the artifact. Library initialization code (e.g.
   dynamic-initializer constructors) can run during this step, before your
   code has looked at anything — restricted search paths
   (`LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` on
   Windows, no `RTLD_GLOBAL` on POSIX) are the only mitigation applied here.
4. Resolve the symbol `OBICALL_PLUGIN_QUERY_SYMBOL`
   (`"obicall_plugin_query_v1"`) and call it. **A resolved symbol only
   proves a function with that name exists at that address — it proves
   nothing about its signature.** The next step is what actually checks
   compatibility.
5. Validate the returned `obicall_descriptor_t`: `struct_size >=
   sizeof(obicall_descriptor_t)` as this loader's headers define it,
   `abi_version_major == OBICALL_ABI_VERSION_MAJOR` exactly, and
   `abi_version_minor <= OBICALL_ABI_VERSION_MINOR` (a plugin asking for a
   newer minor version than this loader supports is incompatible; older or
   equal is fine, per the additive-only minor-version rule above).
6. Validate the vtable: `struct_size >= sizeof(obicall_provider_vtable_t)`
   and every function pointer this loader intends to call is non-NULL.
7. Only now call `vtable->create()`.

`src/worker/worker_main.c` implements exactly this sequence and exits with a
distinct nonzero code at each failure point (see "Exit codes" below);
`tests/integration/test_incompatible_abi.c` and
`test_altered_artifact.c` exercise steps 5 and 2 respectively against real,
deliberately-broken fixtures.

## Ownership and lifetime rules

- **Observations, results, checkpoints passed by value/pointer into a call**
  are borrowed for the duration of that call only. Copy anything you need to
  keep.
- **`obicall_buffer_t`** (a `const uint8_t* + uint32_t` pair) is always
  borrowed and never owned by the receiver.
- **`obicall_owned_buffer_t`** is returned by `checkpoint()` and must be
  released through `vtable->release_buffer`, never through the caller's own
  `free()` — the allocating module (the provider) is the only one that knows
  which allocator produced it.
- **Provider events delivered through `poll_events`** are valid only for the
  duration of the callback invocation. `poll_events` is synchronous and
  pull-based: the provider queues internally (bounded by whatever the
  provider itself chooses to bound it at) and `poll_events` drains that queue
  once per call, invoking the callback once per queued event before
  returning. This is what "prevent unbounded reentrant calls" means here in
  practice — there is no code path where a provider can call back into the
  loader asynchronously from another thread.
- **A provider instance handle** (`obicall_provider_handle_t`) is destroyed
  exactly once, via `vtable->destroy`, only after you are certain no further
  `submit`/`poll_events`/`checkpoint` calls will be made against it.

## Struct layouts (for ctypes / FFI authors)

Every ABI-facing struct uses only fixed-width types
(`uint32_t`/`uint64_t`/`int64_t`/`double`/fixed-size `char`/`uint8_t`
arrays) and is laid out largest-alignment-first where practical, so the
platform C ABI's natural alignment rules produce the same byte layout under
GCC, Clang, and MSVC for a given target (Windows x64 or SysV x64) without
needing `#pragma pack`. `python/obicall/abi.py`'s `ctypes.Structure`
subclasses mirror these field-for-field and rely on the same natural-
alignment behavior — see that file's docstring for the specific offset
worked example used in `tests/unit/test_wire.c`'s forged-payload-count test.

**This is checked, not just asserted:** the Python worker builds an
`obicall_observation_t` via ctypes, calls the *real* compiled
`obicall_wire_encode_observation` through ctypes to serialize it, and sends
those bytes over a real socket to a listener that decodes them with the same
C function. If the ctypes struct's layout disagreed with the C compiler's,
the C side would read garbage out of it and the round trip would fail —
`tests/integration/test_python_abi_roundtrip.c` is exactly this check.

## Wire payload encodings

All fields little-endian, packed with no implicit padding (see
`src/core/wire_cursor.h`). Fixed-size `char[]` fields are written verbatim,
NUL-padded. Variable-length fields are always preceded by their own count.

| Payload | Encodes (in field order) |
|---|---|
| `obicall_observation_t` | struct_size, schema_version, sensor_id[32], source_boot_id, sequence, sample_time_ns, arrival_time_ns, clock_domain, time_uncertainty_s, coordinate_frame, units, calibration_version, payload_shape, payload_count, payload[payload_count], covariance_count, covariance[covariance_count] |
| `obicall_result_t` | struct_size, schema_version, pipeline_id[32], window_seq, broker_id, epoch, status, payload_shape, payload_count, payload[…], covariance_count, covariance[…], input_digest[32], config_digest[32], timestamp_ns, valid_until_ns, source_count, sources[source_count][32], dgt_action_id |
| `obicall_checkpoint_t` | struct_size, schema_version, pipeline_id[32], window_seq, estimator_mode, state_dim, state_mean[state_dim], state_covariance[state_dim²], calibration_version, policy_version, dgt_action_id, source_count, {sensor_id[32], source_boot_id, last_sequence} × source_count, created_at_ns |
| `obicall_event_t` | struct_size, schema_version, event_type, reserved, timestamp_ns, payload_len, payload[payload_len] |
| Control messages (`auth_hello`, `heartbeat`, `epoch_grant`, `epoch_revoke`, `readiness_report`, `replay_request`, `admission_decision`, `result_ack`, `status_reply`) | See the corresponding `obicall_msg_*_t` struct in `wire.h` — each is a flat, fixed-size encoding of its fields in declaration order |

Every decoder rejects (rather than reading out of bounds on) a payload whose
declared count exceeds the type's static maximum
(`OBICALL_MAX_PAYLOAD_DOUBLES`, `OBICALL_MAX_COV_DOUBLES`,
`OBICALL_MAX_SOURCES`, …) before touching that many array slots —
`tests/unit/test_wire.c`'s forged-payload-count test and
`tests/fuzz/fuzz_wire_decode.c` both check this directly.

## Status codes

`obicall_status_t` is an `int32_t`-backed enum (`status.h`), grouped by
range: `1–19` argument/contract errors, `20–39` ABI/plugin errors, `40–59`
validation/admission errors, `60–79` wire/IO errors, `80–99` gate/publication
errors, `100–119` process/runtime errors, `120–129` DGT policy errors.
`obicall_status_string()` returns a static, human-readable string for any of
them (including unrecognized values, which get `"unknown status code"`
rather than undefined behavior).

## Manifest schema

```jsonc
{
  "schema_version": 1,
  "name": "provider_c_sim_position",     // unique key other manifests depend on by
  "version": "0.1.0",
  "language": "c",                        // "c" | "python"
  "abi_version_major": 1,
  "abi_version_minor": 0,
  "architecture": "x86_64",                // empty string = no architecture check
  "capability_flags": ["position_sensor", "checkpoint", "fault_injection"],
  "capability_slot": "position_sensor",    // empty = not a fungible-alternatives group
  "preference_score": 1.0,                 // lower ranks first among eligible alternatives
  "artifact_path": "provider_c_sim",       // relative to this manifest's directory; the
                                            // platform's .dll/.so/.dylib suffix is appended
                                            // automatically if this has no known suffix already
  "artifact_sha256": "…64 hex chars…",     // optional; omit to skip the integrity check
  "config_schema_version": 1,
  "config": { "...": "provider-defined" },
  "dependencies": ["some_other_manifest_name"]   // all mandatory; see obicall_manifest_resolve
}
```

`dependencies` lists other manifests' `name` fields that **must** resolve for
this one to be usable — `obicall_manifest_resolve` computes the full
transitive closure via topological sort (Kahn's algorithm) and rejects a
cycle or a missing entry outright, rather than finding *a* path and calling
it done. This is a deliberate correction of a real flaw found while
inspecting the `dynamic-cabi-loader` reference architecture: its resolver
finds one shortest path from a root to the requested module, which can
silently omit a second, unrelated mandatory dependency.
`tests/unit/test_manifest.c::test_resolve_requires_all_mandatory_dependencies_not_just_one_path`
is exactly this scenario, passing against Obicall's resolver.

## Pipeline config schema

See `examples/position-fusion.json` for a complete, working example and
`src/supervisor/pipeline_config.c` for the loader. Top-level keys:
`pipeline_id`, `manifests_dir` (relative to the config file's own directory),
`journal` (`reorder_window`, `max_lateness_ms`, `max_pending`), `gate`
(`result_validity_ms`, `suspect_timeout_ms`, `confirm_timeout_ms`,
`max_replay_gap`), `estimator` (Kalman parameters), `dgt` (`enabled`,
`hysteresis_margin`), `sensor_ids` (ordered list — position in this array is
the bit index a DGT action's `sensor_subset_mask` refers to), and
`providers` (array of `{manifest, worker_name, instance_seed}`).

## Exit codes (`obicall-workerd`)

| Code | Meaning |
|---|---|
| 2 | Bad arguments, or the manifest failed to parse |
| 3 | Artifact SHA-256 mismatch |
| 4 | Dynamic load failed (file missing, or a real dependency the OS couldn't resolve) |
| 5 | `obicall_plugin_query_v1` symbol not found |
| 6 | Plugin query call itself failed |
| 7 | Descriptor/vtable failed ABI validation |
| 8 | `vtable->create()` failed |
| 9 | Could not connect/authenticate to the journal |

## Trust model

Summarized because it is easy to conflate with "secure": this is a
**local, single-user desktop prototype**. The run token authenticates
connections to a session's own sockets against *other, unrelated local
processes* — it is not a defense against an attacker who already has
arbitrary code execution as the same OS user (who could, for instance, just
read the token file out of the runtime directory, exactly as `obicall
status` legitimately does). Artifact hashing establishes integrity against
accidental corruption or tampering of a specific file, not publisher
identity — there is no signature scheme here. Both of these are documented
scope boundaries, not gaps discovered after the fact.
