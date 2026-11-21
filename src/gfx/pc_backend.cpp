// SPDX-License-Identifier: AGPL-3.0-or-later
// PC half of d3d12_xs.dll: PC Direct3D 12 (Windows runtime, or Wine/vkd3d).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.h"
#include "xsl/log.h"

#define CH "gfx.pc"

namespace pc {
namespace {
DXGI_FORMAT stand_in(DXGI_FORMAT f);
template <class T> T* as(PcObj o) { return static_cast<T*>(o); }
// With XSL_D3D12_DEBUG=1: the debug layer's info queue, whose stored messages are copied into
// the log whenever a call fails (works without ID3D12InfoQueue1 callbacks, e.g. on Windows 10).
ID3D12InfoQueue* g_info = nullptr;
void dump_debug_messages() {
    if (!g_info) return;
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    UINT64 n = g_info->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < 64; i++) {
        SIZE_T len = 0;
        if (FAILED(g_info->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        auto* msg = (D3D12_MESSAGE*)buf.data();
        if (SUCCEEDED(g_info->GetMessage(i, msg, &len)))
            XSL_ERR("d3d12.debug", "[%d] %.*s", (int)msg->ID, (int)msg->DescriptionByteLength, msg->pDescription);
    }
    g_info->ClearStoredMessages();
}
HRESULT logged(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        XSL_ERR(CH, "%s failed %08lx", what, hr);
        dump_debug_messages();
    }
    return hr;
}
D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint64_t h) { return D3D12_CPU_DESCRIPTOR_HANDLE{(SIZE_T)h}; }
D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint64_t h) { return D3D12_GPU_DESCRIPTOR_HANDLE{h}; }
std::string g_adapter = "unknown";
Caps g_caps{};
IDXGIFactory4* g_factory = nullptr;

// Console resource-state encoding -> PC. They differ in two places: the console's PRESENT is
// 0x100000 (PC: 0, i.e. COMMON), and the console's GENERIC_READ (0x8c3) omits INDIRECT_ARGUMENT.
D3D12_RESOURCE_STATES pc_state(uint32_t x) {
    if (x & 0x100000) x &= ~0x100000u;  // PRESENT -> COMMON (PC PRESENT == 0)
    if ((x & 0x8c3) == 0x8c3) x |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    return (D3D12_RESOURCE_STATES)x;
}

bool env_on(const char* name) {
    char b[16];
    return GetEnvironmentVariableA(name, b, sizeof b) && b[0] != '0';
}
}  // namespace

HRESULT_T create_device(PcObj* out) {
    HRESULT hr;
    if (env_on("XSL_D3D12_DEBUG")) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); }
    }
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&g_factory));
    if (FAILED(hr)) { XSL_ERR(CH, "CreateDXGIFactory2 failed %08lx", hr); return hr; }
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(g_factory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        g_adapter = name;
    }
    ID3D12Device* dev = nullptr;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    for (auto fl : levels) {
        hr = D3D12CreateDevice(adapter, fl, IID_PPV_ARGS(&dev));
        if (SUCCEEDED(hr)) { XSL_INFO(CH, "PC device on '%s' at feature level %x", g_adapter.c_str(), fl); break; }
    }
    if (adapter) adapter->Release();
    if (FAILED(hr)) { XSL_ERR(CH, "D3D12CreateDevice failed %08lx", hr); return hr; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{5, fls, D3D_FEATURE_LEVEL_11_0};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6);
    dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    g_caps.meshShaders = o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    g_caps.samplerFeedback = o7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    g_caps.raytracing = o5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    g_caps.vrs = o6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    g_caps.featureLevel = fl.MaxSupportedFeatureLevel;
    XSL_INFO(CH, "host caps: feature level %x, mesh shaders %s, ray tracing %s, VRS %s, sampler feedback %s", g_caps.featureLevel,
             g_caps.meshShaders ? "yes" : "NO", g_caps.raytracing ? "yes" : "NO", g_caps.vrs ? "yes" : "NO", g_caps.samplerFeedback ? "yes" : "NO");
    if (env_on("XSL_D3D12_DEBUG") && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&g_info)))) {
        g_info->SetMessageCountLimit(4096);
        // The layer probes whether resources can be mapped (unified-memory emulation); the
        // refusals on default heaps are expected, so they are not stored.
        D3D12_MESSAGE_ID deny[] = {(D3D12_MESSAGE_ID)822};
        // Only errors are kept: the store is capped, and warnings would fill it long before the
        // error that matters.
        D3D12_MESSAGE_SEVERITY keep[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR};
        D3D12_INFO_QUEUE_FILTER f{};
        f.AllowList.NumSeverities = 2;
        f.AllowList.pSeverityList = keep;
        f.DenyList.NumIDs = 1;
        f.DenyList.pIDList = deny;
        g_info->AddStorageFilterEntries(&f);
        XSL_INFO(CH, "D3D12 debug layer on: its errors are logged after failing calls and command list closes (channel d3d12.debug)");
    }
    *out = dev;
    return S_OK;
}

const char* adapter_name() { return g_adapter.c_str(); }
const Caps& caps() { return g_caps; }
int format_stand_in(int f) { return stand_in((DXGI_FORMAT)f); }
void drop_map_shadow(PcObj o);
void release(PcObj o) {
    if (o && as<IUnknown>(o)->Release() == 0) drop_map_shadow(o);
}
void addref(PcObj o) { if (o) static_cast<IUnknown*>(o)->AddRef(); }

namespace {
std::mutex g_gv_mu;
ID3D12DescriptorHeap* g_gv = nullptr;
bool g_gv_failed = false;
uint32_t g_gv_size = 0, g_gv_inc = 0;
std::map<uint32_t, uint32_t> g_gv_free;  // free ranges: offset -> count
}  // namespace
PcObj global_view_heap(PcObj dev) {
    std::lock_guard<std::mutex> g(g_gv_mu);
    if (g_gv || g_gv_failed) return g_gv;
    char b[16];
    g_gv_size = GetEnvironmentVariableA("XSL_DESCRIPTOR_HEAP_SIZE", b, sizeof b) ? (uint32_t)atoi(b) : 0;
    if (g_gv_size < 4096) g_gv_size = 1000000;  // the D3D12 tier-2/3 maximum; titles allocate several 73k-entry heaps
    if (g_gv_size > 1000000) g_gv_size = 1000000;
    D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_gv_size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    g_gv_inc = as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(logged(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_gv)), "CreateDescriptorHeap(global)"))) {
        g_gv_failed = true;
        return nullptr;
    }
    g_gv_free[0] = g_gv_size;
    XSL_INFO(CH, "descriptor heaps: %u-entry shader-visible CBV/SRV/UAV heap shared by all title heaps (%u-byte descriptors; console 32)",
             g_gv_size, g_gv_inc);
    return g_gv;
}
PcObj global_view_heap_if_created() { return g_gv; }
bool global_view_alloc(PcObj dev, uint32_t count, uint32_t* offset, uint64_t* cpu, uint64_t* gpu) {
    if (!global_view_heap(dev) || count == 0) return false;
    std::lock_guard<std::mutex> g(g_gv_mu);
    for (auto it = g_gv_free.begin(); it != g_gv_free.end(); ++it) {
        if (it->second < count) continue;
        uint32_t off = it->first, left = it->second - count;
        g_gv_free.erase(it);
        if (left) g_gv_free[off + count] = left;
        *offset = off;
        *cpu = g_gv->GetCPUDescriptorHandleForHeapStart().ptr + (uint64_t)off * g_gv_inc;
        *gpu = g_gv->GetGPUDescriptorHandleForHeapStart().ptr + (uint64_t)off * g_gv_inc;
        return true;
    }
    return false;
}
void global_view_free(uint32_t offset, uint32_t count) {
    std::lock_guard<std::mutex> g(g_gv_mu);
    auto next = g_gv_free.lower_bound(offset);
    if (next != g_gv_free.end() && next->first == offset + count) { count += next->second; next = g_gv_free.erase(next); }
    if (next != g_gv_free.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) { prev->second += count; return; }
    }
    g_gv_free[offset] = count;
}
void add_ref(PcObj o) { if (o) as<IUnknown>(o)->AddRef(); }
void set_name(PcObj o, const wchar_t* name) { if (o && name) as<ID3D12Object>(o)->SetName(name); }

