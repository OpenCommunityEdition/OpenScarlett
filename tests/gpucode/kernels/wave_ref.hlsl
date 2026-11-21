// SPDX-License-Identifier: MIT
// wave.hlsl's results for 64-lane waves, computed through group-shared memory (any PC wave size).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u0);
groupshared uint xs[128];
[RootSignature(RS)][numthreads(128, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint x = A.Load((g.x * 128 + gi) * 4) * 2654435761u;
    xs[gi] = x;
    GroupMemoryBarrierWithGroupSync();
    uint w0 = gi & ~63u, lane = gi & 63u;
    uint sum = 0, pre = 0, mx = 0, cnt = 0;
    uint2 bal = 0;
    for (uint l = 0; l < 64; l++) {
        uint y = xs[w0 + l];
        bool p = (y & 3) == 0;
        sum += y & 255;
        mx = max(mx, y);
        if (l < lane) { pre += y & 15; cnt += p ? 1 : 0; }
        if (p) { if (l < 32) bal.x |= 1u << l; else bal.y |= 1u << (l - 32); }
    }
    O.Store4((g.x * 128 + gi) * 32, uint4(sum, pre, bal.x, bal.y));
    O.Store4((g.x * 128 + gi) * 32 + 16, uint4(xs[w0 + 37], mx, cnt, xs[w0] ^ lane));
}
