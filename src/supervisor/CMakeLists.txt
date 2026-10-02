add_library(obicall_supervisor STATIC
    supervisor.c
    pipeline_config.c
    ${PROJECT_SOURCE_DIR}/src/core/json_min.c
)
target_include_directories(obicall_supervisor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/osal
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/core
)
target_link_libraries(obicall_supervisor PUBLIC Obicall::core obicall_osal obicall_procutil)

# Where the Python adapter's ctypes loader finds the core library (see
# supervisor.c): its real SONAME file name (obicall.so.0 / obicall.0.dylib
# - the core target has PREFIX "", so never "libobicall.*"), next to the
# binaries in a build tree, or at OBICALL_INSTALL_BIN_TO_LIBDIR (top-level
# CMakeLists.txt) relative to the installed bin/. Windows keeps its DLL in
# bin/ in both layouts and needs neither.
if(NOT WIN32)
    target_compile_definitions(obicall_supervisor PRIVATE
        OBICALL_CORE_LIB_FILE_NAME="$<TARGET_SONAME_FILE_NAME:obicall_core>"
        OBICALL_INSTALL_BIN_TO_LIBDIR="${OBICALL_INSTALL_BIN_TO_LIBDIR}")
endif()

# Dev convenience only: lets `obicall run`/`obicall demo` find the Python
# worker script when run straight out of an uninstalled build/bin/, before
# the install-relative path (bin/../share/obicall/python/obicall/...; see
# supervisor.c) exists anywhere. A *packaged* build must turn this off -
# it bakes this build's absolute source checkout path as a literal string
# into the obicall binary, which a release package must never ship (see
# the "no paths reference ... the source checkout" requirement in
# packaging/msys2/mingw-w64-obicall/PKGBUILD and docs/RELEASING_MSYS2.md).
option(OBICALL_EMBED_SOURCE_DIR_FALLBACK
    "Embed this build's source directory as a Python worker fallback path (dev builds only - never for packaging)"
    ON)
if(OBICALL_EMBED_SOURCE_DIR_FALLBACK)
    target_compile_definitions(obicall_supervisor PRIVATE OBICALL_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
endif()
