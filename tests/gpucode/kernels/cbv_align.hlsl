// SPDX-License-Identifier: MIT
// Constants through a descriptor table, written out as they are read. With CB_SKEW=<bytes> set for
// dispatch_check (a multiple of 16: the console's constant buffer alignment, where a PC wants 256)
// the output shows whether the PC driver reads a view at such an address where it is.
#include "common.hlsli"
cbuffer K : register(b0) { uint4 k[16]; };
RWByteAddressBuffer O : register(u1);
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 g : SV_GroupID) {
    O.Store4((g.x * 64 + gi) * 16, k[gi & 15]);
}
