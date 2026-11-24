// SPDX-License-Identifier: MIT
// barrier_exit.hlsl's results with barriers that every thread of the group reaches (PC rules).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u1);
groupshared uint sh[256];
[RootSignature(RS)][numthreads(256, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint n = 70 + (g.x * 37) % 150;
    uint x = A.Load((g.x * 256 + gi) * 4) * 2654435761u;
    bool in_ = gi < n;
    if (in_) sh[gi] = x & 0xffff;
    GroupMemoryBarrierWithGroupSync();
    uint acc = in_ ? sh[(gi + 1) % n] : 0;
    GroupMemoryBarrierWithGroupSync();
    uint rounds = 1 + (gi >> 6);
    for (uint r = 0; r < 4; r++) {
        bool on = in_ && r < rounds;
        if (on) sh[gi] = acc + r;
        GroupMemoryBarrierWithGroupSync();
        if (on) acc += sh[(gi + 7) % n] & 0xff;
        GroupMemoryBarrierWithGroupSync();
    }
    O.Store4((g.x * 256 + gi) * 16, in_ ? uint4(x, acc, n, rounds) : uint4(0xdead0000u + gi, 0, 0, 0));
}
