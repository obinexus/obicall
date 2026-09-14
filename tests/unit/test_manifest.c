#include "obicall_test.h"
#include "obicall/obicall.h"

static void add_manifest(obicall_manifest_set_t* set, const char* name, const char* arch,
                          const char* capability_slot, double preference_score, const char** deps, uint32_t ndeps) {
    obicall_manifest_t* m = &set->items[set->count++];
    memset(m, 0, sizeof(*m));
    m->struct_size = sizeof(*m);
    m->schema_version = OBICALL_MANIFEST_SCHEMA_VERSION;
    strncpy(m->name, name, OBICALL_MAX_NAME_LEN - 1);
    strncpy(m->version, "1.0.0", OBICALL_VERSION_STR_LEN - 1);
    m->language = OBICALL_LANG_C;
    m->abi_version_major = OBICALL_ABI_VERSION_MAJOR;
    m->abi_version_minor = OBICALL_ABI_VERSION_MINOR;
    if (arch) strncpy(m->architecture, arch, OBICALL_MAX_ARCH_LEN - 1);
    if (capability_slot) strncpy(m->capability_slot, capability_slot, OBICALL_MAX_NAME_LEN - 1);
    m->preference_score = preference_score;
    strncpy(m->artifact_path, "artifact", OBICALL_MAX_PATH_LEN - 1);
    m->dependency_count = ndeps;
    for (uint32_t i = 0; i < ndeps; ++i) strncpy(m->dependencies[i], deps[i], OBICALL_MAX_NAME_LEN - 1);
}

static void test_resolve_simple_chain(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* dep_b[] = {"b"};
    add_manifest(&set, "a", NULL, NULL, 0, dep_b, 1);
    add_manifest(&set, "b", NULL, NULL, 0, NULL, 0);

    const char* req[1] = {"a"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(plan.count, 2);
    /* b must precede a in load order. */
    int b_pos = -1, a_pos = -1;
    for (uint32_t i = 0; i < plan.count; ++i) {
        if (strcmp(set.items[plan.order[i]].name, "b") == 0) b_pos = (int)i;
        if (strcmp(set.items[plan.order[i]].name, "a") == 0) a_pos = (int)i;
    }
    OBICALL_CHECK(b_pos >= 0 && a_pos >= 0 && b_pos < a_pos);
}

/* This is exactly the scenario the reference dynamic-cabi-loader gets
 * wrong: it resolves a single shortest path from a root to the requested
 * module, which can omit a second, unrelated mandatory dependency. Here
 * "app" requires BOTH "logging" and "crypto" - a resolver that only
 * proves reachability of one of them is insufficient. */
static void test_resolve_requires_all_mandatory_dependencies_not_just_one_path(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* deps_app[] = {"logging", "crypto"};
    add_manifest(&set, "app", NULL, NULL, 0, deps_app, 2);
    add_manifest(&set, "logging", NULL, NULL, 0, NULL, 0);
    add_manifest(&set, "crypto", NULL, NULL, 0, NULL, 0);

    const char* req[1] = {"app"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(plan.count, 3);

    int has_logging = 0, has_crypto = 0;
    for (uint32_t i = 0; i < plan.count; ++i) {
        if (strcmp(set.items[plan.order[i]].name, "logging") == 0) has_logging = 1;
        if (strcmp(set.items[plan.order[i]].name, "crypto") == 0) has_crypto = 1;
    }
    OBICALL_CHECK(has_logging);
    OBICALL_CHECK(has_crypto);
}

static void test_resolve_transitive_dependencies(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* dep_b[] = {"b"};
    const char* dep_c[] = {"c"};
    add_manifest(&set, "a", NULL, NULL, 0, dep_b, 1);
    add_manifest(&set, "b", NULL, NULL, 0, dep_c, 1);
    add_manifest(&set, "c", NULL, NULL, 0, NULL, 0);

    const char* req[1] = {"a"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(plan.count, 3);
}

static void test_resolve_detects_cycle(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* dep_b[] = {"b"};
    const char* dep_c[] = {"c"};
    const char* dep_a[] = {"a"};
    add_manifest(&set, "a", NULL, NULL, 0, dep_b, 1);
    add_manifest(&set, "b", NULL, NULL, 0, dep_c, 1);
    add_manifest(&set, "c", NULL, NULL, 0, dep_a, 1); /* c -> a closes the cycle */

    const char* req[1] = {"a"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) ==
                  OBICALL_ERR_DEPENDENCY_CYCLE);
}

static void test_resolve_missing_dependency_reported(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* dep_missing[] = {"does_not_exist"};
    add_manifest(&set, "a", NULL, NULL, 0, dep_missing, 1);

    const char* req[1] = {"a"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) ==
                  OBICALL_ERR_DEPENDENCY_MISSING);
    OBICALL_CHECK(strstr(detail, "does_not_exist") != NULL);
}

static void test_resolve_requested_name_not_found(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    add_manifest(&set, "a", NULL, NULL, 0, NULL, 0);
    const char* req[1] = {"nonexistent"};
    obicall_resolution_plan_t plan;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_resolve(&set, req, 1, &plan, detail, sizeof(detail)) ==
                  OBICALL_ERR_DEPENDENCY_MISSING);
}

