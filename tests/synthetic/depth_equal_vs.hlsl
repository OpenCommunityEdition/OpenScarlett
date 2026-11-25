// SPDX-License-Identifier: MIT
// One triangle over the target at the depth of a class: the class number times the float nearest
// 1/65535.
#define RS "RootConstants(num32BitConstants=1, b0)"
cbuffer C : register(b0) { uint cls; }
[RootSignature(RS)]
float4 main(uint i : SV_VertexID) : SV_Position {
    return float4(float(i & 1) * 4.0 - 1.0, float(i >> 1) * 4.0 - 1.0, float(cls) * asfloat(0x37800080u), 1.0);
}
