// SPDX-License-Identifier: MIT
// Wave operations in wave64 compute code (the console's wave size). Reference: wave_ref.hlsl,
// which computes the same wave64 results through group-shared memory.
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u0);
[RootSignature(RS)][numthreads(128, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint x = A.Load((g.x * 128 + gi) * 4) * 2654435761u;
    bool p = (x & 3) == 0;
    uint sum = WaveActiveSum(x & 255);
    uint pre = WavePrefixSum(x & 15);
    uint4 bal = WaveActiveBallot(p);
    uint lane37 = WaveReadLaneAt(x, 37);
    uint mx = WaveActiveMax(x);
    uint cnt = WavePrefixCountBits(p);
    uint first = WaveReadLaneFirst(x);
    O.Store4((g.x * 128 + gi) * 32, uint4(sum, pre, bal.x, bal.y));
    O.Store4((g.x * 128 + gi) * 32 + 16, uint4(lane37, mx, cnt, first ^ WaveGetLaneIndex()));
}
