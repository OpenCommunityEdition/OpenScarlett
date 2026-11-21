#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the layer's pinned vkd3d-proton (v3.1) with the third_party patches applied:
#  - dxil-spirv-llvm-math-intrinsics: LLVM math intrinsics the Xbox DXC emits (llvm.copysign
#    for sign(), ...) that upstream dxil-spirv drops.
#  - dxil-spirv-xbox-intrinsics: xbox.* intrinsics (__XB_MemTime -> OpReadClockKHR).
#  - dxil-spirv-debug-programs: read debug programs (ILDB parts): skip debug locations and llvm.dbg calls.
#  - vkd3d-proton-openscarlett: enables VK_KHR_shader_clock for the above, and console rsqrt clamping
#    (FIXUP_RSQRT_INF_NAN) for every shader.
# Output: build/deps/vkd3d-proton-xsl/x64.
# Needs: git, meson, ninja, mingw-w64 (x86_64-w64-mingw32-gcc), widl, glslang.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
src=$here/build/deps/src/vkd3d-proton
if [ ! -d "$src" ]; then
  git clone --depth 1 --branch v3.1 --recurse-submodules --shallow-submodules \
      https://github.com/HansKristian-Work/vkd3d-proton.git "$src"
fi
for p in "$here"/third_party/dxil-spirv-*.patch; do
  git -C "$src/subprojects/dxil-spirv" apply --check "$p" 2>/dev/null && git -C "$src/subprojects/dxil-spirv" apply "$p" || echo "already applied: $(basename "$p")"
done
for p in "$here"/third_party/vkd3d-proton-*.patch; do
  git -C "$src" apply --check "$p" 2>/dev/null && git -C "$src" apply "$p" || echo "already applied: $(basename "$p")"
done
# enable_renderdoc: VKD3D_AUTO_CAPTURE_COUNTS triggers RenderDoc captures (xsl-run --renderdoc); inert otherwise.
[ -d "$src/build-x64" ] || meson setup "$src/build-x64" "$src" --cross-file "$src/build-win64.txt" --buildtype release -Denable_tests=false -Denable_renderdoc=true
meson configure "$src/build-x64" -Denable_renderdoc=true >/dev/null
ninja -C "$src/build-x64"
out=$here/build/deps/vkd3d-proton-xsl/x64
mkdir -p "$out"
cp "$src/build-x64/libs/d3d12/d3d12.dll" "$src/build-x64/libs/d3d12core/d3d12core.dll" "$out/"
echo "installed to $out"
