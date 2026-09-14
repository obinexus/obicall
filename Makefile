# Obicall - root Makefile
#
# A thin, portable wrapper around the real build system, which is CMake
# (see CMakeLists.txt and cmake/). This file exists only to make the
# documented CMake workflow reachable by typing plain `make` - it does not
# duplicate the CMake target graph (see docs/ARCHITECTURE.md and
# README.md "Repository layout" for what actually builds what).
#
# GNU Make on Windows picks its recipe shell by searching for sh.exe; if
# none is found it falls back to cmd.exe - which shell that is has nothing
# to do with which shell *launched* `make`. Running `make` from PowerShell
# on this project's own dev machine, for example, resolves a GNU Make
# 3.81 build with no sh.exe on PATH, so recipes there run under cmd.exe;
# running it from an MSYS2 UCRT64 shell resolves a newer GNU Make that
# finds /usr/bin/sh.exe and runs recipes under a real POSIX shell. Every
# recipe below is written to mean the same thing under both: plain
# `$(CMAKE)`/`$(CTEST)` invocations with no shell operators beyond the one
# redirect (`2>&1`) that both dialects happen to agree on, and `cmake -E`
# for every directory/file operation instead of shell builtins like
# `mkdir -p`, `rm -rf`, or `if exist`.

# --------------------------------------------------------------------------
# Overridable variables (e.g. `make BUILD_TYPE=Release JOBS=8`)
# --------------------------------------------------------------------------

CMAKE          ?= cmake
CTEST          ?= ctest
BUILD_DIR      ?= build
BUILD_TYPE     ?= Debug
GENERATOR      ?=
INSTALL_PREFIX ?= $(CURDIR)/install
JOBS           ?=

# Dedicated directory for the `release` convenience target, so it can
# never collide with a Debug-configured $(BUILD_DIR) - see "Debug vs
# Release" in `make help`. Derived with `?=` (not `:=`) so it still tracks
# an overridden BUILD_DIR correctly.
BUILD_DIR_RELEASE ?= $(BUILD_DIR)-release

# --------------------------------------------------------------------------
# Derived flags
# --------------------------------------------------------------------------

ifneq ($(strip $(GENERATOR)),)
CONFIGURE_GENERATOR_FLAG := -G "$(GENERATOR)"
else
CONFIGURE_GENERATOR_FLAG :=
endif

ifneq ($(strip $(JOBS)),)
BUILD_PARALLEL_FLAG := --parallel $(JOBS)
TEST_PARALLEL_FLAG   := -j $(JOBS)
else
BUILD_PARALLEL_FLAG :=
TEST_PARALLEL_FLAG   :=
endif

# --------------------------------------------------------------------------
# Tool checks - report a missing cmake/ctest clearly instead of letting an
# unrelated shell error ("command not found" / "is not recognized") be the
# first thing a user sees. Skipped for a bare `make help`, since that
# should work even before CMake is installed.
# --------------------------------------------------------------------------

ifeq ($(MAKECMDGOALS),help)
NEEDS_CMAKE_CHECK :=
else
NEEDS_CMAKE_CHECK := 1
endif

ifdef NEEDS_CMAKE_CHECK
CMAKE_CHECK_OUTPUT := $(shell $(CMAKE) --version 2>&1)
ifeq ($(findstring cmake version,$(CMAKE_CHECK_OUTPUT)),)
$(error CMake was not found (tried to run: '$(CMAKE)'). Install CMake 3.20+ \
and ensure it is on PATH, or pass CMAKE=/full/path/to/cmake. \
See README.md "Platform prerequisites" for MSYS2 UCRT64 setup. \
Raw output was: $(CMAKE_CHECK_OUTPUT))
endif
endif

