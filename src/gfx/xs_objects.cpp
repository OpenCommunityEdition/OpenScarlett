// SPDX-License-Identifier: AGPL-3.0-or-later
// Console Direct3D 12.X objects implemented over the PC back end.
#include <cstring>
#include <mutex>
#include <vector>

#include "xs_object.h"

void xsl_fixed_time_frame();   // fixed_time.cpp (global namespace)
#include "xs_cbv.h"
#include "xs_unified.h"

namespace xs {

// ============================================================================================
// IGraphicsUnknown / ID3D12Object / ID3D12DeviceChild
// ============================================================================================
const char* iid_name(REFIID r) {
#define N(T) if (IsEqualGUID(r, __uuidof(T))) return #T;
    N(IGraphicsUnknown) N(ID3D12Object) N(ID3D12DeviceChild) N(ID3D12Pageable) N(ID3D12Device) N(ID3D12Device1) N(ID3D12Device2)
    N(ID3D12Device3) N(ID3D12Device4) N(ID3D12Device5) N(ID3D12Device6) N(ID3D12Device7) N(ID3D12Device8) N(ID3D12Device9)
    N(ID3D12Device10) N(ID3D12Device11) N(ID3D12CommandQueue) N(ID3D12CommandAllocator) N(ID3D12CommandList)
    N(ID3D12GraphicsCommandList) N(ID3D12GraphicsCommandList1) N(ID3D12GraphicsCommandList2) N(ID3D12GraphicsCommandList3)
    N(ID3D12GraphicsCommandList4) N(ID3D12GraphicsCommandList5) N(ID3D12GraphicsCommandList6) N(ID3D12GraphicsCommandList7)
    N(ID3D12GraphicsCommandList8) N(ID3D12Fence) N(ID3D12Fence1) N(ID3D12Resource) N(ID3D12Resource1) N(ID3D12Resource2)
    N(ID3D12Heap) N(ID3D12Heap1) N(ID3D12DescriptorHeap) N(ID3D12RootSignature) N(ID3D12PipelineState) N(IDXGIObject)
    N(IDXGIDevice) N(IDXGIDevice1) N(IDXGIDevice2) N(IDXGIAdapter) N(IDXGIAdapter1) N(IDXGIAdapter2) N(IDXGIOutput) N(IDXGIFactory) N(IDXGIFactory1) N(IDXGIFactory2)
    N(ID3D12Debug) N(ID3D12DebugDevice)
#undef N
    static thread_local char buf[64];
    snprintf(buf, sizeof buf, "{%08lx-%04x-%04x-...}", r.Data1, r.Data2, r.Data3);
    return buf;
}

HRESULT D3DAPI QueryInterface(IGraphicsUnknown* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    ObjHeader* h = hdr(self);
    for (const IID* const* p = h->iids; p && *p; p++) {
        if (IsEqualGUID(riid, **p)) { AddRef(self); *out = self; return S_OK; }
    }
    if (h->kind == Kind::Device && (IsEqualGUID(riid, __uuidof(IDXGIDevice)) || IsEqualGUID(riid, __uuidof(IDXGIDevice1)) ||
                                    IsEqualGUID(riid, __uuidof(IDXGIDevice2)) || IsEqualGUID(riid, __uuidof(IDXGIObject)))) {
        return create_dxgi_device((Device*)self, out);
    }
    XSL_WARN(CH, "QueryInterface(kind %u) for unsupported %s", (unsigned)h->kind, iid_name(riid));
    return E_NOINTERFACE;
}
ULONG D3DAPI AddRef(IGraphicsUnknown* self) { return InterlockedIncrement((volatile LONG*)&self->m_RefCount); }
ULONG D3DAPI Release(IGraphicsUnknown* self) {
    ULONG n = InterlockedDecrement((volatile LONG*)&self->m_RefCount);
    if (n == 0) {
        ObjHeader* h = hdr(self);
        if (h->destroy) h->destroy(self);
        free_object(self);
    }
    return n;
}
void free_object(IGraphicsUnknown* self) {
    ObjHeader* h = hdr(self);
    if (h->pc) pc::release(h->pc);
    h->magic = 0;
    _aligned_free(h);
}

static HRESULT D3DAPI Obj_GetPrivateData(ID3D12Object*, REFGUID, UINT* size, void*) {
    if (size) *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}
static HRESULT D3DAPI Obj_SetPrivateData(ID3D12Object* self, REFGUID g, UINT n, const void* d) {
    if (IsEqualGUID(g, WKPDID_D3DDebugObjectNameW) && d) {
        std::wstring name((const wchar_t*)d, n / 2);
        pc::set_name(pc_of(self), name.c_str());
    }
    return S_OK;
}
static HRESULT D3DAPI Obj_SetPrivateDataInterface(ID3D12Object*, REFGUID, const IUnknown*) { return S_OK; }
static HRESULT D3DAPI Obj_SetName(ID3D12Object* self, LPCWSTR name) { pc::set_name(pc_of(self), name); return S_OK; }
static void D3DAPI Child_GetDevice(ID3D12DeviceChild* self, REFIID riid, void** out) {
    Device* d = hdr(self)->device;
    if (d) QueryInterface((IGraphicsUnknown*)d, riid, out);
    else if (out) *out = nullptr;
}

void install_object_base(void* table) {
    auto* u = (GraphicsUnknown_Function_Table*)table;
    u->QueryInterface = QueryInterface;
    u->AddRef = AddRef;
    u->Release = Release;
    auto* o = (D3D12Object_Function_Table*)table;
    o->GetPrivateData = Obj_GetPrivateData;
    o->SetPrivateData = Obj_SetPrivateData;
    o->SetPrivateDataInterface = Obj_SetPrivateDataInterface;
    o->SetName = Obj_SetName;
}
void install_device_child_base(void* table) {
    install_object_base(table);
    ((D3D12DeviceChild_Function_Table*)table)->GetDevice = Child_GetDevice;
}

namespace {
HRESULT hand_out(IGraphicsUnknown* obj, REFIID riid, void** out) {
    // Creation APIs return the object through riid; answer through QueryInterface so a
    // title asking for an interface the object lacks gets E_NOINTERFACE, then drop our ref.
    if (!out) { Release(obj); return S_FALSE; }
    HRESULT hr = QueryInterface(obj, riid, out);
    Release(obj);
    return hr;
}
template <class T> struct Iids;
#define IIDS(T, ...) template <> struct Iids<T> { static inline const IID* const list[] = {__VA_ARGS__, nullptr}; };
IIDS(ID3D12CommandQueue, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandQueue))
IIDS(ID3D12CommandAllocator, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandAllocator))
IIDS(ID3D12GraphicsCommandList8, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12CommandList),
     &__uuidof(ID3D12GraphicsCommandList), &__uuidof(ID3D12GraphicsCommandList1), &__uuidof(ID3D12GraphicsCommandList2), &__uuidof(ID3D12GraphicsCommandList3),
     &__uuidof(ID3D12GraphicsCommandList4), &__uuidof(ID3D12GraphicsCommandList5), &__uuidof(ID3D12GraphicsCommandList6), &__uuidof(ID3D12GraphicsCommandList7),
     &__uuidof(ID3D12GraphicsCommandList8))
