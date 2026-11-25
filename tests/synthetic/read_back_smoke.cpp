// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: a small render target read back through a buffer placed on title
// memory. A title meters its exposure this way every frame: it copies a 5 x 1 and a 1 x 1 float
// target into a buffer and reads the buffer's memory on the CPU. On the console the buffer is
// that memory; the copy must reach it.
#include <windows.h>

#include <d3d12_xs.h>
#include <xmem.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

static std::vector<char> read_file(const char* path) {
    std::vector<char> d;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        d.resize(std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
    return d;
}

int main() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params = {};
    params.Version = D3D12_SDK_VERSION;
    params.GraphicsCommandQueueRingSizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.GraphicsScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.ComputeScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.DisableGeometryShaderAllocations = TRUE;
    params.DisableTessellationShaderAllocations = TRUE;
    params.DisableDXR = TRUE;
    ID3D12Device* dev = nullptr;
    CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_GRAPHICS_PPV_ARGS(&dev))) && dev);
    if (!dev) return 1;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    ID3D12CommandQueue* q = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_GRAPHICS_PPV_ARGS(&q))));
    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_GRAPHICS_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_GRAPHICS_PPV_ARGS(&list));
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(&fence));

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 5;
    td.Height = td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* target = nullptr;
    CHECK(SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_GRAPHICS_PPV_ARGS(&target))) && target);
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap* rtvs = nullptr;
    dev->CreateDescriptorHeap(&rh, IID_GRAPHICS_PPV_ARGS(&rtvs));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(target, nullptr, rtv);

    // The buffer the title reads: placed on its own memory, filled with a pattern first.
    auto* mem = (float*)XMemVirtualAlloc(nullptr, 1 << 16, MEM_COMMIT | MEM_RESERVE | MEM_64K_PAGES, XMEM_GRAPHICS, PAGE_READWRITE);
    CHECK(mem != nullptr);
    if (!mem || !target) return 1;
    for (int i = 0; i < 1024; i++) mem[i] = -7.0f;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    CHECK(SUCCEEDED(dev->CreatePlacedResourceX((D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)mem, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&buffer))) && buffer);
    if (!buffer) return 1;

    const float colour[4] = {0.25f, 1.5f, 3.0f, 1.0f};
    list->ClearRenderTargetView(rtv, colour, 0, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {target, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, from = {};
    dst.pResource = buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 512;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 5, 1, 1, 256};
    from.pResource = target;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    list->Close();
    ID3D12CommandList* l[] = {list};
    q->ExecuteCommandLists(1, l);
    q->Signal(fence, 1);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, e);
    WaitForSingleObject(e, INFINITE);
    // The title reads its memory some time after its fence: within a few milliseconds here.
    bool there = false;
    for (int tries = 0; tries < 200 && !there; tries++) {
        there = mem[128] == colour[0] && mem[129] == colour[1] && mem[130] == colour[2] && mem[128 + 16] == colour[0] && mem[128 + 19] == colour[3];
        if (!there) Sleep(1);
    }
    std::printf("     the memory at the footprint reads %.2f %.2f %.2f %.2f; before it %.2f, after its 80 bytes %.2f\n", mem[128], mem[129], mem[130], mem[131], mem[127], mem[148]);
    CHECK(there);
    CHECK(mem[127] == -7.0f && mem[148] == -7.0f);   // only the texels were stored, not the padding of the row
    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
