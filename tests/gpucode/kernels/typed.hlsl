// SPDX-License-Identifier: MIT
// Typed buffers: integer and float component types (the console view format decides; the
// translation infers it from how values are produced and used).
#include "common.hlsli"
ByteAddressBuffer A : register(t1);
Buffer<float4> F : register(t5);
Buffer<uint> U : register(t6);
RWBuffer<uint> Args : register(u4);
RWBuffer<float4> Out : register(u5);
cbuffer K : register(b0) { uint4 k[16]; };
[RootSignature(RS)][numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    uint i = t.x;
    uint n = A.Load(i * 4) & 1023, d = (k[0].y & 7) + 1;
    if (i < 3) Args[i] = i == 0 ? (n + d - 1) / d : 1;          // dispatch arguments
    Args[8 + i] = U[i * 2] * 3 + n;
    float4 f = F[i];
    Out[i] = f * 0.5 + float4(n, d, i, 1);
}
