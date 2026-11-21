#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch Microsoft's DirectXShaderCompiler release (PC dxc) into build/deps/dxc-pc for the desktop
# reference builds (tests/desktop). Not committed; checksum pinned.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/deps/dxc-pc
url=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
sha=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
[ -x "$out/bin/x64/dxc.exe" ] && { echo "already present: $out"; exit 0; }
rm -rf "$out"; mkdir -p "$out"
curl -sSL -o "$out/dxc.zip" "$url"
echo "$sha  $out/dxc.zip" | sha256sum -c -
(cd "$out" && unzip -q dxc.zip && rm dxc.zip)
echo "installed to $out"
