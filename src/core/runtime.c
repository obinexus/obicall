#include "obicall/runtime.h"
#include "obicall/plugin.h"

#ifndef OBICALL_VERSION_STRING
#define OBICALL_VERSION_STRING "0.0.0-dev"
#endif

static int g_initialized = 0;

obicall_status_t OBICALL_CALL obicall_global_init(void) {
    if (g_initialized) return OBICALL_OK;
    g_initialized = 1;
    return OBICALL_OK;
}

void OBICALL_CALL obicall_global_shutdown(void) { g_initialized = 0; }

const char* OBICALL_CALL obicall_version_string(void) { return OBICALL_VERSION_STRING; }

void OBICALL_CALL obicall_abi_version(uint32_t* out_major, uint32_t* out_minor) {
    if (out_major) *out_major = OBICALL_ABI_VERSION_MAJOR;
    if (out_minor) *out_minor = OBICALL_ABI_VERSION_MINOR;
}
