// SPDX-License-Identifier: MIT
// Data-dependent loops, group-shared atomics and barriers, 128-thread groups.
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
cbuffer K : register(b0) { uint4 k[16]; };
RWByteAddressBuffer O : register(u1);
groupshared uint sh[256];
groupshared uint count;
[RootSignature(RS)][numthreads(128, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    if (gi == 0) count = 0;
    GroupMemoryBarrierWithGroupSync();
    uint x = A.Load((g.x * 128 + gi) * 4) * 2654435761u;
    uint n = 0;
    while (x > 1 && n < 200) { x = (x & 1) ? x * 3 + 1 : x >> 1; n++; }
    uint slot;
    InterlockedAdd(count, (n & 1) ? 1u : 0u, slot);
    sh[gi] = n;
    sh[gi + 128] = slot * 0;   // order of atomics is not deterministic
    GroupMemoryBarrierWithGroupSync();
    uint acc = 0;
    for (uint j = gi; j < 128; j += 3 + (k[0].x & 3)) acc += sh[j];
    GroupMemoryBarrierWithGroupSync();
    O.Store4((g.x * 128 + gi) * 16, uint4(n, acc, count, sh[127 - gi]));
}
