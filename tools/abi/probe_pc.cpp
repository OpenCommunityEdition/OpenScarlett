// SPDX-License-Identifier: MIT
// ABI probe: prints sizes and offsets of shared D3D12 structs (compare with probe_xs).
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
int main(){
#define S(T) printf("%s %zu\n", #T, sizeof(T));
#include "structs.inc"
printf("PSO.pRootSignature %zu VS %zu InputLayout %zu RTVFormats %zu CachedPSO %zu Flags %zu\n", offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,pRootSignature),offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,VS),offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,InputLayout),offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,RTVFormats),offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,CachedPSO),offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC,Flags));
}
