#ifndef OBICALL_PLUGIN_H
#define OBICALL_PLUGIN_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/observation.h"
#include "obicall/event.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

#define OBICALL_ABI_VERSION_MAJOR 1u
#define OBICALL_ABI_VERSION_MINOR 0u

/* Symbol every provider artifact must export. The loader resolves this
 * symbol, calls it, and validates the returned descriptor before touching
 * the vtable - see docs/ABI.md "Loading sequence". */
#define OBICALL_PLUGIN_QUERY_SYMBOL "obicall_plugin_query_v1"

typedef struct obicall_provider* obicall_provider_handle_t;

/* capability_flags bits. A worker only routes a pipeline slot to a
 * provider whose flags are a superset of what the slot's manifest entry
 * requires. */
#define OBICALL_CAP_POSITION_SENSOR (1ull << 0)
#define OBICALL_CAP_INERTIAL_SENSOR (1ull << 1)
#define OBICALL_CAP_RANGE_SENSOR (1ull << 2)
#define OBICALL_CAP_SUPPORTS_CHECKPOINT (1ull << 3)
#define OBICALL_CAP_SUPPORTS_FAULT_INJECTION (1ull << 4)

/* Returned by obicall_plugin_query_v1. struct_size must be checked by the
 * loader before reading any field beyond it (see docs/ABI.md) so a future
 * ABI minor version can append fields without breaking older loaders. */
typedef struct obicall_descriptor {
    uint32_t struct_size;
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    uint32_t wire_schema_version;
    uint32_t checkpoint_schema_version;
    uint32_t config_schema_version;
    uint32_t reserved;
    uint64_t capability_flags;
    char provider_name[OBICALL_PROVIDER_NAME_LEN];
    char provider_version[OBICALL_VERSION_STR_LEN];
} obicall_descriptor_t;

/* JSON text from the provider's manifest "config" object, validated by the
 * loader against config_schema_version before this is handed to create().
 * Ownership: borrowed, valid only for the duration of the create() call. */
typedef struct obicall_provider_config {
    uint32_t struct_size;
    uint32_t config_schema_version;
    obicall_buffer_t config_json;
    uint64_t instance_seed; /* deterministic seed for simulated/reference providers */
} obicall_provider_config_t;

typedef obicall_status_t(OBICALL_CALL* obicall_provider_create_fn)(
    const obicall_provider_config_t* config, obicall_provider_handle_t* out_handle);

/* Application-to-provider request: the other half of the dual FFI
 * contract. Borrowed input - obs is not read after the call returns. */
typedef obicall_status_t(OBICALL_CALL* obicall_provider_submit_fn)(
    obicall_provider_handle_t handle, const obicall_observation_t* obs);

/* Drains up to the provider's internal queue, invoking on_event once per
 * queued event, synchronously, before returning. Never blocks waiting for
 * new events. Returns the number of events delivered via *out_count. */
typedef void(OBICALL_CALL* obicall_event_callback_fn)(void* user_data,
                                                        const obicall_event_t* event);
typedef obicall_status_t(OBICALL_CALL* obicall_provider_poll_events_fn)(
    obicall_provider_handle_t handle, obicall_event_callback_fn on_event,
    void* user_data, uint32_t* out_count);

/* Produces a pointer-free, versioned checkpoint blob (provider-defined
 * encoding beyond the version header) via the allocator the provider
 * itself controls; release it with release_buffer, not free(). */
typedef obicall_status_t(OBICALL_CALL* obicall_provider_checkpoint_fn)(
    obicall_provider_handle_t handle, obicall_owned_buffer_t* out_blob);

typedef obicall_status_t(OBICALL_CALL* obicall_provider_restore_fn)(
    obicall_provider_handle_t handle, obicall_buffer_t blob);

typedef void(OBICALL_CALL* obicall_release_buffer_fn)(obicall_owned_buffer_t* buf);

typedef void(OBICALL_CALL* obicall_provider_destroy_fn)(obicall_provider_handle_t handle);

/* struct_size is the ABI evolution mechanism for this table: a loader
 * built against a newer header must only call through offsets it knows
 * are present, i.e. offset < struct_size as reported by the plugin. */
typedef struct obicall_provider_vtable {
    uint32_t struct_size;
    uint32_t reserved;
    obicall_provider_create_fn create;
    obicall_provider_submit_fn submit;
    obicall_provider_poll_events_fn poll_events;
    obicall_provider_checkpoint_fn checkpoint;
    obicall_provider_restore_fn restore;
    obicall_release_buffer_fn release_buffer;
    obicall_provider_destroy_fn destroy;
} obicall_provider_vtable_t;

/* Entry point signature. Implementations must return static storage (not
 * heap-allocated) for both out parameters - there is no release call for
 * the descriptor or vtable themselves, only for buffers/handles they
 * produce. */
typedef obicall_status_t(OBICALL_CALL* obicall_plugin_query_v1_fn)(
    const obicall_descriptor_t** out_descriptor, const obicall_provider_vtable_t** out_vtable);

OBICALL_END_DECLS

#endif /* OBICALL_PLUGIN_H */