HRESULT_T create_queue(PcObj dev, const void* desc, PcObj* out) {
    D3D12_COMMAND_QUEUE_DESC d = *static_cast<const D3D12_COMMAND_QUEUE_DESC*>(desc);
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;  // drop Xbox priority bits
    d.Priority = 0;
    return logged(as<ID3D12Device>(dev)->CreateCommandQueue(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12CommandQueue**>(out))), "CreateCommandQueue");
}
HRESULT_T create_allocator(PcObj dev, int type, PcObj* out) {
    return logged(as<ID3D12Device>(dev)->CreateCommandAllocator((D3D12_COMMAND_LIST_TYPE)type, IID_PPV_ARGS(reinterpret_cast<ID3D12CommandAllocator**>(out))), "CreateCommandAllocator");
}
HRESULT_T create_list(PcObj dev, int type, PcObj alloc, PcObj pso, PcObj* out) {
    return logged(as<ID3D12Device>(dev)->CreateCommandList(0, (D3D12_COMMAND_LIST_TYPE)type, as<ID3D12CommandAllocator>(alloc),
                                                    as<ID3D12PipelineState>(pso), IID_PPV_ARGS(reinterpret_cast<ID3D12GraphicsCommandList**>(out))), "CreateCommandList");
}
HRESULT_T create_descriptor_heap(PcObj dev, const void* desc, PcObj* out) {
    D3D12_DESCRIPTOR_HEAP_DESC d = *static_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(desc);
    d.Flags &= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if ((int)d.Type > 3) d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;  // Xbox "sampler unbounded"
    return logged(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12DescriptorHeap**>(out))), "CreateDescriptorHeap");
}
uint32_t descriptor_increment(PcObj dev, int type) {
    if (type > 3) type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    return as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)type);
}
void heap_desc(PcObj h, void* out) { *static_cast<D3D12_DESCRIPTOR_HEAP_DESC*>(out) = as<ID3D12DescriptorHeap>(h)->GetDesc(); }
uint64_t heap_cpu_start(PcObj h) { return as<ID3D12DescriptorHeap>(h)->GetCPUDescriptorHandleForHeapStart().ptr; }
uint64_t heap_gpu_start(PcObj h) {
    D3D12_DESCRIPTOR_HEAP_DESC d = as<ID3D12DescriptorHeap>(h)->GetDesc();
    if (!(d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) return 0;
    return as<ID3D12DescriptorHeap>(h)->GetGPUDescriptorHandleForHeapStart().ptr;
}

HRESULT_T create_root_signature(PcObj dev, const void* blob, size_t size, PcObj* out) {
    void* clean = nullptr;
    size_t cleanSize = 0;
    bool owned = sanitize_container(blob, size, &clean, &cleanSize);
    HRESULT hr = as<ID3D12Device>(dev)->CreateRootSignature(0, owned ? clean : blob, owned ? cleanSize : size,
                                                            IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(out)));
    if (owned) free_container(clean);
    if (FAILED(hr)) XSL_ERR(CH, "CreateRootSignature failed %08lx (blob %zu bytes)", hr, size);
    return hr;
}

namespace {
struct ShaderFix {
    void* p[8] = {};
    ~ShaderFix() { for (void* x : p) if (x) free_container(x); }
    void fix(D3D12_SHADER_BYTECODE& bc, int slot) {
        if (!bc.pShaderBytecode || !bc.BytecodeLength) return;
        void* o; size_t n;
        if (sanitize_container(bc.pShaderBytecode, bc.BytecodeLength, &o, &n)) { p[slot] = o; bc.pShaderBytecode = o; bc.BytecodeLength = n; }
    }
};
}  // namespace

HRESULT_T create_graphics_pso(PcObj dev, const void* desc, PcObj* out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc);
    for (UINT i = 0; i < 8; i++)
        if (DXGI_FORMAT sf = stand_in(d.RTVFormats[i]); sf != DXGI_FORMAT_UNKNOWN) d.RTVFormats[i] = sf;
    ShaderFix fx;
    fx.fix(d.VS, 0); fx.fix(d.PS, 1); fx.fix(d.DS, 2); fx.fix(d.HS, 3); fx.fix(d.GS, 4);
    d.Flags = (D3D12_PIPELINE_STATE_FLAGS)(d.Flags & D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG);
    d.CachedPSO = {};
    d.NodeMask = 0;
    HRESULT hr = as<ID3D12Device>(dev)->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
    if (FAILED(hr)) XSL_ERR(CH, "CreateGraphicsPipelineState failed %08lx", hr);
    return hr;
}
HRESULT_T create_compute_pso(PcObj dev, const void* desc, PcObj* out) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc);
    ShaderFix fx;
    fx.fix(d.CS, 0);
    d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    d.CachedPSO = {};
    d.NodeMask = 0;
    HRESULT hr = as<ID3D12Device>(dev)->CreateComputePipelineState(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
    if (FAILED(hr)) XSL_ERR(CH, "CreateComputePipelineState failed %08lx", hr);
    return hr;
}

HRESULT_T create_pso_stream(PcObj dev, void* stream, size_t size, PcObj* out) {
    *out = nullptr;
    D3D12_PIPELINE_STATE_STREAM_DESC d{size, stream};
    ID3D12Device2* d2 = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d2));
    if (SUCCEEDED(hr)) {
        hr = d2->CreatePipelineState(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
        d2->Release();
    }
    if (FAILED(hr)) XSL_ERR(CH, "CreatePipelineState (stream, %zu bytes) failed %08lx", size, hr);
    return hr;
}
HRESULT_T import_host_memory(PcObj dev, void* base, uint64_t size, PcObj* res, uint64_t* gpuVa) {
    ID3D12Device3* d3 = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d3));
    if (FAILED(hr)) return logged(hr, "QueryInterface(ID3D12Device3)");
    ID3D12Heap* heap = nullptr;
    hr = d3->OpenExistingHeapFromAddress(base, IID_PPV_ARGS(&heap));
    d3->Release();
    if (FAILED(hr)) return logged(hr, "OpenExistingHeapFromAddress");
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    hr = as<ID3D12Device>(dev)->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    heap->Release();  // the placed resource keeps the heap alive
    if (FAILED(hr)) return logged(hr, "CreatePlacedResource(host memory)");
    *res = r;
    *gpuVa = r->GetGPUVirtualAddress();
    return S_OK;
}

void list_write_timestamp(PcObj dev, PcObj l, PcObj res, uint64_t offset) {
    static ID3D12QueryHeap* heap = nullptr;
    static std::atomic<uint32_t> next{0};
    constexpr uint32_t kCount = 4096;
    if (!heap) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kCount, 0};
        if (FAILED(as<ID3D12Device>(dev)->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)))) return;
    }
    uint32_t i = next++ % kCount;
    auto* lst = as<ID3D12GraphicsCommandList>(l);
    lst->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, i);
    lst->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, i, 1, as<ID3D12Resource>(res), offset);
}
HRESULT_T create_query_heap(PcObj dev, uint32_t type, uint32_t count, PcObj* out) {
    D3D12_QUERY_HEAP_DESC qd{(D3D12_QUERY_HEAP_TYPE)type, count, 0};
    return logged(as<ID3D12Device>(dev)->CreateQueryHeap(&qd, IID_PPV_ARGS(reinterpret_cast<ID3D12QueryHeap**>(out))), "CreateQueryHeap");
}
void list_begin_query(PcObj l, PcObj h, uint32_t t, uint32_t i) { as<ID3D12GraphicsCommandList>(l)->BeginQuery(as<ID3D12QueryHeap>(h), (D3D12_QUERY_TYPE)t, i); }
void list_end_query(PcObj l, PcObj h, uint32_t t, uint32_t i) { as<ID3D12GraphicsCommandList>(l)->EndQuery(as<ID3D12QueryHeap>(h), (D3D12_QUERY_TYPE)t, i); }
void list_resolve_query(PcObj l, PcObj h, uint32_t t, uint32_t s, uint32_t n, PcObj dst, uint64_t off) {
    as<ID3D12GraphicsCommandList>(l)->ResolveQueryData(as<ID3D12QueryHeap>(h), (D3D12_QUERY_TYPE)t, s, n, as<ID3D12Resource>(dst), off);
}
HRESULT_T queue_timestamp_frequency(PcObj q, uint64_t* f) { return as<ID3D12CommandQueue>(q)->GetTimestampFrequency(f); }
HRESULT_T queue_clock_calibration(PcObj q, uint64_t* g, uint64_t* c) { return as<ID3D12CommandQueue>(q)->GetClockCalibration(g, c); }

HRESULT_T create_command_signature(PcObj dev, const void* desc, PcObj rs, PcObj* out) {
    return logged(as<ID3D12Device>(dev)->CreateCommandSignature(static_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(desc), as<ID3D12RootSignature>(rs),
                                                         IID_PPV_ARGS(reinterpret_cast<ID3D12CommandSignature**>(out))), "CreateCommandSignature");
}

