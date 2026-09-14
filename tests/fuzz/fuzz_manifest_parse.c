#include "obicall/obicall.h"
#include "fuzz_driver.h"

/* Provider manifests are untrusted-ish input in the sense that a
 * malformed or hostile one must be rejected cleanly, not crash the
 * loader - this fuzzes obicall_manifest_parse_json (and, through it, the
 * hand-rolled JSON scanner in src/core/json_min.c) directly with
 * arbitrary bytes. */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    obicall_manifest_t manifest;
    char detail[256];
    obicall_manifest_parse_json(data, (uint32_t)size, &manifest, detail, sizeof(detail));
    return 0;
}
