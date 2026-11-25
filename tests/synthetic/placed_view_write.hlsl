// SPDX-License-Identifier: MIT
// Packs a colour per texel as R9G9B9E5 bits into a 32-bit unsigned texture: mantissas 4x, 4y and
// 100 with exponent field 15 (a mantissa unit is then 1/512).
#define RS "DescriptorTable(UAV(u0))"
RWTexture2D<uint> Packed : register(u0);
[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 t : SV_DispatchThreadID) {
    Packed[t.xy] = (15u << 27) | (100u << 18) | ((t.y * 4u) << 9) | (t.x * 4u);
}
