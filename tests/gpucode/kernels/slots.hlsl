// SPDX-License-Identifier: MIT
// Buffers and constants in table slots other than the first, raw/structured/typed views.
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
StructuredBuffer<uint4> B : register(t2);
Buffer<uint> C : register(t3);
cbuffer K : register(b1) { uint4 k[16]; };
RWByteAddressBuffer O : register(u2);
RWStructuredBuffer<uint2> P : register(u3);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    uint a = A.Load(i * 4) + A.Load((i * 7 % 256) * 4 + 1024);
    uint4 b = B[i % 300];
    uint c = C[i * 3];
    uint s = 0;
    for (uint j = 0; j < (k[1].x & 15); j++) s += A.Load(j * 16) ^ k[j & 15].y;   // bound from a constant buffer
    O.Store4(i * 16, uint4(a, b.y + b.w, c, s));
    P[i] = uint2(b.x * k[2].z, k[i & 15].w);
}