IIDS(ID3D12Fence1, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Fence), &__uuidof(ID3D12Fence1))
IIDS(ID3D12Resource2, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Resource),
     &__uuidof(ID3D12Resource1), &__uuidof(ID3D12Resource2))
IIDS(ID3D12Heap1, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Heap), &__uuidof(ID3D12Heap1))
IIDS(ID3D12DescriptorHeap, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12DescriptorHeap))
IIDS(ID3D12RootSignature, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12RootSignature))
IIDS(ID3D12CommandSignature, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandSignature))
IIDS(ID3D12QueryHeap, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12QueryHeap))
IIDS(ID3D12PipelineState, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12Pageable), &__uuidof(ID3D12PipelineState))
#undef IIDS
struct NoExtra { uint64_t pad; };
}  // namespace

// ============================================================================================
// Fence, root signature, pipeline state, heap (simple objects)
// ============================================================================================
namespace {
UINT64 D3DAPI Fence_GetCompletedValue(ID3D12Fence* f) { return pc::fence_completed(pc_of(f)); }
HRESULT D3DAPI Fence_SetEventOnCompletion(ID3D12Fence* f, UINT64 v, HANDLE e) { return pc::fence_set_event(pc_of(f), v, e); }
HRESULT D3DAPI Fence_Signal(ID3D12Fence* f, UINT64 v) { return pc::fence_signal(pc_of(f), v); }

D3D12Fence1_Function_Table g_fence_tbl;
D3D12RootSignature_Function_Table g_rs_tbl;
D3D12PipelineState_Function_Table g_pso_tbl;
D3D12Heap1_Function_Table g_heap_tbl;
D3D12CommandSignature_Function_Table g_sig_tbl;
D3D12QueryHeap_Function_Table g_qh_tbl;
// The PC pipeline's cached blob (driver-specific; titles store it and pass it back on creation).
HRESULT D3DAPI Pso_GetCachedBlob(ID3D12PipelineState* pso, ID3DBlob** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    PcObj b = nullptr;
    HRESULT hr = pc::pso_cached_blob(pc_of(pso), &b);
    if (FAILED(hr) || !b) return FAILED(hr) ? hr : E_FAIL;
    *out = (ID3DBlob*)make_pc_blob(b);
    return S_OK;
}
std::once_flag g_simple_once;
// Memory heaps keep the description the title created them with: the PC heap's may differ (upload
// heaps are created as write-back custom heaps, console-only flags are dropped).
struct MemHeapExtra { D3D12_HEAP_DESC desc; };
void D3DAPI Heap_GetDesc(ID3D12Heap* h, D3D12_HEAP_DESC* d) { if (d) *d = ex<ID3D12Heap, MemHeapExtra>(h)->desc; }
void init_simple_tables() {
    std::call_once(g_simple_once, [] {
        xsl_fill_default(&g_fence_tbl);
        install_device_child_base(&g_fence_tbl);
        auto* f = (D3D12Fence_Function_Table*)&g_fence_tbl;
        f->GetCompletedValue = Fence_GetCompletedValue;
        f->SetEventOnCompletion = Fence_SetEventOnCompletion;
        f->Signal = Fence_Signal;
        xsl_fill_default(&g_rs_tbl);
        install_device_child_base(&g_rs_tbl);
        xsl_fill_default(&g_pso_tbl);
        install_device_child_base(&g_pso_tbl);
        g_pso_tbl.GetCachedBlob = Pso_GetCachedBlob;
        xsl_fill_default(&g_heap_tbl);
        install_device_child_base(&g_heap_tbl);
        g_heap_tbl.HeapTable.GetDesc = Heap_GetDesc;
        xsl_fill_default(&g_sig_tbl);
        install_device_child_base(&g_sig_tbl);
        xsl_fill_default(&g_qh_tbl);
        install_device_child_base(&g_qh_tbl);
    });
}
}  // namespace

HRESULT make_heap(Device* dev, PcObj pc, const D3D12_HEAP_DESC* desc, REFIID riid, void** out) {
    init_simple_tables();
    IGraphicsUnknown* o = make<ID3D12Heap1, MemHeapExtra>(Kind::Heap, &g_heap_tbl, pc, dev, Iids<ID3D12Heap1>::list, nullptr);
    ex<ID3D12Heap, MemHeapExtra>((ID3D12Heap*)o)->desc = *desc;
    return hand_out(o, riid, out);
}

HRESULT make_simple(Device* dev, Kind kind, PcObj pc, REFIID riid, void** out) {
    init_simple_tables();
    IGraphicsUnknown* o = nullptr;
    switch (kind) {
        case Kind::Fence: o = make<ID3D12Fence1, NoExtra>(kind, &g_fence_tbl, pc, dev, Iids<ID3D12Fence1>::list, nullptr); break;
        case Kind::RootSignature: o = make<ID3D12RootSignature, NoExtra>(kind, &g_rs_tbl, pc, dev, Iids<ID3D12RootSignature>::list, nullptr); break;
        case Kind::Pso: o = make<ID3D12PipelineState, NoExtra>(kind, &g_pso_tbl, pc, dev, Iids<ID3D12PipelineState>::list, nullptr); break;
        case Kind::Heap: o = make<ID3D12Heap1, NoExtra>(kind, &g_heap_tbl, pc, dev, Iids<ID3D12Heap1>::list, nullptr); break;
        case Kind::CommandSignature: o = make<ID3D12CommandSignature, NoExtra>(kind, &g_sig_tbl, pc, dev, Iids<ID3D12CommandSignature>::list, nullptr); break;
        case Kind::QueryHeap: o = make<ID3D12QueryHeap, NoExtra>(kind, &g_qh_tbl, pc, dev, Iids<ID3D12QueryHeap>::list, nullptr); break;
        default: XSL_ERR(CH, "make_simple: unsupported kind %u", (unsigned)kind); pc::release(pc); return E_NOTIMPL;
    }
    return hand_out(o, riid, out);
}

// ============================================================================================
// Resource
// ============================================================================================
namespace {
HRESULT D3DAPI Res_Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** out) { return pc::res_map(pc_of(r), sub, range, out); }
void D3DAPI Res_Unmap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range) { pc::res_unmap(pc_of(r), sub, range); }
void D3DAPI Res_GetDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC* d) { pc::res_desc(pc_of(r), d); }
D3D12_GPU_VIRTUAL_ADDRESS D3DAPI Res_GetGPUVirtualAddress(ID3D12Resource* r) { return pc::res_gpu_va(pc_of(r)); }
HRESULT D3DAPI Res_GetHeapProperties(ID3D12Resource*, D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS* f) {
    if (p) { *p = {}; p->Type = D3D12_HEAP_TYPE_DEFAULT; }
    if (f) *f = D3D12_HEAP_FLAG_NONE;
    return S_OK;
}
D3D12Resource2_Function_Table g_res_tbl;
std::once_flag g_res_once;
struct ResExtra { uint64_t trackedVa; uint64_t placedVa; };
void Res_Destroy(IGraphicsUnknown* self) {
    auto* e = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)self);
    if (e->trackedVa) cbv_untrack(e->trackedVa);
    if (e->placedVa) pagemap_forget(e->placedVa, hdr(self)->pc);
}
}  // namespace

