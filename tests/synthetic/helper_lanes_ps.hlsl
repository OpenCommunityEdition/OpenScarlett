// SPDX-License-Identifier: MIT
// Samples one of four textures, picked per pixel: the console compiler makes a loop that takes the
// first lane's index, serves the lanes with that index and goes on with the rest (a "waterfall").
#define RS "DescriptorTable(SRV(t0, numDescriptors=4), visibility=SHADER_VISIBILITY_PIXEL), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, visibility=SHADER_VISIBILITY_PIXEL)"
Texture2D<float4> T[4] : register(t0);
SamplerState S : register(s0);
[RootSignature(RS)]
float4 main(float4 p : SV_Position) : SV_Target {
    uint i = (uint(p.x) / 2 + uint(p.y) / 2 * 3) & 3;
    return T[NonUniformResourceIndex(i)].Sample(S, p.xy / 128.0);
}
