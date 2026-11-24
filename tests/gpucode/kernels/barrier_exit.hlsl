// SPDX-License-Identifier: MIT
// Barriers that part of a thread group never reaches: threads past a count leave before the first
// barrier, and waves leave a loop with barriers in it at different rounds. A console wave that has
// ended keeps nobody waiting; on a PC every thread of the group has to make each barrier call (a
// group with a thread missing waits for good, and the GPU is reset). Reference:
// barrier_exit_ref.hlsl, the same results with barriers every thread reaches.
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u1);
groupshared uint sh[256];
[RootSignature(RS)][numthreads(256, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint n = 70 + (g.x * 37) % 150;   // threads taking part: whole waves and part of one stay out
    uint x = A.Load((g.x * 256 + gi) * 4) * 2654435761u;
    O.Store4((g.x * 256 + gi) * 16, uint4(0xdead0000u + gi, 0, 0, 0));
    if (gi >= n) return;
    sh[gi] = x & 0xffff;
    GroupMemoryBarrierWithGroupSync();
    uint acc = sh[(gi + 1) % n];
    GroupMemoryBarrierWithGroupSync();
    uint rounds = 1 + (gi >> 6);      // (the same for a whole 64-lane wave)
    for (uint r = 0; r < rounds; r++) {
        sh[gi] = acc + r;
        GroupMemoryBarrierWithGroupSync();
        acc += sh[(gi + 7) % n] & 0xff;
        GroupMemoryBarrierWithGroupSync();
    }
    O.Store4((g.x * 256 + gi) * 16, uint4(x, acc, n, rounds));
}
