// SPDX-License-Identifier: MIT
// A barrier in straight-line code that waves past a count skip (see barrier_exit.hlsl). Reference:
// barrier_flat_ref.hlsl.
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u1);
groupshared uint sh[256];
[RootSignature(RS)][numthreads(256, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint n = 40 + (g.x * 53) % 200;
    uint x = A.Load((g.x * 256 + gi) * 4) * 2654435761u;
    uint r0 = 0xdead0000u + gi, r1 = 0;
    if (gi < n) {
        sh[gi] = x >> 8;
        GroupMemoryBarrierWithGroupSync();
        r0 = sh[n - 1 - gi];
        r1 = sh[(gi * 5 + 3) % n];
    }
    O.Store4((g.x * 256 + gi) * 16, uint4(r0, r1, n, x));
}
