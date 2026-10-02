#!/usr/bin/env bash
# ONE-TIME maintainer step: creates the OpenPGP key that signs the APT and
# pacman repositories, in a throwaway GNUPGHOME (your own keyring is never
# touched). Run it on a trusted machine you control.
#
# Usage: packaging/repo/create-signing-key.sh SECRET_OUTPUT_DIR [PUBLIC_KEY_FILE]
#
# Writes:
#   PUBLIC_KEY_FILE (default: packaging/repo/obicall-archive-keyring.asc)
#       the public key - commit it; it is the trust anchor users check
#       (CI's publication rehearsal passes a scratch path for a throwaway key)
#   SECRET_OUTPUT_DIR/obicall-repo-signing.secret.asc
#       the secret key - store it as the OBICALL_REPO_SIGNING_KEY Actions
#       secret, keep an offline backup, then delete this copy; never commit
#   SECRET_OUTPUT_DIR/obicall-repo-signing.revoke.asc
#       revocation certificate - keep offline with the backup
#
# Key: a v4 Ed25519 signing key, expiring in 3 years - accepted by apt's
# sqv/gpgv policies on Ubuntu 22.04-26.04 and Debian 12-13, and by pacman.
# It has no passphrase: in CI the Actions secret store is its protection.
# To extend it before it expires: gpg --quick-set-expire FPR 3y, re-export
# both halves, update the secret and the committed public key.
set -euo pipefail

out=${1:?usage: create-signing-key.sh SECRET_OUTPUT_DIR [PUBLIC_KEY_FILE]}
here=$(cd "$(dirname "$0")" && pwd)
pub=${2:-$here/obicall-archive-keyring.asc}
uid="Obicall package repository <obinexusmk2@proton.me>"

[ ! -e "$pub" ] || { echo "$pub already exists - a key was already created; refusing to replace it" >&2; exit 1; }
mkdir -p "$out"
for f in obicall-repo-signing.secret.asc obicall-repo-signing.revoke.asc; do
    [ ! -e "$out/$f" ] || { echo "$out/$f already exists" >&2; exit 1; }
done

GNUPGHOME=$(mktemp -d)
export GNUPGHOME
trap 'gpgconf --kill gpg-agent 2>/dev/null || true; rm -rf "$GNUPGHOME"' EXIT

gpg --batch --passphrase '' --quick-gen-key "$uid" ed25519 sign 3y
fpr=$(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" && !n++ { print $10 }')

# apt 3's sqv cannot use v5/v6 keys; insist on v4 (algo 22 = EdDSA).
packets=$(gpg --batch --export "$fpr" | gpg --list-packets 2>/dev/null)
if [[ "$packets" != *"version 4, algo 22"* ]]; then
    echo "generated key is not a v4 Ed25519 key - check your GnuPG version" >&2
    exit 1
fi

gpg --batch --armor --export "$fpr" > "$pub"
(umask 077 && gpg --batch --armor --export-secret-keys "$fpr" > "$out/obicall-repo-signing.secret.asc")
cp "$GNUPGHOME/openpgp-revocs.d/$fpr.rev" "$out/obicall-repo-signing.revoke.asc"
chmod 600 "$out/obicall-repo-signing.revoke.asc"

cat <<EOF

Created repository signing key
  fingerprint: $fpr
  public key:  $pub   (commit this)
  secret key:  $out/obicall-repo-signing.secret.asc
  revocation:  $out/obicall-repo-signing.revoke.asc

Next:
  gh secret set OBICALL_REPO_SIGNING_KEY --repo obinexus/obicall < "$out/obicall-repo-signing.secret.asc"
  Back up both files in $out offline, then delete them from this machine.
  Put the fingerprint above into README.md and docs/RELEASING_LINUX.md.
EOF
