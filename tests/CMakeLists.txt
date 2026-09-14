# --- Unit tests: pure logic, no subprocesses, no network -------------------

set(OBICALL_UNIT_TESTS
    test_wire
    test_observation
    test_kalman
    test_dgt
    test_journal
    test_manifest
    test_gate
)

foreach(t ${OBICALL_UNIT_TESTS})
    add_executable(${t} unit/${t}.c)
    target_include_directories(${t} PRIVATE unit ${PROJECT_SOURCE_DIR}/src/core)
    target_link_libraries(${t} PRIVATE Obicall::core)
    if(UNIX)
        target_link_libraries(${t} PRIVATE m)
    endif()
    add_test(NAME ${t} COMMAND $<TARGET_FILE:${t}>)
    set_tests_properties(${t} PROPERTIES TIMEOUT 30)
endforeach()

# --- Fixture: a deliberately ABI-incompatible provider module --------------

add_library(provider_bad_abi MODULE fixtures/provider_bad_abi.c)
set_target_properties(provider_bad_abi PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
target_link_libraries(provider_bad_abi PRIVATE Obicall::core)
add_custom_command(TARGET provider_bad_abi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:provider_bad_abi> ${PROJECT_SOURCE_DIR}/tests/fixtures/
)

# --- Integration tests: real subprocesses, real dynamic loading, real IPC --

set(OBICALL_TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/test_runtime")
file(MAKE_DIRECTORY ${OBICALL_TEST_RUNTIME_DIR})

find_program(OBICALL_PYTHON_EXE NAMES python3 python)

function(obicall_add_integration_test name)
    add_executable(${name} integration/${name}.c)
    target_include_directories(${name} PRIVATE
        integration
        ${PROJECT_SOURCE_DIR}/src/osal
        ${PROJECT_SOURCE_DIR}/src/common
    )
    target_link_libraries(${name} PRIVATE Obicall::core obicall_osal obicall_procutil)
    target_compile_definitions(${name} PRIVATE
        OBICALL_WORKERD_PATH="$<TARGET_FILE:obicall-workerd>"
        OBICALL_GATED_PATH="$<TARGET_FILE:obicall-gated>"
        OBICALL_TEST_MANIFEST_C_SIM="${PROJECT_SOURCE_DIR}/examples/providers/provider_c_sim.manifest.json"
        OBICALL_TEST_MANIFEST_PY="${PROJECT_SOURCE_DIR}/examples/providers/provider_py_inertial.manifest.json"
        OBICALL_TEST_MANIFEST_BAD_ABI="${PROJECT_SOURCE_DIR}/tests/fixtures/provider_bad_abi.manifest.json"
        OBICALL_TEST_PROVIDER_ARTIFACT_PATH="$<TARGET_FILE:provider_c_sim>"
        OBICALL_TEST_PY_SCRIPT="${PROJECT_SOURCE_DIR}/python/obicall/provider_worker.py"
        OBICALL_TEST_CORE_LIB_PATH="$<TARGET_FILE:Obicall::core>"
        OBICALL_TEST_RUNTIME_DIR="${OBICALL_TEST_RUNTIME_DIR}"
    )
    add_dependencies(${name} obicall-workerd obicall-gated provider_c_sim provider_bad_abi)
    add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

obicall_add_integration_test(test_worker_abi_roundtrip)
obicall_add_integration_test(test_incompatible_abi)
obicall_add_integration_test(test_altered_artifact)
obicall_add_integration_test(test_gate_restart)
obicall_add_integration_test(test_worker_crash_hook)

obicall_add_integration_test(test_python_abi_roundtrip)
set_tests_properties(test_python_abi_roundtrip PROPERTIES SKIP_RETURN_CODE 125 TIMEOUT 60)
if(OBICALL_PYTHON_EXE)
    set_tests_properties(test_python_abi_roundtrip PROPERTIES ENVIRONMENT "OBICALL_PYTHON=${OBICALL_PYTHON_EXE}")
endif()

# --- CLI smoke tests: exercise the documented commands end to end ----------

add_test(NAME cli_doctor COMMAND $<TARGET_FILE:obicall_cli> doctor --json)

add_test(NAME cli_validate_example
    COMMAND $<TARGET_FILE:obicall_cli> validate --config ${PROJECT_SOURCE_DIR}/examples/position-fusion.json --json)

add_test(NAME cli_replay_fixture
    COMMAND $<TARGET_FILE:obicall_cli> replay --input ${PROJECT_SOURCE_DIR}/recordings/demo.obr --json)

# All of these are ordinary add_test() invocations of already-built
# binaries, not CMake targets themselves, so there is nothing here for
# add_dependencies to attach to; `cmake --build` (the default "all"
# target) already builds obicall_cli and every process/provider binary
# these tests exec, before `ctest` is ever run as a separate step.
add_test(NAME cli_demo_broker_failover
    COMMAND $<TARGET_FILE:obicall_cli> demo --scenario broker-failover
            --config ${PROJECT_SOURCE_DIR}/examples/position-fusion.json --json)
set_tests_properties(cli_demo_broker_failover PROPERTIES TIMEOUT 60)
if(OBICALL_PYTHON_EXE)
    set_tests_properties(cli_demo_broker_failover PROPERTIES ENVIRONMENT "OBICALL_PYTHON=${OBICALL_PYTHON_EXE}")
endif()

if(NOT OBICALL_PYTHON_EXE)
    message(WARNING "No python3/python found at configure time - test_python_abi_roundtrip will skip itself, "
                     "and cli_demo_broker_failover will fail at its python-provider spawn step unless "
                     "OBICALL_PYTHON is set in the test environment.")
endif()
