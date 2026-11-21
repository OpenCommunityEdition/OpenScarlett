// SPDX-License-Identifier: MIT
// dispatch_check <translated.dxil> <original.cso> <translated.hlsl> [groups-x [groups-y [fill]]]
//
// Runs one translated compute shader in isolation: the root signature comes from the original
// console container, every descriptor table entry gets a view of pattern-filled memory (view kinds
// from the container's PSV0, structured strides from the translated HLSL), the dispatch is timed
// with GPU timestamps and waited for with a timeout. Fill: zero, ramp (dword i holds i) or rand.
// Used to tell slow or hanging translations apart from bad title data.
#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "container.h"
#include "meta.h"

static std::vector<char> read(const char* p) {
    std::vector<char> v;
    FILE* f = fopen(p, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    v.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(v.data(), 1, v.size(), f);
    fclose(f);
    return v;
}

#define CHECK(x)                                                                    \
    do {                                                                            \
        HRESULT hr_ = (x);                                                          \
        if (FAILED(hr_)) { printf("%s failed %08lx\n", #x, hr_); dump_messages(); return 1; } \
    } while (0)

static ID3D12Device* g_dev;
static void dump_messages() {
    ID3D12InfoQueue* q = nullptr;
    if (!g_dev || FAILED(g_dev->QueryInterface(IID_PPV_ARGS(&q)))) return;
    for (UINT64 i = 0; i < q->GetNumStoredMessages(); i++) {
        SIZE_T n = 0;
        q->GetMessage(i, nullptr, &n);
        std::vector<char> m(n);
        auto* msg = (D3D12_MESSAGE*)m.data();
        q->GetMessage(i, msg, &n);
        printf("  [%d] %s\n", (int)msg->Severity, msg->pDescription);
    }
    q->ClearStoredMessages();
    q->Release();
}

static ID3D12Resource* buffer(D3D12_HEAP_TYPE heap, uint64_t size, bool uav, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: dispatch_check <translated.dxil> <original.cso> <translated.hlsl> [groups-x [groups-y [zero|ramp|rand]]]\n");
        return 2;
    }
    auto dxil = read(argv[1]), cso = read(argv[2]), hlsl = read(argv[3]);
    unsigned gx = argc > 4 ? atoi(argv[4]) : 1, gy = argc > 5 ? atoi(argv[5]) : 1;
    std::string fill = argc > 6 ? argv[6] : "ramp";
    hlsl.push_back(0);

    gpucode::Container c;
    std::string err;
    if (!gpucode::read_container(cso.data(), cso.size(), c, &err)) { printf("container: %s\n", err.c_str()); return 1; }
    gpucode::RootSig rs;
    auto* rts = c.part("RTS0");
    if (!rts || !gpucode::parse_root_signature(rts->data(), rts->size(), rs, &err)) { printf("root signature: %s\n", err.c_str()); return 1; }
    std::vector<gpucode::PsvResource> psv;
    if (auto* p = c.part("PSV0")) psv = gpucode::parse_psv0(*p);

    // Structured strides: "struct E_Rn { uint w[k]; };" ... "StructuredBuffer<E_Rn> Rn : register(tR, spaceS)".
    std::map<std::string, unsigned> stride_of;   // "t3/0" -> bytes
    {
        std::map<int, unsigned> words;
        const char* s = hlsl.data();
        for (const char* p = s; (p = strstr(p, "struct E_R")); p++) {
            int n, k;
            if (sscanf(p, "struct E_R%d { uint w[%d]; };", &n, &k) == 2) words[n] = 4u * k;
        }
        for (const char* p = s; (p = strstr(p, "StructuredBuffer<E_R")); p++) {
            int n, r, sp;
            char kind;
            const char* q = strstr(p, "register(");
            if (sscanf(p, "StructuredBuffer<E_R%d>", &n) == 1 && q && sscanf(q, "register(%c%d, space%d)", &kind, &r, &sp) == 3)
                stride_of[std::string(1, kind) + std::to_string(r) + "/" + std::to_string(sp)] = words.count(n) ? words[n] : 4;
        }
    }

    ID3D12Debug* dbg = nullptr;
    if (getenv("DEBUG_LAYER") && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)));
    ID3D12RootSignature* root = nullptr;
    CHECK(g_dev->CreateRootSignature(0, cso.data(), cso.size(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS = {dxil.data(), dxil.size()};
    ID3D12PipelineState* pso = nullptr;
    CHECK(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

    // Memory: one UAV-capable buffer for every buffer view, one upload buffer for constants.
    const uint64_t kBig = 64ull << 20, kCb = 1ull << 20;
    ID3D12Resource* big = buffer(D3D12_HEAP_TYPE_DEFAULT, kBig, true, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* up = buffer(D3D12_HEAP_TYPE_UPLOAD, kBig, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* cb = buffer(D3D12_HEAP_TYPE_UPLOAD, kCb, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!big || !up || !cb) { printf("buffer creation failed\n"); return 1; }
    uint32_t* m = nullptr;
    srand(1);
    auto pattern = [&](uint32_t i) { return fill == "zero" ? 0u : fill == "rand" ? (uint32_t)rand() * 2654435761u : i; };
    up->Map(0, nullptr, (void**)&m);
    for (uint64_t i = 0; i < kBig / 4; i++) m[i] = pattern((uint32_t)i);
    up->Unmap(0, nullptr);
    cb->Map(0, nullptr, (void**)&m);
    for (uint64_t i = 0; i < kCb / 4; i++) m[i] = fill == "zero" ? 0u : fill == "rand" ? (uint32_t)rand() : (uint32_t)(i % 64);
    cb->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 256;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* tex = nullptr;
    CHECK(g_dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex)));

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* views = nullptr;
    CHECK(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views)));
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* samplers = nullptr;
    CHECK(g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&samplers)));
    const UINT vinc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT sinc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    auto psv_kind = [&](uint32_t uav, uint32_t reg, uint32_t space) -> uint32_t {
        for (auto& r : psv) {
            bool is_uav = r.type >= 6;
            if (r.type < 3 || is_uav != !!uav || r.space != space || reg < r.lo || reg > r.hi) continue;
            return r.kind;
        }
        return 0;
    };
    unsigned next_view = 0, next_sampler = 0;
    std::vector<std::pair<uint32_t, uint64_t>> uav_windows;   // register, byte offset in `big`
    std::vector<std::pair<int, D3D12_GPU_DESCRIPTOR_HANDLE>> tables;
    std::vector<std::pair<int, D3D12_GPU_VIRTUAL_ADDRESS>> roots;
    for (size_t pi = 0; pi < rs.params.size(); pi++) {
        auto& p = rs.params[pi];
        if (p.type == gpucode::ParamType::CBV) roots.push_back({(int)pi * 4 + 0, cb->GetGPUVirtualAddress()});
        if (p.type == gpucode::ParamType::SRV) roots.push_back({(int)pi * 4 + 1, big->GetGPUVirtualAddress()});
        if (p.type == gpucode::ParamType::UAV) roots.push_back({(int)pi * 4 + 2, big->GetGPUVirtualAddress()});
        if (p.type != gpucode::ParamType::Table) continue;
        bool smp = !p.ranges.empty() && p.ranges[0].type == gpucode::RangeType::Sampler;
        uint32_t size = 0;
        for (auto& r : p.ranges) size = std::max(size, r.offset + (r.count == ~0u ? 64 : r.count));
        unsigned base = smp ? next_sampler : next_view;
        (smp ? next_sampler : next_view) += size;
        for (auto& r : p.ranges) {
            for (uint32_t i = 0; i < (r.count == ~0u ? 64 : r.count); i++) {
                uint32_t reg = r.base + i, slot = base + r.offset + i;
                if (smp) {
                    D3D12_SAMPLER_DESC s{};
                    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                    s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
                    s.MaxLOD = 1000;
                    g_dev->CreateSampler(&s, {samplers->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * sinc});
                    continue;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE h{views->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * vinc};
                uint64_t off = (uint64_t)(slot % 64) * (1u << 20);   // 1 MB window per slot
                if (r.type == gpucode::RangeType::CBV) {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC v{cb->GetGPUVirtualAddress() + (slot % 16) * 65536, 65536};
                    g_dev->CreateConstantBufferView(&v, h);
                    continue;
                }
                bool uav = r.type == gpucode::RangeType::UAV;
                uint32_t kind = psv_kind(uav, reg, r.space);
                std::string key = std::string(uav ? "u" : "t") + std::to_string(reg) + "/" + std::to_string(r.space);
                if (kind == 10 || kind == 11 || kind == 12 || kind == 0) {
                    unsigned stride = kind == 12 ? (stride_of.count(key) ? stride_of[key] : 4) : 4;
                    UINT n = (UINT)((1u << 20) / stride);
                    if (uav) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        v.Format = kind == 11 || kind == 0 ? DXGI_FORMAT_R32_TYPELESS : kind == 10 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
                        v.Buffer.FirstElement = off / stride;
                        v.Buffer.NumElements = n;
                        v.Buffer.StructureByteStride = kind == 12 ? stride : 0;
                        v.Buffer.Flags = kind == 11 || kind == 0 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
                        g_dev->CreateUnorderedAccessView(big, nullptr, &v, h);
                    } else {
                        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        v.Format = kind == 11 || kind == 0 ? DXGI_FORMAT_R32_TYPELESS : kind == 10 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
                        v.Buffer.FirstElement = off / stride;
                        v.Buffer.NumElements = n;
                        v.Buffer.StructureByteStride = kind == 12 ? stride : 0;
                        v.Buffer.Flags = kind == 11 || kind == 0 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
                        g_dev->CreateShaderResourceView(big, &v, h);
                    }
                } else if (uav) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_R32_UINT;
                    v.ViewDimension = kind == 2 ? D3D12_UAV_DIMENSION_TEXTURE2D : kind == 7 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
                    if (kind == 7) v.Texture2DArray.ArraySize = 1;
                    g_dev->CreateUnorderedAccessView(kind == 2 || kind == 7 ? tex : nullptr, nullptr, &v, h);
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_R32_UINT;
                    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    v.ViewDimension = kind == 7 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
                    if (kind == 7) v.Texture2DArray.ArraySize = 1, v.Texture2DArray.MipLevels = 1;
                    else v.Texture2D.MipLevels = 1;
                    g_dev->CreateShaderResourceView(kind == 2 || kind == 7 ? tex : nullptr, &v, h);
                }
            }
        }
        if (!smp)
            for (auto& r : p.ranges)
                if (r.type == gpucode::RangeType::UAV)
                    for (uint32_t i = 0; i < (r.count == ~0u ? 64 : r.count); i++)
                        uav_windows.push_back({r.base + i, (uint64_t)((base + r.offset + i) % 64) * (1u << 20)});
        D3D12_GPU_DESCRIPTOR_HANDLE g{(smp ? samplers : views)->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)base * (smp ? sinc : vinc)};
        tables.push_back({(int)pi, g});
    }

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ID3D12CommandAllocator* al = nullptr;
    CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ID3D12GraphicsCommandList* l = nullptr;
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, pso, IID_PPV_ARGS(&l)));
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
    ID3D12QueryHeap* ts = nullptr;
    CHECK(g_dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts)));
    ID3D12Resource* rb = buffer(D3D12_HEAP_TYPE_READBACK, 16, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rbig = buffer(D3D12_HEAP_TYPE_READBACK, kBig, false, D3D12_RESOURCE_STATE_COPY_DEST);

    l->CopyBufferRegion(big, 0, up, 0, kBig);
    D3D12_RESOURCE_BARRIER b{};
    b.Transition.pResource = big;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
    ID3D12DescriptorHeap* heaps[] = {views, samplers};
    l->SetDescriptorHeaps(2, heaps);
    l->SetComputeRootSignature(root);
    for (auto& t : tables) l->SetComputeRootDescriptorTable(t.first, t.second);
    for (auto& r : roots) {
        if (r.first % 4 == 0) l->SetComputeRootConstantBufferView(r.first / 4, r.second);
        if (r.first % 4 == 1) l->SetComputeRootShaderResourceView(r.first / 4, r.second);
        if (r.first % 4 == 2) l->SetComputeRootUnorderedAccessView(r.first / 4, r.second);
    }
    for (size_t pi = 0; pi < rs.params.size(); pi++)
        if (rs.params[pi].type == gpucode::ParamType::Constants)
            for (uint32_t k = 0; k < rs.params[pi].num32; k++) l->SetComputeRoot32BitConstant((UINT)pi, k, k);
    l->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    l->Dispatch(gx, gy, 1);
    l->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    l->ResolveQueryData(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 0);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    l->ResourceBarrier(1, &b);
    l->CopyBufferRegion(rbig, 0, big, 0, kBig);
    CHECK(l->Close());
    ID3D12CommandList* lists[] = {l};
    q->ExecuteCommandLists(1, lists);
    ID3D12Fence* f = nullptr;
    CHECK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    q->Signal(f, 1);
    f->SetEventOnCompletion(1, ev);
    DWORD w = WaitForSingleObject(ev, 10000);
    HRESULT removed = g_dev->GetDeviceRemovedReason();
    if (w != WAIT_OBJECT_0 || FAILED(removed)) {
        printf("dispatch (%u, %u) did not finish: wait %lu, device %08lx\n", gx, gy, w, removed);
        return 3;
    }
    uint64_t* t = nullptr, freq = 0;
    q->GetTimestampFrequency(&freq);
    rb->Map(0, nullptr, (void**)&t);
    printf("dispatch (%u, %u), %s fill: %.3f ms\n", gx, gy, fill.c_str(), (double)(t[1] - t[0]) * 1000.0 / (double)freq);
    rb->Unmap(0, nullptr);
    if (getenv("SHOW_UAVS")) {   // first dwords of each UAV window (debug stores land here)
        uint32_t* d = nullptr;
        rbig->Map(0, nullptr, (void**)&d);
        for (auto& w : uav_windows)
            for (int row = 0; row < (getenv("SHOW_UAVS")[0] == '2' ? 4 : 1); row++)
                printf("  u%u+%02x: %08x %08x %08x %08x\n", w.first, row * 16, d[w.second / 4 + row * 4], d[w.second / 4 + row * 4 + 1],
                       d[w.second / 4 + row * 4 + 2], d[w.second / 4 + row * 4 + 3]);
        rbig->Unmap(0, nullptr);
    }
    dump_messages();
    return 0;
}
