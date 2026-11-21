// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: __XB_MemTime() in compute shaders, measured against GPU timestamps.
// The console clock runs at the GPU core clock (FramePacing assumes 1565 MHz on Lockhart, 1825 MHz
// on Anaconda); the layer must present it at that rate whatever the host's shader clock is.
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

int main(int argc, char** argv) {
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
    auto flush = [&] {
        q->Signal(fence, ++fenceValue);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fenceValue, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
    };

    // An alternative shader (argv[1]) may write up to 64 bytes; they are dumped raw.
    auto cs = read_file(argc > 1 ? argv[1] : "memtime.cso");
    CHECK(!cs.empty());
    ID3D12RootSignature* rs = nullptr;
    CHECK(SUCCEEDED(dev->CreateRootSignature(0, cs.data(), cs.size(), IID_GRAPHICS_PPV_ARGS(&rs))));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.CS = {cs.data(), cs.size()};
    ID3D12PipelineState* pso = nullptr;
    CHECK(SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_GRAPHICS_PPV_ARGS(&pso))) && pso);
    if (!pso) return 1;

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *gpuBuf = nullptr, *readBuf = nullptr;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_GRAPHICS_PPV_ARGS(&gpuBuf));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&readBuf));
    D3D12_QUERY_HEAP_DESC qhd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
    ID3D12QueryHeap* qh = nullptr;
    CHECK(SUCCEEDED(dev->CreateQueryHeap(&qhd, IID_GRAPHICS_PPV_ARGS(&qh))));

    auto sample = [&](UINT slot) {
        alloc->Reset();
        list->Reset(alloc, pso);
        list->SetComputeRootSignature(rs);
        list->SetComputeRootUnorderedAccessView(0, gpuBuf->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstant(1, slot, 0);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        list->Dispatch(1, 1, 1);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        list->Close();
        ID3D12CommandList* l[] = {list};
        q->ExecuteCommandLists(1, l);
        flush();
    };
    if (argc > 2) {
        // Pair mode (diagnostics): argv[1] writes a start time, argv[2] spins until argv[3] ms of
        // console clocks have passed (FramePacing's GetStartTime / EnforceGpuTime root layouts).
        auto cs2 = read_file(argv[2]);
        ID3D12RootSignature* rs2 = nullptr;
        dev->CreateRootSignature(0, cs2.data(), cs2.size(), IID_GRAPHICS_PPV_ARGS(&rs2));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd2 = {};
        pd2.pRootSignature = rs2;
        pd2.CS = {cs2.data(), cs2.size()};
        ID3D12PipelineState* pso2 = nullptr;
        dev->CreateComputePipelineState(&pd2, IID_GRAPHICS_PPV_ARGS(&pso2));
        UINT64 desired = UINT64(std::atof(argc > 3 ? argv[3] : "15") * 1000.0) * 1565;
        alloc->Reset();
        list->Reset(alloc, pso);
        list->SetComputeRootSignature(rs);
        list->SetComputeRootUnorderedAccessView(0, gpuBuf->GetGPUVirtualAddress());
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER u = {};
        u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        u.UAV.pResource = gpuBuf;
        list->ResourceBarrier(1, &u);
        list->SetPipelineState(pso2);
        list->SetComputeRootSignature(rs2);
        list->SetComputeRoot32BitConstants(0, 2, &desired, 0);
        list->SetComputeRootUnorderedAccessView(1, gpuBuf->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
        list->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        list->Close();
        ID3D12CommandList* l0[] = {list};
        q->ExecuteCommandLists(1, l0);
        flush();
    } else {
        sample(0);
        Sleep(250);
        sample(1);
    }

    alloc->Reset();
    list->Reset(alloc, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {gpuBuf, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(readBuf, 0, gpuBuf, 0, 64);
    list->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, readBuf, 128);
    list->Close();
    ID3D12CommandList* l[] = {list};
    q->ExecuteCommandLists(1, l);
    flush();

    UINT64* data = nullptr;
    D3D12_RANGE r = {0, 256};
    readBuf->Map(0, &r, (void**)&data);
    UINT64 m0 = data[0], m1 = data[1], ts0 = data[16], ts1 = data[18];
    if (argc > 2) {
        UINT64 f = 0;
        q->GetTimestampFrequency(&f);
        std::printf("pair: start %llu, last %llu, delta %llu ticks; spin took %.3f ms\n", data[0], data[1], data[1] - data[0],
                    double(data[18] - data[16]) / double(f) * 1e3);
        return 0;
    }
    if (argc > 1) {
        UINT64 f = 0;
        q->GetTimestampFrequency(&f);
        // Shaders given on the command line write {start, end} clocks of one dispatch at [0], [1].
        double dispatch = double(data[17] - data[16]) / double(f);
        std::printf("dispatch 0: %.3f ms, clock delta %llu: %.1f MHz\n", dispatch * 1e3, data[1] - data[0], double(data[1] - data[0]) / dispatch / 1e6);
    }
    if (argc > 1)
        for (int i = 0; i < 8; i++) std::printf("raw[%d] = %llu (hi %u lo %u)\n", i, data[i], (unsigned)(data[i] >> 32), (unsigned)data[i]);
    UINT64 tsFreq = 0;
    q->GetTimestampFrequency(&tsFreq);
    double seconds = double(ts1 - ts0) / double(tsFreq);
    double mhz = double(m1 - m0) / seconds / 1e6;
    std::printf("memtime %llu -> %llu over %.3f s (GPU timestamps): %.1f MHz\n", m0, m1, seconds, mhz);
    // The layer's back end reports which host clock stands in for the console's (vkd3d-proton sets
    // XSL_SHADER_CLOCK_SCOPE=device when the GPU has a device-scope shader clock).
    char scope[16] = "", scale[64] = "";
    GetEnvironmentVariableA("XSL_SHADER_CLOCK_SCOPE", scope, sizeof scope);
    GetEnvironmentVariableA("XSL_MEMTIME_SCALE", scale, sizeof scale);
    if (std::strcmp(scope, "device") == 0) {
        CHECK(m1 > m0);
        // With a scale configured the clock must read in console units: Lockhart's GPU runs at 1565 MHz.
        if (scale[0]) CHECK(mhz > 1565 * 0.9 && mhz < 1565 * 1.1);
        else std::printf("skip rate check: XSL_MEMTIME_SCALE not set (host clock %.1f MHz)\n", mhz);
    } else {
        std::printf("skip cross-dispatch checks: host has only a subgroup-scope shader clock\n");
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
