// SPDX-License-Identifier: MIT
// Reads a texture as colours and stores them, 16 bytes a texel in rows of 64.
#define RS "DescriptorTable(SRV(t0)), UAV(u0)"
Texture2D<float4> Colour : register(t0);
RWByteAddressBuffer Out : register(u0);
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    Out.Store4((t.y * 64 + t.x) * 16, asuint(Colour.Load(int3(t.xy, 0))));
}
