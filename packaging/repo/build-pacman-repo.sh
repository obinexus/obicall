#!/usr/bin/env bash
# Builds a signed pacman repository from a directory of packages - used
# for both the Arch Linux repository ([obicall], x86_64) and the MSYS2
# UCRT64 one ([obicall-ucrt64]); the formats are identical.
#
#   OUT_DIR/<pkg>.pkg.tar.zst + <pkg>.pkg.tar.zst.sig   (binary signatures)
#   OUT_DIR/REPO.db, REPO.db.sig, REPO.files, REPO.files.sig (+ .tar.gz)
#
# pacman.conf's [REPO] section name must match the database name. The
# symlinks repo-add creates (REPO.db -> REPO.db.tar.gz, ...) are replaced
# by copies, since static hosts such as GitHub Pages do not serve links.
#
# Usage: packaging/repo/build-pacman-repo.sh REPO PKG_DIR OUT_DIR KEY_FINGERPRINT
#   Signs with KEY_FINGERPRINT from the current $GNUPGHOME (see
#   packaging/repo/import-signing-key.sh). OUT_DIR must not exist yet.
#
# Needs: repo-add and pacman-key's gpg (Arch Linux, or MSYS2).
set -euo pipefail

repo=${1:?usage: build-pacman-repo.sh REPO PKG_DIR OUT_DIR KEY_FINGERPRINT}
pkgs=${2:?usage: build-pacman-repo.sh REPO PKG_DIR OUT_DIR KEY_FINGERPRINT}
out=${3:?usage: build-pacman-repo.sh REPO PKG_DIR OUT_DIR KEY_FINGERPRINT}
key=${4:?usage: build-pacman-repo.sh REPO PKG_DIR OUT_DIR KEY_FINGERPRINT}

[ ! -e "$out" ] || { echo "$out already exists" >&2; exit 1; }
shopt -s nullglob
files=("$pkgs"/*.pkg.tar.zst)
[ "${#files[@]}" -gt 0 ] || { echo "no .pkg.tar.zst files in $pkgs" >&2; exit 1; }

mkdir -p "$out"
for p in "${files[@]}"; do
    cp "$p" "$out/"
    # pacman requires binary (not ASCII-armored) detached signatures.
    gpg --batch --yes --local-user "$key" --detach-sign --no-armor \
        -o "$out/$(basename "$p").sig" "$out/$(basename "$p")"
done

include_sigs=()
if [[ "$(repo-add --help 2>&1 || true)" == *--include-sigs* ]]; then
    include_sigs=(--include-sigs) # pacman >= 7 no longer embeds them by default
fi
(cd "$out" && repo-add --sign --key "$key" "${include_sigs[@]}" "$repo.db.tar.gz" ./*.pkg.tar.zst)

for link in "$out"/*; do
    if [ -L "$link" ]; then
        cp --remove-destination "$(readlink -f "$link")" "$link"
    fi
done
rm -f "$out"/*.old "$out"/*.old.sig

keyring=$(mktemp)
trap 'rm -f "$keyring"' EXIT
gpg --batch --export "$key" > "$keyring"
for sig in "$out"/*.sig; do
    gpgv --keyring "$keyring" "$sig" "${sig%.sig}" 2>/dev/null || { echo "bad signature: $sig" >&2; exit 1; }
done

echo "pacman repository [$repo]: $out"
ls -l "$out"
