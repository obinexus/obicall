cmake_minimum_required(VERSION 3.20)

project(obicall
    VERSION 0.1.1
    DESCRIPTION "OBINexus dynamic dual-FFI polyglot runtime for sensor fusion"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# All executables, the core DLL/so/dylib, and dynamically-loaded provider
# modules land in one directory. On Windows this is load-bearing, not
# cosmetic: obicall-workerd loads provider_c_sim.dll via
# LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DEFAULT_DIRS), which searches
# only the loading module's own directory, the application directory, and
# system directories - never PATH or the CWD - so obicall.dll and every
# provider module must sit next to the .exe that loads them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(OBICALL_ENABLE_TESTS "Build the test suite" ON)
option(OBICALL_ENABLE_SANITIZERS "Build with -fsanitize=address,undefined (GCC/Clang debug builds)" OFF)
option(OBICALL_ENABLE_FUZZING "Build fuzz targets" OFF)
option(OBICALL_TEST_HOOKS "Compile in deliberate crash/hang fault injection for tests" ${OBICALL_ENABLE_TESTS})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(GenerateExportHeader)

if(OBICALL_ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "OBICALL_ENABLE_SANITIZERS requested but compiler ${CMAKE_C_COMPILER_ID} is not GCC/Clang; ignoring")
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpointer-arith)
endif()

add_subdirectory(src/core)
add_subdirectory(src/osal)
add_subdirectory(src/common)
add_subdirectory(src/gate)
add_subdirectory(src/journal)
add_subdirectory(src/broker)
add_subdirectory(src/worker)
add_subdirectory(src/supervisor)
add_subdirectory(src/cli)
add_subdirectory(src/providers/c_sim)

if(OBICALL_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(OBICALL_ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

# --- Package install / export ------------------------------------------------

install(FILES examples/position-fusion.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/examples)

# Provider manifests referenced by examples/position-fusion.json's own
# "manifests_dir": "providers" - without these an installed `obicall run`
# or `obicall demo` against the installed example config fails at manifest
# load, even though the example config file itself was installed above.
install(FILES
    examples/providers/provider_c_sim.manifest.json
    examples/providers/provider_py_inertial.manifest.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/examples/providers)

install(FILES recordings/demo.obr
    DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/recordings)

# The Python language adapter (src/supervisor/supervisor.c's
# `resolve_python_exe`/script_path logic looks for it at
# ${prefix}/share/obicall/python/obicall/provider_worker.py, relative to
# its own installed bin/ directory) - installed as a package directory,
# not a loose script, since provider_worker.py imports its sibling
# obicall.abi module.
install(DIRECTORY python/obicall
    DESTINATION ${CMAKE_INSTALL_DATADIR}/obicall/python
    FILES_MATCHING PATTERN "*.py"
    PATTERN "__pycache__" EXCLUDE
)

install(FILES
    docs/ABI.md
    docs/ARCHITECTURE.md
    docs/DGT.md
    docs/FAULT_TOLERANCE.md
    docs/IMPLEMENTATION_STATUS.md
    docs/VALIDATION.md
    docs/RELEASING_MSYS2.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/obicall)

install(FILES LICENSE README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/obicall)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ObicallConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ObicallConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ObicallConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/obicall")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ObicallConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ObicallConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/obicall")

install(EXPORT ObicallTargets
    FILE ObicallTargets.cmake
    NAMESPACE Obicall::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/obicall")

message(STATUS "obicall ${PROJECT_VERSION} : tests=${OBICALL_ENABLE_TESTS} sanitizers=${OBICALL_ENABLE_SANITIZERS} fuzzing=${OBICALL_ENABLE_FUZZING} test-hooks=${OBICALL_TEST_HOOKS}")
