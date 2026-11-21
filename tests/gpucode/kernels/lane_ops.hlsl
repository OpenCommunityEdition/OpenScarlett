// SPDX-License-Identifier: MIT
// Per-lane cross-lane reads in wave64 code (ds_bpermute), LDS atomics at offsets past 256 bytes,
// and an unfused multiply-add. Reference: lane_ops_ref.hlsl (wave64 results through group-shared
// memory).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
RWByteAddressBuffer O : register(u0);
groupshared uint cnt[512];
[RootSignature(RS)][numthreads(128, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    uint x = A.Load((g.x * 128 + gi) * 4) * 2654435761u;
    if (gi < 8) cnt[300 + gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    InterlockedAdd(cnt[300 + (x & 7)], 1);
    uint other = WaveReadLaneAt(x, (x >> 8) & 31);           // per-lane index within the 32-lane half
    GroupMemoryBarrierWithGroupSync();
    float a = float(x & 0xffff) * 1.0e-3, b = float(x >> 16) * 3.3e-4, c = -a * b * 0.999;
    precise float p = a * b;
    precise float q = p + c;
    O.Store4((g.x * 128 + gi) * 16, uint4(other, cnt[300 + (gi & 7)], asuint(q), 0));
}