ifneq ($(filter test,$(MAKECMDGOALS)),)
CTEST_CHECK_OUTPUT := $(shell $(CTEST) --version 2>&1)
ifeq ($(findstring ctest version,$(CTEST_CHECK_OUTPUT)),)
$(error CTest was not found (tried to run: '$(CTEST)'). It ships with \
CMake - if cmake works but ctest does not, add CMake's bin directory to \
PATH, or pass CTEST=/full/path/to/ctest. Raw output was: $(CTEST_CHECK_OUTPUT))
endif
endif

# --------------------------------------------------------------------------
# .PHONY - every target here is an action, never a file this Makefile
# itself produces (CMake owns the real file-level dependency graph).
# --------------------------------------------------------------------------

.PHONY: all configure build debug release test install clean rebuild help

# `all` must stay the first real target in the file so it is picked up as
# the default goal by every GNU Make version, including ones too old to
# know about .DEFAULT_GOAL.
all: build

.DEFAULT_GOAL := all

# --------------------------------------------------------------------------
# configure - generate (or refresh) the CMake build system in $(BUILD_DIR).
# Safe to re-run: an existing cache is reused, never deleted. A generator
# passed via GENERATOR that conflicts with what the cache already recorded
# is rejected by CMake itself with an actionable message (it names the
# previously-used generator and tells you to remove the cache or pick a
# different binary directory) - this Makefile does not second-guess that.
# --------------------------------------------------------------------------

configure:
	$(CMAKE) -DOBICALL_CHECK_DIR="$(BUILD_DIR)" -P cmake/CheckBuildEnv.cmake
	$(CMAKE) -S . -B "$(BUILD_DIR)" $(CONFIGURE_GENERATOR_FLAG) -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)"

# --------------------------------------------------------------------------
# build - build every target CMakeLists.txt defines (the core shared
# library, obicall-gated/-journald/-brokerd/-workerd, the CLI, the
# provider modules, and the test binaries) via CMake's own default `all`
# target - not an invented, separately-maintained list of target names.
# --config is passed unconditionally: CMake documents it as ignored for a
# single-configuration generator (Ninja, Makefiles) and required for a
# multi-configuration one (Visual Studio, Xcode), so this one invocation
# is correct either way without this Makefile needing to know which kind
# $(GENERATOR) resolved to.
# --------------------------------------------------------------------------

build: configure
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(BUILD_TYPE)" $(BUILD_PARALLEL_FLAG)

# --------------------------------------------------------------------------
# debug / release - explicit single-configuration convenience builds.
# `debug` reuses the same $(BUILD_DIR) as `all`/`build` (BUILD_TYPE
# already defaults to Debug, so this does not disturb whatever is already
# configured there). `release` deliberately builds into a *separate*
# directory ($(BUILD_DIR_RELEASE)) so a Debug-configured $(BUILD_DIR) -
# such as this repository's own, already-configured `build/` - is never
# silently reconfigured out from under it. Each is a recursive `$(MAKE)`
# call so it goes through the exact same configure-then-build path as
# every other target, with BUILD_TYPE/BUILD_DIR overridden for that one
# invocation only.
# --------------------------------------------------------------------------

debug:
	$(MAKE) BUILD_TYPE=Debug BUILD_DIR="$(BUILD_DIR)" build

release:
	$(MAKE) BUILD_TYPE=Release BUILD_DIR="$(BUILD_DIR_RELEASE)" build

# --------------------------------------------------------------------------
# test - build first (a real prerequisite, so `make -j test` still waits
# for a successful build before ctest runs), then run the suite with
# failure output on. -C mirrors --config's build-time behavior: ignored
# for a single-configuration generator, required for a multi-configuration
# one.
# --------------------------------------------------------------------------

test: build
	$(CTEST) --test-dir "$(BUILD_DIR)" -C "$(BUILD_TYPE)" --output-on-failure $(TEST_PARALLEL_FLAG)

# --------------------------------------------------------------------------
# install - build first, then install to an overridable *local* prefix
# (default: ./install under this repository, not a system directory - so
# this never needs Administrator/root). cmake --install creates missing
# destination directories itself; the explicit make_directory below is
# just belt-and-suspenders and is a no-op if it already exists.
# --------------------------------------------------------------------------

install: build
	$(CMAKE) -E make_directory "$(INSTALL_PREFIX)"
	$(CMAKE) --install "$(BUILD_DIR)" --config "$(BUILD_TYPE)" --prefix "$(INSTALL_PREFIX)"

