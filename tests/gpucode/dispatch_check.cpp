// SPDX-License-Identifier: MIT
// dispatch_check <translated.dxil> <original.cso> <translated.hlsl> [options]
//   -g X Y        thread groups (default 1 1)
//   -f FILL       memory fill: zero, ramp (dword i holds i; default) or rand
//   --ref R.dxil  also run a reference shader (the same kernel compiled for PC) and compare the
//                 memory both leave behind
//   --show        print the first dwords of every UAV window
//
// Runs one translated compute shader in isolation: the root signature comes from the original
// console container, every descriptor-table entry gets a view of pattern-filled memory (view kinds
// from the container's PSV0, structured strides and texture component types from the translated
// HLSL), the dispatch is timed with GPU timestamps and waited for with a timeout. With --ref the
// tool is a tier-1 test: exit code 0 means identical results.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cmath>
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
#define CHECK(x)                                                                                    \
    do {                                                                                            \
        HRESULT hr_ = (x);                                                                          \
        if (FAILED(hr_)) { printf("%s failed %08lx\n", #x, hr_); dump_messages(); exit(1); }        \
    } while (0)

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
    CHECK(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static ID3D12Resource* texture(DXGI_FORMAT f) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = 256;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES dh{D3D12_HEAP_TYPE_DEFAULT};
    ID3D12Resource* t = nullptr;
    CHECK(g_dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)));
    return t;
}
static void transition(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Transition.pResource = r;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &x);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: dispatch_check <translated.dxil> <original.cso> <translated.hlsl> [-g X Y] [-f zero|ramp|rand] [--ref R.dxil] [--show]\n");
        return 2;
    }
    auto dxil = read(argv[1]), cso = read(argv[2]), hlsl = read(argv[3]);
    std::vector<char> ref;
    unsigned gx = 1, gy = 1;
    std::string fill = "ramp";
    bool show = false;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 2 < argc) gx = atoi(argv[++i]), gy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fill = argv[++i];
        else if (!strcmp(argv[i], "--ref") && i + 1 < argc) ref = read(argv[++i]);
        else if (!strcmp(argv[i], "--show")) show = true;
    }
    if (dxil.empty() || cso.empty()) { printf("cannot read inputs\n"); return 1; }
    hlsl.push_back(0);

    gpucode::Container c;
    std::string err;
    if (!gpucode::read_container(cso.data(), cso.size(), c, &err)) { printf("container: %s\n", err.c_str()); return 1; }
    gpucode::RootSig rs;
    auto* rts = c.part("RTS0");
    if (!rts || !gpucode::parse_root_signature(rts->data(), rts->size(), rs, &err)) { printf("root signature: %s\n", err.c_str()); return 1; }
    std::vector<gpucode::PsvResource> psv;
    if (auto* p = c.part("PSV0")) psv = gpucode::parse_psv0(*p);

    // From the translated HLSL: structured strides ("struct E_Rn { uint w[k]; };" and
    // "StructuredBuffer<E_Rn> Rn : register(tR, spaceS)") and texture component types.
    std::map<std::string, unsigned> stride_of;   // "t3/0" -> bytes
    std::map<std::string, bool> uint_tex;        // "t3/0" -> integer texture
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
        for (const char* p = s; (p = strstr(p, "Buffer<")); p++) {   // typed buffers (Buffer<T>, RWBuffer<T>)
            if (p > s && (p[-1] == 'd' || p[-1] == 'e')) continue;   // StructuredBuffer<, ...
            const char* q = strstr(p, "register(");
            const char* nl = strchr(p, '\n');
            int r, sp;
            char kind;
            if (!q || (nl && q > nl)) continue;
            if (sscanf(q, "register(%c%d, space%d)", &kind, &r, &sp) == 3)
                uint_tex[std::string(1, kind) + std::to_string(r) + "/" + std::to_string(sp)] = !strncmp(p + 7, "uint", 4) || !strncmp(p + 7, "int", 3);
        }
        for (const char* p = s; (p = strstr(p, "Texture")); p++) {
            const char* lt = strchr(p, '<');
            const char* q = strstr(p, "register(");
            const char* nl = strchr(p, '\n');
            int r, sp;
            char kind;
            if (!lt || !q || (nl && q > nl) || (nl && lt > nl)) continue;
            if (sscanf(q, "register(%c%d, space%d)", &kind, &r, &sp) == 3)
                uint_tex[std::string(1, kind) + std::to_string(r) + "/" + std::to_string(sp)] = !strncmp(lt + 1, "uint", 4) || !strncmp(lt + 1, "int", 3);
        }
    }

    ID3D12Debug* dbg = nullptr;
    if (getenv("DEBUG_LAYER") && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev)));
    ID3D12RootSignature* root = nullptr;
    CHECK(g_dev->CreateRootSignature(0, cso.data(), cso.size(), IID_PPV_ARGS(&root)));
    auto make_pso = [&](std::vector<char>& code) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.CS = {code.data(), code.size()};
        ID3D12PipelineState* pso = nullptr;
        CHECK(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));
        return pso;
    };
    ID3D12PipelineState* pso = make_pso(dxil);
    ID3D12PipelineState* ref_pso = ref.empty() ? nullptr : make_pso(ref);

    // Memory: one UAV-capable buffer for every buffer view (1 MB window per descriptor slot), an
    // upload copy of its initial contents, upload memory for constants, and two textures.
    const uint64_t kBig = 64ull << 20, kCb = 1ull << 20;
    ID3D12Resource* big = buffer(D3D12_HEAP_TYPE_DEFAULT, kBig, true, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* up = buffer(D3D12_HEAP_TYPE_UPLOAD, kBig, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* cb = buffer(D3D12_HEAP_TYPE_UPLOAD, kCb, false, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* rbig = buffer(D3D12_HEAP_TYPE_READBACK, kBig, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rb = buffer(D3D12_HEAP_TYPE_READBACK, 16, false, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t* m = nullptr;
    srand(1);
    auto pattern = [&](uint32_t i) { return fill == "zero" ? 0u : fill == "rand" ? (uint32_t)rand() * 2654435761u : i; };
    up->Map(0, nullptr, (void**)&m);
    for (uint64_t i = 0; i < kBig / 4; i++) m[i] = pattern((uint32_t)i);
    // The float texture's source (at 32 MB): small finite values.
    for (uint64_t i = 0; i < 256 * 256 * 4; i++) ((float*)m)[(32ull << 20) / 4 + i] = (float)((i * 37) % 1024) / 64.0f;
    up->Unmap(0, nullptr);
    cb->Map(0, nullptr, (void**)&m);
    for (uint64_t i = 0; i < kCb / 4; i++) m[i] = fill == "zero" ? 0u : fill == "rand" ? (uint32_t)rand() : (uint32_t)(i % 64);
    cb->Unmap(0, nullptr);
    ID3D12Resource* tex_u = texture(DXGI_FORMAT_R32_UINT);
    ID3D12Resource* tex_f = texture(DXGI_FORMAT_R32G32B32A32_FLOAT);

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* views = nullptr;
    CHECK(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&views)));
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap* samplers = nullptr;
    CHECK(g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&samplers)));
    const UINT vinc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT sinc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    auto psv_kind = [&](bool uav, uint32_t reg, uint32_t space) -> uint32_t {
        for (auto& r : psv) {
            bool is_uav = r.type >= 6;
            if (r.type < 3 || is_uav != uav || r.space != space || reg < r.lo || reg > r.hi) continue;
            return r.kind;
        }
        return 0;
    };
    unsigned next_view = 0, next_sampler = 0;
    std::vector<std::pair<int, D3D12_GPU_DESCRIPTOR_HANDLE>> tables;
    std::vector<std::pair<int, D3D12_GPU_VIRTUAL_ADDRESS>> roots;
    std::vector<std::pair<uint32_t, uint64_t>> uav_windows;   // register, byte offset in `big`
    for (size_t pi = 0; pi < rs.params.size(); pi++) {
        auto& p = rs.params[pi];
        if (p.type == gpucode::ParamType::CBV) roots.push_back({(int)pi * 4 + 0, cb->GetGPUVirtualAddress()});
        if (p.type == gpucode::ParamType::SRV) roots.push_back({(int)pi * 4 + 1, big->GetGPUVirtualAddress()});
        if (p.type == gpucode::ParamType::UAV) {
            roots.push_back({(int)pi * 4 + 2, big->GetGPUVirtualAddress()});
            uav_windows.push_back({p.reg, 0});
        }
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
                uint64_t off = (uint64_t)(slot % 32) * (1u << 20);   // 1 MB window per slot (below the texture source)
                if (r.type == gpucode::RangeType::CBV) {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC v{cb->GetGPUVirtualAddress() + (slot % 16) * 65536, 65536};
                    g_dev->CreateConstantBufferView(&v, h);
                    continue;
                }
                bool uav = r.type == gpucode::RangeType::UAV;
                uint32_t kind = psv_kind(uav, reg, r.space);
                std::string key = std::string(uav ? "u" : "t") + std::to_string(reg) + "/" + std::to_string(r.space);
                if (kind == 10 || kind == 11 || kind == 12 || kind == 0) {
                    if (uav) uav_windows.push_back({reg, off});
                    unsigned stride = kind == 12 ? (stride_of.count(key) ? stride_of[key] : 4) : 4;
                    UINT n = (UINT)((1u << 20) / stride);
                    bool raw = kind == 11 || kind == 0;
                    bool tu = uint_tex.count(key) ? uint_tex[key] : true;
                    DXGI_FORMAT f = raw ? DXGI_FORMAT_R32_TYPELESS : kind == 10 ? (tu ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT) : DXGI_FORMAT_UNKNOWN;
                    if (uav) {
                        D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                        v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                        v.Format = f;
                        v.Buffer.FirstElement = off / stride;
                        v.Buffer.NumElements = n;
                        v.Buffer.StructureByteStride = kind == 12 ? stride : 0;
                        v.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
                        g_dev->CreateUnorderedAccessView(big, nullptr, &v, h);
                    } else {
                        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        v.Format = f;
                        v.Buffer.FirstElement = off / stride;
                        v.Buffer.NumElements = n;
                        v.Buffer.StructureByteStride = kind == 12 ? stride : 0;
                        v.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
                        g_dev->CreateShaderResourceView(big, &v, h);
                    }
                    continue;
                }
                bool is_uint = uint_tex.count(key) ? uint_tex[key] : true;
                ID3D12Resource* t = kind == 2 || kind == 7 ? (is_uint ? tex_u : tex_f) : nullptr;
                DXGI_FORMAT f = is_uint ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
                if (uav) {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC v{};
                    v.Format = f;
                    v.ViewDimension = kind == 7 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
                    if (kind == 7) v.Texture2DArray.ArraySize = 1;
                    g_dev->CreateUnorderedAccessView(t, nullptr, &v, h);
                } else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = f;
                    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    v.ViewDimension = kind == 7 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
                    if (kind == 7) v.Texture2DArray.ArraySize = 1, v.Texture2DArray.MipLevels = 1;
                    else v.Texture2D.MipLevels = 1;
                    g_dev->CreateShaderResourceView(t, &v, h);
                }
            }
        }
        D3D12_GPU_DESCRIPTOR_HANDLE g{(smp ? samplers : views)->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)base * (smp ? sinc : vinc)};
        tables.push_back({(int)pi, g});
    }

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ID3D12CommandAllocator* al = nullptr;
    CHECK(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ID3D12GraphicsCommandList* l = nullptr;
    CHECK(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&l)));
    CHECK(l->Close());
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2};
    ID3D12QueryHeap* ts = nullptr;
    CHECK(g_dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&ts)));
    ID3D12Fence* fence = nullptr;
    CHECK(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    uint64_t fv = 0;
    bool first = true;

    // One run: fresh memory, the dispatch, then a copy of the buffer memory to `out`.
    auto run = [&](ID3D12PipelineState* p, std::vector<uint32_t>& out, double& ms) -> bool {
        CHECK(al->Reset());
        CHECK(l->Reset(al, p));
        if (!first) {
            transition(l, big, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(l, tex_u, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            transition(l, tex_f, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        first = false;
        l->CopyBufferRegion(big, 0, up, 0, kBig);
        for (int k = 0; k < 2; k++) {
            D3D12_TEXTURE_COPY_LOCATION dst{k ? tex_f : tex_u, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION src{up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Offset = k ? (32ull << 20) : 0;
            src.PlacedFootprint.Footprint = {k ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32_UINT, 256, 256, 1, k ? 4096u : 1024u};
            l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        transition(l, big, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(l, tex_u, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(l, tex_f, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
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
        transition(l, big, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(rbig, 0, big, 0, kBig);
        CHECK(l->Close());
        ID3D12CommandList* lists[] = {l};
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fv);
        fence->SetEventOnCompletion(fv, ev);
        DWORD w = WaitForSingleObject(ev, 10000);
        HRESULT removed = g_dev->GetDeviceRemovedReason();
        if (w != WAIT_OBJECT_0 || FAILED(removed)) {
            printf("dispatch (%u, %u) did not finish: wait %lu, device %08lx\n", gx, gy, w, removed);
            return false;
        }
        uint64_t* t = nullptr, freq = 0;
        q->GetTimestampFrequency(&freq);
        rb->Map(0, nullptr, (void**)&t);
        ms = (double)(t[1] - t[0]) * 1000.0 / (double)freq;
        rb->Unmap(0, nullptr);
        uint32_t* d = nullptr;
        rbig->Map(0, nullptr, (void**)&d);
        out.assign(d, d + kBig / 4);
        rbig->Unmap(0, nullptr);
        return true;
    };

    std::vector<uint32_t> got, want;
    double ms = 0, ref_ms = 0;
    if (!run(pso, got, ms)) return 3;
    printf("dispatch (%u, %u), %s fill: %.3f ms\n", gx, gy, fill.c_str(), ms);
    if (show)
        for (auto& w : uav_windows)
            printf("  u%u: %08x %08x %08x %08x\n", w.first, got[w.second / 4], got[w.second / 4 + 1], got[w.second / 4 + 2], got[w.second / 4 + 3]);
    dump_messages();
    if (!ref_pso) return 0;
    if (!run(ref_pso, want, ref_ms)) return 3;
    printf("reference: %.3f ms\n", ref_ms);
    // Compare the UAV windows (1 MB each).
    size_t bad = 0, shown = 0;
    for (auto& w : uav_windows)
        for (size_t i = w.second / 4; i < (w.second + (1u << 20)) / 4; i++) {
            if (got[i] == want[i]) continue;
            if (shown++ < 12)
                printf("  u%u dword %zu: translated %08x (%g) reference %08x (%g)\n", w.first, i - w.second / 4, got[i], *(float*)&got[i], want[i],
                       *(float*)&want[i]);
            bad++;
        }
    printf("%s: %zu differing dwords\n", bad ? "MISMATCH" : "match", bad);
    return bad ? 4 : 0;
}
