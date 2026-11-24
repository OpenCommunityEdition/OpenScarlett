// SPDX-License-Identifier: MIT
// texture_cost_check [count] [size]: what committed textures in video memory cost the process.
// Creates <count> (default 500) BC7 textures of <size> x <size> (default 1024) with all their
// levels in the DEFAULT heap and reports the process's private bytes and its write-combined
// regions before and after, and after releasing them.
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static double private_mb() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&c, sizeof c);
    return c.PrivateUsage / 1048576.0;
}
// Write-combined private regions: count and megabytes.
static void wc(size_t* count, double* mb) {
    *count = 0;
    *mb = 0;
    MEMORY_BASIC_INFORMATION m;
    for (uint8_t* a = (uint8_t*)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m; a = (uint8_t*)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && (m.Protect & PAGE_WRITECOMBINE)) { ++*count; *mb += m.RegionSize / 1048576.0; }
        if ((uintptr_t)m.BaseAddress + m.RegionSize >= 0x7FFFFFFF0000ull) break;
    }
}

int main(int argc, char** argv) {
    const int count = argc > 1 ? atoi(argv[1]) : 500, size = argc > 2 ? atoi(argv[2]) : 1024;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no device\n"); return 1; }
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = size;
    d.Height = size;
    d.DepthOrArraySize = 1;
    d.MipLevels = 0;
    d.Format = DXGI_FORMAT_BC7_UNORM;
    d.SampleDesc.Count = 1;
    const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &d);
    size_t n0, n1, n2;
    double w0, w1, w2;
    const double p0 = private_mb();
    wc(&n0, &w0);
    std::vector<ID3D12Resource*> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    for (int i = 0; i < count; i++) {
        ID3D12Resource* r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) break;
        tex.push_back(r);
    }
    const double p1 = private_mb();
    wc(&n1, &w1);
    for (auto* r : tex) r->Release();
    Sleep(1000);
    const double p2 = private_mb();
    wc(&n2, &w2);
    printf("%zu textures of %dx%d BC7 (%.2f MB each, %.0f MB in all): private bytes +%.0f MB, write-combined regions %zu -> %zu (%.0f -> %.0f MB); after release: private +%.0f MB, %zu regions (%.0f MB)\n",
           tex.size(), size, size, info.SizeInBytes / 1048576.0, tex.size() * (info.SizeInBytes / 1048576.0), p1 - p0, n0, n1, w0, w1, p2 - p0, n2, w2);
    return 0;
}
