#!/usr/bin/env bash
# Builds the obicall .deb from a source tree, in the release configuration
# the package must use (no tests, no test hooks, no embedded source-dir
# fallback, /usr prefix), then checks the result before copying it out.
# Run the test suite separately, from its own test-enabled build.
#
# Usage: packaging/linux/build-deb.sh [SOURCE_DIR] [OUTPUT_DIR]
#   SOURCE_DIR  obicall source tree (default: this script's checkout)
#   OUTPUT_DIR  where the .deb is copied (default: SOURCE_DIR/dist)
#
# Needs: cmake (>= 3.20), ninja, a C compiler, dpkg-dev, and file (which
# CPack's dpkg-shlibdeps support requires).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "${1:-$here/../..}" && pwd)
out=${2:-$src/dist}
mkdir -p "$out"
out=$(cd "$out" && pwd)

build=$(mktemp -d "${TMPDIR:-/tmp}/obicall-deb-build.XXXXXX")
stage=$(mktemp -d "${TMPDIR:-/tmp}/obicall-deb-check.XXXXXX")
trap 'rm -rf "$build" "$stage"' EXIT

cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DOBICALL_ENABLE_TESTS=OFF \
    -DOBICALL_TEST_HOOKS=OFF \
    -DOBICALL_EMBED_SOURCE_DIR_FALLBACK=OFF \
    -DOBICALL_ENABLE_DEB_PACKAGING=ON
cmake --build "$build"
(cd "$build" && cpack -G DEB)

shopt -s nullglob
debs=("$build"/obicall_*.deb)
[ "${#debs[@]}" -eq 1 ] || { echo "expected exactly one obicall_*.deb in $build, found ${#debs[@]}" >&2; exit 1; }
deb=${debs[0]}

dpkg-deb --info "$deb"
dpkg-deb --contents "$deb"
dpkg-deb --extract "$deb" "$stage"
# Also catches a build-tree RUNPATH, which is just another embedded path.
bash "$here/../check-package-files.sh" "$stage" "$src" "$build"

cp "$deb" "$out/"
echo "built $out/$(basename "$deb")"