namespace {
// Formats the console can render to or write as UAVs but the PC cannot. Such resources are created
// with a renderable stand-in of similar precision, and views of them follow.
std::mutex g_remap_mu;
std::set<ID3D12Resource*> g_remapped;
DXGI_FORMAT stand_in(DXGI_FORMAT f) {
    // R16G16B16A16_FLOAT, not R11G11B10: typed UAV stores to the latter are optional on PC GPUs.
    return f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
}
bool remapped(PcObj r) {
    std::lock_guard<std::mutex> g(g_remap_mu);
    return g_remapped.count(static_cast<ID3D12Resource*>(r)) != 0;
}
template <class D> const D* view_fix(PcObj res, const void* desc, D& tmp) {
    if (!desc || !res || !remapped(res)) return static_cast<const D*>(desc);
    tmp = *static_cast<const D*>(desc);
    if (DXGI_FORMAT s = stand_in(tmp.Format); s != DXGI_FORMAT_UNKNOWN) tmp.Format = s;
    return &tmp;
}

D3D12_RESOURCE_DESC clean_desc(const void* desc) {
    D3D12_RESOURCE_DESC d = *static_cast<const D3D12_RESOURCE_DESC*>(desc);
    d.Flags = (D3D12_RESOURCE_FLAGS)(d.Flags & 0xBF);  // PC flags only; Xbox compression/DCC bits dropped
    if (d.Layout > D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}
D3D12_HEAP_FLAGS clean_heap_flags(uint32_t f) {
    // Xbox reuses 0x2 (ALLOW_INDIRECT_BUFFERS) where PC has SHARED; ALLOW_DISPLAY has no PC meaning here.
    return (D3D12_HEAP_FLAGS)(f & ~(0x2u | 0x8u) & 0xFFFFu);
}
}  // namespace

// Console memory is unified: titles Map DEFAULT-heap buffers (FramePacing reads its query buffer that
// way). Such buffers get CPU-visible memory chosen once per device: the GPU-upload heap (resizable BAR)
// where supported, else write-back system memory on UMA GPUs, else (discrete GPUs on runtimes
// without the GPU upload heap, e.g. Windows 10's) write-back system memory the GPU reads over
// the bus: slower for GPU-heavy buffers, but titles' CPU writes then reach the GPU.
// XSL_MAPPABLE_DEFAULT=0 disables this.
enum class MappableDefault { None, GpuUpload, UmaCustom, SystemCustom };
MappableDefault mappable_default(ID3D12Device* dev) {
    static MappableDefault m = [dev] {
        char b[4];
        if (GetEnvironmentVariableA("XSL_MAPPABLE_DEFAULT", b, 4) && b[0] == '0') return MappableDefault::None;
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof o16)) && o16.GPUUploadHeapSupported) {
            XSL_INFO(CH, "DEFAULT-heap buffers are CPU-mappable (GPU upload heap)");
            return MappableDefault::GpuUpload;
        }
        D3D12_FEATURE_DATA_ARCHITECTURE arch{};
        if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch)) && arch.UMA) {
            XSL_INFO(CH, "DEFAULT-heap buffers are CPU-mappable (UMA, write-back system memory)");
            return MappableDefault::UmaCustom;
        }
        XSL_WARN(CH, "no GPU upload heap on this runtime: DEFAULT-heap buffers live in system memory (CPU-mappable, slower GPU access)");
        return MappableDefault::SystemCustom;
    }();
    return m;
}

// Console memory is CPU-cached and coherent; a PC UPLOAD heap is write-combined, where CPU reads
// are uncached and two orders of magnitude slower. Titles (and the layer's constant-buffer
// re-alignment, which copies from the title's buffers at bind time) read their upload memory, so
// UPLOAD heaps become write-back system memory: the same memory pool, read by the GPU over the
// bus as before, but cached for the CPU. XSL_UPLOAD_CACHED=0 keeps PC UPLOAD heaps.
bool cached_upload(D3D12_HEAP_PROPERTIES* hp) {
    static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_UPLOAD_CACHED", b, 4) && b[0] == '0'); }();
    if (!on || hp->Type != D3D12_HEAP_TYPE_UPLOAD) return false;
    hp->Type = D3D12_HEAP_TYPE_CUSTOM;
    hp->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    hp->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    static volatile long once = 0;
    if (!InterlockedExchange(&once, 1)) XSL_INFO(CH, "UPLOAD heaps are CPU-cached system memory, as on the console (XSL_UPLOAD_CACHED=0 disables)");
    return true;
}

HRESULT_T create_committed_resource(PcObj dev, const void* heapProps, uint32_t heapFlags, const void* desc,
                                    uint32_t state, const void* clear, PcObj* out) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    D3D12_HEAP_PROPERTIES hp = *static_cast<const D3D12_HEAP_PROPERTIES*>(heapProps);
    hp.CreationNodeMask = hp.VisibleNodeMask = 0;
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        switch (mappable_default(as<ID3D12Device>(dev))) {
            case MappableDefault::GpuUpload: hp.Type = D3D12_HEAP_TYPE_GPU_UPLOAD; break;
            case MappableDefault::UmaCustom:
            case MappableDefault::SystemCustom:
                hp.Type = D3D12_HEAP_TYPE_CUSTOM;
                hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
                hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
                break;
            case MappableDefault::None: break;
        }
    }
    const D3D12_CLEAR_VALUE* cv = static_cast<const D3D12_CLEAR_VALUE*>(clear);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        !(d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        cv = nullptr;
    D3D12_CLEAR_VALUE cvTmp;
    DXGI_FORMAT standIn = stand_in(d.Format);
    bool remap = standIn != DXGI_FORMAT_UNKNOWN &&
                 (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    if (remap) {
        XSL_INFO(CH, "format %d is not renderable on PC; using %d for this resource", d.Format, standIn);
        d.Format = standIn;
        if (cv) { cvTmp = *cv; cvTmp.Format = standIn; cv = &cvTmp; }
    }
    D3D12_RESOURCE_STATES st = pc_state(state);
    if (hp.Type == D3D12_HEAP_TYPE_UPLOAD) st = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && cached_upload(&hp)) st = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (hp.Type == D3D12_HEAP_TYPE_READBACK) st = D3D12_RESOURCE_STATE_COPY_DEST;
    HRESULT hr = as<ID3D12Device>(dev)->CreateCommittedResource(&hp, clean_heap_flags(heapFlags), &d, st, cv,
                                                                IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out)));
    if (SUCCEEDED(hr) && remap) {
        std::lock_guard<std::mutex> g(g_remap_mu);
        g_remapped.insert(*reinterpret_cast<ID3D12Resource**>(out));
    }
    if (FAILED(hr))
        XSL_ERR(CH, "CreateCommittedResource failed %08lx (dim %d %llux%ux%u mips %u fmt %d flags %x layout %d align %llu "
                    "samples %u/%u heap %d cpu %d pool %d heapflags %x state %x)", hr, d.Dimension, (unsigned long long)d.Width, d.Height,
                d.DepthOrArraySize, d.MipLevels, d.Format, d.Flags, d.Layout, (unsigned long long)d.Alignment, d.SampleDesc.Count,
                d.SampleDesc.Quality, hp.Type, hp.CPUPageProperty, hp.MemoryPoolPreference, clean_heap_flags(heapFlags), state);
    return hr;
}
HRESULT_T create_reserved_resource(PcObj dev, const void* desc, uint32_t state, const void* clear, PcObj* out) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    d.Alignment = 0;
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) d.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    const D3D12_CLEAR_VALUE* cv = static_cast<const D3D12_CLEAR_VALUE*>(clear);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        !(d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        cv = nullptr;
    D3D12_CLEAR_VALUE cvTmp;
    DXGI_FORMAT standIn = stand_in(d.Format);
    bool remap = standIn != DXGI_FORMAT_UNKNOWN;
    if (remap) {
        d.Format = standIn;
        if (cv) { cvTmp = *cv; cvTmp.Format = standIn; cv = &cvTmp; }
    }
    HRESULT hr = logged(as<ID3D12Device>(dev)->CreateReservedResource(&d, pc_state(state), cv,
                                                                     IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out))), "CreateReservedResource");
    if (SUCCEEDED(hr) && remap) {
        std::lock_guard<std::mutex> g(g_remap_mu);
        g_remapped.insert(*reinterpret_cast<ID3D12Resource**>(out));
    }
    return hr;
}
std::vector<TileCoord> resource_tiles(PcObj dev, PcObj res) {
    std::vector<TileCoord> out;
    D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
    UINT numTiles = 0;
    D3D12_PACKED_MIP_INFO pm{};
    D3D12_TILE_SHAPE shape{};
    if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        as<ID3D12Device>(dev)->GetResourceTiling(as<ID3D12Resource>(res), &numTiles, nullptr, nullptr, nullptr, 0, nullptr);
        for (UINT i = 0; i < numTiles; i++) out.push_back({i, 0, 0, 0});
        return out;
    }
    UINT slices = rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : rd.DepthOrArraySize;
    UINT nSub = rd.MipLevels * slices;
    std::vector<D3D12_SUBRESOURCE_TILING> st(nSub);
    as<ID3D12Device>(dev)->GetResourceTiling(as<ID3D12Resource>(res), &numTiles, &pm, &shape, &nSub, 0, st.data());
    for (UINT s = 0; s < slices; s++) {
        for (UINT m = 0; m < pm.NumStandardMips; m++) {
            UINT sub = m + s * rd.MipLevels;
            const auto& t = st[sub];
            for (UINT z = 0; z < t.DepthInTiles; z++)
                for (UINT y = 0; y < t.HeightInTiles; y++)
                    for (UINT x = 0; x < t.WidthInTiles; x++) out.push_back({x, y, z, sub});
        }
        for (UINT i = 0; i < pm.NumTilesForPackedMips; i++) out.push_back({i, 0, 0, pm.NumStandardMips + s * rd.MipLevels});
    }
    return out;
}
HRESULT_T create_page_pool_heap(PcObj dev, uint64_t bytes, PcObj* out) {
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = bytes;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hd.Flags = D3D12_HEAP_FLAG_NONE;  // buffers and textures (resource heap tier 2)
    return logged(as<ID3D12Device>(dev)->CreateHeap(&hd, IID_PPV_ARGS(reinterpret_cast<ID3D12Heap**>(out))), "CreateHeap(page pool)");
}
void queue_map_tiles(PcObj q, PcObj res, uint32_t n, const TileCoord* tiles, PcObj heap, const uint32_t* heapPages) {
    if (!n) return;
    std::vector<D3D12_TILED_RESOURCE_COORDINATE> coords(n);
    std::vector<D3D12_TILE_REGION_SIZE> sizes(n);
    std::vector<D3D12_TILE_RANGE_FLAGS> flags(n, heap ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL);
    std::vector<UINT> offsets(n, 0), counts(n, 1);
    for (uint32_t i = 0; i < n; i++) {
        coords[i] = {tiles[i].x, tiles[i].y, tiles[i].z, tiles[i].subresource};
        sizes[i] = {1, FALSE, 1, 1, 1};
        if (heap) offsets[i] = heapPages[i];
    }
    as<ID3D12CommandQueue>(q)->UpdateTileMappings(as<ID3D12Resource>(res), n, coords.data(), sizes.data(), as<ID3D12Heap>(heap), n, flags.data(),
                                                  heap ? offsets.data() : nullptr, counts.data(), D3D12_TILE_MAPPING_FLAG_NONE);
}
HRESULT_T create_heap(PcObj dev, const void* desc, PcObj* out) {
    D3D12_HEAP_DESC d = *static_cast<const D3D12_HEAP_DESC*>(desc);
    d.Flags = clean_heap_flags(d.Flags);
    d.Properties.CreationNodeMask = d.Properties.VisibleNodeMask = 0;
    // Upload heaps (PC upload heaps hold buffers only, so the custom heap places the same resources);
    // the PC upload heap is used if the custom one cannot be created.
    if (d.Properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
        D3D12_HEAP_PROPERTIES hp = d.Properties;
        if (cached_upload(&hp)) {
            D3D12_HEAP_DESC c = d;
            c.Properties = hp;
            ID3D12Heap* h = nullptr;
            if (SUCCEEDED(as<ID3D12Device>(dev)->CreateHeap(&c, IID_PPV_ARGS(&h)))) {
                *out = h;
                return S_OK;
            }
        }
    }
    return logged(as<ID3D12Device>(dev)->CreateHeap(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12Heap**>(out))), "CreateHeap");
}
HRESULT_T create_placed_resource(PcObj dev, PcObj heap, uint64_t offset, const void* desc, uint32_t state, const void* clear, PcObj* out) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    const D3D12_CLEAR_VALUE* cv = static_cast<const D3D12_CLEAR_VALUE*>(clear);
    if (!(d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) cv = nullptr;
    return logged(as<ID3D12Device>(dev)->CreatePlacedResource(as<ID3D12Heap>(heap), offset, &d, pc_state(state), cv,
                                                       IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out))), "CreatePlacedResource");
}

