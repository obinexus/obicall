# Releasing Obicall for MSYS2 UCRT64

This documents how the `mingw-w64-ucrt-x86_64-obicall` package
(`packaging/msys2/mingw-w64-obicall/PKGBUILD`) is built, tested, and
released, and how to reproduce every one of those steps yourself.

## Scope

This first release targets **UCRT64 only** (`mingw_arch=('ucrt64')` in the
PKGBUILD). MINGW64, CLANG64, and CLANGARM64 are not built or tested as
MSYS2 packages yet - adding them means adding their compiler/toolchain
package names to `makedepends`/`depends` and to `mingw_arch`, then running
the same `makepkg-mingw` workflow below once per environment; nothing in
the CMake build itself is UCRT64-specific.

Conan packaging and Debian/APT packaging (`cpack -G DEB`) are explicitly
out of scope for this release.

## CI status

`.github/workflows/windows-ucrt64.yml` has a `package` job that runs the
same `makepkg-mingw --syncdeps --cleanbuild`, `pacman -U`, and
installed-package smoke test described below, on a clean `windows-latest`
GitHub Actions runner.

- [Run #8](https://github.com/obinexus/obicall/actions/runs/34877309454)
  (commit `eed0f9c`) - **failed**: `ERROR: PKGBUILD contains CRLF
  characters and cannot be sourced.` The PKGBUILD blob committed to the
  repo was LF-only; GitHub's `windows-latest` runners default to git
  `core.autocrlf=true`, which rewrote it to CRLF on checkout, and
  `makepkg` refuses to source a CRLF script. Fixed by commit `6ecbb1f`
  (`.gitattributes`, `text eol=lf` pinned for `packaging/msys2/**/PKGBUILD`
  and `*.sh`).
- [Run #9](https://github.com/obinexus/obicall/actions/runs/34881442150)
  (commit `6ecbb1f`) - **passed**, both jobs: `build-and-test` (plain
  CMake build/test/install smoke test) and `package` (makepkg-mingw
  build, `pacman -U` install, and the full installed-package smoke test:
  `--help`, `doctor`, `validate`, `replay`, `demo --scenario
  broker-failover`, all from an isolated working directory).

The `mingw-w64-ucrt-x86_64-obicall-0.1.1-1-any.pkg.tar.zst` attached to
the [v0.1.1 release](https://github.com/obinexus/obicall/releases/tag/v0.1.1)
predates the CRLF fix - it was built and verified locally (never through a
fresh git checkout on a Windows runner, so it never hit the checkout-time
CRLF rewrite that broke CI) before the fix landed, and its checksum and
test results are recorded in the release notes. The CRLF bug was specific
to *checking out* the PKGBUILD on a Windows runner's default git config,
not to the package contents themselves; see the release for the actual
tested-and-published artifact.

## Why the PKGBUILD builds from a tagged archive, not the checkout

`source=()` in the PKGBUILD points at a specific tagged GitHub release
archive (`https://github.com/obinexus/obicall/archive/refs/tags/v<pkgver>.tar.gz`),
pinned by a verified `sha256sums` entry - not a branch, and not this local
checkout. `makepkg-mingw` downloads and extracts that archive into a
scratch `src/` directory and builds only from there. This is what makes
the resulting package reproducible: anyone who trusts the checksum ends up
building the exact same source tree, independent of whatever else is
sitting in a given machine's working copy (uncommitted changes, a stale
`build/`, editor swap files, etc.).

If you are validating a new PKGBUILD before its intended release commit
has been tagged and pushed, point `source=()` at a snapshot archive of
that exact commit instead (e.g. a `git archive` you host, or GitHub's
`https://github.com/<owner>/<repo>/archive/<commit-sha>.tar.gz`), verify
against that, and then re-point `source=()` and `sha256sums=()` at the
real tagged release archive and rebuild/retest before calling the recipe
released. Never leave `source=()` pointing at a moving branch.

## Prerequisites (real MSYS2 UCRT64 environment)

Install MSYS2 (https://www.msys2.org/), then from a **UCRT64** shell
(`C:\msys64\ucrt64.exe`, or the "MSYS2 UCRT64" Start Menu shortcut - not
plain MSYS2/MINGW64/CLANG64; see `README.md` "Platform prerequisites" for
why the shell matters):

```bash
pacman -Syu   # then re-open the shell if it asks you to, and run it again
pacman -S --needed base-devel git \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python
```

## Build the package

```bash
cd packaging/msys2/mingw-w64-obicall
makepkg-mingw --syncdeps --cleanbuild
```

This downloads the pinned source archive, verifies its SHA-256, configures
with CMake (Ninja, `CMAKE_BUILD_TYPE=Release`,
`CMAKE_INSTALL_PREFIX=$MINGW_PREFIX`, `OBICALL_EMBED_SOURCE_DIR_FALLBACK=OFF`
- see "Why `OBICALL_EMBED_SOURCE_DIR_FALLBACK`" below), builds, runs the
full `ctest` suite in `check()`, and stages the install through `DESTDIR`
in `package()`. It produces
`mingw-w64-ucrt-x86_64-obicall-<pkgver>-<pkgrel>-any.pkg.tar.zst` in that
same directory.

### Why `OBICALL_EMBED_SOURCE_DIR_FALLBACK`

`src/supervisor/supervisor.c` spawns the Python provider adapter
(`python/obicall/provider_worker.py`) by locating it relative to
`obicall.exe`'s own directory - the installed layout
(`<prefix>/bin/../share/obicall/python/obicall/provider_worker.py`). A
build run straight out of an *uninstalled* `build/bin/` has nothing at
that relative path yet, so `src/supervisor/CMakeLists.txt` also offers a
dev-convenience fallback, `OBICALL_SOURCE_DIR`, which bakes this build's
absolute source directory into the binary as a literal string. That is
fine for local development, but a **released package must never ship a
binary containing this build machine's checkout path** - so the PKGBUILD
passes `-DOBICALL_EMBED_SOURCE_DIR_FALLBACK=OFF`, and `check()` instead
stages the adapter under `build-${MSYSTEM}/share/obicall/python/obicall/`
before running `ctest`, exercising the exact same install-relative
resolution path the shipped binary uses - not the disabled dev fallback.

## Run the test suite

Already run as part of `makepkg-mingw`'s `check()` above. To run it
directly against an ad hoc build (e.g. while developing the PKGBUILD
itself, not for the release build):

```bash
cmake -S . -B build-ucrt64-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ucrt64-release
ctest --test-dir build-ucrt64-release --output-on-failure
```

## Inspect the package before installing it

```bash
tar -tf mingw-w64-ucrt-x86_64-obicall-*.pkg.tar.zst | sort
# Native runtime DLL dependencies (should be only system/UCRT DLLs - see
# "Runtime dependencies" below):
objdump -p /path/to/extracted/ucrt64/bin/obicall.exe | grep "DLL Name"
```

## Install and verify, isolated from the checkout

Install the package (ideally in a clean or separate MSYS2 UCRT64
environment, so nothing on `PATH` or already installed can mask a real
installation-independence bug):

```bash
pacman -U ./mingw-w64-ucrt-x86_64-obicall-<pkgver>-<pkgrel>-any.pkg.tar.zst
```

Then, from a working directory **outside the source checkout**, with
`PYTHONPATH` unset so nothing but the installed adapter can be found:

```bash
mkdir -p /tmp/obicall-smoke && cd /tmp/obicall-smoke
unset PYTHONPATH

obicall --help
obicall doctor --json
obicall validate --config "$MINGW_PREFIX/share/obicall/examples/position-fusion.json" --json
obicall replay --input "$MINGW_PREFIX/share/obicall/recordings/demo.obr" --json
obicall run --config "$MINGW_PREFIX/share/obicall/examples/position-fusion.json" \
  --duration-seconds 5 --json
obicall demo --scenario broker-failover \
  --config "$MINGW_PREFIX/share/obicall/examples/position-fusion.json" --json
```

Confirm the `run` and `demo` output mentions both `worker_position_*`
(the C provider) and `worker_inertial_*` (the Python provider) connecting,
and that `demo`'s JSON ends with `"overall":"pass"`. Runtime state
(`.pid`/`.port`/journal files) lands under whatever `--runtime-dir`
defaults to (`./obicall-run` relative to the current directory) or names
explicitly - always inside the current, user-writable working directory,
never anywhere requiring elevated privileges.

## Runtime dependencies

Confirmed by `objdump -p` on the Release build, not assumed: the CLI and
every daemon binary import only `KERNEL32.dll`, `bcrypt.dll`, `WS2_32.dll`,
and the `api-ms-win-crt-*` Universal CRT split DLLs - all of which ship
with Windows 10+ itself - plus this package's own `obicall.dll`. There is
no dynamic dependency on `libgcc_s_seh-1.dll` or `libwinpthread-1.dll`.
The only MSYS2 package dependency is `${MINGW_PACKAGE_PREFIX}-python`, for
the Python provider adapter.

## Known, disclosed limitations

- **Single architecture**: UCRT64 only (see "Scope" above).
- **`pacman -S mingw-w64-ucrt-x86_64-obicall` does not work yet.** That
  only works once this package is in a repository pacman is configured to
  use - either the official `msys2/MINGW-packages` (after review and
  acceptance) or a self-hosted pacman repository. Until then, install the
  downloaded `.pkg.tar.zst` directly with `pacman -U`.
- **`build_references.sh`'s packaging lint** (`makepkg-mingw`'s
  "Checking for packaging issues" step) prints `WARNING: Package contains
  reference to $(cygpath -w /)`. This is a broad text search for any
  literal `C:\` anywhere in any installed file; verified (`grep -l
  'C:\\' -r pkg/`) to match only two documentation files
  (`README.md`, `VALIDATION.md`) that describe example Windows paths in
  prose (e.g. `C:\msys64\ucrt64.exe`), not a leaked build-machine path in
  a binary or CMake export file - see `OBICALL_EMBED_SOURCE_DIR_FALLBACK`
  above for the one place an actual leak was found and fixed.
- **This is a local desktop prototype**, not a production-hardened
  release - see `docs/FAULT_TOLERANCE.md` and
  `docs/IMPLEMENTATION_STATUS.md` for exactly what is and is not
  implemented or tested. `test_worker_crash_hook` is known to fail under
  MSVC specifically (a difference in `abort()` timing, unrelated to this
  package, which is GCC-only); it passes under the UCRT64 GCC build this
  package actually ships.

## Publishing a GitHub release

1. Confirm `CMakeLists.txt`'s `project(... VERSION ...)` matches the
   PKGBUILD's `pkgver`.
2. Tag the exact commit being released and push the tag:
   `git tag -a vX.Y.Z -m "..." && git push origin vX.Y.Z`. Never move or
   force-overwrite a tag that has already been pushed or released.
3. Download `https://github.com/<owner>/<repo>/archive/refs/tags/vX.Y.Z.tar.gz`,
   compute its SHA-256, and put both the URL and checksum into the
   PKGBUILD's `source=()`/`sha256sums=()`.
4. Rebuild and retest the package against that real, published archive
   (`makepkg-mingw --syncdeps --cleanbuild`) - this is the point at which
   the recipe can honestly be called validated against a release, not a
   local checkout.
5. Create the GitHub release itself (`gh release create vX.Y.Z ...`),
   attaching the built `.pkg.tar.zst` and a `SHA256SUMS` file covering
   every attached asset (the source archive included).
6. To later make `pacman -S mingw-w64-ucrt-x86_64-obicall` work by name,
   either submit this PKGBUILD to `msys2/MINGW-packages` for review, or
   publish and configure your own signed pacman repository - a release
   asset alone does not make either happen automatically.
