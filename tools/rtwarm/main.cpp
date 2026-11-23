// SPDX-License-Identifier: MIT
// xsl-rtwarm <shard> <shards>: creates the GPU driver's compiled form of every cached ray-tracing
// program (rt_*.dxil in the shader cache, XSL_SHADER_CACHE) whose index modulo <shards> is <shard>,
// and stores it next to the DXIL (rtpso_*.bin) for the layer to load. The driver compiles one
// pipeline at a time per process and takes 15 s to minutes for each of these programs: run one
// process per shard (e.g. 12 of them) instead of waiting for the game to do it one by one.
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>

namespace pc { int rt_warm_shard(void* pcDevice, unsigned shard, unsigned shards); }

int main(int argc, char** argv) {
    unsigned shard = argc > 1 ? (unsigned)atoi(argv[1]) : 0, shards = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { fprintf(stderr, "no Direct3D 12 device (%08lx)\n", hr); return 1; }
    int n = pc::rt_warm_shard(dev, shard, shards ? shards : 1);
    fprintf(stderr, "shard %u/%u: %d programs\n", shard, shards, n);
    return 0;
}
