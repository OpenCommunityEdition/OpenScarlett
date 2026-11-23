#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetch Microsoft's D3D12 Agility SDK runtime (NuGet package Microsoft.Direct3D.D3D12) into
# build/deps/agility. xsl-run places its D3D12Core.dll and d3d12SDKLayers.dll next to the title
# (in a D3D12 subdirectory) and the layer selects that runtime at start: shader model 6.6+
# DXIL that titles compile at run time needs it on Windows 10. Not committed; checksum pinned.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/build/deps/agility
ver=1.620.1
url=https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$ver/microsoft.direct3d.d3d12.$ver.nupkg
sha=a8a494c3e5f687d9be0a465ec8248da70781ab689d888db8806b600422722be6
[ -f "$out/x64/D3D12Core.dll" ] && { echo "already present: $out"; exit 0; }
rm -rf "$out"; mkdir -p "$out/x64"
curl -sSL -o "$out/pkg.nupkg" "$url"
echo "$sha  $out/pkg.nupkg" | sha256sum -c -
unzip -q -j -o "$out/pkg.nupkg" build/native/bin/x64/D3D12Core.dll build/native/bin/x64/d3d12SDKLayers.dll LICENSE.txt -d "$out/x64"
rm "$out/pkg.nupkg"
echo "$ver" > "$out/VERSION"
echo "installed to $out"
