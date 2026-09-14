# Usage: cmake -DOBICALL_CHECK_DIR=<build-dir> -P cmake/CheckBuildEnv.cmake
#
# Invoked by the root Makefile's `configure` target, before it touches the
# build directory. If OBICALL_CHECK_DIR already holds a configured cache,
# this warns (does not fail) when that cache's compiler looks like an
# MSYS2/MinGW toolchain (ucrt64/mingw64/clang64/msys64 in its path) but the
# *current* shell does not have that MSYS2 environment active (MSYSTEM is
# unset).
#
# This is not a hypothetical: reusing a UCRT64-configured cache from plain
# PowerShell was tested directly while building this Makefile. CMake finds
# the cached absolute compiler path fine and configure succeeds, but the
# actual compile step then fails with no useful output at all - the
# compiler driver (cc.exe) can run standalone (e.g. `cc --version` works),
# but the real compiler back end it execs needs its own runtime DLLs found
# via PATH, which are only on PATH inside an activated MSYS2 shell. The
# failure surfaces deep inside a compile step, which is a much more
# confusing place to discover a toolchain/environment mismatch than here.

if(NOT DEFINED OBICALL_CHECK_DIR)
  message(FATAL_ERROR "CheckBuildEnv.cmake: OBICALL_CHECK_DIR was not set")
endif()

set(_cache "${OBICALL_CHECK_DIR}/CMakeCache.txt")
if(NOT EXISTS "${_cache}")
  return() # nothing configured yet in this directory - nothing to check
endif()

file(STRINGS "${_cache}" _compiler_line REGEX "^CMAKE_C_COMPILER:[A-Za-z]+=")
if(NOT _compiler_line)
  return()
endif()

# The cache entry's type (normally FILEPATH) can end up as STRING instead
# after a failed compiler check writes it back differently - match either
# rather than hardcoding one, and strip whichever type was actually there.
string(REGEX REPLACE "^CMAKE_C_COMPILER:[A-Za-z]+=" "" _compiler "${_compiler_line}")
string(TOLOWER "${_compiler}" _compiler_lower)

set(_looks_like_msys2 FALSE)
foreach(_marker ucrt64 mingw64 mingw32 clang64 msys64)
  if(_compiler_lower MATCHES "${_marker}")
    set(_looks_like_msys2 TRUE)
  endif()
endforeach()

if(_looks_like_msys2 AND "$ENV{MSYSTEM}" STREQUAL "")
  message(WARNING
    "The build directory '${OBICALL_CHECK_DIR}' was previously configured "
    "with an MSYS2/MinGW compiler:\n"
    "    ${_compiler}\n"
    "but this shell does not have that MSYS2 environment active "
    "(the MSYSTEM environment variable is unset).\n"
    "The compiler driver can appear to work here (e.g. running it with "
    "--version succeeds) while real compilation still fails, because the "
    "compiler's back end cannot find its own runtime DLLs outside that "
    "environment - the failure shows up mid-build with little useful "
    "output, not here.\n"
    "Fix: run make from the matching MSYS2 shell (e.g. C:\\msys64\\ucrt64.exe, "
    "or a UCRT64 'MSYS2 MinGW UCRT64' shortcut), or build into a separate "
    "directory for this shell's own toolchain, e.g.:\n"
    "    make BUILD_DIR=build-msvc")
endif()
