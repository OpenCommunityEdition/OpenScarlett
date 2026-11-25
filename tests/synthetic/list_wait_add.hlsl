// SPDX-License-Identifier: MIT
// Adds a constant to the first 64 words of a buffer.
#define RS "UAV(u0), RootConstants(num32BitConstants=1, b0)"
RWByteAddressBuffer B : register(u0);
cbuffer C : register(b0) { uint amount; }
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) { B.Store(t.x * 4, B.Load(t.x * 4) + amount); }
