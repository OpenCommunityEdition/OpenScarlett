// SPDX-License-Identifier: MIT
// Gathers at a named level (the console's XB_GatherLevel methods: image_gather4_l and _l_o), with
// coordinates past the edges of a wrapping sampler.
#include "common.hlsli"
Texture2D<float4> T : register(t0);
SamplerState S : register(s0);
RWByteAddressBuffer O : register(u0);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    float2 uv = float2((float(i % 16) + 0.31) / 16.0 * 1.5 - 0.25, (float(i / 16) + 0.31) / 16.0);
    float lv = float(i % 3);
    float4 a = T.XB_GatherLevelRed(S, uv, lv);
    float4 b = T.XB_GatherLevelGreen(S, uv, lv, int2(2, -1));
    O.Store4(i * 32, asuint(a));
    O.Store4(i * 32 + 16, asuint(b));
}