void resource_set_placed(void* resource, uint64_t va) { ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)resource)->placedVa = va; }
// Diagnostics: XSL_DBG_PRESENT_RES=k presents the k-th render-target texture created (0-based)
// instead of the title's plane, to look at intermediate passes.
std::vector<PcObj> g_dbg_targets;
std::mutex g_dbg_mu;
PcObj dbg_present_override() {
    static int k = [] { char b[16]; return GetEnvironmentVariableA("XSL_DBG_PRESENT_RES", b, sizeof b) ? atoi(b) : -1; }();
    if (k < 0) return nullptr;
    std::lock_guard<std::mutex> g(g_dbg_mu);
    return k < (int)g_dbg_targets.size() ? g_dbg_targets[k] : nullptr;
}
HRESULT make_resource(Device* dev, PcObj pcr, REFIID riid, void** out, bool upload) {
    {
        D3D12_RESOURCE_DESC d;
        pc::res_desc(pcr, &d);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || getenv("XSL_DBG_ALL_TEX"))) {
            std::lock_guard<std::mutex> g(g_dbg_mu);
            if (GetEnvironmentVariableA("XSL_DBG_PRESENT_RES", nullptr, 0))
                XSL_INFO(CH, "dbg target %zu: %llux%u format %d", g_dbg_targets.size(), (unsigned long long)d.Width, d.Height, (int)d.Format);
            g_dbg_targets.push_back(pcr);
        }
    }
    std::call_once(g_res_once, [] {
        xsl_fill_default(&g_res_tbl);
        install_device_child_base(&g_res_tbl);
        auto* t = (D3D12Resource_Function_Table*)&g_res_tbl;
        t->Map = Res_Map;
        t->Unmap = Res_Unmap;
        t->GetDesc = Res_GetDesc;
        t->GetGPUVirtualAddress = Res_GetGPUVirtualAddress;
        t->GetHeapProperties = Res_GetHeapProperties;
    });
    auto* o = make<ID3D12Resource2, ResExtra>(Kind::Resource, &g_res_tbl, pcr, dev, Iids<ID3D12Resource2>::list, Res_Destroy);
    // Every CPU-mappable buffer, not only upload heaps: the layer makes default-heap buffers
    // mappable (unified memory), and titles bind constants from them at 16-byte offsets too.
    // cbv_track_upload skips buffers that cannot be mapped.
    (void)upload;
    {
        D3D12_RESOURCE_DESC d;
        pc::res_desc(pcr, &d);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            uint64_t va = pc::res_gpu_va(pcr);
            cbv_track_upload(pcr, va, d.Width);
            ex<ID3D12Resource2, ResExtra>(o)->trackedVa = va;
        }
    }
    return hand_out(o, riid, out);
}

// ============================================================================================
// Descriptor heap: the title receives the PC heap's own handles, so CPU/GPU handle
// arithmetic with our GetDescriptorHandleIncrementSize lands on valid PC descriptors.
// ============================================================================================
namespace {
struct HeapExtra { D3D12_DESCRIPTOR_HEAP_DESC desc; uint64_t cpu, gpu; bool global; uint32_t offset; };
void D3DAPI DH_GetDesc(ID3D12DescriptorHeap* h, D3D12_DESCRIPTOR_HEAP_DESC* d) { *d = ex<ID3D12DescriptorHeap, HeapExtra>(h)->desc; }
D3D12_CPU_DESCRIPTOR_HANDLE D3DAPI DH_Cpu(ID3D12DescriptorHeap* h) { return {(SIZE_T)ex<ID3D12DescriptorHeap, HeapExtra>(h)->cpu}; }
D3D12_GPU_DESCRIPTOR_HANDLE D3DAPI DH_Gpu(ID3D12DescriptorHeap* h) { return {ex<ID3D12DescriptorHeap, HeapExtra>(h)->gpu}; }
D3D12DescriptorHeap_Function_Table g_dh_tbl;
std::once_flag g_dh_once;
}  // namespace

namespace {
ID3D12DescriptorHeap* new_descriptor_heap(Device* dev, PcObj pch, const D3D12_DESCRIPTOR_HEAP_DESC* d) {
    std::call_once(g_dh_once, [] {
        xsl_fill_default(&g_dh_tbl);
        install_device_child_base(&g_dh_tbl);
        g_dh_tbl.GetDesc = DH_GetDesc;
        g_dh_tbl.GetCPUDescriptorHandleForHeapStart = DH_Cpu;
        g_dh_tbl.GetGPUDescriptorHandleForHeapStart = DH_Gpu;
    });
    auto* o = make<ID3D12DescriptorHeap, HeapExtra>(Kind::DescriptorHeap, &g_dh_tbl, pch, dev, Iids<ID3D12DescriptorHeap>::list, nullptr);
    ex<ID3D12DescriptorHeap, HeapExtra>(o)->desc = *d;
    return o;
}
void DH_DestroyRange(IGraphicsUnknown* self) {
    auto* e = ex<ID3D12DescriptorHeap, HeapExtra>((ID3D12DescriptorHeap*)self);
    pc::global_view_free(e->offset, e->desc.NumDescriptors);
}
}  // namespace

HRESULT make_descriptor_heap(Device* dev, PcObj pch, const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID riid, void** out) {
    auto* o = new_descriptor_heap(dev, pch, d);
    auto* e = ex<ID3D12DescriptorHeap, HeapExtra>(o);
    e->cpu = pc::heap_cpu_start(pch);
    e->gpu = pc::heap_gpu_start(pch);
    return hand_out(o, riid, out);
}
void DH_DestroyShadow(IGraphicsUnknown* self) {
    desc_rtv_dsv_heap_free(ex<ID3D12DescriptorHeap, HeapExtra>((ID3D12DescriptorHeap*)self)->cpu);
}
HRESULT make_rtv_dsv_shadow_heap(Device* dev, PcObj pch, const D3D12_DESCRIPTOR_HEAP_DESC* d, uint64_t shadow, REFIID riid, void** out) {
    auto* o = new_descriptor_heap(dev, pch, d);
    auto* e = ex<ID3D12DescriptorHeap, HeapExtra>(o);
    e->cpu = shadow;
    e->gpu = 0;
    hdr(o)->destroy = DH_DestroyShadow;
    return hand_out(o, riid, out);
}
HRESULT make_descriptor_heap_range(Device* dev, const D3D12_DESCRIPTOR_HEAP_DESC* d, uint32_t offset, uint64_t cpu, uint64_t gpu,
                                   REFIID riid, void** out) {
    PcObj gh = pc::global_view_heap_if_created();
    pc::addref(gh);  // released with the object, like any PC object it holds
    auto* o = new_descriptor_heap(dev, gh, d);
    auto* e = ex<ID3D12DescriptorHeap, HeapExtra>(o);
    e->cpu = cpu;
    e->gpu = gpu;
    e->global = true;
    e->offset = offset;
    hdr(o)->destroy = DH_DestroyRange;
    return hand_out(o, riid, out);
}

// ============================================================================================
// Command allocator
// ============================================================================================
namespace {
struct AllocExtra { D3D12_COMMAND_LIST_TYPE type; };
HRESULT D3DAPI Alloc_Reset(ID3D12CommandAllocator* a) { return pc::alloc_reset(pc_of(a)); }
void D3DAPI Alloc_SetCallbackX(ID3D12CommandAllocator*, D3D12XBOX_COMMAND_ALLOCATOR_CALLBACK, UINT64) {}
D3D12CommandAllocator_Function_Table g_alloc_tbl;
std::once_flag g_alloc_once;
}  // namespace

