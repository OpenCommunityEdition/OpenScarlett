// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: depth textures that compute shaders also write.
// The console gives a depth texture unordered-access views; a title writes a class number into
// each texel of the depth plane and a mask into the stencil plane from a compute shader, then
// draws with the depth test "equal" (and a stencil test) to reach the texels of one class.
//  A. 16-bit depth of one plane: written through a view, drawn against; then cleared as depth,
//     written in part through the view again (what the clear left must still be there).
//  B. 16-bit depth with stencil: both planes written through views, then one draw with the depth
//     and stencil tests, with all of its state set before the texture leaves the unordered-access
//     state.
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

static const UINT kSize = 64;
static const float kStep = 1.0f / 65535.0f;   // (the float nearest 1/65535, as the shaders have it)

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

    auto cs = read_file("depth_uav.cso"), vs = read_file("depth_equal_vs.cso"), ps = read_file("depth_equal_ps.cso");
    CHECK(!cs.empty() && !vs.empty() && !ps.empty());
    ID3D12RootSignature *csRs = nullptr, *gfxRs = nullptr;
    CHECK(SUCCEEDED(dev->CreateRootSignature(0, cs.data(), cs.size(), IID_GRAPHICS_PPV_ARGS(&csRs))));
    CHECK(SUCCEEDED(dev->CreateRootSignature(0, vs.data(), vs.size(), IID_GRAPHICS_PPV_ARGS(&gfxRs))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd = {};
    cpd.pRootSignature = csRs;
    cpd.CS = {cs.data(), cs.size()};
    ID3D12PipelineState* csPso = nullptr;
    CHECK(SUCCEEDED(dev->CreateComputePipelineState(&cpd, IID_GRAPHICS_PPV_ARGS(&csPso))) && csPso);
    // Draw pipelines: depth test "equal" without depth writes; for B also the stencil test "equal".
    auto draw_pso = [&](DXGI_FORMAT dsv, bool stencil) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = gfxRs;
        d.VS = {vs.data(), vs.size()};
        d.PS = {ps.data(), ps.size()};
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
        d.DepthStencilState.StencilEnable = stencil;
        d.DepthStencilState.StencilReadMask = 0xff;
        d.DepthStencilState.StencilWriteMask = 0;
        const D3D12_DEPTH_STENCILOP_DESC op = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL};
        d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace = op;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.DSVFormat = dsv;
        d.SampleDesc.Count = 1;
        ID3D12PipelineState* p = nullptr;
        CHECK(SUCCEEDED(dev->CreateGraphicsPipelineState(&d, IID_GRAPHICS_PPV_ARGS(&p))) && p);
        return p;
    };
    ID3D12PipelineState* psoA = draw_pso(DXGI_FORMAT_D16_UNORM, false);
    ID3D12PipelineState* psoB = draw_pso(DXGI_FORMAT_D16_UNORM_S8_UINT, true);
    if (!csPso || !psoA || !psoB) return 1;

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    auto texture = [&](DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = kSize;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Flags = flags;
        ID3D12Resource* r = nullptr;
        CHECK(SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_GRAPHICS_PPV_ARGS(&r))) && r);
        return r;
    };
    const D3D12_RESOURCE_FLAGS both = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* depthA = texture(DXGI_FORMAT_R16_TYPELESS, both, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depthB = texture(DXGI_FORMAT_D16_UNORM_S8_UINT, both, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* spare = texture(DXGI_FORMAT_R8_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* target = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!depthA || !depthB || !spare || !target) return 1;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kSize * 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readBuf = nullptr;
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&readBuf));

    D3D12_DESCRIPTOR_HEAP_DESC vh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ID3D12DescriptorHeap *views = nullptr, *rtvs = nullptr, *dsvs = nullptr;
    dev->CreateDescriptorHeap(&vh, IID_GRAPHICS_PPV_ARGS(&views));
    dev->CreateDescriptorHeap(&rh, IID_GRAPHICS_PPV_ARGS(&rtvs));
    dev->CreateDescriptorHeap(&dh, IID_GRAPHICS_PPV_ARGS(&dsvs));
    const UINT vInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT dInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    auto view = [&](UINT slot, ID3D12Resource* r, DXGI_FORMAT f, UINT plane) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = f;
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Texture2D.PlaneSlice = plane;
        D3D12_CPU_DESCRIPTOR_HANDLE h = views->GetCPUDescriptorHandleForHeapStart();
        h.ptr += slot * vInc;
        dev->CreateUnorderedAccessView(r, nullptr, &u, h);
    };
    // Tables of two views: A (depth, a spare for the unused stencil slot), B (depth plane, stencil plane).
    view(0, depthA, DXGI_FORMAT_R16_UNORM, 0);
    view(1, spare, DXGI_FORMAT_R8_UINT, 0);
    view(2, depthB, DXGI_FORMAT_R16_UNORM_X8_TYPELESS, 0);
    view(3, depthB, DXGI_FORMAT_X16_TYPELESS_G8_UINT, 1);
    auto table = [&](UINT slot) { D3D12_GPU_DESCRIPTOR_HANDLE g = views->GetGPUDescriptorHandleForHeapStart(); g.ptr += slot * vInc; return g; };
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(target, nullptr, rtv);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvA = dsvs->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvB = dsvA;
    dsvB.ptr += dInc;
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dv.Format = DXGI_FORMAT_D16_UNORM;
    dev->CreateDepthStencilView(depthA, &dv, dsvA);
    dv.Format = DXGI_FORMAT_D16_UNORM_S8_UINT;
    dev->CreateDepthStencilView(depthB, &dv, dsvB);

    auto move = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
        list->ResourceBarrier(1, &x);
    };
    auto begin = [&] {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        ID3D12DescriptorHeap* heaps[] = {views};
        list->SetDescriptorHeaps(1, heaps);
    };
    auto write = [&](UINT tableSlot, UINT withStencil, UINT leftHalfOnly) {
        list->SetPipelineState(csPso);
        list->SetComputeRootSignature(csRs);
        list->SetComputeRootDescriptorTable(0, table(tableSlot));
        const UINT c[2] = {withStencil, leftHalfOnly};
        list->SetComputeRoot32BitConstants(1, 2, c, 0);
        list->Dispatch(kSize / 8, kSize / 8, 1);
    };
    const D3D12_VIEWPORT vp = {0, 0, (float)kSize, (float)kSize, 0, 1};
    const D3D12_RECT sr = {0, 0, (LONG)kSize, (LONG)kSize};
    const float black[4] = {0, 0, 0, 0};
    auto draw_state = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE dsv, UINT cls) {
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearRenderTargetView(rtv, black, 0, nullptr);
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(gfxRs);
        list->SetGraphicsRoot32BitConstant(0, cls, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    std::vector<uint8_t> lit(kSize * kSize);
    // Ends the list with a copy of the target, runs it, and fills `lit` (1: the texel was drawn).
    auto finish = [&] {
        move(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = readBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kSize, kSize, 1, 256};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        move(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit();
        uint8_t* data = nullptr;
        D3D12_RANGE r = {0, kSize * 256};
        readBuf->Map(0, &r, (void**)&data);
        for (UINT y = 0; y < kSize; y++)
            for (UINT x = 0; x < kSize; x++) lit[y * kSize + x] = data && data[y * 256 + x * 4] >= 128;
        readBuf->Unmap(0, nullptr);
    };
    // How many texels differ from what `want(x, y)` says should be drawn.
    auto wrong = [&](auto want) {
        UINT n = 0, drawn = 0;
        for (UINT y = 0; y < kSize; y++)
            for (UINT x = 0; x < kSize; x++) { n += lit[y * kSize + x] != (want(x, y) ? 1 : 0); drawn += lit[y * kSize + x]; }
        std::printf("     %u texels drawn, %u wrong\n", drawn, n);
        return n;
    };

    // A. One plane of 16-bit depth: class numbers written through the view, class 3 drawn.
    begin();
    write(0, 0, 0);
    move(depthA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    draw_state(psoA, dsvA, 3);
    list->DrawInstanced(3, 1, 0, 0);
    finish();
    CHECK(wrong([](UINT x, UINT) { return x / 8 + 1 == 3; }) == 0);
    // Cleared as depth to class 5, then the left half written through the view: the right half
    // must still hold the clear when the view is written and when the texture is depth again.
    begin();
    list->ClearDepthStencilView(dsvA, D3D12_CLEAR_FLAG_DEPTH, 5.0f * kStep, 0, 0, nullptr);
    move(depthA, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    write(0, 0, 1);
    move(depthA, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    draw_state(psoA, dsvA, 5);
    list->DrawInstanced(3, 1, 0, 0);
    finish();
    CHECK(wrong([](UINT x, UINT) { return x >= 32; }) == 0);
    begin();
    draw_state(psoA, dsvA, 2);
    list->DrawInstanced(3, 1, 0, 0);
    finish();
    CHECK(wrong([](UINT x, UINT) { return x / 8 + 1 == 2; }) == 0);

    // B. Depth with stencil: both planes written through views. The draw's state is all set
    // before the texture leaves the unordered-access state; nothing is set again after.
    begin();
    write(2, 1, 0);
    draw_state(psoB, dsvB, 3);
    list->OMSetStencilRef(5);
    move(depthB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list->DrawInstanced(3, 1, 0, 0);
    finish();
    CHECK(wrong([](UINT x, UINT y) { return x / 8 + 1 == 3 && y / 8 + 1 == 5; }) == 0);
    begin();
    draw_state(psoB, dsvB, 7);
    list->OMSetStencilRef(2);
    list->DrawInstanced(3, 1, 0, 0);
    finish();
    CHECK(wrong([](UINT x, UINT y) { return x / 8 + 1 == 7 && y / 8 + 1 == 2; }) == 0);

    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
