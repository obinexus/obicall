#!/usr/bin/env bash
# Plays a new Debian/Ubuntu user against a published Obicall APT repository:
# performs the one-time setup documented in README.md (fetch the keyring,
# check its fingerprint, add a deb822 source), installs `obicall` BY NAME,
# smoke-tests it as an unprivileged user, then removes it.
#
# Usage (as root, in a clean Debian/Ubuntu system or container):
#   packaging/repo/verify-apt-client.sh BASE_URL FINGERPRINT [VERSION]
#     BASE_URL     site root, e.g. https://obinexus.github.io/obicall
#     FINGERPRINT  expected signing-key fingerprint (from the committed key)
#     VERSION      expected package version (e.g. 0.1.2-1); when given, waits
#                  up to ~10 minutes for the site to serve it (CDN caching)
set -euo pipefail

base=${1:?usage: verify-apt-client.sh BASE_URL FINGERPRINT [VERSION]}
fpr=${2:?usage: verify-apt-client.sh BASE_URL FINGERPRINT [VERSION]}
version=${3:-}
base=${base%/}
here=$(cd "$(dirname "$0")" && pwd)
keyring=/etc/apt/keyrings/obicall-archive-keyring.gpg
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends ca-certificates curl gpg

if [ -n "$version" ]; then
    index="$base/apt/dists/stable/main/binary-amd64/Packages"
    for attempt in $(seq 1 20); do
        if curl -fsSL "$index" -o /tmp/obicall-Packages && grep -qx "Version: $version" /tmp/obicall-Packages; then
            break
        fi
        [ "$attempt" -lt 20 ] || { echo "FAIL: $index never listed $version" >&2; exit 1; }
        echo "waiting for $index to list $version ($attempt/20)"
        sleep 30
    done
fi

# --- the documented one-time setup ---------------------------------------
install -d -m 0755 /etc/apt/keyrings
curl -fsSL "$base/obicall-archive-keyring.gpg" -o "$keyring"
got=$(gpg --show-keys --with-colons "$keyring" | awk -F: '$1 == "fpr" && !n++ { print $10 }')
[ "$got" = "$fpr" ] || { echo "FAIL: keyring fingerprint $got, expected $fpr" >&2; exit 1; }
echo "keyring fingerprint verified: $got"
cat > /etc/apt/sources.list.d/obicall.sources <<EOF
Types: deb
URIs: $base/apt
Suites: stable
Components: main
Architectures: amd64
Signed-By: $keyring
EOF
apt-get update
apt-get install -y obicall
# -------------------------------------------------------------------------

installed=$(dpkg-query -W -f='${Version}' obicall)
echo "installed obicall $installed from $(apt-cache policy obicall | grep -m1 -o 'http[^ ]*')"
if [ -n "$version" ] && [ "$installed" != "$version" ]; then
    echo "FAIL: installed $installed, expected $version" >&2
    exit 1
fi

install -m 0755 "$here/../smoke-test-installed.sh" /tmp/obicall-smoke-test.sh
id obicall-smoke >/dev/null 2>&1 || useradd -m obicall-smoke
su obicall-smoke -c 'cd ~ && bash /tmp/obicall-smoke-test.sh /usr'

apt-get remove -y obicall
echo "PASS: apt install obicall from $base/apt"
