#ifndef OBICALL_MANIFEST_H
#define OBICALL_MANIFEST_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/observation.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

#define OBICALL_MANIFEST_SCHEMA_VERSION 1u
#define OBICALL_MAX_DEPENDENCIES 8u
#define OBICALL_MAX_MANIFESTS 64u
#define OBICALL_MAX_NAME_LEN 64u
#define OBICALL_MAX_PATH_LEN 260u
#define OBICALL_MAX_ARCH_LEN 16u
#define OBICALL_MAX_CONFIG_JSON_LEN 2048u

typedef enum obicall_provider_language {
    OBICALL_LANG_UNSPECIFIED = 0,
    OBICALL_LANG_C = 1,
    OBICALL_LANG_PYTHON = 2
} obicall_provider_language_t;

/* One provider's declared identity, requirements, and (optionally) its
 * membership in a fungible "capability_slot" of interchangeable
 * alternatives. Parsed from <name>.manifest.json by
 * obicall_manifest_parse_json; see docs/ABI.md "Manifest schema". */
typedef struct obicall_manifest {
    uint32_t struct_size;
    uint32_t schema_version;

    char name[OBICALL_MAX_NAME_LEN]; /* unique key other manifests depend on by */
    char version[OBICALL_VERSION_STR_LEN];
    uint32_t language;
    uint32_t abi_version_major;
    uint32_t abi_version_minor;
    char architecture[OBICALL_MAX_ARCH_LEN]; /* "x86_64", "arm64"; must match host */
    uint64_t capability_flags;

    char capability_slot[OBICALL_MAX_NAME_LEN]; /* empty = not an alternatives group */
    double preference_score; /* lower ranks first among eligible alternatives in a slot */

    char artifact_path[OBICALL_MAX_PATH_LEN]; /* relative to the manifest's directory */
    uint8_t artifact_sha256[OBICALL_DIGEST_LEN];
    uint32_t has_artifact_sha256;

    uint32_t config_schema_version;
    char config_json[OBICALL_MAX_CONFIG_JSON_LEN];

    uint32_t dependency_count; /* all mandatory: every entry must resolve */
    char dependencies[OBICALL_MAX_DEPENDENCIES][OBICALL_MAX_NAME_LEN];
} obicall_manifest_t;

typedef struct obicall_manifest_set {
    uint32_t count;
    obicall_manifest_t items[OBICALL_MAX_MANIFESTS];
} obicall_manifest_set_t;

/* out_issue_detail (if non-NULL) receives a NUL-terminated human-readable
 * reason on failure; detail_cap is its capacity in bytes. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_manifest_parse_json(
    const uint8_t* json, uint32_t len, obicall_manifest_t* out, char* out_issue_detail,
    uint32_t detail_cap);

OBICALL_API obicall_status_t OBICALL_CALL obicall_manifest_load_dir(
    const char* dir_path, obicall_manifest_set_t* out_set, char* out_issue_detail,
    uint32_t detail_cap);

typedef struct obicall_resolution_plan {
    uint32_t count;
    uint32_t order[OBICALL_MAX_MANIFESTS]; /* indices into the manifest_set, load order */
} obicall_resolution_plan_t;

/* Computes the transitive closure of every mandatory dependency reachable
 * from requested_names, topologically sorted (Kahn's algorithm) so a
 * dependency always precedes its dependents in order[]. Fails with
 * OBICALL_ERR_DEPENDENCY_CYCLE (out_issue_detail names the cycle) or
 * OBICALL_ERR_DEPENDENCY_MISSING (names the absent manifest) rather than
 * silently resolving a partial graph. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_manifest_resolve(
    const obicall_manifest_set_t* set, const char* const* requested_names,
    uint32_t requested_count, obicall_resolution_plan_t* out_plan, char* out_issue_detail,
    uint32_t detail_cap);

/* Among manifests in capability_slot, returns indices of those eligible
 * (architecture, ABI, integrity, and full dependency resolution all pass),
 * ordered by ascending preference_score, ties broken by name. Ineligible
 * alternatives are omitted, not ranked last - eligibility is a
 * prerequisite to ranking, never traded off against it. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_manifest_rank_alternatives(
    const obicall_manifest_set_t* set, const char* capability_slot, const char* host_architecture,
    uint32_t* out_indices, uint32_t out_cap, uint32_t* out_count);

OBICALL_END_DECLS

#endif /* OBICALL_MANIFEST_H */
