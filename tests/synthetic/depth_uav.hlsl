// SPDX-License-Identifier: MIT
// Writes a class number per 8-texel column into a depth plane (as a 16-bit step) and a number per
// 8-texel row into a stencil plane, through unordered-access views.
#define RS "DescriptorTable(UAV(u0, numDescriptors=2)), RootConstants(num32BitConstants=2, b0)"
RWTexture2D<float> Depth : register(u0);
RWTexture2D<uint> Stencil : register(u1);
cbuffer C : register(b0) { uint withStencil, leftHalfOnly; }
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    if (leftHalfOnly != 0 && t.x >= 32) return;
    Depth[t.xy] = float(t.x / 8 + 1) / 65535.0;
    if (withStencil != 0) Stencil[t.xy] = t.y / 8 + 1;
}