void create_rtv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    D3D12_RENDER_TARGET_VIEW_DESC t;
    as<ID3D12Device>(dev)->CreateRenderTargetView(as<ID3D12Resource>(res), view_fix(res, desc, t), cpu(h));
}
void create_dsv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateDepthStencilView(as<ID3D12Resource>(res), static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc), cpu(h));
}
void create_srv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC t;
    as<ID3D12Device>(dev)->CreateShaderResourceView(as<ID3D12Resource>(res), view_fix(res, desc, t), cpu(h));
}
void create_uav(PcObj dev, PcObj res, PcObj counter, const void* desc, uint64_t h) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC t;
    as<ID3D12Device>(dev)->CreateUnorderedAccessView(as<ID3D12Resource>(res), as<ID3D12Resource>(counter), view_fix(res, desc, t), cpu(h));
}
void create_cbv(PcObj dev, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateConstantBufferView(static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc), cpu(h));
}
void create_sampler(PcObj dev, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateSampler(static_cast<const D3D12_SAMPLER_DESC*>(desc), cpu(h));
}
void copy_descriptors_simple(PcObj dev, uint32_t n, uint64_t dst, uint64_t src, int type) {
    if (type > 3) type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    as<ID3D12Device>(dev)->CopyDescriptorsSimple(n, cpu(dst), cpu(src), (D3D12_DESCRIPTOR_HEAP_TYPE)type);
}
void get_copyable_footprints(PcObj dev, const void* desc, uint32_t first, uint32_t num, uint64_t base, void* layouts,
                             uint32_t* rows, uint64_t* rowSizes, uint64_t* total) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    as<ID3D12Device>(dev)->GetCopyableFootprints(&d, first, num, base, static_cast<D3D12_PLACED_SUBRESOURCE_FOOTPRINT*>(layouts),
                                                 rows, rowSizes, total);
}
void get_resource_allocation_info(PcObj dev, uint32_t mask, uint32_t n, const void* descs, void* out) {
    auto* in = static_cast<const D3D12_RESOURCE_DESC*>(descs);
    D3D12_RESOURCE_DESC tmp[16];
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) tmp[i] = clean_desc(&in[i]);
    *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(out) = as<ID3D12Device>(dev)->GetResourceAllocationInfo(0, n, tmp);
}
HRESULT_T check_feature_support(PcObj dev, int feature, void* data, uint32_t size) {
    return as<ID3D12Device>(dev)->CheckFeatureSupport((D3D12_FEATURE)feature, data, size);
}
HRESULT_T create_fence(PcObj dev, uint64_t initial, uint32_t flags, PcObj* out) {
    return logged(as<ID3D12Device>(dev)->CreateFence(initial, (D3D12_FENCE_FLAGS)(flags & 0x7), IID_PPV_ARGS(reinterpret_cast<ID3D12Fence**>(out))), "CreateFence");
}

uint64_t fence_completed(PcObj f) { return as<ID3D12Fence>(f)->GetCompletedValue(); }
HRESULT_T fence_set_event(PcObj f, uint64_t v, void* e) { return as<ID3D12Fence>(f)->SetEventOnCompletion(v, (HANDLE)e); }
HRESULT_T fence_signal(PcObj f, uint64_t v) { return as<ID3D12Fence>(f)->Signal(v); }

// Console memory is unified: a title may Map any texture, GPU-only heaps and opaque (tiled)
// layouts included. PC D3D12 refuses that (E_INVALIDARG), so such a mapping gets CPU shadow
// memory: big enough for the console's tiled layout (which pads up to 64 KiB tiles, so twice the
// PC footprint), zeroed, and kept until the resource dies. CPU writes do not reach the GPU copy
// yet; titles seen so far only initialise these (Minecraft Preview's 1912x1046 render target).
struct MapShadow { void* base; uint64_t size; std::vector<uint64_t> offsets; };
std::mutex g_shadow_lock;
std::map<PcObj, MapShadow> g_shadows;

void drop_map_shadow(PcObj o) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto it = g_shadows.find(o);
    if (it == g_shadows.end()) return;
    VirtualFree(it->second.base, 0, MEM_RELEASE);
    g_shadows.erase(it);
}

