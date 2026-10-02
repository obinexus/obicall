#!/usr/bin/env bash
# Points a PKGBUILD (packaging/archlinux/PKGBUILD or
# packaging/msys2/mingw-w64-obicall/PKGBUILD) at release VERSION and pins
# its sha256sums to the SHA-256 of ARCHIVE - a file you have downloaded,
# so the checksum is always computed, never typed in.
#
# Usage: packaging/pin-pkgbuild.sh [--local] PKGBUILD VERSION ARCHIVE
#
#   Release: ARCHIVE is the GitHub archive of tag vVERSION, e.g.
#     curl -fL -o obicall-0.1.2.tar.gz \
#       https://github.com/obinexus/obicall/archive/refs/tags/v0.1.2.tar.gz
#     packaging/pin-pkgbuild.sh packaging/archlinux/PKGBUILD 0.1.2 obicall-0.1.2.tar.gz
#
#   --local: validate an untagged commit instead. ARCHIVE is a snapshot
#     (`git archive --prefix=obicall-VERSION/ ...`), copied next to the
#     PKGBUILD, and source=() is rewritten to that local file. Never commit
#     a PKGBUILD pinned this way.
set -euo pipefail

local_mode=0
if [ "${1:-}" = "--local" ]; then
    local_mode=1
    shift
fi
[ $# -eq 3 ] || { sed -n '/^# Usage/,/^set -euo/p' "$0" | sed '$d' | cut -c3- >&2; exit 2; }
pkgbuild=$1 version=$2 archive=$3

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "not a release version: $version" >&2; exit 2; }
[ -f "$pkgbuild" ] || { echo "no such PKGBUILD: $pkgbuild" >&2; exit 2; }
[ -s "$archive" ] || { echo "no such archive: $archive" >&2; exit 2; }

# The archive must unpack to the directory every recipe's build() expects.
top=$(tar -tzf "$archive" | sed -n 1p)
[ "${top%%/*}" = "obicall-$version" ] || {
    echo "$archive unpacks to '${top%%/*}/', expected 'obicall-$version/'" >&2
    exit 1
}

sha=$(sha256sum "$archive" | cut -d' ' -f1)
old_version=$(sed -n 's/^pkgver=//p' "$pkgbuild")

for key in pkgver pkgrel source sha256sums; do
    [ "$(grep -c "^$key=" "$pkgbuild")" -eq 1 ] || { echo "$pkgbuild: expected exactly one '$key=' line" >&2; exit 1; }
done
grep -q "^sha256sums=('[0-9a-f]\{64\}')$" "$pkgbuild" || {
    echo "$pkgbuild: sha256sums must be a single one-line entry" >&2
    exit 1
}

sed -i "s/^pkgver=.*/pkgver=$version/" "$pkgbuild"
if [ "$old_version" != "$version" ]; then
    sed -i "s/^pkgrel=.*/pkgrel=1/" "$pkgbuild"
fi
sed -i "s/^sha256sums=.*/sha256sums=('$sha')/" "$pkgbuild"

if [ "$local_mode" -eq 1 ]; then
    dir=$(dirname "$pkgbuild")
    name=$(basename "$archive")
    if [ ! "$archive" -ef "$dir/$name" ]; then
        cp "$archive" "$dir/$name"
    fi
    sed -i "s|^source=.*|source=(\"$name\")|" "$pkgbuild"
fi

echo "$pkgbuild: pkgver=$version pkgrel=$(sed -n 's/^pkgrel=//p' "$pkgbuild") sha256=$sha$([ "$local_mode" -eq 1 ] && echo " (local snapshot source)")"
