# Releasing Obicall for APT and pacman

This covers the Debian/Ubuntu `.deb`, the Arch Linux package, and the
signed APT and pacman repositories that let users install Obicall **by
name**. The MSYS2 UCRT64 package recipe itself is documented in
[RELEASING_MSYS2.md](RELEASING_MSYS2.md); its packages are published here,
in a pacman repository of their own.

| Users run | Repository (one-time setup required) | Package built by |
|---|---|---|
| `sudo apt install obicall` | APT: `<site>/apt`, suite `stable`, component `main` (amd64) | `packaging/linux/` (CPack) |
| `sudo pacman -Syu obicall` | Arch Linux: `[obicall]`, `<site>/arch/$arch` (x86_64) | `packaging/archlinux/PKGBUILD` |
| `pacman -S mingw-w64-ucrt-x86_64-obicall` | MSYS2 UCRT64: `[obicall-ucrt64]`, `<site>/msys2/ucrt64` | `packaging/msys2/mingw-w64-obicall/PKGBUILD` |

`<site>` is `https://obinexus.github.io/obicall` (GitHub Pages for this
repository; there is no custom domain). The user-facing setup is in
[README.md](../README.md#installing-from-package-repositories).

**Arch Linux is not MSYS2.** Arch is a Linux distribution; MSYS2 is a
Windows environment that happens to use pacman. They have different
packages (`obicall` vs `mingw-w64-ucrt-x86_64-obicall`), different
repositories, and MSYS2 has no `sudo`.

**A release asset or an AUR recipe is not a repository.** `apt install`
and `pacman -S` only find packages in repositories the system is configured
to use. GitHub release assets can be installed by file (`apt install
./obicall_*.deb`, `pacman -U ...`); an AUR `PKGBUILD` is a recipe users
build themselves. Neither makes install-by-name work. Being included in
Debian, Ubuntu, Arch's official repositories, or MSYS2's own repositories
is a separate submission and review process, not attempted here.

## Package contents and release configuration

Every package is built in the **release configuration**, and the test
suite runs from a **separate, test-enabled build** that is never packaged:

- `OBICALL_ENABLE_TESTS=OFF`, `OBICALL_TEST_HOOKS=OFF` - no
  fault-injection hooks (`--test-crash-after-ms` and friends) in the shipped
  `obicall-workerd`. (The published MSYS2 `0.1.1-1` package does contain
  them; see RELEASING_MSYS2.md.)
- `OBICALL_EMBED_SOURCE_DIR_FALLBACK=OFF` - no build-machine path compiled
  into the binaries.
- `packaging/check-package-files.sh` enforces both on the extracted package
  (it fails on the hook flag in any ELF/PE binary, and on the build's source
  or build directory anywhere in the payload).

How the installed files find each other, with nothing but the package
installed (no `LD_LIBRARY_PATH`, no `PYTHONPATH`):

- The core library is `obicall.so.0` (deliberately no `lib` prefix, so
  `ldconfig` does not index it). Binaries find it through a relative
  `RUNPATH` (`$ORIGIN/../lib`, or `$ORIGIN/../lib/x86_64-linux-gnu` in the
  `.deb`), set by the top-level `CMakeLists.txt` for every install.
- The Python provider adapter (`share/obicall/python/obicall/`) loads the
  core library via ctypes from `<bin>/<that same relative path>/obicall.so.0`
  (`src/supervisor/supervisor.c`). Before 0.1.2 it looked for
  `<bin>/libobicall.so`, which never exists on Linux, so the Python
  provider silently failed there.
- `share/obicall/examples/providers/provider_c_sim.so` (the example
  manifest's artifact path) is a relative symlink to
  `lib/obicall/providers/provider_c_sim.so`, keeping ELF files out of
  `/usr/share`. (Windows keeps a real copy.)
- Runtime dependencies: glibc (`libc6 (>= 2.34)`, detected by
  `dpkg-shlibdeps`) and Python 3.

Linux fixes that 0.1.2 needed before any of this could be packaged (none of
them affect the Windows build):

- Strict C11 hid POSIX declarations from glibc: the build failed. Fixed with
  `_POSIX_C_SOURCE=200809L`/`_DEFAULT_SOURCE` on Linux plus the missing
  `<time.h>`, `<stdio.h>`, `<sys/select.h>` includes (these also broke
  macOS, and break GCC 14+ anywhere).
- The core library did not link `libm`.
- A process that wrote to a dead peer's socket was killed by `SIGPIPE`,
  so killing one broker (the broker-failover demo) took down the journal and
  gate too; `osal_net_init()` now ignores `SIGPIPE` (Windows has none).

## Building and testing a package yourself

### Debian / Ubuntu (WSL Ubuntu works)

```bash
sudo apt install build-essential cmake ninja-build dpkg-dev file python3
packaging/linux/build-deb.sh . dist     # release configuration + package checks
sudo apt install ./dist/obicall_*_amd64.deb
packaging/smoke-test-installed.sh /usr  # run as a normal user, outside the checkout
```

`build-deb.sh` refuses dev-only configurations itself: `CPackDeb.cmake`
fails the configure step if tests, test hooks, or the source-dir fallback
are on, or the prefix is not `/usr`. Run the test suite from its own build:

```bash
cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DOBICALL_ENABLE_TESTS=ON
cmake --build build-test && ctest --test-dir build-test --output-on-failure
```

Release packages are built on **Ubuntu 22.04** (the oldest glibc among the
tested targets), so one `.deb` serves every target listed below.

### Arch Linux

Build as an ordinary user, not root:

```bash
sudo pacman -S --needed base-devel cmake ninja python
cd packaging/archlinux
makepkg --syncdeps --cleanbuild   # runs check(): the full test suite, from a separate build
sudo pacman -U obicall-*-x86_64.pkg.tar.zst
```

Preferably build in a clean chroot (devtools):

```bash
sudo pacman -S --needed devtools
mkdir -p ~/chroot
mkarchroot -C /usr/share/devtools/pacman.conf.d/extra.conf \
  -M /usr/share/devtools/makepkg.conf.d/x86_64.conf ~/chroot/root base-devel
makechrootpkg -c -r ~/chroot
```

The committed recipe builds only from a pinned release archive (see
"Pinning" below). To build an untagged commit, pin a copy of the recipe to
a snapshot - never commit that copy:

```bash
v=$(sed -n 's/^pkgver=//p' packaging/archlinux/PKGBUILD)
git archive --format=tar.gz --prefix=obicall-$v/ -o /tmp/obicall-$v.tar.gz HEAD
mkdir -p /tmp/obicall-arch && cp packaging/archlinux/PKGBUILD /tmp/obicall-arch/
packaging/pin-pkgbuild.sh --local /tmp/obicall-arch/PKGBUILD $v /tmp/obicall-$v.tar.gz
cd /tmp/obicall-arch && makepkg --syncdeps --cleanbuild
```

`namcap` reports three warnings that are false positives: it cannot see
that `obicall.abi` and `obicall.so.0` ship inside this same package, or
resolve the `#!/usr/bin/env python3` interpreter (`python` is in `depends`).

### Pinning

`source=()` in both PKGBUILDs names the GitHub archive of the release tag;
`sha256sums=()` must be that archive's real checksum. Before a tag exists
the committed value is all zeros, which makepkg rejects. Never type a
checksum in: download the archive and let the script compute it.

```bash
curl -fL -o obicall-X.Y.Z.tar.gz https://github.com/obinexus/obicall/archive/refs/tags/vX.Y.Z.tar.gz
packaging/pin-pkgbuild.sh packaging/archlinux/PKGBUILD X.Y.Z obicall-X.Y.Z.tar.gz
packaging/pin-pkgbuild.sh packaging/msys2/mingw-w64-obicall/PKGBUILD X.Y.Z obicall-X.Y.Z.tar.gz
```

The Release workflow does exactly this (after checking that the archive's
embedded commit ID is the tagged commit) and attaches the pinned recipes to
the release as `PKGBUILD-archlinux` and `PKGBUILD-msys2`.

## Automation

| Workflow | When | What |
|---|---|---|
| `linux-packages.yml` | every push and PR | Builds the `.deb` and Arch package from a snapshot of the commit; test suite; installs each on clean systems and runs the smoke test; then **rehearses publication**: a throwaway key signs APT and pacman repositories served locally, and clean Ubuntu, Debian, and Arch containers install `obicall` from them by name using the documented setup. Publishes nothing. |
| `windows-ucrt64.yml` | every push and PR | The committed MSYS2 recipe: build, `check()`, `pacman -U`, package checks, smoke test. |
| `release.yml` | push of a `vX.Y.Z` tag | Pins both recipes to the tag's archive; builds and tests all three packages (`.deb`, Arch, MSYS2), install-tests each on clean systems, then attaches the packages, `obicall-X.Y.Z-src.tar.gz`, the pinned recipes, and `SHA256SUMS` to the GitHub release (creating it, as a prerelease for 0.x, if it does not exist). Never replaces an existing asset. |
| `publish-repos.yml` | after a successful Release run, or manually with a tag | Downloads that release's packages, checks them against `SHA256SUMS`, signs the APT repository and both pacman repositories, deploys them to GitHub Pages, then **verifies the live site**: clean Ubuntu 22.04/24.04/26.04, Debian 12/13, Arch Linux and a Windows MSYS2 runner each do the documented setup and install by name. |
| `build-packages.yml` | called by the two above | The shared build, check, and install-test jobs. |

Each publish replaces the whole site with one release's packages (pacman
repositories only carry the latest version anyway). Older packages remain
attached to their GitHub releases.

## One-time setup before the first publication

Nothing here can be done by the workflows themselves.

1. **Create the signing key** on a machine you trust (Linux, WSL, or the
   MSYS2 shell; needs GnuPG 2.2+):

   ```bash
   packaging/repo/create-signing-key.sh ~/obicall-signing-key
   ```

   This makes a v4 Ed25519 signing key that expires in 3 years (accepted by
   apt 2.4 through 3.x, including Ubuntu 24.04+'s algorithm policy and
   apt 3's `sqv`, and by pacman), writes the public key to
   `packaging/repo/obicall-archive-keyring.asc`, and the secret key plus a
   revocation certificate to `~/obicall-signing-key/`. Your own keyring is
   not touched.

2. **Store the secret key in GitHub Actions** - it must never be committed
   or printed:

   ```bash
   gh secret set OBICALL_REPO_SIGNING_KEY --repo obinexus/obicall < ~/obicall-signing-key/obicall-repo-signing.secret.asc
   ```

   (If you give the key a passphrase yourself, also set
   `OBICALL_REPO_SIGNING_PASSPHRASE`.) Back up both files from
   `~/obicall-signing-key/` offline, then delete them from the machine.
   `packaging/repo/import-signing-key.sh` refuses any key whose fingerprint
   differs from the committed public key, so CI can only ever sign with it.

3. **Publish the fingerprint.** Commit
   `packaging/repo/obicall-archive-keyring.asc`, and replace the "not yet
   created" fingerprint in README.md's "Installing from package
   repositories" with the one the script printed. Users compare the
   downloaded key against the README on GitHub - a different channel from
   the package site.

4. **Enable GitHub Pages with source "GitHub Actions"**: repository
   Settings, Pages, Build and deployment, Source: GitHub Actions. Or:

   ```bash
   gh api -X POST repos/obinexus/obicall/pages -f build_type=workflow
   ```

   `publish-repos.yml` deploys from the default branch (a `workflow_run` or
   a manual run on `main`), so the `github-pages` environment's default
   branch protection is compatible with it.

## Releasing a version

1. Bump `project(... VERSION X.Y.Z)` in `CMakeLists.txt`; merge to `main`
   with the Linux, macOS, Windows, and Linux packages workflows green.
2. Tag the merge commit and push the tag. Never move or re-push a tag that
   has been pushed or released.

   ```bash
   git tag -a vX.Y.Z -m "Obicall X.Y.Z" && git push origin vX.Y.Z
   ```

3. Watch **Release**, then **Publish package repositories** (Actions tab).
   The release is done when both are green: the second one's verification
   jobs are the proof that `apt install obicall`, `pacman -Syu obicall`, and
   `pacman -S mingw-w64-ucrt-x86_64-obicall` work from the live site.
   If `workflow_run` did not start the publish run, start it by hand:
   `gh workflow run publish-repos.yml -f tag=vX.Y.Z`.
4. Commit the pinned recipes back to `main` (as was done for v0.1.1 in
   commit `f04cf98`): download `PKGBUILD-archlinux` and `PKGBUILD-msys2`
   from the release, or rerun the pinning commands above, and commit them
   over the two PKGBUILDs.
5. Optionally submit `PKGBUILD-archlinux` to the AUR (needs an AUR account
   and a `.SRCINFO`). That gives Arch users a recipe to build, not a binary
   repository; the `[obicall]` repository is what makes `pacman -S` work.

A failed publish leaves the previous site in place unless the deploy step
itself ran; to roll back, rerun `publish-repos.yml` for the previous tag.

## Key maintenance

- **Before it expires** (3 years after creation): in a GNUPGHOME holding the
  secret key, `gpg --quick-set-expire FPR 3y`, then re-export both halves,
  update the secret and the committed public key, and republish. Users
  re-download the key (`/etc/apt/keyrings/obicall-archive-keyring.gpg`, or
  `pacman-key --add` again) - the fingerprint does not change.
- **If the secret key leaks**: publish the revocation certificate, create a
  new key (steps 1-3 above), republish, and tell users to replace the key.

## Tested targets

"Tested" means: the package installed with its dependencies resolved by the
package manager on a clean system, and `packaging/smoke-test-installed.sh`
passed as an unprivileged user - `--help`, `doctor`, `validate`, `replay`,
a 6-second `run` in which both the C and the Python provider workers
connected for both brokers and both sensors' observations reached the
journal, and the broker-failover demo (`"overall":"pass"`).

- `.deb` (amd64, built on Ubuntu 22.04): Ubuntu 22.04, 24.04, 26.04;
  Debian 12, 13.
- Arch Linux package (x86_64): current Arch Linux (rolling).
- MSYS2 UCRT64 package: Windows, MSYS2 UCRT64.

Not built or tested, so not claimed: arm64 or any other architecture,
other Debian derivatives (Linux Mint, Pop!_OS, ...), Fedora/RPM, Alpine,
Arch derivatives (Manjaro, EndeavourOS, ...), other MSYS2 environments
(MINGW64, CLANG64, CLANGARM64).

## Validation record (2026-10-02, before the first Linux release)

Run on a Windows 11 workstation with Docker Desktop (clean official images,
`ubuntu:22.04/24.04/26.04`, `debian:12/13`, `archlinux:base-devel`) and the
local MSYS2 UCRT64 installation, from `git archive` snapshots of this
branch (version 0.1.2):

- **Test suite**: 18/18 on Ubuntu 24.04 (GCC 13.3, Debug, three
  consecutive runs); 18/18 with ASan/UBSan (GCC, the `linux.yml`
  configuration, twice); 18/18 in the Arch recipe's `check()` (GCC 16.2,
  CMake 4.4, Python 3.14); 18/18 in the MSYS2 recipe's `check()` (UCRT64
  GCC 16.2). Unmodified `main` did not compile on Linux.
