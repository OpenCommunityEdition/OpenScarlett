// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: a view placed, in another format, on a texture the GPU writes.
// A compute shader packs colours as R9G9B9E5 bits into a 32-bit unsigned texture placed on title
// memory (shaders cannot store R9G9B9E5); the title then reads the texture through an R9G9B9E5
// view placed at the same address, without a resource of its own. On the console both are the
// same memory, so the view shows what the shader wrote.
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

static const UINT kSize = 64;

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

    auto wr = read_file("placed_view_write.cso"), rd = read_file("placed_view_read.cso");
    CHECK(!wr.empty() && !rd.empty());
    auto pipeline = [&](std::vector<char>& code, ID3D12RootSignature** rs) {
        CHECK(SUCCEEDED(dev->CreateRootSignature(0, code.data(), code.size(), IID_GRAPHICS_PPV_ARGS(rs))));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = *rs;
        d.CS = {code.data(), code.size()};
        ID3D12PipelineState* p = nullptr;
        CHECK(SUCCEEDED(dev->CreateComputePipelineState(&d, IID_GRAPHICS_PPV_ARGS(&p))) && p);
        return p;
    };
    ID3D12RootSignature *wrRs = nullptr, *rdRs = nullptr;
    ID3D12PipelineState *wrPso = pipeline(wr, &wrRs), *rdPso = pipeline(rd, &rdRs);
    if (!wrPso || !rdPso) return 1;

    // Title memory for the texture, and the texture the shader writes, placed on it.
    void* mem = XMemVirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE | MEM_64K_PAGES, XMEM_GRAPHICS, PAGE_READWRITE);
    CHECK(mem != nullptr);
    if (!mem) return 1;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = kSize;
    td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* packed = nullptr;
    CHECK(SUCCEEDED(dev->CreatePlacedResourceX((D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)mem, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_GRAPHICS_PPV_ARGS(&packed))) && packed);
    if (!packed) return 1;

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSize * kSize * 16;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *outBuf = nullptr, *readBuf = nullptr;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_GRAPHICS_PPV_ARGS(&outBuf));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&readBuf));

    D3D12_DESCRIPTOR_HEAP_DESC vh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    ID3D12DescriptorHeap* views = nullptr;
    dev->CreateDescriptorHeap(&vh, IID_GRAPHICS_PPV_ARGS(&views));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE c0 = views->GetCPUDescriptorHandleForHeapStart(), c1 = c0;
    c1.ptr += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = views->GetGPUDescriptorHandleForHeapStart(), g1 = g0;
    g1.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R32_UINT;
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(packed, nullptr, &uv, c0);
    // The view in the other format: described by a texture description and the address alone.
    D3D12_RESOURCE_DESC vd = td;
    vd.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    vd.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12XBOX_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    sv.ResourceLocation = (D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)mem;
    dev->CreatePlacedRawShaderResourceViewX(&vd, &sv, c1);

    ID3D12DescriptorHeap* heaps[] = {views};
    list->SetDescriptorHeaps(1, heaps);
    list->SetPipelineState(wrPso);
    list->SetComputeRootSignature(wrRs);
    list->SetComputeRootDescriptorTable(0, g0);
    list->Dispatch(kSize / 8, kSize / 8, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {packed, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &b);
    list->SetPipelineState(rdPso);
    list->SetComputeRootSignature(rdRs);
    list->SetComputeRootDescriptorTable(0, g1);
    list->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());
    list->Dispatch(kSize / 8, kSize / 8, 1);
    b.Transition = {outBuf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(readBuf, 0, outBuf, 0, kSize * kSize * 16);
    list->Close();
    ID3D12CommandList* l[] = {list};
    q->ExecuteCommandLists(1, l);
    q->Signal(fence, 1);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, e);
    WaitForSingleObject(e, INFINITE);

    float* data = nullptr;
    D3D12_RANGE r = {0, kSize * kSize * 16};
    readBuf->Map(0, &r, (void**)&data);
    UINT wrong = 0;
    if (data)
        for (UINT y = 0; y < kSize; y++)
            for (UINT x = 0; x < kSize; x++) {
                const float* p = data + (y * kSize + x) * 4;
                const float want[3] = {x * 4 / 512.0f, y * 4 / 512.0f, 100 / 512.0f};
                for (int k = 0; k < 3; k++) wrong += std::fabs(p[k] - want[k]) > 1e-6f;
            }
    if (data) std::printf("     texel (5, 9) reads %.5f %.5f %.5f (written: %.5f %.5f %.5f); %u components wrong\n", data[(9 * kSize + 5) * 4], data[(9 * kSize + 5) * 4 + 1],
                          data[(9 * kSize + 5) * 4 + 2], 20 / 512.0f, 36 / 512.0f, 100 / 512.0f, wrong);
    CHECK(data && wrong == 0);
    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