HRESULT_T res_map(PcObj r, uint32_t sub, const void* range, void** out) {
    auto* res = as<ID3D12Resource>(r);
    HRESULT hr = res->Map(sub, static_cast<const D3D12_RANGE*>(range), out);
    if (SUCCEEDED(hr) || !out) return hr;
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return logged(hr, "Map");
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto it = g_shadows.find(r);
    if (it == g_shadows.end()) {
        ID3D12Device* dev = nullptr;
        res->GetDevice(IID_PPV_ARGS(&dev));
        UINT n = d.MipLevels * (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize);
        if (!n) n = 1;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(n);
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, n, 0, fp.data(), nullptr, nullptr, &total);
        UINT64 alloc = dev->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
        dev->Release();
        uint64_t size = (std::max<uint64_t>(total, alloc) * 2 + 0xFFFF) & ~0xFFFFull;
        void* base = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) return E_OUTOFMEMORY;
        MapShadow sh{base, size, {}};
        for (auto& f : fp) sh.offsets.push_back(f.Offset * 2);
        it = g_shadows.emplace(r, std::move(sh)).first;
        XSL_WARN(CH, "Map of a GPU-only texture (%llux%u x%u, %u mips, format %d): CPU shadow of %llu KiB; CPU writes stay on the CPU",
                 (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, (unsigned long long)(size >> 10));
    }
    const MapShadow& sh = it->second;
    *out = (uint8_t*)sh.base + (sub < sh.offsets.size() ? sh.offsets[sub] : 0);
    return S_OK;
}
void res_unmap(PcObj r, uint32_t sub, const void* range) {
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        if (g_shadows.count(r)) return;
    }
    as<ID3D12Resource>(r)->Unmap(sub, static_cast<const D3D12_RANGE*>(range));
}
void res_desc(PcObj r, void* out) { *static_cast<D3D12_RESOURCE_DESC*>(out) = as<ID3D12Resource>(r)->GetDesc(); }
uint64_t res_gpu_va(PcObj r) { return as<ID3D12Resource>(r)->GetGPUVirtualAddress(); }

HRESULT_T alloc_reset(PcObj a) { return logged(as<ID3D12CommandAllocator>(a)->Reset(), "CommandAllocator::Reset"); }

#define L as<ID3D12GraphicsCommandList>(l)
HRESULT_T list_close(PcObj l) {
    HRESULT hr = logged(L->Close(), "CommandList::Close");
    if (g_info && g_info->GetNumStoredMessages()) dump_debug_messages();  // errors recorded into a list that still closed
    return hr;
}
HRESULT_T list_reset(PcObj l, PcObj a, PcObj pso) { return logged(L->Reset(as<ID3D12CommandAllocator>(a), as<ID3D12PipelineState>(pso)), "CommandList::Reset"); }
void list_barriers(PcObj l, uint32_t n, const void* b) {
    if (!n) return;
    D3D12_RESOURCE_BARRIER tmp[64];
    auto* in = static_cast<const D3D12_RESOURCE_BARRIER*>(b);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        tmp[k] = in[i];
        if (tmp[k].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            tmp[k].Transition.StateBefore = pc_state(tmp[k].Transition.StateBefore);
            tmp[k].Transition.StateAfter = pc_state(tmp[k].Transition.StateAfter);
            if (tmp[k].Transition.StateBefore == tmp[k].Transition.StateAfter) continue;  // e.g. PRESENT <-> COMMON
        }
        k++;
    }
    if (k) L->ResourceBarrier(k, tmp);
}
void list_om_set_render_targets(PcObj l, uint32_t n, const uint64_t* rtvs, int single, const uint64_t* dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE r[8];
    uint32_t cnt = single ? (n ? 1 : 0) : n;
    for (uint32_t i = 0; i < cnt && i < 8; i++) r[i] = cpu(rtvs[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE d = dsv ? cpu(*dsv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    L->OMSetRenderTargets(n, n ? r : nullptr, single, dsv ? &d : nullptr);
}
void list_clear_rtv(PcObj l, uint64_t rtv, const float* c, uint32_t n, const void* rects) { L->ClearRenderTargetView(cpu(rtv), c, n, static_cast<const D3D12_RECT*>(rects)); }
void list_clear_dsv(PcObj l, uint64_t dsv, uint32_t f, float d, uint8_t s, uint32_t n, const void* rects) {
    L->ClearDepthStencilView(cpu(dsv), (D3D12_CLEAR_FLAGS)f, d, s, n, static_cast<const D3D12_RECT*>(rects));
}
void list_viewports(PcObj l, uint32_t n, const void* v) { L->RSSetViewports(n, static_cast<const D3D12_VIEWPORT*>(v)); }
void list_scissors(PcObj l, uint32_t n, const void* r) { L->RSSetScissorRects(n, static_cast<const D3D12_RECT*>(r)); }
void list_set_gfx_root_signature(PcObj l, PcObj rs) { L->SetGraphicsRootSignature(as<ID3D12RootSignature>(rs)); }
void list_set_compute_root_signature(PcObj l, PcObj rs) { L->SetComputeRootSignature(as<ID3D12RootSignature>(rs)); }
void list_set_pso(PcObj l, PcObj pso) { L->SetPipelineState(as<ID3D12PipelineState>(pso)); }
void list_ia_topology(PcObj l, int t) { L->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)t); }
void list_ia_vbs(PcObj l, uint32_t s, uint32_t n, const void* v) { L->IASetVertexBuffers(s, n, static_cast<const D3D12_VERTEX_BUFFER_VIEW*>(v)); }
void list_ia_ib(PcObj l, const void* v) { L->IASetIndexBuffer(static_cast<const D3D12_INDEX_BUFFER_VIEW*>(v)); }
void list_draw(PcObj l, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { L->DrawInstanced(a, b, c, d); }
void list_draw_indexed(PcObj l, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { L->DrawIndexedInstanced(a, b, c, d, e); }
void list_dispatch(PcObj l, uint32_t x, uint32_t y, uint32_t z) { L->Dispatch(x, y, z); }
void list_dispatch_mesh(PcObj l, uint32_t x, uint32_t y, uint32_t z) {
    if (!g_caps.meshShaders) return;
    ID3D12GraphicsCommandList6* l6 = nullptr;
    if (SUCCEEDED(L->QueryInterface(IID_PPV_ARGS(&l6)))) { l6->DispatchMesh(x, y, z); l6->Release(); }
}
void list_execute_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t ao, PcObj cnt, uint64_t co) {
    L->ExecuteIndirect(as<ID3D12CommandSignature>(sig), max, as<ID3D12Resource>(args), ao, as<ID3D12Resource>(cnt), co);
}
void list_clear_uav_uint(PcObj l, uint64_t g, uint64_t c, PcObj r, const uint32_t* v, uint32_t n, const void* rects) {
    L->ClearUnorderedAccessViewUint(gpu(g), cpu(c), as<ID3D12Resource>(r), v, n, static_cast<const D3D12_RECT*>(rects));
}
void list_clear_uav_float(PcObj l, uint64_t g, uint64_t c, PcObj r, const float* v, uint32_t n, const void* rects) {
    L->ClearUnorderedAccessViewFloat(gpu(g), cpu(c), as<ID3D12Resource>(r), v, n, static_cast<const D3D12_RECT*>(rects));
}
void list_write_immediate(PcObj l, uint64_t va, uint32_t value, bool bottom) {
    ID3D12GraphicsCommandList2* l2 = nullptr;
    if (FAILED(L->QueryInterface(IID_PPV_ARGS(&l2)))) return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{va, value};
    D3D12_WRITEBUFFERIMMEDIATE_MODE m = bottom ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
    l2->WriteBufferImmediate(1, &p, &m);
    l2->Release();
}
void list_discard(PcObj l, PcObj r) { L->DiscardResource(as<ID3D12Resource>(r), nullptr); }
void list_set_heaps(PcObj l, uint32_t n, PcObj* h) { L->SetDescriptorHeaps(n, reinterpret_cast<ID3D12DescriptorHeap* const*>(h)); }
void list_gfx_root_table(PcObj l, uint32_t i, uint64_t h) { L->SetGraphicsRootDescriptorTable(i, gpu(h)); }
void list_compute_root_table(PcObj l, uint32_t i, uint64_t h) { L->SetComputeRootDescriptorTable(i, gpu(h)); }
void list_gfx_root_cbv(PcObj l, uint32_t i, uint64_t va) { L->SetGraphicsRootConstantBufferView(i, va); }
void list_compute_root_cbv(PcObj l, uint32_t i, uint64_t va) { L->SetComputeRootConstantBufferView(i, va); }
void list_gfx_root_srv(PcObj l, uint32_t i, uint64_t va) { L->SetGraphicsRootShaderResourceView(i, va); }
void list_compute_root_srv(PcObj l, uint32_t i, uint64_t va) { L->SetComputeRootShaderResourceView(i, va); }
void list_gfx_root_uav(PcObj l, uint32_t i, uint64_t va) { L->SetGraphicsRootUnorderedAccessView(i, va); }
void list_compute_root_uav(PcObj l, uint32_t i, uint64_t va) { L->SetComputeRootUnorderedAccessView(i, va); }
void list_gfx_root_constants(PcObj l, uint32_t i, uint32_t n, const void* d, uint32_t o) { L->SetGraphicsRoot32BitConstants(i, n, d, o); }
void list_compute_root_constants(PcObj l, uint32_t i, uint32_t n, const void* d, uint32_t o) { L->SetComputeRoot32BitConstants(i, n, d, o); }
// Bytes per element and block width (1, or 4 for block-compressed) of a DXGI format; {0, 0} for
// formats this does not know.
struct FormatSize { UINT bytes, block; };
FormatSize format_size(DXGI_FORMAT f) {
    UINT v = (UINT)f;
    if (v >= 1 && v <= 4) return {16, 1};
    if (v >= 5 && v <= 8) return {12, 1};
    if (v >= 9 && v <= 22) return {8, 1};
    if ((v >= 23 && v <= 47) || v == 67 || (v >= 87 && v <= 93)) return {4, 1};
    if ((v >= 48 && v <= 59) || v == 85 || v == 86 || v == 115) return {2, 1};
    if (v >= 60 && v <= 64) return {1, 1};
    if ((v >= 70 && v <= 72) || (v >= 79 && v <= 81)) return {8, 4};
    if ((v >= 73 && v <= 78) || (v >= 82 && v <= 84) || (v >= 94 && v <= 99)) return {16, 4};
    return {0, 0};
}
bool misaligned_footprint(const D3D12_TEXTURE_COPY_LOCATION& c) {
    return c.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && c.PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
}
// Moves the footprint's offset down to the placement alignment, growing it by the rows and
// texels that now precede the data; *x0, *y0 (texels) is where the original data starts in it.
bool rebase_footprint(D3D12_PLACED_SUBRESOURCE_FOOTPRINT* p, UINT* x0, UINT* y0) {
    D3D12_SUBRESOURCE_FOOTPRINT& f = p->Footprint;
    UINT64 delta = p->Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    FormatSize fs = format_size(f.Format);
    bool ok = fs.bytes && f.RowPitch;
    UINT rows = 0, cols = 0;
    if (ok) {
        rows = (UINT)(delta / f.RowPitch);
        UINT rem = (UINT)(delta % f.RowPitch);
        cols = rem / fs.bytes;
        UINT widthBlocks = (f.Width + fs.block - 1) / fs.block;
        ok = rem % fs.bytes == 0 && (UINT64)(widthBlocks + cols) * fs.bytes <= f.RowPitch && (rows == 0 || f.Depth == 1);
    }
    if (!ok) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 4)
            XSL_WARN(CH, "texture copy with a buffer footprint at offset %llu (format %d, %ux%u, pitch %u) cannot be realigned: skipped",
                     (unsigned long long)p->Offset, (int)f.Format, f.Width, f.Height, f.RowPitch);
        return false;
    }
    p->Offset -= delta;
    *x0 = cols * fs.block;
    *y0 = rows * fs.block;
    f.Width += *x0;
    f.Height += *y0;
    static volatile long once = 0;
    if (!InterlockedExchange(&once, 1))
        XSL_INFO(CH, "texture copies with buffer footprints at 256-byte offsets are realigned to D3D12's 512 (first: offset %llu, "
                     "pitch %u, shifted %u rows, %u texels)", (unsigned long long)(p->Offset + delta), f.RowPitch, *y0, *x0);
    return true;
}
void list_copy_buffer_region(PcObj l, PcObj dst, uint64_t doff, PcObj src, uint64_t soff, uint64_t n) {
    L->CopyBufferRegion(as<ID3D12Resource>(dst), doff, as<ID3D12Resource>(src), soff, n);
}
void list_copy_texture_region(PcObj l, const void* dst, uint32_t x, uint32_t y, uint32_t z, const void* src, const void* box) {
    // Buffer footprints: the console places them at 256-byte offsets, PC D3D12 requires 512
    // (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) and fails the whole list otherwise. Such a
    // footprint is rebased to the aligned offset below it, and the copy region shifted by the
    // difference (whole rows, or texels within the row padding), which addresses the same bytes.
    D3D12_TEXTURE_COPY_LOCATION d = *static_cast<const D3D12_TEXTURE_COPY_LOCATION*>(dst);
    D3D12_TEXTURE_COPY_LOCATION s = *static_cast<const D3D12_TEXTURE_COPY_LOCATION*>(src);
    const D3D12_BOX* pb = static_cast<const D3D12_BOX*>(box);
    D3D12_BOX b{};
    UINT x0, y0;
    if (misaligned_footprint(d)) {
        if (!rebase_footprint(&d.PlacedFootprint, &x0, &y0)) return;
        x += x0;
        y += y0;
    }
    if (misaligned_footprint(s)) {
        const D3D12_SUBRESOURCE_FOOTPRINT f = s.PlacedFootprint.Footprint;
        if (!rebase_footprint(&s.PlacedFootprint, &x0, &y0)) return;
        b = pb ? *pb : D3D12_BOX{0, 0, 0, f.Width, f.Height, f.Depth};
        b.left += x0;
        b.right += x0;
        b.top += y0;
        b.bottom += y0;
        pb = &b;
    }
    L->CopyTextureRegion(&d, x, y, z, &s, pb);
}
void list_copy_resource(PcObj l, PcObj dst, PcObj src) { L->CopyResource(as<ID3D12Resource>(dst), as<ID3D12Resource>(src)); }
void list_resolve_subresource(PcObj l, PcObj dst, uint32_t ds, PcObj src, uint32_t ss, int fmt) {
    // The console resolves between resources whose sizes may differ (a 1080p target into a plane
    // sized from the window's client area); PC D3D12 requires identical sizes and puts the list in
    // an error state otherwise, so differing sizes resolve their overlap.
    auto* d = as<ID3D12Resource>(dst);
    auto* s = as<ID3D12Resource>(src);
    D3D12_RESOURCE_DESC dd = d->GetDesc(), sd = s->GetDesc();
    if (dd.Width != sd.Width || dd.Height != sd.Height) {
        ID3D12GraphicsCommandList1* l1 = nullptr;
        if (SUCCEEDED(L->QueryInterface(IID_PPV_ARGS(&l1)))) {
            D3D12_RECT r{0, 0, (LONG)std::min<UINT64>(dd.Width, sd.Width), (LONG)std::min<UINT>(dd.Height, sd.Height)};
            l1->ResolveSubresourceRegion(d, ds, 0, 0, s, ss, &r, (DXGI_FORMAT)fmt, D3D12_RESOLVE_MODE_AVERAGE);
            l1->Release();
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 2)
                XSL_INFO(CH, "resolve between %llux%u and %llux%u: resolving the overlap", (unsigned long long)sd.Width, sd.Height,
                         (unsigned long long)dd.Width, dd.Height);
            return;
        }
    }
    L->ResolveSubresource(d, ds, s, ss, (DXGI_FORMAT)fmt);
}
void list_blend_factor(PcObj l, const float* f) { L->OMSetBlendFactor(f); }
void list_stencil_ref(PcObj l, uint32_t r) { L->OMSetStencilRef(r); }
#undef L

