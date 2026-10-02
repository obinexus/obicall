#!/usr/bin/env bash
# Builds a signed APT repository from a directory of .deb files:
#
#   OUT_DIR/pool/main/o/obicall/*.deb
#   OUT_DIR/dists/stable/main/binary-<arch>/Packages{,.gz}
#   OUT_DIR/dists/stable/{Release,InRelease,Release.gpg}
#
# Users point apt at it with:  URIs: <base>/apt  Suites: stable  Components: main
#
# Usage: packaging/repo/build-apt-repo.sh DEB_DIR OUT_DIR KEY_FINGERPRINT
#   Signs with KEY_FINGERPRINT from the current $GNUPGHOME (see
#   packaging/repo/import-signing-key.sh). OUT_DIR must not exist yet.
#
# Needs: apt-ftparchive (apt-utils), dpkg-deb, gpg, gpgv.
set -euo pipefail

debs=${1:?usage: build-apt-repo.sh DEB_DIR OUT_DIR KEY_FINGERPRINT}
out=${2:?usage: build-apt-repo.sh DEB_DIR OUT_DIR KEY_FINGERPRINT}
key=${3:?usage: build-apt-repo.sh DEB_DIR OUT_DIR KEY_FINGERPRINT}
suite=stable
component=main

[ ! -e "$out" ] || { echo "$out already exists" >&2; exit 1; }
shopt -s nullglob
files=("$debs"/*.deb)
[ "${#files[@]}" -gt 0 ] || { echo "no .deb files in $debs" >&2; exit 1; }

pool="$out/pool/$component/o/obicall"
mkdir -p "$pool"
archs=()
for deb in "${files[@]}"; do
    [ "$(dpkg-deb -f "$deb" Package)" = obicall ] || { echo "$deb is not an obicall package" >&2; exit 1; }
    a=$(dpkg-deb -f "$deb" Architecture)
    [[ " ${archs[*]} " == *" $a "* ]] || archs+=("$a")
    cp "$deb" "$pool/"
done

cd "$out"
for a in "${archs[@]}"; do
    d="dists/$suite/$component/binary-$a"
    mkdir -p "$d"
    apt-ftparchive --arch "$a" packages "pool/$component" > "$d/Packages"
    gzip -9nk "$d/Packages"
    grep -q '^Package: obicall$' "$d/Packages" || { echo "empty index for $a" >&2; exit 1; }
done

# Written outside dists/ first: apt-ftparchive would otherwise hash the
# Release file it is in the middle of writing.
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=OBINexus \
    -o APT::FTPArchive::Release::Label=Obicall \
    -o APT::FTPArchive::Release::Suite="$suite" \
    -o APT::FTPArchive::Release::Codename="$suite" \
    -o APT::FTPArchive::Release::Architectures="${archs[*]}" \
    -o APT::FTPArchive::Release::Components="$component" \
    -o APT::FTPArchive::Release::Description="Obicall packages from github.com/obinexus/obicall" \
    release "dists/$suite" > Release.tmp
mv Release.tmp "dists/$suite/Release"

gpg --batch --yes --local-user "$key" --digest-algo SHA512 --clearsign \
    -o "dists/$suite/InRelease" "dists/$suite/Release"
gpg --batch --yes --local-user "$key" --digest-algo SHA512 --armor --detach-sign \
    -o "dists/$suite/Release.gpg" "dists/$suite/Release"

# Verify against nothing but the public key, the way apt will.
keyring=$(mktemp)
trap 'rm -f "$keyring"' EXIT
gpg --batch --export "$key" > "$keyring"
gpgv --keyring "$keyring" "dists/$suite/InRelease"
gpgv --keyring "$keyring" "dists/$suite/Release.gpg" "dists/$suite/Release"

echo "APT repository: $out (suite $suite, component $component, architectures ${archs[*]})"
find . -type f | sort
