#include "obicall/plugin.h"
#include "obicall/status.h"

/* Deliberately declares an incompatible ABI major version, and a vtable
 * missing a required entry, so tests/integration/test_incompatible_abi.c
 * can verify obicall-workerd's descriptor/vtable validation actually
 * rejects a bad plugin instead of crashing or loading it anyway. */

static const obicall_descriptor_t g_descriptor = {sizeof(obicall_descriptor_t),
                                                    OBICALL_ABI_VERSION_MAJOR + 1, /* wrong on purpose */
                                                    0,
                                                    1,
                                                    1,
                                                    1,
                                                    0,
                                                    OBICALL_CAP_POSITION_SENSOR,
                                                    "provider_bad_abi",
                                                    "0.0.0"};

static const obicall_provider_vtable_t g_vtable = {
    sizeof(obicall_provider_vtable_t), 0, NULL /* missing create() on purpose */, NULL, NULL, NULL, NULL, NULL, NULL};

OBICALL_EXPORT obicall_status_t OBICALL_CALL obicall_plugin_query_v1(
    const obicall_descriptor_t** out_descriptor, const obicall_provider_vtable_t** out_vtable) {
    if (!out_descriptor || !out_vtable) return OBICALL_ERR_NULL_POINTER;
    *out_descriptor = &g_descriptor;
    *out_vtable = &g_vtable;
    return OBICALL_OK;
}
