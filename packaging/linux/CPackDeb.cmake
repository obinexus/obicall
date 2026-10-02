# Opt-in Debian package generation (`cpack -G DEB`), enabled with
# -DOBICALL_ENABLE_DEB_PACKAGING=ON from the top-level CMakeLists.txt.
# This only builds a .deb; publishing it in a signed APT repository is a
# separate step - see docs/RELEASING_LINUX.md.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "OBICALL_ENABLE_DEB_PACKAGING: Debian packages must be built on Linux")
endif()

# A package must never ship the worker's fault-injection hooks or this
# build machine's source checkout path (see OBICALL_TEST_HOOKS and
# OBICALL_EMBED_SOURCE_DIR_FALLBACK) - refuse to configure rather than
# silently package a dev build. Run the test suite from a separate,
# test-enabled build directory instead.
if(OBICALL_ENABLE_TESTS OR OBICALL_TEST_HOOKS OR OBICALL_EMBED_SOURCE_DIR_FALLBACK)
    message(FATAL_ERROR
        "OBICALL_ENABLE_DEB_PACKAGING requires -DOBICALL_ENABLE_TESTS=OFF -DOBICALL_TEST_HOOKS=OFF "
        "-DOBICALL_EMBED_SOURCE_DIR_FALLBACK=OFF")
endif()

# GNUInstallDirs picks the Debian multiarch libdir (lib/<triplet>) only for
# a /usr prefix, and the bin-relative RPATH/library lookups are computed at
# configure time - so configure for the prefix the package installs to.
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    message(FATAL_ERROR "OBICALL_ENABLE_DEB_PACKAGING requires -DCMAKE_INSTALL_PREFIX=/usr")
endif()

# Debian keeps a package's licence at /usr/share/doc/<package>/copyright.
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/obicall"
    RENAME copyright)

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME obicall)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "OBINexus")
set(CPACK_PACKAGE_CONTACT "Nnamdi Michael Okpala <obinexusmk2@proton.me>")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/obinexus/obicall")
set(CPACK_DEBIAN_PACKAGE_SECTION science)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
# Synopsis line: CPACK_PACKAGE_DESCRIPTION_SUMMARY (PROJECT_DESCRIPTION by
# default). CPack indents this extended text itself.
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Local desktop prototype of a dual-FFI polyglot sensor-fusion runtime:
replicated brokers, a bounded admission journal, an epoch-fenced
publication gate, C and Python provider workers behind a versioned C ABI,
and the obicall CLI (doctor, validate, run, status, replay, demo).")
# libc comes from dpkg-shlibdeps; python3 runs the Python provider adapter
# (resolve_python_exe() in src/supervisor/supervisor.c), which only uses
# the standard library plus this package's own core library via ctypes.
set(CPACK_DEBIAN_PACKAGE_DEPENDS python3)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
if(NOT DEFINED CPACK_DEBIAN_PACKAGE_RELEASE)
    set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)

include(CPack)
