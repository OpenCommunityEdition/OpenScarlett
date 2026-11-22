// SPDX-License-Identifier: MIT
// Packed half math with swizzles (op_sel), 16-bit group-shared memory and half texture loads,
// as used by temporal upscalers (compile with -enable-16bit-types).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
Texture2D<float4> T : register(t0);
RWByteAddressBuffer O : register(u0);
groupshared float16_t sh[256];
groupshared uint16_t shu[256];
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint i = g.x * 64 + gi;
    uint w = A.Load(i * 4) * 2654435761u;
    half4 t = (half4)T.Load(int3(i % 256, (i / 7) % 256, 0));
    half2 a = half2(half((w & 255) / 32.0), half(((w >> 8) & 255) / -16.0));
    half2 b = t.zx * a.yx + half2(t.w, 0.25h);
    half2 c = min(b, a.yy) - max(a, t.xy);
    sh[gi] = c.x;
    sh[gi + 64] = c.y;
    shu[gi] = uint16_t(w >> 16);
    GroupMemoryBarrierWithGroupSync();
    half2 d = half2(sh[63 - gi], sh[64 + (gi ^ 5)]);
    half2 e = d * c.yx + b;
    uint16_t u = shu[(gi * 3) & 63] + uint16_t(7);
    O.Store4(i * 16, uint4(asuint16(e.x) | (uint(asuint16(e.y)) << 16), asuint16(d.x) | (uint(asuint16(d.y)) << 16), u, asuint16(t.y)));
}