void queue_execute(PcObj q, uint32_t n, PcObj* lists) {
    as<ID3D12CommandQueue>(q)->ExecuteCommandLists(n, reinterpret_cast<ID3D12CommandList* const*>(lists));
}
HRESULT_T queue_signal(PcObj q, PcObj f, uint64_t v) { return as<ID3D12CommandQueue>(q)->Signal(as<ID3D12Fence>(f), v); }
HRESULT_T queue_wait(PcObj q, PcObj f, uint64_t v) { return as<ID3D12CommandQueue>(q)->Wait(as<ID3D12Fence>(f), v); }

// Root signatures: console-only flags (0x01000000 and up: memory-based ABI, compute spill area)
// have no PC meaning and are masked off before the PC serializer sees them.
constexpr UINT kPcRootFlags = 0xFFF;
HRESULT_T serialize_root_signature(const void* desc, int version, PcObj* blob, PcObj* error) {
    D3D12_ROOT_SIGNATURE_DESC d = *static_cast<const D3D12_ROOT_SIGNATURE_DESC*>(desc);
    d.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(d.Flags & kPcRootFlags);
    HRESULT hr = D3D12SerializeRootSignature(&d, (D3D_ROOT_SIGNATURE_VERSION)version, reinterpret_cast<ID3DBlob**>(blob),
                                             reinterpret_cast<ID3DBlob**>(error));
    if (FAILED(hr)) XSL_ERR(CH, "D3D12SerializeRootSignature failed %08lx", hr);
    return hr;
}
HRESULT_T serialize_versioned_root_signature(const void* desc, PcObj* blob, PcObj* error) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = *static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(desc);
    switch (d.Version) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0: d.Desc_1_0.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(d.Desc_1_0.Flags & kPcRootFlags); break;
        case D3D_ROOT_SIGNATURE_VERSION_1_1: d.Desc_1_1.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(d.Desc_1_1.Flags & kPcRootFlags); break;
        default: d.Desc_1_2.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(d.Desc_1_2.Flags & kPcRootFlags); break;
    }
    HRESULT hr = D3D12SerializeVersionedRootSignature(&d, reinterpret_cast<ID3DBlob**>(blob), reinterpret_cast<ID3DBlob**>(error));
    if (FAILED(hr)) XSL_ERR(CH, "D3D12SerializeVersionedRootSignature failed %08lx", hr);
    return hr;
}
void* blob_ptr(PcObj b) { return as<ID3DBlob>(b)->GetBufferPointer(); }
size_t blob_size(PcObj b) { return as<ID3DBlob>(b)->GetBufferSize(); }
void* buffer_try_map(PcObj res) {
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    return SUCCEEDED(as<ID3D12Resource>(res)->Map(0, &none, &p)) ? p : nullptr;
}
HRESULT_T pso_cached_blob(PcObj pso, PcObj* blob) {
    return as<ID3D12PipelineState>(pso)->GetCachedBlob(reinterpret_cast<ID3DBlob**>(blob));
}

