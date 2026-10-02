#!/usr/bin/env bash
# Writes the landing page (index.html) of the published repository site,
# with the one-time setup for each package manager.
#
# Usage: packaging/repo/write-site-index.sh SITE_DIR BASE_URL VERSION FINGERPRINT
set -euo pipefail

site=${1:?usage: write-site-index.sh SITE_DIR BASE_URL VERSION FINGERPRINT}
base=${2:?} version=${3:?} fpr=${4:?}
base=${base%/}
fpr_spaced=$(echo "$fpr" | sed 's/.\{4\}/& /g; s/ $//')

cat > "$site/index.html" <<EOF
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Obicall package repositories</title>
<style>
  body { font: 16px/1.5 system-ui, sans-serif; max-width: 52rem; margin: 2rem auto; padding: 0 1rem; color: #1b1b1b; background: #fff; }
  pre { background: #f3f3f3; padding: .75rem; overflow-x: auto; }
  code { font-family: ui-monospace, monospace; }
  @media (prefers-color-scheme: dark) { body { color: #e6e6e6; background: #161616; } pre { background: #262626; } }
</style>
</head>
<body>
<h1>Obicall package repositories</h1>
<p>Signed APT and pacman repositories for <a href="https://github.com/obinexus/obicall">Obicall</a>
(current version ${version}). Packages are built and tested by the project's
<a href="https://github.com/obinexus/obicall/actions">GitHub Actions</a> release workflow.</p>

<h2>Signing key</h2>
<p>Fingerprint: <code>${fpr_spaced}</code></p>
<p>Check this fingerprint against the one in the project's
<a href="https://github.com/obinexus/obicall#installing-from-package-repositories">README on GitHub</a>
before trusting the key - not only against this page.
Key files: <a href="obicall-archive-keyring.gpg">obicall-archive-keyring.gpg</a> (APT),
<a href="obicall-archive-keyring.asc">obicall-archive-keyring.asc</a> (pacman).</p>

<h2>Debian and Ubuntu (amd64)</h2>
<pre><code>sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL ${base}/obicall-archive-keyring.gpg | sudo tee /etc/apt/keyrings/obicall-archive-keyring.gpg &gt; /dev/null
gpg --show-keys /etc/apt/keyrings/obicall-archive-keyring.gpg
sudo tee /etc/apt/sources.list.d/obicall.sources &gt; /dev/null &lt;&lt;'SRC'
Types: deb
URIs: ${base}/apt
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/obicall-archive-keyring.gpg
SRC
sudo apt update
sudo apt install obicall</code></pre>

<h2>Arch Linux (x86_64)</h2>
<pre><code>curl -fsSLO ${base}/obicall-archive-keyring.asc
gpg --show-keys obicall-archive-keyring.asc
sudo pacman-key --add obicall-archive-keyring.asc
sudo pacman-key --lsign-key ${fpr}
printf '\n[obicall]\nSigLevel = Required DatabaseRequired\nServer = ${base}/arch/\$arch\n' | sudo tee -a /etc/pacman.conf
sudo pacman -Syu obicall</code></pre>

<h2>Windows: MSYS2 UCRT64</h2>
<p>In the <strong>MSYS2 UCRT64</strong> shell (no sudo):</p>
<pre><code>curl -fsSLO ${base}/obicall-archive-keyring.asc
gpg --show-keys obicall-archive-keyring.asc
pacman-key --add obicall-archive-keyring.asc
pacman-key --lsign-key ${fpr}
printf '\n[obicall-ucrt64]\nSigLevel = Required DatabaseRequired\nServer = ${base}/msys2/ucrt64\n' &gt;&gt; /etc/pacman.conf
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-obicall</code></pre>
</body>
</html>
EOF
echo "wrote $site/index.html"
