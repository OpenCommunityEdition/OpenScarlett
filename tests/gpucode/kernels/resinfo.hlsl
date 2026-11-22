// SPDX-License-Identifier: MIT
// Texture size queries (image_get_resinfo on the console) at a run-time mip level.
#include "common.hlsli"
Texture2D<float4> T : register(t0);
Texture2D<uint> U : register(t4);
RWByteAddressBuffer O : register(u0);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    uint w, h, l, w2, h2, l2;
    T.GetDimensions(i & 1u ? 0u : 0u, w, h, l);
    U.GetDimensions(i >> 6, w2, h2, l2);
    O.Store4(i * 16, uint4(w + (h << 16), l, w2 ^ (h2 << 12), l2 * 7u + i));
}