// ---------------------------------------------------------------------------------------------
// Presentation. The console scans out title-owned buffers; a PC needs a swap chain bound to the
// title's window. One swap chain per window, sized to the presented plane and stretched by DXGI.
namespace {
struct SwapState {
    IDXGISwapChain3* sc = nullptr;
    UINT w = 0, h = 0, count = 3;
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12CommandAllocator* alloc[3] = {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0, slotValue[3] = {};
    HANDLE event = nullptr;
    UINT slot = 0;
};
std::mutex g_present_mu;
uint64_t g_present_count = 0;

// XSL_CAPTURE=<path.bmp>[@frame]: write the presented plane of one frame (default 60) to a BMP.
// Used to check rendering against reference images without a screen grab.
void capture_plane(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12Resource* res, const char* path) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12Fence* f = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    l->Close();
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(f, 1);
    f->SetEventOnCompletion(1, e);
    WaitForSingleObject(e, INFINITE);
    CloseHandle(e);
    void* p = nullptr;
    D3D12_RANGE r{0, (SIZE_T)total};
    if (SUCCEEDED(rb->Map(0, &r, &p))) {
        UINT w = fp.Footprint.Width, h = fp.Footprint.Height;
        bool rgba = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        FILE* out = fopen(path, "wb");
        if (out) {
            BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0,
                                (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
            BITMAPINFOHEADER ih{sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB};
            fwrite(&fh, sizeof fh, 1, out);
            fwrite(&ih, sizeof ih, 1, out);
            for (UINT y = 0; y < h; y++) {
                auto* row = (uint8_t*)p + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
                if (rgba) for (UINT x = 0; x < w; x++) std::swap(row[x * 4], row[x * 4 + 2]);
                // Scan-out ignores alpha; titles may leave it 0, which viewers would show as transparent.
                for (UINT x = 0; x < w; x++) row[x * 4 + 3] = 0xFF;
                fwrite(row, 4, w, out);
            }
            fclose(out);
            XSL_INFO(CH, "captured %ux%u frame (format %d) to %s", w, h, rd.Format, path);
        }
        rb->Unmap(0, nullptr);
    }
    f->Release(); l->Release(); a->Release(); rb->Release();
}
std::map<HWND, SwapState> g_swap;

// Swap-chain format for a presented plane, and whether a plain copy can move the texels.
DXGI_FORMAT swap_format_for(DXGI_FORMAT f, bool* copyable) {
    *copyable = true;
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            // Float and other planes (R16G16B16A16_FLOAT, R11G11B10, stand-ins) are drawn into an
            // sRGB swap chain by the blit shader: SDR output for now (HDR scan-out not implemented).
            *copyable = false;  // e.g. R11G11B10 / X-format planes: drawn with the blit shader
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

// Shader blit for planes that cannot be copied into the swap chain. Shaders are compiled at run time
// by d3dcompiler_47 (present on Windows and in Wine), so no SDK tool is involved.
struct Blitter {
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* srv = nullptr;
    ID3D12DescriptorHeap* rtv = nullptr;
    UINT srvInc = 0, rtvInc = 0, slot = 0;
    bool failed = false;
};
Blitter g_blit;
typedef HRESULT(WINAPI* PFN_D3DCompile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT,
                                         ID3DBlob**, ID3DBlob**);
bool blit_init(ID3D12Device* dev) {
    if (g_blit.pso) return true;
    if (g_blit.failed) return false;
    g_blit.failed = true;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = m ? (PFN_D3DCompile)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) { XSL_ERR(CH, "blit: d3dcompiler_47 unavailable"); return false; }
    // mode 0: SDR plane values as they are. HDR planes become nits, Rec.709 primaries, then SDR
    // relative to `white` nits with a soft highlight roll-off (the sRGB RTV applies the curve):
    // mode 1 linear P3-D65, 1.0 = 10000 nits (console DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_D65P3);
    // mode 2 ST.2084 Rec.2020 (HDR10); mode 3 linear Rec.709, 1.0 = 80 nits (scRGB).
    static const char src[] =
        "Texture2D t : register(t0); SamplerState s : register(s0);\n"
        "cbuffer C : register(b0) { uint mode; float white; };\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "static const float3x3 P3to709 = { 1.2249, -0.2247, 0.0, -0.0420, 1.0419, 0.0, -0.0197, -0.0786, 1.0979 };\n"
        "static const float3x3 BT2020to709 = { 1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187 };\n"
        "float3 pq_nits(float3 n) { const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
        "  float3 p = pow(saturate(n), 1.0 / m2); return 10000.0 * pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1); }\n"
        "float3 rolloff(float3 c) { const float k = 0.8; return c <= k ? c : k + (1 - k) * (1 - exp(-(c - k) / (1 - k))); }\n"
        "float4 ps(V i) : SV_Target { float3 c = t.Sample(s, i.uv).rgb;\n"
        "  if (mode == 1) c = rolloff(max(mul(P3to709, c * 10000.0), 0) / white);\n"
        "  else if (mode == 2) c = rolloff(max(mul(BT2020to709, pq_nits(c)), 0) / white);\n"
        "  else if (mode == 3) c = rolloff(max(c * 80.0, 0) / white);\n"
        "  return float4(saturate(c), 1); }\n";
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(compile(src, sizeof src - 1, "xsl_blit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(compile(src, sizeof src - 1, "xsl_blit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) {
        XSL_ERR(CH, "blit: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 2};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rsb = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&g_blit.rs))))
        return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_blit.rs;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;  // linear plane values -> sRGB scan-out
    pd.SampleDesc.Count = 1;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_blit.pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_blit.srv));
    D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_blit.rtv));
    g_blit.srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_blit.rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_blit.failed = false;
    XSL_INFO(CH, "presentation: shader blit ready for non-copyable plane formats");
    return true;
}

// Blit mode for a console plane colour space (see the blit shader).
uint32_t blit_mode(uint32_t colorSpace) {
    switch (colorSpace) {
        case 0x10000001: return 1;  // DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_D65P3 (Xbox extension)
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return 2;
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return 3;
        default: return 0;
    }
}
float sdr_white_nits() {
    static float w = [] {
        char b[16];
        float v = GetEnvironmentVariableA("XSL_SDR_WHITE_NITS", b, sizeof b) ? (float)atof(b) : 0.0f;
        return v > 0 ? v : 200.0f;
    }();
    return w;
}
void blit(ID3D12Device* dev, ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h, uint32_t mode) {
    UINT i = g_blit.slot++ % 8;
    D3D12_CPU_DESCRIPTOR_HANDLE sc = g_blit.srv->GetCPUDescriptorHandleForHeapStart();
    sc.ptr += (SIZE_T)i * g_blit.srvInc;
    D3D12_GPU_DESCRIPTOR_HANDLE sg = g_blit.srv->GetGPUDescriptorHandleForHeapStart();
    sg.ptr += (UINT64)i * g_blit.srvInc;
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = rd.Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(src, &sd, sc);
    D3D12_CPU_DESCRIPTOR_HANDLE rc = g_blit.rtv->GetCPUDescriptorHandleForHeapStart();
    rc.ptr += (SIZE_T)i * g_blit.rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(dst, &rv, rc);
    l->SetGraphicsRootSignature(g_blit.rs);
    l->SetPipelineState(g_blit.pso);
    ID3D12DescriptorHeap* heaps[] = {g_blit.srv};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootDescriptorTable(0, sg);
    float white = sdr_white_nits();
    uint32_t consts[2] = {mode, 0};
    memcpy(&consts[1], &white, 4);
    l->SetGraphicsRoot32BitConstants(1, 2, consts, 0);
    D3D12_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    D3D12_RECT sr{0, 0, (LONG)w, (LONG)h};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->OMSetRenderTargets(1, &rc, FALSE, nullptr);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->DrawInstanced(3, 1, 0, 0);
}
}  // namespace