- **`.deb`**: built on Ubuntu 22.04 with `build-deb.sh` (package checks
  passed: 7 binaries, no hooks, no build paths); installed and smoke-tested
  on all five targets above; removed cleanly. Negative controls: the smoke
  test fails when the Python adapter is removed; the reference
  `obicall_0.1.1-1_amd64.deb` that accompanied this work fails broker
  failover on Ubuntu 24.04 (the `SIGPIPE` cascade above).
- **Arch**: built with `makepkg --syncdeps` in a fresh container and with
  `makechrootpkg -c` in a devtools clean chroot (both 18/18 in `check()`;
  only `cmake` and `ninja` plus their dependencies were installed into the
  chroot); package checks passed; installed into a separate fresh container
  and smoke-tested. The earlier v0.1.1 Arch recipe does not compile with
  GCC 16 (v0.1.1 lacks the include fixes) and also discarded Arch's
  `CFLAGS`.
- **MSYS2**: `0.1.1-2` (from the pinned v0.1.1 archive, checksum
  re-verified: `45072f86...922f`) and `0.1.2-1` (snapshot) both built with
  `makepkg-mingw`; package checks passed; both smoke-tested from the
  extracted package. The released `0.1.1-1` fails the package check
  (hooks in `obicall-workerd.exe`).
- **Repositories, with a throwaway key over local HTTP**: `apt install
  obicall` by name on all five apt targets (apt 2.4.14, 2.6.1, 2.8.3,
  3.0.3, 3.2.0; no key-policy warnings); `pacman -Syu obicall` on Arch
  (pacman reports `Validated By: Signature`; the package signatures are
  embedded in the database); MSYS2's own pacman, with an isolated
  configuration, synced `[obicall-ucrt64]` and downloaded and verified
  `mingw-w64-ucrt-x86_64-obicall` by name. Rejections confirmed: unknown
  key (apt `NO_PUBKEY`; pacman "unknown key"), tampered APT index (size
  and hash mismatch), and a signing key that does not match the committed
  public key.
- **Not done here**: GitHub Pages hosting and the real signing key (both
  need the one-time setup above), the GitHub Actions runs themselves, and
  `pacman -S` into a real MSYS2 installation (covered by
  `publish-repos.yml`'s `verify-msys2` job once published).
