#!/usr/bin/env bash
# Checks an extracted package tree (a .deb, an Arch package, or the MSYS2
# package) for two things a released obicall package must never ship:
#
#  - the worker's fault-injection hooks (OBICALL_TEST_HOOKS): rejected in
#    any ELF/PE binary that still carries the --test-crash-after-ms flag
#    (the docs describe that flag, so only binaries are searched for it);
#  - a build machine's paths (OBICALL_EMBED_SOURCE_DIR_FALLBACK, or any
#    other leak): rejected in any file containing one of the given strings
#    - pass the source and build directories the package was built from.
#
# Usage: packaging/check-package-files.sh EXTRACTED_ROOT [FORBIDDEN_STRING...]
set -euo pipefail

root=${1:?usage: check-package-files.sh EXTRACTED_ROOT [FORBIDDEN_STRING...]}
shift

# Top-level dotfiles are package-manager metadata, not installed files -
# pacman's .BUILDINFO, for one, records the build directory by design.
entries=()
while IFS= read -r -d '' e; do
    entries+=("$e")
done < <(find "$root" -mindepth 1 -maxdepth 1 ! -name '.*' -print0)
[ "${#entries[@]}" -gt 0 ] || { echo "FAIL: nothing extracted under $root"; exit 1; }

status=0
binaries=0
while IFS= read -r -d '' f; do
    magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
    case "$magic" in
        7f454c46 | 4d5a*) ;; # ELF, or a PE image ("MZ")
        *) continue ;;
    esac
    binaries=$((binaries + 1))
    if grep -qaF -- '--test-crash-after-ms' "$f"; then
        echo "FAIL: ${f#"$root"/} was built with OBICALL_TEST_HOOKS (fault-injection hooks)"
        status=1
    fi
done < <(find "${entries[@]}" -type f -print0)

if [ "$binaries" -eq 0 ]; then
    echo "FAIL: no ELF/PE binaries found under $root - wrong directory?"
    exit 1
fi

for s in "$@"; do
    [ -n "$s" ] || continue
    if hits=$(grep -rlaF -- "$s" "${entries[@]}"); then
        echo "FAIL: packaged files embed build-machine path '$s':"
        echo "$hits" | sed "s|^$root/|  |"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "OK: $binaries binaries without test hooks; no build-machine paths ($#) under $root"
fi
exit "$status"
