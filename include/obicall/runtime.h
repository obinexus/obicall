#ifndef OBICALL_RUNTIME_H
#define OBICALL_RUNTIME_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/status.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/* Idempotent; safe to call more than once (each call after the first is a
 * no-op returning OBICALL_OK). Not thread-safe against concurrent first
 * calls - call once from the process's main thread before using any other
 * obicall_* function. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_global_init(void);
OBICALL_API void OBICALL_CALL obicall_global_shutdown(void);

OBICALL_API const char* OBICALL_CALL obicall_version_string(void);
OBICALL_API void OBICALL_CALL obicall_abi_version(uint32_t* out_major, uint32_t* out_minor);

OBICALL_END_DECLS

#endif /* OBICALL_RUNTIME_H */