HRESULT make_allocator(Device* dev, PcObj pca, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out) {
    std::call_once(g_alloc_once, [] {
        xsl_fill_default(&g_alloc_tbl);
        install_device_child_base(&g_alloc_tbl);
        g_alloc_tbl.Reset = Alloc_Reset;
        g_alloc_tbl.SetAllocatorCallbackX = Alloc_SetCallbackX;
    });
    auto* o = make<ID3D12CommandAllocator, AllocExtra>(Kind::Allocator, &g_alloc_tbl, pca, dev, Iids<ID3D12CommandAllocator>::list, nullptr);
    ex<ID3D12CommandAllocator, AllocExtra>(o)->type = t;
    return hand_out(o, riid, out);
}

// ============================================================================================
// Graphics command list. The SDK struct (1,384 bytes) is allocated with every put-buffer
// limit null, so each inline method in d3d12_xs.h takes its out-of-line branch.
// ============================================================================================
namespace {
using XList = ID3D12GraphicsCommandList8;
struct ListExtra {
    D3D12_COMMAND_LIST_TYPE type;
    int topology;                 // console topology last set
    bool ibValid;                 // title's index buffer view, restored after quad draws
    D3D12_INDEX_BUFFER_VIEW ib;
    PcObj sampler;                // sampler heap last bound by the title
    bool placeholderPso;          // current PSO has no PC pipeline (serialized native code): skip work
    // Whole-resource states set by earlier transitions in this recording (see L_ResourceBarrier).
    static constexpr uint32_t kTracked = 64;
    uint32_t nTracked;
    struct { void* res; uint32_t state; } tracked[kTracked];
};
ListExtra* lx(ID3D12GraphicsCommandList* l) { return ex<XList, ListExtra>((XList*)l); }

// Quad emulation: a layer-owned index buffer turning every 4 vertices into 2 triangles.
constexpr uint32_t kQuadMax = 1u << 18;
D3D12_INDEX_BUFFER_VIEW quad_ib(Device* dev) {
    static D3D12_INDEX_BUFFER_VIEW view{};
    static std::once_flag once;
    std::call_once(once, [dev] {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)kQuadMax * 6 * 4;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        PcObj res = nullptr;
        if (FAILED(pc::create_committed_resource(dev_extra(dev)->pcDevice, &hp, 0, &rd, 0x8c3, nullptr, &res))) return;
        uint32_t* p = nullptr;
        pc::res_map(res, 0, nullptr, (void**)&p);
        for (uint32_t q = 0; q < kQuadMax; q++) {
            uint32_t b = q * 4;
            uint32_t* o = p + q * 6;
            o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
        }
        view.BufferLocation = pc::res_gpu_va(res);
        view.SizeInBytes = (UINT)rd.Width;
        view.Format = DXGI_FORMAT_R32_UINT;
        XSL_INFO(CH, "quad lists are drawn as indexed triangle lists (%u quads max per draw)", kQuadMax);
    });
    return view;
}
#define LPC(l) pc_of(l)

