// SPDX-License-Identifier: MIT
// Image access with 16-bit coordinates and 16-bit results (A16/D16; -enable-16bit-types).
#include "common.hlsli"
Texture2D<float4> T : register(t0);
Texture2D<uint> U : register(t4);
SamplerState S : register(s0);
RWByteAddressBuffer O : register(u0);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID, uint3 gt : SV_GroupThreadID) {
    uint i = t.x;
    // Coordinates the compiler can prove fit in 16 bits (it then uses 16-bit addresses).
    uint2 c = uint2((i * 5) & 0xffu, (gt.x * 3) & 0xffu);
    half4 a = (half4)T.Load(int3(c, 0));
    uint b = U.Load(int3(c.yx, 0));
    half2 uv = half2(half(i) / 64.0h, half(i % 7) / 8.0h);
    half4 s = (half4)T.SampleLevel(S, uv, 0);
    O.Store4(i * 16, uint4(asuint16(a.x) | (uint(asuint16(a.w)) << 16), b, asuint16(s.y) | (uint(asuint16(s.z)) << 16), asuint16(a.y)));
}
