#!/usr/bin/env bash
# Plays a new user against a published Obicall pacman repository - Arch
# Linux ([obicall]) or MSYS2 UCRT64 ([obicall-ucrt64]): performs the
# one-time setup documented in README.md (fetch the key, check its
# fingerprint, pacman-key --add/--lsign-key, add the repository to
# pacman.conf), installs the package BY NAME, checks pacman validated its
# signature, smoke-tests it, then removes it.
#
# Usage (Arch: as root in a clean system/container; MSYS2: in a UCRT64 shell):
#   packaging/repo/verify-pacman-client.sh BASE_URL FINGERPRINT REPO PATH PACKAGE [VERSION]
#     BASE_URL     site root, e.g. https://obinexus.github.io/obicall
#     FINGERPRINT  expected signing-key fingerprint (from the committed key)
#     REPO         pacman.conf section / database name: obicall | obicall-ucrt64
#     PATH         repository path under BASE_URL: 'arch/$arch' | msys2/ucrt64
#     PACKAGE      obicall | mingw-w64-ucrt-x86_64-obicall
#     VERSION      expected version (e.g. 0.1.2-1); when given, waits up to
#                  ~10 minutes for the site to serve it (CDN caching)
set -euo pipefail

base=${1:?usage: verify-pacman-client.sh BASE_URL FINGERPRINT REPO PATH PACKAGE [VERSION]}
fpr=${2:?} repo=${3:?} path=${4:?} package=${5:?}
version=${6:-}
base=${base%/}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)

if [ -n "$version" ]; then
    db="$base/${path//\$arch/$(uname -m)}/$repo.db"
    for attempt in $(seq 1 20); do
        if curl -fsSL "$db" -o "$work/db" && tar -xzOf "$work/db" > "$work/desc" && grep -qx "$version" "$work/desc"; then
            break
        fi
        [ "$attempt" -lt 20 ] || { echo "FAIL: $db never listed $version" >&2; exit 1; }
        echo "waiting for $db to list $version ($attempt/20)"
        sleep 30
    done
fi

# A real installation already has pacman's local master key; container
# images ship without one, and --lsign-key needs it.
secret_keys=$(pacman-key --list-secret 2>/dev/null || true)
if [[ "$secret_keys" != *sec* ]]; then
    pacman-key --init
    if [ -n "${MSYSTEM:-}" ]; then pacman-key --populate msys2; else pacman-key --populate archlinux; fi
fi

# --- the documented one-time setup ---------------------------------------
curl -fsSL "$base/obicall-archive-keyring.asc" -o "$work/obicall-archive-keyring.asc"
got=$(gpg --show-keys --with-colons "$work/obicall-archive-keyring.asc" 2>/dev/null \
    | awk -F: '$1 == "fpr" && !n++ { print $10 }')
[ "$got" = "$fpr" ] || { echo "FAIL: key fingerprint $got, expected $fpr" >&2; exit 1; }
echo "key fingerprint verified: $got"
pacman-key --add "$work/obicall-archive-keyring.asc"
pacman-key --lsign-key "$fpr"
printf '\n[%s]\nSigLevel = Required DatabaseRequired\nServer = %s/%s\n' "$repo" "$base" "$path" >> /etc/pacman.conf
if [ -n "${MSYSTEM:-}" ]; then
    pacman -Sy --noconfirm
    pacman -S --noconfirm "$package"
else
    pacman -Syu --noconfirm "$package"
fi
# -------------------------------------------------------------------------

pacman -Qi "$package" | grep -E '^(Name|Version|Validated By)'
pacman -Qi "$package" | grep -q '^Validated By.*Signature' \
    || { echo "FAIL: pacman did not validate $package by signature" >&2; exit 1; }
installed=$(pacman -Q "$package" | cut -d' ' -f2)
if [ -n "$version" ] && [ "$installed" != "$version" ]; then
    echo "FAIL: installed $installed, expected $version" >&2
    exit 1
fi

if [ -n "${MSYSTEM:-}" ]; then
    bash "$here/../smoke-test-installed.sh" "$MINGW_PREFIX"
else
    install -m 0755 "$here/../smoke-test-installed.sh" /tmp/obicall-smoke-test.sh
    id obicall-smoke >/dev/null 2>&1 || useradd -m obicall-smoke
    su obicall-smoke -c 'cd ~ && bash /tmp/obicall-smoke-test.sh /usr'
fi

pacman -R --noconfirm "$package"
rm -rf "$work"
echo "PASS: pacman -S $package from [$repo] at $base/$path"
