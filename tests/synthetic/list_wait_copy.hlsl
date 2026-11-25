// SPDX-License-Identifier: MIT
// Copies the first 64 words of one buffer to another.
#define RS "UAV(u0), UAV(u1)"
RWByteAddressBuffer From : register(u0);
RWByteAddressBuffer To : register(u1);
[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) { To.Store(t.x * 4, From.Load(t.x * 4)); }
