// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: a pixel shader that picks its texture per pixel, drawn so that most
// lanes are helper lanes. The console compiler turns the pick into a loop over the wave: take the
// first lane's index, serve the lanes that have it, go on with the rest. On the console the lanes
// that only exist for derivatives take part in that loop; a PC's wave operations leave them out.
// 4,096 triangles each cover one pixel of a 2 x 2 block: every drawn pixel must show the texture
// its own index names, and the draw must not take long (a loop that never ends for the helper
// lanes runs to the translator's step limit in each of them).
#include <windows.h>

#include <d3d12_xs.h>

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

static const UINT kSize = 128;

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
    list->Close();
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(&fence));
    UINT64 fenceValue = 0;
    auto submit = [&] {
        list->Close();
        ID3D12CommandList* l[] = {list};
        q->ExecuteCommandLists(1, l);
        q->Signal(fence, ++fenceValue);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
    };

    auto vs = read_file("helper_lanes_vs.cso"), ps = read_file("helper_lanes_ps.cso");
    CHECK(!vs.empty() && !ps.empty());
    ID3D12RootSignature* rs = nullptr;
    CHECK(SUCCEEDED(dev->CreateRootSignature(0, vs.data(), vs.size(), IID_GRAPHICS_PPV_ARGS(&rs))));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = nullptr;
    CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_GRAPHICS_PPV_ARGS(&pso))) && pso);
    if (!pso) return 1;

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    auto texture = [&](UINT size) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = size;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* r = nullptr;
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_GRAPHICS_PPV_ARGS(&r))) && r);
        return r;
    };
    // Four textures of one colour each (cleared as targets), and the target drawn to.
    static const float colours[4][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
    ID3D12Resource* tex[4];
    for (auto& t : tex) t = texture(4);
    ID3D12Resource* target = texture(kSize);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSize * 512;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readBuf = nullptr;
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&readBuf));
    D3D12_DESCRIPTOR_HEAP_DESC vh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *views = nullptr, *rtvs = nullptr;
    dev->CreateDescriptorHeap(&vh, IID_GRAPHICS_PPV_ARGS(&views));
    dev->CreateDescriptorHeap(&rh, IID_GRAPHICS_PPV_ARGS(&rtvs));
    const UINT vInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvs->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rInc; return h; };
    for (UINT i = 0; i < 4; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = views->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * vInc;
        dev->CreateShaderResourceView(tex[i], nullptr, h);
        dev->CreateRenderTargetView(tex[i], nullptr, rtv(i));
    }
    dev->CreateRenderTargetView(target, nullptr, rtv(4));
    auto move = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &x);
    };

    alloc->Reset();
    list->Reset(alloc, nullptr);
    for (UINT i = 0; i < 4; i++) {
        list->ClearRenderTargetView(rtv(i), colours[i], 0, nullptr);
        move(tex[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    const float black[4] = {0, 0, 0, 0};
    const D3D12_CPU_DESCRIPTOR_HANDLE out = rtv(4);
    list->ClearRenderTargetView(out, black, 0, nullptr);
    list->OMSetRenderTargets(1, &out, FALSE, nullptr);
    const D3D12_VIEWPORT vp = {0, 0, (float)kSize, (float)kSize, 0, 1};
    const D3D12_RECT sr = {0, 0, (LONG)kSize, (LONG)kSize};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sr);
    ID3D12DescriptorHeap* heaps[] = {views};
    list->SetDescriptorHeaps(1, heaps);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(rs);
    list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(64 * 64 * 3, 1, 0, 0);
    move(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, from = {};
    dst.pResource = readBuf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 512};
    from.pResource = target;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
    const ULONGLONG t0 = GetTickCount64();
    submit();
    const ULONGLONG took = GetTickCount64() - t0;

    uint8_t* data = nullptr;
    D3D12_RANGE r = {0, kSize * 512};
    readBuf->Map(0, &r, (void**)&data);
    UINT wrong = 0, drawn = 0, stray = 0;
    if (data)
        for (UINT y = 0; y < kSize; y++)
            for (UINT x = 0; x < kSize; x++) {
                const uint8_t* p = data + y * 512 + x * 4;
                const bool lit = p[3] >= 128;
                if (x % 2 || y % 2) { stray += lit; continue; }   // the three other pixels of each block are not drawn
                const float* c = colours[(x / 2 + y / 2 * 3) & 3];
                drawn += lit;
                wrong += !lit || (p[0] >= 128) != (c[0] > 0.5f) || (p[1] >= 128) != (c[1] > 0.5f) || (p[2] >= 128) != (c[2] > 0.5f);
            }
    std::printf("     %u of 4096 pixels drawn, %u with the wrong texture or missing, %u drawn outside; the draw took %llu ms\n", drawn, wrong, stray, took);
    CHECK(data && wrong == 0 && stray == 0);
    CHECK(took < 2000);
    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