static void test_rank_alternatives_orders_by_preference(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    add_manifest(&set, "slow", "x86_64", "position_sensor", 5.0, NULL, 0);
    add_manifest(&set, "fast", "x86_64", "position_sensor", 1.0, NULL, 0);
    add_manifest(&set, "medium", "x86_64", "position_sensor", 3.0, NULL, 0);

    uint32_t indices[8];
    uint32_t count = 0;
    OBICALL_CHECK(obicall_manifest_rank_alternatives(&set, "position_sensor", "x86_64", indices, 8, &count) ==
                  OBICALL_OK);
    OBICALL_CHECK_EQ_INT(count, 3);
    OBICALL_CHECK(strcmp(set.items[indices[0]].name, "fast") == 0);
    OBICALL_CHECK(strcmp(set.items[indices[1]].name, "medium") == 0);
    OBICALL_CHECK(strcmp(set.items[indices[2]].name, "slow") == 0);
}

static void test_rank_alternatives_excludes_architecture_mismatch(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    add_manifest(&set, "native", "x86_64", "position_sensor", 1.0, NULL, 0);
    add_manifest(&set, "foreign", "arm64", "position_sensor", 0.5, NULL, 0); /* better score, wrong arch */

    uint32_t indices[8];
    uint32_t count = 0;
    OBICALL_CHECK(obicall_manifest_rank_alternatives(&set, "position_sensor", "x86_64", indices, 8, &count) ==
                  OBICALL_OK);
    OBICALL_CHECK_EQ_INT(count, 1);
    OBICALL_CHECK(strcmp(set.items[indices[0]].name, "native") == 0);
}

static void test_rank_alternatives_excludes_abi_mismatch(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    add_manifest(&set, "compatible", "x86_64", "position_sensor", 1.0, NULL, 0);
    add_manifest(&set, "wrong_major", "x86_64", "position_sensor", 0.1, NULL, 0);
    set.items[1].abi_version_major = OBICALL_ABI_VERSION_MAJOR + 1;
    add_manifest(&set, "too_new_minor", "x86_64", "position_sensor", 0.1, NULL, 0);
    set.items[2].abi_version_minor = OBICALL_ABI_VERSION_MINOR + 1;

    uint32_t indices[8];
    uint32_t count = 0;
    OBICALL_CHECK(obicall_manifest_rank_alternatives(&set, "position_sensor", "x86_64", indices, 8, &count) ==
                  OBICALL_OK);
    OBICALL_CHECK_EQ_INT(count, 1);
    OBICALL_CHECK(strcmp(set.items[indices[0]].name, "compatible") == 0);
}

/* Ineligibility (here: an unresolved mandatory dependency) must remove a
 * candidate outright, never just push it down the ranking - the flaw
 * this project's reference loader has. */
