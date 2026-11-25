// SPDX-License-Identifier: MIT
// 64 x 64 triangles, each over the centre of one pixel at even coordinates of a 128 x 128 target:
// in every 2 x 2 block of pixels one is drawn and three are helper lanes.
#define RS "DescriptorTable(SRV(t0, numDescriptors=4), visibility=SHADER_VISIBILITY_PIXEL), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_POINT, visibility=SHADER_VISIBILITY_PIXEL)"
[RootSignature(RS)]
float4 main(uint v : SV_VertexID) : SV_Position {
    uint t = v / 3, c = v % 3;
    float2 px = float2(float(t % 64) * 2.0, float(t / 64) * 2.0) + float2(c == 1 ? 1.4 : 0.0, c == 2 ? 1.4 : 0.0);
    return float4(px / 64.0 - 1.0, 0.5, 1.0) * float4(1.0, -1.0, 1.0, 1.0);
}
