// SPDX-License-Identifier: MIT
// 16-bit float and integer math (compile with -enable-16bit-types).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u0);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    uint w = A.Load(i * 4) * 2654435761u;
    float16_t2 h = float16_t2(float16_t((w & 255) / 16.0), float16_t(((w >> 8) & 255) / -32.0));
    float16_t2 g = h * h + float16_t2(1.5, -0.25);
    float16_t m = max(g.x, g.y) - min(h.x, h.y);
    int16_t2 s = int16_t2(int16_t(w), int16_t(w >> 16));
    int16_t2 r = max(s, int16_t2(-100, 100)) >> 2;
    uint16_t2 u = uint16_t2(uint16_t(w), uint16_t(w >> 16)) * uint16_t(3) + uint16_t(7);
    O.Store4(i * 16, uint4(asuint16(g.x) | (uint(asuint16(g.y)) << 16), asuint16(m), uint(uint16_t(r.x)) | (uint(uint16_t(r.y)) << 16), uint(u.x) | (uint(u.y) << 16)));
}
