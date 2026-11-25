// SPDX-License-Identifier: MIT
// What gather_level.hlsl computes, with loads at the level: the four texels a filter would take
// there, in a gather's order, wrapped at the edges.
#include "common.hlsli"
Texture2D<float4> T : register(t0);
SamplerState S : register(s0);
RWByteAddressBuffer O : register(u0);
float4 gather(float2 uv, float lv, int2 ofs, uint ch) {
    uint w, h, l;
    T.GetDimensions(0, w, h, l);
    uint m = min(uint(round(lv)), l - 1);
    T.GetDimensions(m, w, h, l);
    int2 sz = int2(w, h);
    int2 q = int2(floor(uv * float2(sz) - 0.5)) + ofs;
    static const int2 corner[4] = {int2(0, 1), int2(1, 1), int2(1, 0), int2(0, 0)};
    float4 r;
    for (uint k = 0; k < 4; k++) r[k] = T.Load(int3(((q + corner[k]) % sz + sz) % sz, m))[ch];
    return r;
}
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    float2 uv = float2((float(i % 16) + 0.31) / 16.0 * 1.5 - 0.25, (float(i / 16) + 0.31) / 16.0);
    float lv = float(i % 3);
    O.Store4(i * 32, asuint(gather(uv, lv, int2(0, 0), 0)));
    O.Store4(i * 32 + 16, asuint(gather(uv, lv, int2(2, -1), 1)));
}