HRESULT D3DAPI L_Close(ID3D12GraphicsCommandList* l) { XSL_TRACE("gfx.cmd", "list %p Close", (void*)l); return pc::list_close(LPC(l)); }
HRESULT D3DAPI L_Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    lx(l)->topology = 0;
    lx(l)->ibValid = false;
    lx(l)->nTracked = 0;
    lx(l)->sampler = nullptr;
    lx(l)->placeholderPso = p && !P(p);
    HRESULT hr = pc::list_reset(LPC(l), P(a), P(p));
    if (SUCCEEDED(hr)) list_bind_global_heap(l, LPC(l));
    return hr;
}
// Console drivers tolerate a transition whose StateBefore is not the resource's current state (the
// GDK samples' Present() assumes RENDER_TARGET after a compute pass left the back buffer in
// UNORDERED_ACCESS). On PC that is undefined and can lose the image's contents, so a transition
// following another transition of the same resource in the same command list starts from the state
// that transition actually left. Order within one list is deterministic; across lists nothing is assumed.
bool barrier_fix_enabled() {
    static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_BARRIER_FIX", b, 4) && b[0] == '0'); }();
    return on;
}
uint32_t* tracked_state(ListExtra* e, void* res, bool add) {
    for (uint32_t i = 0; i < e->nTracked; i++)
        if (e->tracked[i].res == res) return &e->tracked[i].state;
    if (!add || e->nTracked == ListExtra::kTracked) return nullptr;
    e->tracked[e->nTracked].res = res;
    return &e->tracked[e->nTracked++].state;
}
D3D12XBOX_RESOURCE_BARRIER_ACTIONS D3DAPI L_ResourceBarrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    D3D12_RESOURCE_BARRIER tmp[64];
    ListExtra* e = lx(l);
    for (UINT base = 0; base < n; base += 64) {
        UINT c = n - base < 64 ? n - base : 64;
        UINT k = 0;
        for (UINT i = 0; i < c; i++) {
            D3D12_RESOURCE_BARRIER x = b[base + i];
            x.Flags = (D3D12_RESOURCE_BARRIER_FLAGS)(x.Flags & 3);
            switch (x.Type) {
                case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: {
                    void* res = b[base + i].Transition.pResource;
                    x.Transition.pResource = (ID3D12Resource*)P(res);
                    // Split barriers keep their begin/end pairing; only whole-resource transitions are tracked.
                    bool whole = x.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && x.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    if (uint32_t* st = tracked_state(e, res, false); st && !whole) {
                        *st = UINT32_MAX;  // per-subresource state now: stop correcting this resource
                    } else if (st && *st != UINT32_MAX && *st != (uint32_t)x.Transition.StateBefore && barrier_fix_enabled()) {
                        static volatile long warned = 0;
                        if (InterlockedIncrement(&warned) <= 3)
                            XSL_INFO(CH, "barrier StateBefore %#x does not match the list's last transition (%#x); using the latter",
                                     (unsigned)x.Transition.StateBefore, *st);
                        x.Transition.StateBefore = (D3D12_RESOURCE_STATES)*st;
                    }
                    if (whole)
                        if (uint32_t* st = tracked_state(e, res, true)) *st = (uint32_t)x.Transition.StateAfter;
                    if (x.Transition.StateBefore == x.Transition.StateAfter) continue;
                    break;
                }
                case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
                    x.Aliasing.pResourceBefore = (ID3D12Resource*)P(x.Aliasing.pResourceBefore);
                    x.Aliasing.pResourceAfter = (ID3D12Resource*)P(x.Aliasing.pResourceAfter);
                    break;
                case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                    x.UAV.pResource = (ID3D12Resource*)P(x.UAV.pResource);
                    break;
                default:
                    XSL_WARN(CH, "dropping console-only barrier type %d", (int)x.Type);
                    continue;
            }
            if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                XSL_TRACE("gfx.cmd", "list %p barrier res %p %x -> %x", (void*)l, (void*)b[base + i].Transition.pResource,
                          b[base + i].Transition.StateBefore, b[base + i].Transition.StateAfter);
            tmp[k++] = x;
        }
        pc::list_barriers(LPC(l), k, tmp);
    }
    return (D3D12XBOX_RESOURCE_BARRIER_ACTIONS)0;
}
void D3DAPI L_OMSetRenderTargets(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv, BOOL single,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    uint64_t r[8] = {};
    // Console-layout handles: translate each through its tag (a single-handle range steps by the
    // console RTV size), then pass the PC handles individually.
    const uint32_t step = desc_increment(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0);
    if (single && n && step) {
        for (UINT i = 0; i < n && i < 8; i++) r[i] = desc_pc_cpu(rtv[0].ptr + (uint64_t)i * step, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        single = FALSE;
    } else {
        UINT cnt = single ? (n ? 1 : 0) : n;
        for (UINT i = 0; i < cnt && i < 8; i++) r[i] = desc_pc_cpu(rtv[i].ptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
    uint64_t d = dsv ? desc_pc_cpu(dsv->ptr, D3D12_DESCRIPTOR_HEAP_TYPE_DSV) : 0;
    XSL_TRACE("gfx.cmd", "list %p OMSetRenderTargets n=%u rtv0=%llx dsv=%llx", (void*)l, n, (unsigned long long)r[0], (unsigned long long)d);
    pc::list_om_set_render_targets(LPC(l), n, r, single, dsv ? &d : nullptr);
}
void D3DAPI L_ClearRTV(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c[4], UINT n, const D3D12_RECT* r) {
    XSL_TRACE("gfx.cmd", "list %p ClearRTV %llx", (void*)l, (unsigned long long)h.ptr);
    pc::list_clear_rtv(LPC(l), desc_pc_cpu(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_RTV), c, n, r);
}
void D3DAPI L_ClearDSV(ID3D12GraphicsCommandList* l, D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s, UINT n, const D3D12_RECT* r) {
    XSL_TRACE("gfx.cmd", "list %p ClearDSV %llx", (void*)l, (unsigned long long)h.ptr);
    pc::list_clear_dsv(LPC(l), desc_pc_cpu(h.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_DSV), f, d, s, n, r);
}
void D3DAPI L_RSSetViewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* v) { pc::list_viewports(LPC(l), n, v); }
void D3DAPI L_RSSetScissorRects(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RECT* r) { pc::list_scissors(LPC(l), n, r); }
void D3DAPI L_SetGfxRootSig(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    XSL_TRACE("gfx.cmd", "list %p RootSig %p", (void*)l, (void*)rs);
    pc::list_set_gfx_root_signature(LPC(l), P(rs));
}
void D3DAPI L_SetComputeRootSig(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) { pc::list_set_compute_root_signature(LPC(l), P(rs)); }
void D3DAPI L_SetPipelineState(ID3D12GraphicsCommandList* l, ID3D12PipelineState* p) {
    if (null_graphics()) return;
    lx(l)->placeholderPso = p && !P(p);
    if (lx(l)->placeholderPso) return;
    XSL_TRACE("gfx.cmd", "list %p PSO %p", (void*)l, (void*)p);
    pc::list_set_pso(LPC(l), P(p));
}
void D3DAPI L_IASetPrimitiveTopology(ID3D12GraphicsCommandList* l, D3D12_PRIMITIVE_TOPOLOGY t) {
    XSL_TRACE("gfx.cmd", "list %p topology %d", (void*)l, (int)t);
    lx(l)->topology = t;
    D3D12_PRIMITIVE_TOPOLOGY pt = t;
    switch ((int)t) {
        case D3D_PRIMITIVE_TOPOLOGY_QUADLIST: pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
        case D3D_PRIMITIVE_TOPOLOGY_QUADSTRIP: pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
        case D3D_PRIMITIVE_TOPOLOGY_RECTLIST: {
            static volatile long warned = 0;
            if (InterlockedIncrement(&warned) == 1) XSL_WARN(CH, "RECTLIST is drawn as a triangle list (each rect's fourth corner is missing)");
            pt = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        }
        case D3D_PRIMITIVE_TOPOLOGY_LINELOOP: pt = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
        default: break;
    }
    pc::list_ia_topology(LPC(l), pt);
}
void D3DAPI L_IASetVertexBuffers(ID3D12GraphicsCommandList* l, UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    XSL_TRACE("gfx.cmd", "list %p VB %u n=%u va=%llx size=%u stride=%u", (void*)l, s, n, n ? (unsigned long long)v[0].BufferLocation : 0ull, n ? v[0].SizeInBytes : 0, n ? v[0].StrideInBytes : 0);
    pc::list_ia_vbs(LPC(l), s, n, v);
}
void D3DAPI L_IASetIndexBuffer(ID3D12GraphicsCommandList* l, const D3D12_INDEX_BUFFER_VIEW* v) {
    lx(l)->ibValid = v != nullptr;
    if (v) lx(l)->ib = *v;
    XSL_TRACE("gfx.cmd", "list %p IB va=%llx size=%u fmt=%d", (void*)l, v ? (unsigned long long)v->BufferLocation : 0ull, v ? v->SizeInBytes : 0, v ? v->Format : 0);
    pc::list_ia_ib(LPC(l), v);
}
void D3DAPI L_DrawInstanced(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) {
    if (null_graphics()) return;
    if (lx(l)->placeholderPso) return;
    if (lx(l)->topology == D3D_PRIMITIVE_TOPOLOGY_QUADLIST) {
        D3D12_INDEX_BUFFER_VIEW q = quad_ib(hdr(l)->device);
        uint32_t quads = a / 4;
        if (quads > kQuadMax) quads = kQuadMax;
        pc::list_ia_ib(LPC(l), &q);
        pc::list_draw_indexed(LPC(l), quads * 6, b, 0, (int32_t)c, d);
        if (lx(l)->ibValid) pc::list_ia_ib(LPC(l), &lx(l)->ib);
        return;
    }
    pc::list_draw(LPC(l), a, b, c, d);
}
void D3DAPI L_DrawIndexedInstanced(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, INT d, UINT e) {
    if (null_graphics()) return;
    if (lx(l)->placeholderPso) return;
    XSL_TRACE("gfx.cmd", "list %p DrawIndexed %u x%u", (void*)l, a, b);
    pc::list_draw_indexed(LPC(l), a, b, c, d, e);
}
void D3DAPI L_Dispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
    if (!null_graphics() && !lx(l)->placeholderPso) pc::list_dispatch(LPC(l), x, y, z);
}
// Console lists have no heap binding: descriptor tables address descriptors directly. The layer's
// global view heap stays bound; the title's call only chooses the sampler heap (or a heap that did not
// fit in the global one).
void D3DAPI L_SetDescriptorHeaps(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* h) {
    XSL_TRACE("gfx.cmd", "list %p SetDescriptorHeaps n=%u %p %p", (void*)l, n, n > 0 ? (void*)h[0] : nullptr, n > 1 ? (void*)h[1] : nullptr);
    PcObj view = pc::global_view_heap_if_created(), sampler = nullptr;
    for (UINT i = 0; i < n; i++) {
        if (!h[i]) continue;
        auto* e = ex<ID3D12DescriptorHeap, HeapExtra>(h[i]);
        if (e->desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) view = P(h[i]);  // the global heap, or a fallback heap
        else sampler = P(h[i]);
    }
    lx(l)->sampler = sampler;
    PcObj tmp[2];
    UINT k = 0;
    if (view) tmp[k++] = view;
    if (sampler) tmp[k++] = sampler;
    if (k) pc::list_set_heaps(LPC(l), k, tmp);
}
void D3DAPI L_SetGfxRootTable(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    XSL_TRACE("gfx.cmd", "list %p RootTable %u %llx", (void*)l, i, (unsigned long long)h.ptr);
    pc::list_gfx_root_table(LPC(l), i, desc_pc_gpu(h.ptr));
}
void D3DAPI L_SetComputeRootTable(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    pc::list_compute_root_table(LPC(l), i, desc_pc_gpu(h.ptr));
}
void D3DAPI L_SetGfxRootCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    XSL_TRACE("gfx.cmd", "list %p RootCBV %u %llx", (void*)l, i, (unsigned long long)a);
    pc::list_gfx_root_cbv(LPC(l), i, cbv_fix_root(hdr(l)->device, a));
}
void D3DAPI L_SetComputeRootCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    pc::list_compute_root_cbv(LPC(l), i, cbv_fix_root(hdr(l)->device, a));
}
// Root SRVs/UAVs may address GPU-visible title memory (XMem graphics allocations).
void D3DAPI L_SetGfxRootSrv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_gfx_root_srv(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetComputeRootSrv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_compute_root_srv(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetGfxRootUav(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_gfx_root_uav(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetComputeRootUav(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_compute_root_uav(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetGfxRoot32(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT o) { pc::list_gfx_root_constants(LPC(l), i, n, d, o); }
void D3DAPI L_SetGfxRoot32One(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT o) { pc::list_gfx_root_constants(LPC(l), i, 1, &v, o); }
void D3DAPI L_SetComputeRoot32(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT o) { pc::list_compute_root_constants(LPC(l), i, n, d, o); }
void D3DAPI L_SetComputeRoot32One(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT o) { pc::list_compute_root_constants(LPC(l), i, 1, &v, o); }
void D3DAPI L_CopyBufferRegion(ID3D12GraphicsCommandList* l, ID3D12Resource* d, UINT64 doff, ID3D12Resource* s, UINT64 soff, UINT64 n) {
    pc::list_copy_buffer_region(LPC(l), P(d), doff, P(s), soff, n);
}
void D3DAPI L_CopyTextureRegion(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
                                const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box) {
    D3D12_TEXTURE_COPY_LOCATION dd = *d, ss = *s;
    dd.pResource = (ID3D12Resource*)P(d->pResource);
    ss.pResource = (ID3D12Resource*)P(s->pResource);
    pc::list_copy_texture_region(LPC(l), &dd, x, y, z, &ss, box);
}
void D3DAPI L_CopyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* s) { pc::list_copy_resource(LPC(l), P(d), P(s)); }
void D3DAPI L_ResolveSubresource(ID3D12GraphicsCommandList* l, ID3D12Resource* d, UINT ds, ID3D12Resource* s, UINT ss, DXGI_FORMAT f) {
    XSL_TRACE("gfx.cmd", "list %p Resolve dst %p src %p fmt %d", (void*)l, (void*)d, (void*)s, f);
    pc::list_resolve_subresource(LPC(l), P(d), ds, P(s), ss, f);
}
// Queries. The console-only predication-status query has no PC counterpart and is dropped.
bool pc_query_type(D3D12_QUERY_TYPE t, uint32_t* out) {
    if ((uint32_t)t > D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 2) XSL_WARN(CH, "query type %d has no PC equivalent; ignored", (int)t);
        return false;
    }
    *out = (uint32_t)t;
    return true;
}
void D3DAPI L_BeginQuery(ID3D12GraphicsCommandList* l, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    uint32_t pt;
    if (pc_query_type(t, &pt)) pc::list_begin_query(LPC(l), P(h), pt, i);
}
void D3DAPI L_EndQuery(ID3D12GraphicsCommandList* l, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) {
    uint32_t pt;
    if (pc_query_type(t, &pt)) pc::list_end_query(LPC(l), P(h), pt, i);
}
void D3DAPI L_ResolveQueryData(ID3D12GraphicsCommandList* l, ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource* d, UINT64 off) {
    uint32_t pt;
    if (pc_query_type(t, &pt)) pc::list_resolve_query(LPC(l), P(h), pt, s, n, P(d), off);
}
// Console-only list methods.
void D3DAPI L_KickoffX(ID3D12GraphicsCommandList*) {}  // early submission: no PC equivalent needed
void D3DAPI L_Write64BOP(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, UINT64 v, D3D12XBOX_FLUSH, D3D12XBOX_WRITE_VALUE_BOP_FLAGS flags) {
    Device* dev = hdr(l)->device;
    if (flags & (D3D12XBOX_WRITE_VALUE_BOP_FLAG_GPU_TIMESTAMP | D3D12XBOX_WRITE_VALUE_BOP_FLAG_SYSTEM_TIMESTAMP)) {
        // A GPU timestamp: resolve a timestamp query straight into the destination memory.
        UnifiedRef r;
        if (unified_lookup(dev, va, &r)) pc::list_write_timestamp(dev_extra(dev)->pcDevice, LPC(l), r.res, r.offset);
        else XSL_WARN(CH, "timestamp write to %llx: not in GPU-visible title memory", (unsigned long long)va);
        return;
    }
    uint64_t g = gpu_va(dev, va);
    pc::list_write_immediate(LPC(l), g, (uint32_t)v, true);
    pc::list_write_immediate(LPC(l), g + 4, (uint32_t)(v >> 32), true);
}
void D3DAPI L_Write32BOP(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, UINT v, D3D12XBOX_FLUSH, D3D12XBOX_WRITE_VALUE_BOP_FLAGS) {
    pc::list_write_immediate(LPC(l), gpu_va(hdr(l)->device, va), v, true);
}
void D3DAPI L_FlushPipelineX(ID3D12GraphicsCommandList* l, D3D12XBOX_FLUSH, D3D12_GPU_VIRTUAL_ADDRESS, SIZE_T) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // global UAV barrier: the closest PC flush
    pc::list_barriers(LPC(l), 1, &b);
}
void D3DAPI L_ExecuteIndirect(ID3D12GraphicsCommandList* l, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args, UINT64 ao,
                              ID3D12Resource* cnt, UINT64 co) {
    if (null_graphics()) return;
    pc::list_execute_indirect(LPC(l), P(sig), max, P(args), ao, P(cnt), co);
}
void D3DAPI L_ClearUAVUint(ID3D12GraphicsCommandList* l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* r,
                           const UINT v[4], UINT n, const D3D12_RECT* rects) {
    pc::list_clear_uav_uint(LPC(l), desc_pc_gpu(g.ptr), desc_pc_cpu(c.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), P(r), v, n, rects);
}
void D3DAPI L_ClearUAVFloat(ID3D12GraphicsCommandList* l, D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE c, ID3D12Resource* r,
                            const FLOAT v[4], UINT n, const D3D12_RECT* rects) {
    pc::list_clear_uav_float(LPC(l), desc_pc_gpu(g.ptr), desc_pc_cpu(c.ptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), P(r), v, n, rects);
}
void D3DAPI L_DiscardResource(ID3D12GraphicsCommandList* l, ID3D12Resource* r, const D3D12_DISCARD_REGION*) { pc::list_discard(LPC(l), P(r)); }
void D3DAPI L_DispatchMesh(ID3D12GraphicsCommandList6* l, UINT x, UINT y, UINT z) {
    if (!null_graphics()) pc::list_dispatch_mesh(pc_of(l), x, y, z);
}

// Console copy variants: the flags select copy engines and cache behaviour, which have no PC meaning.
void D3DAPI L_CopyTextureRegionX(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
                                 const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box, D3D12XBOX_COPY_FLAGS) {
    L_CopyTextureRegion(l, d, x, y, z, s, box);
}
void D3DAPI L_CopyResourceX(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* s, D3D12XBOX_COPY_FLAGS) { L_CopyResource(l, d, s); }
void D3DAPI L_OMSetBlendFactor(ID3D12GraphicsCommandList* l, const FLOAT f[4]) { pc::list_blend_factor(LPC(l), f); }
void D3DAPI L_OMSetStencilRef(ID3D12GraphicsCommandList* l, UINT r) { pc::list_stencil_ref(LPC(l), r); }
// PIX markers are accepted and dropped.
void D3DAPI L_Event(ID3D12GraphicsCommandList*, UINT, const void*, UINT) {}
void D3DAPI L_EndEvent(ID3D12GraphicsCommandList*) {}
D3D12_COMMAND_LIST_TYPE D3DAPI L_GetType(ID3D12CommandList* l) { return ex<XList, ListExtra>((XList*)l)->type; }

D3D12GraphicsCommandList8_Function_Table g_list_tbl;
std::once_flag g_list_once;
}  // namespace

void list_bind_global_heap(void* list, PcObj pcl) {
    auto* e = ex<XList, ListExtra>((XList*)list);
    if (e->type != D3D12_COMMAND_LIST_TYPE_DIRECT && e->type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return;
    if (PcObj gh = pc::global_view_heap_if_created()) pc::list_set_heaps(pcl, 1, &gh);
}
HRESULT make_list(Device* dev, PcObj pcl, D3D12_COMMAND_LIST_TYPE t, REFIID riid, void** out) {
    std::call_once(g_list_once, [] {
        xsl_fill_default(&g_list_tbl);
        install_device_child_base(&g_list_tbl);
        ((D3D12CommandList_Function_Table*)&g_list_tbl)->GetType = L_GetType;
        auto* g = (D3D12GraphicsCommandList_Function_Table*)&g_list_tbl;
        g->Close = L_Close;
        g->Reset = L_Reset;
        g->ResourceBarrier = L_ResourceBarrier;
        g->OMSetRenderTargets = L_OMSetRenderTargets;
        g->ClearRenderTargetView = L_ClearRTV;
        g->ClearDepthStencilView = L_ClearDSV;
        g->RSSetViewports = L_RSSetViewports;
        g->RSSetScissorRects = L_RSSetScissorRects;
        g->SetGraphicsRootSignature = L_SetGfxRootSig;
        g->SetComputeRootSignature = L_SetComputeRootSig;
        g->SetPipelineState = L_SetPipelineState;
        g->IASetPrimitiveTopology = L_IASetPrimitiveTopology;
        g->IASetVertexBuffers = L_IASetVertexBuffers;
        g->IASetIndexBuffer = L_IASetIndexBuffer;
        g->DrawInstanced = L_DrawInstanced;
        g->DrawIndexedInstanced = L_DrawIndexedInstanced;
        g->Dispatch = L_Dispatch;
        g->SetDescriptorHeaps = L_SetDescriptorHeaps;
        g->SetGraphicsRootDescriptorTable = L_SetGfxRootTable;
        g->SetComputeRootDescriptorTable = L_SetComputeRootTable;
        g->SetGraphicsRootConstantBufferView = L_SetGfxRootCbv;
        g->SetComputeRootConstantBufferView = L_SetComputeRootCbv;
        g->SetGraphicsRootShaderResourceView = L_SetGfxRootSrv;
        g->SetComputeRootShaderResourceView = L_SetComputeRootSrv;
        g->SetGraphicsRootUnorderedAccessView = L_SetGfxRootUav;
        g->SetComputeRootUnorderedAccessView = L_SetComputeRootUav;
        g->SetGraphicsRoot32BitConstants = L_SetGfxRoot32;
        g->SetGraphicsRoot32BitConstant = L_SetGfxRoot32One;
        g->SetComputeRoot32BitConstants = L_SetComputeRoot32;
        g->SetComputeRoot32BitConstant = L_SetComputeRoot32One;
        g->CopyBufferRegion = L_CopyBufferRegion;
        g->CopyTextureRegion = L_CopyTextureRegion;
        g->CopyResource = L_CopyResource;
        g->CopyTextureRegionX = L_CopyTextureRegionX;
        g->KickoffX = L_KickoffX;
        g->Write64BitValueBottomOfPipeX = L_Write64BOP;
        g->Write32BitValueBottomOfPipeX = L_Write32BOP;
        g->FlushPipelineX = L_FlushPipelineX;
        g->ExecuteIndirect = L_ExecuteIndirect;
        g->BeginQuery = L_BeginQuery;
        g->EndQuery = L_EndQuery;
        g->ResolveQueryData = L_ResolveQueryData;
        g->ClearUnorderedAccessViewUint = L_ClearUAVUint;
        g->ClearUnorderedAccessViewFloat = L_ClearUAVFloat;
        g->DiscardResource = L_DiscardResource;
        ((D3D12GraphicsCommandList6_Function_Table*)&g_list_tbl)->DispatchMesh = L_DispatchMesh;
        g->ResolveSubresource = L_ResolveSubresource;
        g->CopyResourceX = L_CopyResourceX;
        g->OMSetBlendFactor = L_OMSetBlendFactor;
        g->OMSetStencilRef = L_OMSetStencilRef;
        g->BeginEvent = L_Event;
        g->SetMarker = L_Event;
        g->EndEvent = L_EndEvent;
    });
    auto* o = make<XList, ListExtra>(Kind::List, &g_list_tbl, pcl, dev, Iids<XList>::list, nullptr);
    ex<XList, ListExtra>(o)->type = t;
    list_bind_global_heap(o, pcl);
    return hand_out(o, riid, out);
}

// ============================================================================================
// Command queue, including PresentX.
// ============================================================================================
namespace {
struct QueueExtra { D3D12_COMMAND_QUEUE_DESC desc; };
void D3DAPI Q_Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    desc_sync();  // descriptors the title wrote inline since the last submission
    std::vector<PcObj> v(n);
    for (UINT i = 0; i < n; i++) { v[i] = P(lists[i]); XSL_TRACE("gfx.cmd", "Execute list %p", (void*)lists[i]); }
    pc::queue_execute(pc_of(q), n, v.data());
}
HRESULT D3DAPI Q_Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) { return pc::queue_signal(pc_of(q), P(f), v); }
// Signal with console cache-flush flags: a PC queue signal already follows completion of all prior
// work, caches included.
HRESULT D3DAPI Q_SignalX(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v, D3D12XBOX_FLUSH) { return pc::queue_signal(pc_of(q), P(f), v); }
HRESULT D3DAPI Q_Wait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) { return pc::queue_wait(pc_of(q), P(f), v); }
void D3DAPI Q_GetDesc(ID3D12CommandQueue* q, D3D12_COMMAND_QUEUE_DESC* d) { *d = ex<ID3D12CommandQueue, QueueExtra>(q)->desc; }
HRESULT D3DAPI Q_SuspendX(ID3D12CommandQueue*, UINT) { return S_OK; }
HRESULT D3DAPI Q_ResumeX(ID3D12CommandQueue*) { return S_OK; }
HRESULT D3DAPI Q_GetTimestampFrequency(ID3D12CommandQueue* q, UINT64* f) { return pc::queue_timestamp_frequency(pc_of(q), f); }
HRESULT D3DAPI Q_GetClockCalibration(ID3D12CommandQueue* q, UINT64* g, UINT64* c) { return pc::queue_clock_calibration(pc_of(q), g, c); }
HRESULT D3DAPI Q_PresentX(ID3D12CommandQueue* q, UINT planes, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* p, const D3D12XBOX_PRESENT_PARAMETERS*) {
    if (planes == 0 || !p || !p->ResourceCount || !p->ppResources) return S_OK;  // "present blank": nothing to show yet
    xsl_fixed_time_frame();
    LARGE_INTEGER callQpc;
    QueryPerformanceCounter(&callQpc);
    Device* dev = hdr(q)->device;
    DeviceExtra* de = dev_extra(dev);
    if (!de->window || !IsWindow(de->window)) de->window = find_title_window();
    if (!de->window) { XSL_WARN(CH, "PresentX: no title window yet"); return S_OK; }
    XSL_TRACE("gfx.cmd", "PresentX planes=%u res %p", planes, (void*)p->ppResources[0]);
    if (planes > 1) XSL_TRACE(CH, "PresentX: %u planes, presenting plane 0 only", planes);
    // Swap chains need a direct queue; titles may present from a compute queue (FramePacing does).
    PcObj presentQ = pc_of(q);
    if (ex<ID3D12CommandQueue, QueueExtra>(q)->desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        if (!de->presentQueue) {
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (FAILED(pc::create_queue(de->pcDevice, &qd, &de->presentQueue))) return E_FAIL;
        }
        if (!de->presentSyncFence && FAILED(pc::create_fence(de->pcDevice, 0, 0, &de->presentSyncFence))) return E_FAIL;
        static volatile long once = 0;
        if (InterlockedIncrement(&once) == 1) XSL_INFO(CH, "PresentX on a type-%d queue: presenting through a direct queue", (int)ex<ID3D12CommandQueue, QueueExtra>(q)->desc.Type);
        uint64_t v = ++de->presentSyncValue;
        pc::queue_signal(pc_of(q), de->presentSyncFence, v);
        pc::queue_wait(de->presentQueue, de->presentSyncFence, v);
        presentQ = de->presentQueue;
    }
    HRESULT hr = null_graphics() ? pc::null_present(de->window)
                                 : pc::present(de->pcDevice, presentQ, dbg_present_override() ? dbg_present_override() : P(p->ppResources[0]),
                                               de->window, 0, (uint32_t)p->ColorSpace);
    // Enforce the console's frame-latency bound: block until the GPU has finished every frame
    // older than maxLatency. Without this a title resets command allocators still in flight.
    if (!de->latencyFence) {
        pc::create_fence(de->pcDevice, 0, 0, &de->latencyFence);
        de->latencyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    uint64_t v = ++de->presentCount;
    pc::queue_signal(presentQ, de->latencyFence, v);
    LARGE_INTEGER retQpc;
    QueryPerformanceCounter(&retQpc);
    frame_stats_present(dev, p->Token, callQpc.QuadPart, retQpc.QuadPart, v);
    if (v > de->maxLatency) {
        uint64_t need = v - de->maxLatency;
        if (pc::fence_completed(de->latencyFence) < need) {
            pc::fence_set_event(de->latencyFence, need, de->latencyEvent);
            WaitForSingleObject(de->latencyEvent, 2000);
        }
    }
    return hr;
}
void D3DAPI Q_Event(ID3D12CommandQueue*, UINT, const void*, UINT) {}
void D3DAPI Q_EndEvent(ID3D12CommandQueue*) {}
D3D12CommandQueue_Function_Table g_queue_tbl;
std::once_flag g_queue_once;
}  // namespace

HRESULT make_queue(Device* dev, PcObj pcq, const D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** out) {
    std::call_once(g_queue_once, [] {
        xsl_fill_default(&g_queue_tbl);
        install_device_child_base(&g_queue_tbl);
        g_queue_tbl.ExecuteCommandLists = Q_Execute;
        g_queue_tbl.Signal = Q_Signal;
        g_queue_tbl.SignalX = Q_SignalX;
        g_queue_tbl.Wait = Q_Wait;
        g_queue_tbl.GetDesc = Q_GetDesc;
        g_queue_tbl.SuspendX = Q_SuspendX;
        g_queue_tbl.ResumeX = Q_ResumeX;
        g_queue_tbl.GetTimestampFrequency = Q_GetTimestampFrequency;
        g_queue_tbl.GetClockCalibration = Q_GetClockCalibration;
        g_queue_tbl.PresentX = Q_PresentX;
        g_queue_tbl.CopyPageMappingsX = Q_CopyPageMappingsX;
        g_queue_tbl.CopyPageMappingsBatchX = Q_CopyPageMappingsBatchX;
        g_queue_tbl.ClearPageMappingsX = Q_ClearPageMappingsX;
        g_queue_tbl.BeginEvent = Q_Event;
        g_queue_tbl.SetMarker = Q_Event;
        g_queue_tbl.EndEvent = Q_EndEvent;
    });
    auto* o = make<ID3D12CommandQueue, QueueExtra>(Kind::Queue, &g_queue_tbl, pcq, dev, Iids<ID3D12CommandQueue>::list, nullptr);
    o->m_Type = d->Type;
    ex<ID3D12CommandQueue, QueueExtra>(o)->desc = *d;
    return hand_out(o, riid, out);
}

// ============================================================================================
// Title window discovery for presentation.
// ============================================================================================
HWND find_title_window() {
    struct Ctx { DWORD pid; HWND best; } c{GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == c->pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { c->best = w; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.best;
}

}  // namespace xs

namespace xs {
static void module_of(const void* a, char* out, size_t n, uintptr_t* off) {
    HMODULE m = nullptr;
    *off = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        char path[MAX_PATH];
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\');
        snprintf(out, n, "%s", b ? b + 1 : path);
        *off = (uintptr_t)a - (uintptr_t)m;
    } else {
        snprintf(out, n, "?");
    }
}
void report_foreign(void* obj, void* caller, const char* fn) {
    char vm[64] = "?", cm[64];
    uintptr_t voff = 0, coff;
    void* vtbl = nullptr;
    if (!IsBadReadPtr(obj, sizeof(void*))) {
        vtbl = *(void**)obj;
        module_of(vtbl, vm, sizeof vm, &voff);
    }
    module_of(caller, cm, sizeof cm, &coff);
    XSL_ERR(CH, "foreign object %p (vtable %p = %s+%#llx) passed to %s from %s+%#llx", obj, vtbl, vm,
            (unsigned long long)voff, fn, cm, (unsigned long long)coff);
}
}  // namespace xs