// Window title "<title> - FPS 60.0", refreshed about once a second (XSL_FPS_TITLE=0 disables).
// The title's own caption is remembered and restored as the prefix, so a title that changes its
// caption keeps working.
void update_fps_title(HWND hwnd) {
    static bool enabled = [] { char b[8]; return !(GetEnvironmentVariableA("XSL_FPS_TITLE", b, sizeof b) && b[0] == '0'); }();
    if (!enabled) return;
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static LARGE_INTEGER last{};
    static uint64_t frames = 0;
    static wchar_t base[256] = L"";
    static wchar_t shown[300] = L"";
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!last.QuadPart) { last = now; return; }
    frames++;
    double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
    if (dt < 1.0) return;
    wchar_t cur[300] = L"";
    // InternalGetWindowText reads the stored caption without sending WM_GETTEXT, which would
    // deadlock against a window thread that is waiting for this (render) thread.
    InternalGetWindowText(hwnd, cur, 300);
    if (wcscmp(cur, shown) != 0) wcsncpy(base, cur, 255);  // the title changed its caption itself
    swprintf(shown, 300, L"%ls - FPS %.1f", base, frames / dt);
    XSL_TRACE(CH, "fps %.1f (window thread %lu, presenting thread %lu)", frames / dt, GetWindowThreadProcessId(hwnd, nullptr), GetCurrentThreadId());
    // Never block the render thread on another thread's message loop: titles that present from a
    // render thread while their window thread waits on it (FramePacing) would never see the update.
    // A helper thread delivers the latest caption whenever the window thread next pumps messages.
    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) {
        SetWindowTextW(hwnd, shown);
    } else {
        static CRITICAL_SECTION cs = [] { CRITICAL_SECTION c; InitializeCriticalSection(&c); return c; }();
        static HWND pendingHwnd = nullptr;
        static wchar_t pending[300] = L"";
        static HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        static HANDLE worker = CreateThread(nullptr, 0, [](void*) -> DWORD {
            for (;;) {
                WaitForSingleObject(wake, INFINITE);
                wchar_t text[300];
                EnterCriticalSection(&cs);
                HWND h = pendingHwnd;
                wcscpy(text, pending);
                LeaveCriticalSection(&cs);
                if (h) SendMessageW(h, WM_SETTEXT, 0, (LPARAM)text);
            }
        }, nullptr, 0, nullptr);
        (void)worker;
        EnterCriticalSection(&cs);
        pendingHwnd = hwnd;
        wcscpy(pending, shown);
        LeaveCriticalSection(&cs);
        SetEvent(wake);
    }
    frames = 0;
    last = now;
}

HRESULT_T null_present(void* hwnd) {
    update_fps_title(static_cast<HWND>(hwnd));
    return S_OK;
}
HRESULT_T present(PcObj devObj, PcObj queueObj, PcObj resObj, void* hwndPtr, uint32_t sync, uint32_t colorSpace) {
    auto* dev = as<ID3D12Device>(devObj);
    auto* q = as<ID3D12CommandQueue>(queueObj);
    auto* res = as<ID3D12Resource>(resObj);
    HWND hwnd = (HWND)hwndPtr;
    std::lock_guard<std::mutex> g(g_present_mu);
    SwapState& s = g_swap[hwnd];
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    bool copyable = true;
    DXGI_FORMAT want = swap_format_for(rd.Format, &copyable);
    static bool forceBlit = [] { char b[4]; return GetEnvironmentVariableA("XSL_PRESENT_BLIT", b, 4) && b[0] == '1'; }();
    const uint32_t mode = blit_mode(colorSpace);
    if (forceBlit || mode != 0) { copyable = false; want = DXGI_FORMAT_B8G8R8A8_UNORM; }
    if (mode != 0) {
        static uint32_t logged = UINT32_MAX;
        if (logged != colorSpace) {
            logged = colorSpace;
            XSL_INFO(CH, "HDR plane (colour space %#x) mapped to SDR, reference white %.0f nits", colorSpace, sdr_white_nits());
        }
    }
    if (!copyable && !blit_init(dev)) return E_FAIL;
    HRESULT hr;
    if (!s.sc || s.w != (UINT)rd.Width || s.h != rd.Height || s.fmt != want) {
        if (s.sc) {
            // Drain before recreating.
            q->Signal(s.fence, ++s.fenceValue);
            s.fence->SetEventOnCompletion(s.fenceValue, s.event);
            WaitForSingleObject(s.event, INFINITE);
            s.sc->Release();
            s.sc = nullptr;
        }
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = (UINT)rd.Width;
        d.Height = rd.Height;
        d.Format = want;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = s.count;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* sc1 = nullptr;
        hr = g_factory->CreateSwapChainForHwnd(q, hwnd, &d, nullptr, nullptr, &sc1);
        if (FAILED(hr)) { XSL_ERR(CH, "CreateSwapChainForHwnd failed %08lx", hr); return hr; }
        sc1->QueryInterface(IID_PPV_ARGS(&s.sc));
        sc1->Release();
        g_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        s.w = d.Width; s.h = d.Height; s.fmt = want;
        if (!s.list) {
            for (auto& a : s.alloc) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc[0], nullptr, IID_PPV_ARGS(&s.list));
            s.list->Close();
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence));
            s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        XSL_INFO(CH, "swap chain %ux%u format %d for window %p", s.w, s.h, (int)want, hwnd);
    }
    // Wait for the allocator slot to be free.
    if (s.fence->GetCompletedValue() < s.slotValue[s.slot]) {
        s.fence->SetEventOnCompletion(s.slotValue[s.slot], s.event);
        WaitForSingleObject(s.event, INFINITE);
    }
    UINT bi = s.sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    s.sc->GetBuffer(bi, IID_PPV_ARGS(&bb));
    s.alloc[s.slot]->Reset();
    s.list->Reset(s.alloc[s.slot], nullptr);
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                       copyable ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    b[1].Transition = {bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                       copyable ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET};
    s.list->ResourceBarrier(2, b);
    D3D12_TEXTURE_COPY_LOCATION dst{bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    if (copyable) s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    else blit(dev, s.list, res, bb, s.w, s.h, mode);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    s.list->ResourceBarrier(2, b);
    s.list->Close();
    ID3D12CommandList* lists[] = {s.list};
    q->ExecuteCommandLists(1, lists);
    {
        // XSL_CAPTURE=<path.bmp>[@start[+count]]: dump presented frames (count > 1 adds _<frame>).
        // The swap-chain buffer is captured, i.e. what is displayed after format/HDR conversion.
        static char cap[MAX_PATH] = "";
        static uint64_t capFrame = 0, capCount = 1;
        static bool init = false;
        if (!init) {
            init = true;
            if (GetEnvironmentVariableA("XSL_CAPTURE", cap, sizeof cap)) {
                char* at = strrchr(cap, '@');
                capFrame = at ? _strtoui64(at + 1, nullptr, 10) : 60;
                if (char* plus = at ? strchr(at, '+') : nullptr) capCount = _strtoui64(plus + 1, nullptr, 10);
                if (at) *at = 0;
            }
        }
        ++g_present_count;
        if (cap[0] && g_present_count >= capFrame && g_present_count < capFrame + capCount) {
            char path[MAX_PATH];
            if (capCount > 1) {
                snprintf(path, sizeof path, "%s", cap);
                char* dot = strrchr(path, '.');
                char ext[16] = ".bmp";
                if (dot) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
                snprintf(path + strlen(path), sizeof path - strlen(path), "_%04llu%s", (unsigned long long)g_present_count, ext);
            } else {
                snprintf(path, sizeof path, "%s", cap);
            }
            // XSL_CAPTURE_SOURCE=plane captures the title's plane as presented, before conversion.
            static bool planeSrc = [] { char b[8]; return GetEnvironmentVariableA("XSL_CAPTURE_SOURCE", b, sizeof b) && !strcmp(b, "plane"); }();
            capture_plane(dev, q, planeSrc ? res : bb, path);
        }
        // XSL_CAPTURE_TRIGGER=<path>: whenever <path> exists, the next presented frame is written to
        // <path>.bmp and <path> is deleted (captures on demand, e.g. after scripted input).
        static char trig[MAX_PATH] = "";
        static bool trigInit = false;
        if (!trigInit) {
            trigInit = true;
            GetEnvironmentVariableA("XSL_CAPTURE_TRIGGER", trig, sizeof trig);
        }
        if (trig[0] && (g_present_count & 3) == 0 && GetFileAttributesA(trig) != INVALID_FILE_ATTRIBUTES) {
            char path[MAX_PATH];
            snprintf(path, sizeof path, "%s.bmp", trig);
            capture_plane(dev, q, bb, path);
            DeleteFileA(trig);
        }
    }
    bb->Release();
    hr = s.sc->Present(sync, 0);
    update_fps_title(hwnd);
    q->Signal(s.fence, ++s.fenceValue);
    s.slotValue[s.slot] = s.fenceValue;
    s.slot = (s.slot + 1) % 3;
    if (FAILED(hr)) XSL_ERR(CH, "Present failed %08lx", hr);
    return hr;
}

}  // namespace pc