static void test_rank_alternatives_excludes_unresolvable_dependency(void) {
    obicall_manifest_set_t set;
    memset(&set, 0, sizeof(set));
    const char* dep_missing[] = {"missing_lib"};
    add_manifest(&set, "broken_but_cheap", "x86_64", "position_sensor", 0.1, dep_missing, 1);
    add_manifest(&set, "working_but_pricier", "x86_64", "position_sensor", 9.0, NULL, 0);

    uint32_t indices[8];
    uint32_t count = 0;
    OBICALL_CHECK(obicall_manifest_rank_alternatives(&set, "position_sensor", "x86_64", indices, 8, &count) ==
                  OBICALL_OK);
    OBICALL_CHECK_EQ_INT(count, 1);
    OBICALL_CHECK(strcmp(set.items[indices[0]].name, "working_but_pricier") == 0);
}

static void test_parse_json_manifest(void) {
    const char* json =
        "{"
        "\"schema_version\":1,"
        "\"name\":\"test_provider\","
        "\"version\":\"2.3.4\","
        "\"language\":\"python\","
        "\"abi_version_major\":1,"
        "\"abi_version_minor\":0,"
        "\"architecture\":\"x86_64\","
        "\"capability_flags\":[\"position_sensor\",\"checkpoint\"],"
        "\"capability_slot\":\"position_sensor\","
        "\"preference_score\":2.5,"
        "\"artifact_path\":\"provider.py\","
        "\"config_schema_version\":1,"
        "\"config\":{\"x\":1,\"nested\":{\"y\":2}},"
        "\"dependencies\":[\"dep_a\",\"dep_b\"]"
        "}";
    obicall_manifest_t m;
    char detail[256];
    obicall_status_t st = obicall_manifest_parse_json((const uint8_t*)json, (uint32_t)strlen(json), &m, detail,
                                                        sizeof(detail));
    OBICALL_CHECK(st == OBICALL_OK);
    OBICALL_CHECK(strcmp(m.name, "test_provider") == 0);
    OBICALL_CHECK(strcmp(m.version, "2.3.4") == 0);
    OBICALL_CHECK_EQ_INT(m.language, OBICALL_LANG_PYTHON);
    OBICALL_CHECK(strcmp(m.architecture, "x86_64") == 0);
    OBICALL_CHECK((m.capability_flags & OBICALL_CAP_POSITION_SENSOR) != 0);
    OBICALL_CHECK((m.capability_flags & OBICALL_CAP_SUPPORTS_CHECKPOINT) != 0);
    OBICALL_CHECK_NEAR(m.preference_score, 2.5, 1e-12);
    OBICALL_CHECK(strcmp(m.artifact_path, "provider.py") == 0);
    OBICALL_CHECK_EQ_INT(m.dependency_count, 2);
    OBICALL_CHECK(strcmp(m.dependencies[0], "dep_a") == 0);
    OBICALL_CHECK(strcmp(m.dependencies[1], "dep_b") == 0);
    OBICALL_CHECK(strstr(m.config_json, "\"nested\"") != NULL);
}

static void test_parse_json_rejects_missing_required_fields(void) {
    const char* json = "{\"schema_version\":1}"; /* missing "name" */
    obicall_manifest_t m;
    char detail[128];
    OBICALL_CHECK(obicall_manifest_parse_json((const uint8_t*)json, (uint32_t)strlen(json), &m, detail,
                                               sizeof(detail)) == OBICALL_ERR_INVALID_ARGUMENT);
}

OBICALL_TEST_MAIN_BEGIN()
    test_resolve_simple_chain();
    test_resolve_requires_all_mandatory_dependencies_not_just_one_path();
    test_resolve_transitive_dependencies();
    test_resolve_detects_cycle();
    test_resolve_missing_dependency_reported();
    test_resolve_requested_name_not_found();
    test_rank_alternatives_orders_by_preference();
    test_rank_alternatives_excludes_architecture_mismatch();
    test_rank_alternatives_excludes_abi_mismatch();
    test_rank_alternatives_excludes_unresolvable_dependency();
    test_parse_json_manifest();
    test_parse_json_rejects_missing_required_fields();
OBICALL_TEST_MAIN_END()
