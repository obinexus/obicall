#ifndef OBICALL_EVENT_H
#define OBICALL_EVENT_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"

OBICALL_BEGIN_DECLS

#define OBICALL_EVENT_SCHEMA_VERSION 1u

/* Provider-to-application direction of the dual FFI contract. payload is
 * borrowed and valid only for the duration of the poll_events call that
 * delivered it (see plugin.h) - copy it before returning if you need it
 * afterward. */
typedef struct obicall_event {
    uint32_t struct_size;
    uint32_t schema_version;
    uint32_t event_type; /* provider-defined, documented per provider capability */
    uint32_t reserved;
    int64_t timestamp_ns;
    obicall_buffer_t payload;
} obicall_event_t;

OBICALL_END_DECLS

#endif /* OBICALL_EVENT_H */