# --------------------------------------------------------------------------
# clean - remove compiled outputs via CMake's own generator-agnostic
# `clean` target (objects, binaries) without touching CMakeCache.txt or
# the generated build files, so the next build does not need to
# reconfigure from scratch. A conditional guards against running this
# against a directory that was never configured in the first place.
# --------------------------------------------------------------------------

clean:
ifneq ($(wildcard $(BUILD_DIR)/CMakeCache.txt),)
	$(CMAKE) --build "$(BUILD_DIR)" --target clean
else
	$(CMAKE) -E echo "Nothing to clean - $(BUILD_DIR) is not configured."
endif

# --------------------------------------------------------------------------
# rebuild - clean then build, guaranteed sequential. This is two recipe
# lines of *one* target, not a `clean build` prerequisite list: GNU Make
# only guarantees prerequisites all finish before their target's own
# recipe starts, not that they run in listed order relative to each other,
# so under `-j` a prerequisite-based `rebuild: clean build` could start
# building while cleaning was still in flight. Recipe lines within a
# single target, by contrast, always run one at a time in the order
# written, regardless of -j - which is what "must never run cleanup and
# compilation concurrently" actually requires here.
# --------------------------------------------------------------------------

rebuild:
	$(MAKE) clean
	$(MAKE) build

# --------------------------------------------------------------------------
# help
# --------------------------------------------------------------------------

help:
	@$(CMAKE) -E echo "Obicall - root Makefile"
	@$(CMAKE) -E echo ""
	@$(CMAKE) -E echo "Targets:"
	@$(CMAKE) -E echo "  make            configure and build (default target)"
	@$(CMAKE) -E echo "  make configure  generate/refresh the CMake build system only"
	@$(CMAKE) -E echo "  make build      build every project target (implies configure)"
	@$(CMAKE) -E echo "  make debug      build the Debug configuration, into BUILD_DIR"
	@$(CMAKE) -E echo "  make release    build the Release configuration, into BUILD_DIR_RELEASE"
	@$(CMAKE) -E echo "  make test       build, then run ctest with failure output shown"
	@$(CMAKE) -E echo "  make install    build, then install under INSTALL_PREFIX"
	@$(CMAKE) -E echo "  make clean      remove compiled outputs; keeps the CMake cache"
	@$(CMAKE) -E echo "  make rebuild    clean, then build (always sequential, never -j together)"
	@$(CMAKE) -E echo "  make help       this message"
	@$(CMAKE) -E echo ""
	@$(CMAKE) -E echo "Variables (default shown, override as VAR=value):"
	@$(CMAKE) -E echo "  CMAKE=cmake"
	@$(CMAKE) -E echo "  CTEST=ctest"
	@$(CMAKE) -E echo "  BUILD_DIR=build"
	@$(CMAKE) -E echo "  BUILD_TYPE=Debug"
	@$(CMAKE) -E echo "  GENERATOR=            (empty: let CMake pick - see README.md)"
	@$(CMAKE) -E echo "  INSTALL_PREFIX=<repo>/install"
	@$(CMAKE) -E echo "  JOBS=                 (empty: let the build tool pick its own default)"
	@$(CMAKE) -E echo "  BUILD_DIR_RELEASE=BUILD_DIR-release   (used only by 'make release')"
	@$(CMAKE) -E echo ""
	@$(CMAKE) -E echo "Examples:"
	@$(CMAKE) -E echo "  make JOBS=8"
	@$(CMAKE) -E echo "  make BUILD_DIR=build-ucrt64 GENERATOR=Ninja test"
	@$(CMAKE) -E echo "  make INSTALL_PREFIX=C:/opt/obicall install"
	@$(CMAKE) -E echo "  make release"
	@$(CMAKE) -E echo "  make BUILD_TYPE=Release BUILD_DIR=build-release install"
	@$(CMAKE) -E echo ""
	@$(CMAKE) -E echo "See README.md for MSYS2 UCRT64 prerequisites and Linux/macOS notes."
