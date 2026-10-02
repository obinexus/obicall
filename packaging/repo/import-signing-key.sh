#!/usr/bin/env bash
# Imports the package-repository signing key into a fresh, private
# GNUPGHOME, for CI. The key comes only from the environment (an Actions
# secret) and is never echoed:
#
#   OBICALL_REPO_SIGNING_KEY         ASCII-armored secret key (required)
#   OBICALL_REPO_SIGNING_PASSPHRASE  its passphrase, if it has one
#
# Refuses a key whose fingerprint differs from the committed public key -
# the trust anchor users verify - so CI can never sign with anything else.
# Prints the primary key fingerprint on stdout.
#
# Usage: packaging/repo/import-signing-key.sh GNUPGHOME_DIR PUBLIC_KEY_FILE
#   then: export GNUPGHOME=GNUPGHOME_DIR
set -euo pipefail

home=${1:?usage: import-signing-key.sh GNUPGHOME_DIR PUBLIC_KEY_FILE}
pubkey=${2:?usage: import-signing-key.sh GNUPGHOME_DIR PUBLIC_KEY_FILE}

if [ -z "${OBICALL_REPO_SIGNING_KEY:-}" ]; then
    echo "OBICALL_REPO_SIGNING_KEY is empty: the repository signing key secret is not configured" >&2
    echo "(see docs/RELEASING_LINUX.md, \"One-time setup\")" >&2
    exit 1
fi
[ -s "$pubkey" ] || { echo "no committed public key at $pubkey (see docs/RELEASING_LINUX.md)" >&2; exit 1; }

mkdir -p "$home"
chmod 700 "$home"
export GNUPGHOME=$home

if [ -n "${OBICALL_REPO_SIGNING_PASSPHRASE:-}" ]; then
    (umask 077 && printf '%s' "$OBICALL_REPO_SIGNING_PASSPHRASE" > "$home/passphrase")
    # Applies to every gpg invocation, including the ones repo-add makes.
    printf 'pinentry-mode loopback\npassphrase-file %s\n' "$home/passphrase" > "$home/gpg.conf"
fi

printf '%s\n' "$OBICALL_REPO_SIGNING_KEY" | gpg --batch --quiet --import 2>/dev/null \
    || { echo "OBICALL_REPO_SIGNING_KEY could not be imported" >&2; exit 1; }

mapfile -t secret_fprs < <(gpg --batch --with-colons --list-secret-keys \
    | awk -F: '$1 == "sec" { want = 1; next } want && $1 == "fpr" { print $10; want = 0 }')
[ "${#secret_fprs[@]}" -eq 1 ] || { echo "expected exactly one secret key, found ${#secret_fprs[@]}" >&2; exit 1; }
fpr=${secret_fprs[0]}

expected=$(gpg --batch --with-colons --show-keys "$pubkey" | awk -F: '$1 == "fpr" && !n++ { print $10 }')
if [ "$fpr" != "$expected" ]; then
    echo "signing key $fpr does not match the committed public key $expected ($pubkey)" >&2
    exit 1
fi

# Prove it can actually sign (catches a wrong or missing passphrase here,
# not halfway through building a repository).
echo test | gpg --batch --local-user "$fpr" --detach-sign -o /dev/null \
    || { echo "the signing key cannot sign (passphrase?)" >&2; exit 1; }

echo "$fpr"
