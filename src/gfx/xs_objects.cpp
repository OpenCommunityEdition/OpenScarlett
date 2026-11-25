// SPDX-License-Identifier: AGPL-3.0-or-later
// Console Direct3D 12.X objects implemented over the PC back end.
#include <cstring>
#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "xs_object.h"

void xsl_fixed_time_frame();   // fixed_time.cpp (global namespace)
#include "xs_cbv.h"
#include "xs_detile.h"
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
    // State objects: ID3D12StateObjectProperties is a second object (header user[0] links them).
    if ((h->kind == Kind::StateObject && IsEqualGUID(riid, __uuidof(ID3D12StateObjectProperties))) ||
        (h->kind == Kind::StateObjectProperties && IsEqualGUID(riid, __uuidof(ID3D12StateObject)))) {
        auto* other = (IGraphicsUnknown*)h->user[0];
        AddRef(other);
        *out = other;
        return S_OK;
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
void keep_foreign(void* pcObject);
std::mutex g_alias_mu;
std::map<PcObj, uint64_t> g_plane_alias;   // PC resource -> offset in the video frame
std::map<void*, PcObj> g_list_frame;       // console list -> decoded frame it last copied from
void note_plane_alias(PcObj r, uint64_t off) {
    std::lock_guard<std::mutex> g(g_alias_mu);
    g_plane_alias[r] = off;
}
void list_parts_free(IGraphicsUnknown* self);   // a list's parts cut at waits and never submitted
void free_object(IGraphicsUnknown* self) {
    ObjHeader* h = hdr(self);
    if (h->kind == Kind::List) list_parts_free(self);
    if (h->pc && h->kind == Kind::Resource) { std::lock_guard<std::mutex> g(g_alias_mu); g_plane_alias.erase(h->pc); }
    // Resources: the PC object outlives the console one by a few frames. Titles free resources once
    // their own fences say the GPU is done, but other components given the PC object (Media
    // Foundation's decoder) may still have work referencing it.
    if (h->pc && h->kind == Kind::Resource) { console_memory_release(h->pc); keep_foreign(h->pc); pc::release(h->pc); }
    else if (h->pc) pc::release(h->pc);
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
IIDS(ID3D12XboxDmaCommandList, &__uuidof(IGraphicsUnknown), &__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild), &__uuidof(ID3D12CommandList),
     &__uuidof(ID3D12XboxDmaCommandList))
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
// XSL_HANG_WATCH=1 (diagnostics): fence polls and waits per fence; when presents stop for 15 s the
// most polled fences are logged with their completed values and the last values waited for.
struct FenceWatch { uint64_t polls = 0, lastWait = 0, lastCompleted = 0; };
std::mutex g_fw_mu;
std::map<void*, FenceWatch> g_fw;
// Every other thread's stack as module+offset frames (x64 unwind data; no symbols needed).
void dump_thread_stacks() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int shown = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && shown < 400; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); continue; }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        DWORD64 pcs[64];
        int n = 0;
        if (GetThreadContext(t, &c)) {   // only unwinding while the thread is suspended (no allocation, no loader calls)
            for (; n < 64 && c.Rip; n++) {
                pcs[n] = c.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!rf) { c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue; }   // leaf function
                void* hd = nullptr;
                DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &est, nullptr);
            }
        }
        ResumeThread(t);
        CloseHandle(t);
        std::string line;
        for (int f = 0; f < n; f++) {
            HMODULE m = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pcs[f], &m) && m) {
                GetModuleFileNameA(m, name, MAX_PATH);
                const char* bs = strrchr(name, '\\');
                if (bs) memmove(name, bs + 1, strlen(bs + 1) + 1);
            }
            char fr[160];
            snprintf(fr, sizeof fr, " %s+%llx", name, (unsigned long long)(pcs[f] - (uint64_t)m));
            line += fr;
        }
        XSL_WARN(CH, "  thread %lu:%s", te.th32ThreadID, line.c_str());
        shown++;
    }
    CloseHandle(snap);
}
bool hang_watch() {
    static bool on = [] {
        char b[4];
        if (!(GetEnvironmentVariableA("XSL_HANG_WATCH", b, 4) && b[0] == '1')) return false;
        std::thread([] {
            uint64_t last = 0;
            int still = 0;
            for (;;) {
                Sleep(5000);
                uint64_t p = pc::present_count();
                still = p == last ? still + 1 : 0;
                last = p;
                if (still != 3) continue;
                std::vector<std::pair<void*, FenceWatch>> v;
                { std::lock_guard<std::mutex> g(g_fw_mu); v.assign(g_fw.begin(), g_fw.end()); for (auto& e : g_fw) e.second.polls = 0; }
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.polls > b.second.polls; });
                XSL_WARN(CH, "no present for 15 s (present %llu); fences polled since the last report:", (unsigned long long)p);
                for (size_t k = 0; k < v.size() && k < 8; k++)
                    XSL_WARN(CH, "  fence %p (PC %p): %llu polls, completed %llu, last wait for %llu", v[k].first, pc_of((ID3D12Fence*)v[k].first),
                             (unsigned long long)v[k].second.polls, (unsigned long long)v[k].second.lastCompleted, (unsigned long long)v[k].second.lastWait);
                pc::dump_queue_state();
                dump_thread_stacks();
            }
        }).detach();
        return true;
    }();
    return on;
}
UINT64 D3DAPI Fence_GetCompletedValue(ID3D12Fence* f) {
    UINT64 v = pc::fence_completed(pc_of(f));
    if (hang_watch()) { std::lock_guard<std::mutex> g(g_fw_mu); auto& w = g_fw[f]; w.polls++; w.lastCompleted = v; }
    return v;
}
HRESULT D3DAPI Fence_SetEventOnCompletion(ID3D12Fence* f, UINT64 v, HANDLE e) {
    if (hang_watch()) { std::lock_guard<std::mutex> g(g_fw_mu); g_fw[f].lastWait = v; }
    return pc::fence_set_event(pc_of(f), v, e);
}
// Fence value memory. Console fences live in memory the title can read: GetGpuAddressX returns the
// address of the 64-bit completed value, and titles wait by polling it (monitorx/mwaitx). Here each
// fence that is asked for its address gets an 8-byte slot in GPU-visible XMem memory; CPU signals
// write it at once, and a watcher thread copies GPU completions into it (an event per fence, armed
// for the next value).
namespace fmem {
struct Entry { PcObj pc; volatile uint64_t* slot; HANDLE ev; uint64_t armed; };
std::mutex g_mu;
std::map<void*, Entry> g_fences;
std::vector<volatile uint64_t*> g_free;
HANDLE g_wake = nullptr;
volatile uint64_t* take_slot() {
    if (g_free.empty()) {
        typedef PVOID(WINAPI * VAlloc)(PVOID, SIZE_T, DWORD, ULONGLONG, DWORD);
        static auto va = [] { HMODULE m = GetModuleHandleW(L"xmem.dll"); return m ? (VAlloc)GetProcAddress(m, "XMemVirtualAlloc") : nullptr; }();
        const SIZE_T bytes = 64 << 10;
        // XMEM_GRAPHICS: GPU commands may name the address too (unified memory)
        void* p = va ? va(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, 0x3ull, PAGE_READWRITE) : nullptr;
        if (!p) p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return nullptr;
        for (size_t k = bytes / 8; k-- > 0;) g_free.push_back((volatile uint64_t*)p + k);
    }
    auto* s = g_free.back();
    g_free.pop_back();
    return s;
}
void watcher() {
    std::vector<HANDLE> evs;
    std::vector<void*> keys;
    for (;;) {
        evs.assign(1, g_wake);
        keys.assign(1, nullptr);
        {
            std::lock_guard<std::mutex> g(g_mu);
            for (auto& [k, e] : g_fences) {
                uint64_t v = pc::fence_completed(e.pc);
                if (v != *e.slot && v != UINT64_MAX) *e.slot = v;
                if (v != UINT64_MAX && v >= e.armed) { e.armed = v + 1; pc::fence_set_event(e.pc, e.armed, e.ev); }
                if (evs.size() < MAXIMUM_WAIT_OBJECTS) { evs.push_back(e.ev); keys.push_back(k); }
            }
        }
        // More fences than one wait can name: those are polled every millisecond.
        WaitForMultipleObjects((DWORD)evs.size(), evs.data(), FALSE, evs.size() < (size_t)MAXIMUM_WAIT_OBJECTS ? 50 : 1);
    }
}
void destroy(IGraphicsUnknown* self) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_fences.find(self);
    if (it == g_fences.end()) return;
    CloseHandle(it->second.ev);
    g_free.push_back(it->second.slot);
    g_fences.erase(it);
}
uint64_t address(ID3D12Fence* f) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_fences.find(f);
    if (it != g_fences.end()) return (uint64_t)(uintptr_t)it->second.slot;
    if (!g_wake) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(watcher).detach();
    }
    volatile uint64_t* slot = take_slot();
    if (!slot) return 0;
    uint64_t v = pc::fence_completed(pc_of(f));
    *slot = v == UINT64_MAX ? 0 : v;
    Entry e{pc_of(f), slot, CreateEventW(nullptr, FALSE, FALSE, nullptr), *slot + 1};
    pc::fence_set_event(e.pc, e.armed, e.ev);
    g_fences[f] = e;
    hdr(f)->destroy = destroy;
    SetEvent(g_wake);
    return (uint64_t)(uintptr_t)slot;
}
void cpu_signal(ID3D12Fence* f, uint64_t v) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_fences.find(f);
    if (it != g_fences.end()) *it->second.slot = v;
}
}  // namespace fmem
HRESULT D3DAPI Fence_Signal(ID3D12Fence* f, UINT64 v) {
    HRESULT hr = pc::fence_signal(pc_of(f), v);
    if (SUCCEEDED(hr)) fmem::cpu_signal(f, v);
    return hr;
}
D3D12_GPU_VIRTUAL_ADDRESS D3DAPI Fence_GetGpuAddressX(ID3D12Fence1* f) { return fmem::address((ID3D12Fence*)f); }

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
// Console-only pipeline information. Translated pipelines have no console code objects: the symbol
// table is empty and no scratch memory is needed.
HRESULT D3DAPI Pso_GetInformationX(ID3D12PipelineState*, D3D12XBOX_PIPELINE_STATE_INFORMATION_TYPE type, UINT64, ID3DBlob** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (type == D3D12XBOX_PIPELINE_STATE_INFORMATION_TYPE_SYMBOL_TABLE) {
        D3D12XBOX_SHADER_SYMBOL_LIST l{};
        *out = (ID3DBlob*)make_pc_blob(pc::create_blob(&l, sizeof l));
        return S_OK;
    }
    if (type == D3D12XBOX_PIPELINE_STATE_INFORMATION_TYPE_SCRATCH_SIZE_PER_WAVE_IN_UNITS_OF_256DW) {
        UINT z = 0;
        *out = (ID3DBlob*)make_pc_blob(pc::create_blob(&z, sizeof z));
        return S_OK;
    }
    return E_INVALIDARG;
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
        g_fence_tbl.GetGpuAddressX = Fence_GetGpuAddressX;
        xsl_fill_default(&g_rs_tbl);
        install_device_child_base(&g_rs_tbl);
        xsl_fill_default(&g_pso_tbl);
        install_device_child_base(&g_pso_tbl);
        g_pso_tbl.GetCachedBlob = Pso_GetCachedBlob;
        g_pso_tbl.GetInformationX = Pso_GetInformationX;
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
struct ResExtra { uint64_t trackedVa; uint64_t placedVa; uint64_t titleVa; };   // titleVa: placed on title memory at that address
HRESULT D3DAPI Res_Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range, void** out) {
    if (out && sub == 0) {
        // A texture placed on title memory is that memory: the title gets its address (a movie
        // decoder is handed it and fills the planes there), or, for an address in a GPU view of
        // the title's CPU memory, the CPU side of it. The texture is kept in step from there.
        auto* te = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)r);
        if (te->titleVa) {
            D3D12_RESOURCE_DESC td{};
            pc::res_desc(pc_of(r), &td);
            if (td.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
                typedef uintptr_t(WINAPI * Fn)(uintptr_t);
                static Fn src = [] { HMODULE m = GetModuleHandleW(L"xmem.dll"); return m ? (Fn)GetProcAddress(m, "xsl_xmem_source") : nullptr; }();
                *out = (void*)(src ? src((uintptr_t)te->titleVa) : (uintptr_t)te->titleVa);
                static volatile long n = 0;
                if (InterlockedIncrement(&n) <= 4)
                    XSL_INFO(CH, "Map of a texture placed on title memory at %llx (%llux%u format %d): that memory, at %p", (unsigned long long)te->titleVa,
                             (unsigned long long)td.Width, td.Height, (int)td.Format, *out);
                return S_OK;
            }
        }
    }
    HRESULT hr = pc::res_map(pc_of(r), sub, range, out);
    if (SUCCEEDED(hr) && out) detile_mapped(pc_of(r), pc::map_shadow_base(pc_of(r)));   // a texture's CPU copy
    if (FAILED(hr) && out) {
        // A buffer in video memory: on the console the title would get the memory the resource took
        // from its allocator (and may check that the address is its own). It gets that memory.
        D3D12_RESOURCE_DESC d{};
        pc::res_desc(pc_of(r), &d);
        auto* e = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)r);
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && e->titleVa && mapped_buffer_known(pc_of(r))) {   // placed on title memory: that memory
            *out = (void*)e->titleVa;
            return S_OK;
        }
        void* cm = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? console_memory_ensure(pc_of(r)) : nullptr;
        if (cm) {
            mapped_buffer_add(pc_of(r), cm, d.Width);
            *out = cm;
            return S_OK;
        }
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5) XSL_WARN(CH, "Map of a resource in video memory failed %08lx (dim %d, %llu wide)", hr, d.Dimension, (unsigned long long)d.Width);
    }
    return hr;
}
void D3DAPI Res_Unmap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* range) {
    if (mapped_buffer_known(pc_of(r))) return;
    if (ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)r)->titleVa && !pc::map_shadow_base(pc_of(r))) return;   // Map gave the title memory itself   // never mapped on the PC side (an Unmap there removes the device)
    pc::res_unmap(pc_of(r), sub, range);
    detile_unmapped(pc_of(r));
}
void D3DAPI Res_GetDesc(ID3D12Resource* r, D3D12_RESOURCE_DESC* d) { pc::res_desc(pc_of(r), d); }
// On the console every resource has an address, textures too, and it is CPU memory as well: titles
// write texels through it. A PC texture has none; the address of the console memory the resource
// holds is returned (what the title writes there is not uploaded to the texture yet).
D3D12_GPU_VIRTUAL_ADDRESS D3DAPI Res_GetGPUVirtualAddress(ID3D12Resource* r) {
    uint64_t va = pc::res_gpu_va(pc_of(r));
    if (va) return va;
    auto* e = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)r);
    if (e->titleVa) return e->titleVa;     // a texture placed on the title's own memory: that memory
    if (e->placedVa) return e->placedVa;
    void* cm = console_memory_of(pc_of(r));
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 3)
        XSL_WARN(CH, "GetGPUVirtualAddress of a texture: %s", cm ? "its console memory (CPU writes there do not reach the PC texture yet)" : "no address on a PC");
    return (uint64_t)(uintptr_t)cm;
}
HRESULT D3DAPI Res_GetHeapProperties(ID3D12Resource*, D3D12_HEAP_PROPERTIES* p, D3D12_HEAP_FLAGS* f) {
    if (p) { *p = {}; p->Type = D3D12_HEAP_TYPE_DEFAULT; }
    if (f) *f = D3D12_HEAP_FLAG_NONE;
    return S_OK;
}
D3D12Resource2_Function_Table g_res_tbl;
std::once_flag g_res_once;
void Res_Destroy(IGraphicsUnknown* self) {
    auto* e = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)self);
    if (e->trackedVa) cbv_untrack(e->trackedVa);
    if (e->placedVa) pagemap_forget(e->placedVa, hdr(self)->pc);
    if (e->titleVa) written_forget(e->titleVa, self);
    pc::gm_forget(hdr(self)->pc);
    detile_forget(hdr(self)->pc);
    pc::note_reduced(hdr(self)->pc, false);
}
}  // namespace

void resource_set_placed(void* resource, uint64_t va) { ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)resource)->placedVa = va; }
void resource_set_title_va(void* resource, uint64_t va) { ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)resource)->titleVa = va; }
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
    desc_view_detach(e->offset);
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
    int placeholderWhy;           // ... and its reason index (g_ph_why), -1 unknown
    const void* psoObj;           // the title's pipeline object last selected (draw dump)
    uint64_t csTables[16];        // console GPU handles of the compute root tables (ray dispatches read them)
    const void* rtPipeline;       // state object of SetPipelineState1
    // Whole-resource states set by earlier transitions in this recording (see L_ResourceBarrier).
    static constexpr uint32_t kTracked = 64;
    uint32_t nTracked;
    struct { void* res; uint32_t state; } tracked[kTracked];
    // Waits for memory values (see "Waits for memory values").
    PcObj alloc;                  // the PC allocator of the current recording
    bool touched;                 // something was recorded into the current PC list
    struct ListWaits* w;          // the parts cut off so far and the waits before the current one (null: none yet)
};
volatile long g_skipped_work = 0;   // draws/dispatches skipped for placeholder pipelines
// Why each placeholder pipeline has no PC pipeline, and how much work each reason skipped.
std::mutex g_ph_mu;
std::map<void*, int> g_ph_of;                           // title pipeline object -> reason index
std::vector<std::pair<std::string, long>> g_ph_why;     // reason, skipped draws/dispatches since the last report
static int placeholder_reason(void* obj) {
    std::lock_guard<std::mutex> g(g_ph_mu);
    auto it = g_ph_of.find(obj);
    return it == g_ph_of.end() ? -1 : it->second;
}
static void placeholder_skip(int why) {
    InterlockedIncrement(&g_skipped_work);
    if (why < 0) return;
    std::lock_guard<std::mutex> g(g_ph_mu);
    if ((size_t)why < g_ph_why.size()) g_ph_why[why].second++;
}
ListExtra* lx(ID3D12GraphicsCommandList* l) { return ex<XList, ListExtra>((XList*)l); }

// ---- Waits for memory values --------------------------------------------------------------------
// The console GPU can stand at a point of a command list until a value in memory passes a
// comparison (Wait32BitValueX, Wait64BitValueX), and write a value to memory when the work before
// a point of a list is done (the bottom-of-pipe writes). A title orders the work of its queues
// with the two: one queue's lighting waits for a value the depth pass of another writes. Skipped,
// as they were here, the work runs in the order the lists happen to be submitted in.
//
// A list is cut at each wait (pc::list_cut): the PC list recorded so far is a part of its own, and
// the wait stands before the part that follows. A wait before any work of a part joins the waits
// before that part. A queue given such parts keeps a backlog: it runs the parts in order, stands
// at an unmet wait, and everything given to it meanwhile (lists, signals, waits for fences) goes
// behind. A thread looks at the backlogs every millisecond, and so does every submission.
//
// A wait reads the title's memory. Writes from lists reach that memory where it is shared with
// the GPU (unified memory, sections); for other addresses the value is written here once the part
// that holds the write has run (a fence of the layer's own after it).
// Shortcuts: a write counts as done when its whole part has run, not at its place in the part;
// a wait not met within XSL_WAIT_TIMEOUT_MS (3000) is given up, with a warning; a list submitted
// while its queue has a backlog is given to the backlog for good (a second submission of the same
// recording runs nothing). XSL_LIST_WAITS=0: waits are skipped, as before.
struct ValueWait { uint64_t va, ref, mask; int cmp; bool wide; };
struct MemWrite {
    uint64_t va, value;
    bool wide;
    // A read-back instead of a value (staging set): rows of texels a list copied into a buffer of
    // the layer's own, to be stored at va once the list has run (see list_read_back).
    PcObj staging = nullptr;
    uint32_t rows = 0, rowBytes = 0, pitch = 0;
};
struct ListPart { std::vector<ValueWait> waits; PcObj list; std::vector<MemWrite> writes; };
struct ListWaits {
    std::vector<ListPart> earlier;   // the parts before the PC list being recorded, with the waits before each
    std::vector<ValueWait> gate;     // the waits before the PC list being recorded
    std::vector<MemWrite> writes;    // its writes the CPU does not see by itself, and its read-backs
};
static void mem_write_drop(std::vector<MemWrite>& v) {   // (of a recording that is never run)
    for (MemWrite& w : v)
        if (w.staging) pc::release(w.staging);
    v.clear();
}
static bool list_waits_on() {
    static const bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_LIST_WAITS", b, 4) && b[0] == '0'); }();
    return on;
}
static ULONGLONG wait_timeout_ms() {
    static const ULONGLONG v = [] { char b[16]; return GetEnvironmentVariableA("XSL_WAIT_TIMEOUT_MS", b, sizeof b) ? (ULONGLONG)strtoull(b, nullptr, 10) : 3000ull; }();
    return v;
}
// The CPU address of a title GPU address (xmem.dll knows the views of CPU memory).
static void* wait_cpu_address(uint64_t va) {
    typedef uintptr_t(WINAPI * Fn)(uintptr_t);
    static Fn src = [] { HMODULE m = GetModuleHandleW(L"xmem.dll"); return m ? (Fn)GetProcAddress(m, "xsl_xmem_source") : nullptr; }();
    return (void*)(src ? src((uintptr_t)va) : (uintptr_t)va);
}
static bool wait_value(const ValueWait& w, uint64_t* out) {
    uint64_t v = 0;
    SIZE_T got = 0;
    const SIZE_T n = w.wide ? 8 : 4;
    // (Through the kernel: memory that is not there is reported, not a fault.)
    if (!w.va || !ReadProcessMemory(GetCurrentProcess(), wait_cpu_address(w.va), &v, n, &got) || got != n) return false;
    *out = w.wide ? v : (v & (w.mask ? w.mask : 0xffffffffull));
    return true;
}
static bool wait_met(const ValueWait& w) {
    uint64_t v = 0;
    if (!wait_value(w, &v)) return true;   // nothing readable there: nothing to wait for
    switch (w.cmp) {
        case D3D12_COMPARISON_FUNC_NEVER: return false;
        case D3D12_COMPARISON_FUNC_LESS: return v < w.ref;
        case D3D12_COMPARISON_FUNC_EQUAL: return v == w.ref;
        case D3D12_COMPARISON_FUNC_LESS_EQUAL: return v <= w.ref;
        case D3D12_COMPARISON_FUNC_GREATER: return v > w.ref;
        case D3D12_COMPARISON_FUNC_NOT_EQUAL: return v != w.ref;
        case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return v >= w.ref;
        default: return true;
    }
}
struct BackItem {
    int kind = 0;   // 0: wait for a memory value, 1: run lists, 2: signal a fence, 3: wait for a fence
    ValueWait wait{};
    std::vector<PcObj> lists, owned;   // owned: let go after they ran
    std::vector<MemWrite> writes;
    PcObj fence = nullptr;
    uint64_t value = 0;
};
struct Backlog {
    std::mutex mu;
    std::deque<BackItem> items;
    PcObj q = nullptr, dev = nullptr, fence = nullptr;
    uint64_t fenceNext = 0;
    std::deque<std::pair<uint64_t, std::vector<MemWrite>>> landing;   // writes of parts that were run, by the fence value after each
    ULONGLONG since = 0;                                              // when the wait at the front was first found unmet
};
static std::mutex g_backlogs_mu;
static std::vector<Backlog*> g_backlogs;
static std::atomic<uint64_t> g_wait_stats[5];   // waits recorded, cuts, waits a queue stood at, given up, parts run from a backlog
static void backlog_run(Backlog* b, std::vector<PcObj>& lists, std::vector<MemWrite>& writes) {
    if (!lists.empty()) pc::queue_execute(b->q, (uint32_t)lists.size(), lists.data());
    if (writes.empty()) return;
    if (!b->fence && FAILED(pc::create_fence(b->dev, 0, 0, &b->fence))) b->fence = nullptr;
    if (!b->fence) return;
    pc::queue_signal(b->q, b->fence, ++b->fenceNext);
    b->landing.push_back({b->fenceNext, std::move(writes)});
}
// Runs what the backlog can run now (its mutex is held).
static void backlog_pump(Backlog* b) {
    if (b->fence && !b->landing.empty()) {
        const uint64_t done = pc::fence_completed(b->fence);
        while (!b->landing.empty() && b->landing.front().first <= done) {
            for (const MemWrite& w : b->landing.front().second) {
                SIZE_T put = 0;
                if (!w.staging) { WriteProcessMemory(GetCurrentProcess(), wait_cpu_address(w.va), &w.value, w.wide ? 8 : 4, &put); continue; }
                if (const uint8_t* from = (const uint8_t*)pc::readback_map(w.staging)) {
                    for (uint32_t r = 0; r < w.rows; r++)
                        WriteProcessMemory(GetCurrentProcess(), (uint8_t*)wait_cpu_address(w.va) + (size_t)r * w.pitch, from + (size_t)r * w.pitch, w.rowBytes, &put);
                    pc::readback_unmap(w.staging);
                }
                pc::release(w.staging);
            }
            b->landing.pop_front();
        }
    }
    while (!b->items.empty()) {
        BackItem& it = b->items.front();
        if (it.kind == 0) {
            if (!wait_met(it.wait)) {
                const ULONGLONG now = GetTickCount64();
                if (!b->since) { b->since = now; g_wait_stats[2]++; }
                if (now - b->since < wait_timeout_ms()) return;
                uint64_t v = 0;
                wait_value(it.wait, &v);
                static volatile long said = 0;
                if (const long k = InterlockedIncrement(&said); k <= 12 || k % 500 == 0)
                    XSL_WARN(CH, "queue %p stood %llu ms at a wait for the value at %llx (comparison %d with %llx, it holds %llx): given up (%ld so far)", b->q,
                             (unsigned long long)(now - b->since), (unsigned long long)it.wait.va, it.wait.cmp, (unsigned long long)it.wait.ref, (unsigned long long)v, k);
                g_wait_stats[3]++;
            }
            b->since = 0;
        } else if (it.kind == 1) {
            backlog_run(b, it.lists, it.writes);
            for (PcObj o : it.owned) { pc::list_forget(o); pc::release(o); }
            g_wait_stats[4]++;
        } else if (it.kind == 2) {
            pc::queue_signal(b->q, it.fence, it.value);
            pc::release(it.fence);
        } else {
            pc::queue_wait(b->q, it.fence, it.value);
            pc::release(it.fence);
        }
        b->items.pop_front();
    }
}
static DWORD WINAPI backlog_thread(void*) {
    // Half a millisecond between looks where the system has high-resolution timers (a plain sleep
    // of 1 ms can last a whole timer tick, 15.6 ms, and a list may hold a dozen waits).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -5000;
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
        else Sleep(1);
        std::vector<Backlog*> all;
        { std::lock_guard<std::mutex> g(g_backlogs_mu); all = g_backlogs; }
        for (Backlog* b : all) {
            std::unique_lock<std::mutex> g(b->mu, std::try_to_lock);
            if (g.owns_lock() && (!b->items.empty() || !b->landing.empty())) backlog_pump(b);
        }
    }
}
struct QueueExtra;
static Backlog* queue_backlog(ID3D12CommandQueue* q, bool create);
// The queue's backlog is given time to empty (before a present, which is not put behind it).
static void backlog_drain(ID3D12CommandQueue* q, ULONGLONG ms) {
    Backlog* b = queue_backlog(q, false);
    if (!b) return;
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        {
            std::lock_guard<std::mutex> g(b->mu);
            backlog_pump(b);
            if (b->items.empty()) return;
        }
        if (GetTickCount64() - t0 >= ms) return;
        Sleep(1);
    }
}

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
// The PC list of a title list, for a call that records into it.
template <class T> inline PcObj list_pc(T* l) { ex<XList, ListExtra>((XList*)l)->touched = true; return pc_of(l); }
#define LPC(l) list_pc(l)

HRESULT D3DAPI L_Close(ID3D12GraphicsCommandList* l) { XSL_TRACE("gfx.cmd", "list %p Close", (void*)l); return pc::list_close(LPC(l)); }
HRESULT D3DAPI L_Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p);
// ResetX: Reset with console flags (minimal state, no sync around clears/copies): the PC list needs none.
HRESULT D3DAPI L_ResetX(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p, D3D12XBOX_COMMAND_LIST_RESET_FLAGS) {
    return L_Reset(l, a, p);
}
HRESULT D3DAPI L_Reset(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* p) {
    lx(l)->topology = 0;
    lx(l)->ibValid = false;
    lx(l)->nTracked = 0;
    lx(l)->sampler = nullptr;
    if (p && !P(p)) pso_wait_ready(p);
    lx(l)->placeholderPso = p && !P(p);
    lx(l)->placeholderWhy = lx(l)->placeholderPso ? placeholder_reason(p) : -1;
    // Parts cut at waits and waits recorded since the last reset, never submitted: gone with the recording.
    if (ListWaits* w = lx(l)->w) {
        for (ListPart& part : w->earlier) { pc::list_forget(part.list); pc::release(part.list); mem_write_drop(part.writes); }
        w->earlier.clear();
        w->gate.clear();
        mem_write_drop(w->writes);
    }
    lx(l)->alloc = P(a);
    HRESULT hr = pc::list_reset(LPC(l), P(a), P(p));
    if (SUCCEEDED(hr)) list_bind_global_heap(l, LPC(l));
    lx(l)->touched = false;   // (nothing of the title's is in the list yet: a wait now stands before all of it)
    return hr;
}
}  // namespace
void list_parts_free(IGraphicsUnknown* self) {
    ListExtra* e = ex<XList, ListExtra>((XList*)self);
    if (!e->w) return;
    for (ListPart& part : e->w->earlier) { pc::list_forget(part.list); pc::release(part.list); mem_write_drop(part.writes); }
    mem_write_drop(e->w->writes);
    delete e->w;
    e->w = nullptr;
}
namespace {
// A wait for a memory value recorded into the list: the list is cut here (see "Waits for memory
// values"), unless nothing was recorded since the last cut or reset.
static void list_wait(ID3D12GraphicsCommandList* l, const ValueWait& w) {
    ListExtra* e = lx(l);
    static std::once_flag once;
    std::call_once(once, [] { pc::list_cuts_prepare(true); });
    if (!e->w) e->w = new ListWaits;
    const uint64_t n = ++g_wait_stats[0];
    if (n <= 4 || n % 20000 == 0)
        XSL_INFO(CH, "a wait for the value at %llx (comparison %d with %llx, %d bits) on list %p: %llu waits recorded so far, %llu lists cut, queues stood at %llu, %llu given up, %llu "
                     "parts run from backlogs",
                 (unsigned long long)w.va, w.cmp, (unsigned long long)w.ref, w.wide ? 64 : 32, (void*)l, (unsigned long long)n, (unsigned long long)g_wait_stats[1].load(),
                 (unsigned long long)g_wait_stats[2].load(), (unsigned long long)g_wait_stats[3].load(), (unsigned long long)g_wait_stats[4].load());
    if (!e->touched || !e->alloc) { e->w->gate.push_back(w); return; }
    PcObj to = nullptr;
    const HRESULT hr = pc::list_cut(dev_extra(hdr(l)->device)->pcDevice, pc_of(l), e->alloc, &to);
    if (FAILED(hr)) {   // no new PC list: the wait stands before the part it was recorded in
        static volatile long said = 0;
        if (InterlockedIncrement(&said) <= 3) XSL_WARN(CH, "a list cannot be cut at a wait (%08lx): the wait moves to the start of the part", hr);
        e->w->gate.push_back(w);
        return;
    }
    g_wait_stats[1]++;
    e->w->earlier.push_back(ListPart{std::move(e->w->gate), pc_of(l), std::move(e->w->writes)});
    e->w->gate.assign(1, w);
    e->w->writes.clear();
    hdr(l)->pc = to;
    e->touched = false;
}
// A value a list writes to memory: remembered when the CPU does not see the GPU's write there by
// itself (memory that is not shared with the GPU), to be written here once the part has run.
static void list_note_write(ID3D12GraphicsCommandList* l, uint64_t va, uint64_t value, bool wide) {
    if (!list_waits_on() || !va) return;
    UnifiedRef r;
    uint64_t run = 0;
    Device* dev = hdr(l)->device;
    if (unified_lookup(dev, va, &r) || section_lookup(dev, va, &r, &run)) return;
    ListExtra* e = lx(l);
    if (!e->w) e->w = new ListWaits;
    e->w->writes.push_back(MemWrite{va, value, wide});
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
                    x.Transition.pResource = (ID3D12Resource*)P_or_pc((ID3D12Resource*)res);   // PC resources come from Media Foundation
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
                    x.Aliasing.pResourceBefore = (ID3D12Resource*)P_or_pc(x.Aliasing.pResourceBefore);
                    x.Aliasing.pResourceAfter = (ID3D12Resource*)P_or_pc(x.Aliasing.pResourceAfter);
                    break;
                case D3D12_RESOURCE_BARRIER_TYPE_UAV:
                    x.UAV.pResource = (ID3D12Resource*)P_or_pc(x.UAV.pResource);
                    break;
                default:
                    XSL_WARN(CH, "dropping console-only barrier type %d", (int)x.Type);
                    continue;
            }
            if (x.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && (b[base + i].Transition.StateBefore == 0x200 || b[base + i].Transition.StateAfter == 0x200 ||
                                                                      ((b[base + i].Transition.StateBefore | b[base + i].Transition.StateAfter) & 0x200))) {
                static volatile long n = 0;
                if (InterlockedIncrement(&n) <= 40)
                    XSL_INFO(CH, "indirect-arg barrier list %p res %p %x -> %x flags %x", (void*)l, (void*)b[base + i].Transition.pResource,
                             b[base + i].Transition.StateBefore, b[base + i].Transition.StateAfter, b[base + i].Flags);
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
static bool draw_dump_now();
static std::string dump_skipped(ID3D12GraphicsCommandList* l);
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
    if (draw_dump_now()) {
        std::string t;
        for (UINT i = 0; i < n && i < 8; i++) t += " [" + pc::rtv_describe(r[i]) + "]";
        XSL_INFO(CH, "draw dump: list %p render targets (%u):%s%s", (void*)l, n, t.c_str(), dsv ? ", with depth" : "");
    }
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
static bool draw_dump_now();
void D3DAPI L_RSSetViewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* v) {
    if (draw_dump_now() && n && v) XSL_INFO(CH, "draw dump: list %p viewport %g,%g %gx%g depth %g..%g", (void*)l, v->TopLeftX, v->TopLeftY, v->Width, v->Height, v->MinDepth, v->MaxDepth);
    pc::list_viewports(LPC(l), n, v);
}
void D3DAPI L_RSSetScissorRects(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RECT* r) {
    if (draw_dump_now() && n && r) XSL_INFO(CH, "draw dump: list %p scissor %ld,%ld..%ld,%ld", (void*)l, r->left, r->top, r->right, r->bottom);
    pc::list_scissors(LPC(l), n, r);
}
// XSL_DRAW_DUMP=<present> (diagnostics): the draws of the frame before that present, with the first
// bytes of the vertex buffers they use.
static bool draw_dump_now() {
    // <present> or <first>-<last>
    static uint64_t last = 0;
    static uint64_t at = [] {
        char b[32];
        if (!GetEnvironmentVariableA("XSL_DRAW_DUMP", b, sizeof b)) return 0ull;
        char* e = nullptr;
        uint64_t f = _strtoui64(b, &e, 10);
        last = e && *e == '-' ? _strtoui64(e + 1, nullptr, 10) : f;
        return f;
    }();
    const uint64_t p = pc::present_count() + 1;
    return at && p >= at && p <= last;
}
// " (skipped: why)" when the list's pipeline is a placeholder (draw dump).
static std::string dump_skipped(ID3D12GraphicsCommandList* l) {
    if (!lx(l)->placeholderPso) return "";
    std::lock_guard<std::mutex> g(g_ph_mu);
    const int why = lx(l)->placeholderWhy;
    char obj[40];
    snprintf(obj, sizeof obj, ", pipeline object %p", lx(l)->psoObj);   // (its creation is logged with XSL_DRAW_DUMP set)
    return " (skipped: " + (why >= 0 && (size_t)why < g_ph_why.size() ? g_ph_why[why].first : std::string("pipeline not built yet or a placeholder")) + obj + ")";
}
// The first bytes at a title address, when the CPU can read them (draw dump).
static std::string dump_bytes(uint64_t a, size_t n = 48) {
    uint32_t w[16] = {};
    SIZE_T got = 0;
    if (n > sizeof w) n = sizeof w;
    if (!a || !ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)a, w, n, &got) || got != n) return " (not CPU-readable)";
    std::string t;
    for (size_t k = 0; k < n / 4; k++) { char b[16]; snprintf(b, sizeof b, " %08x", w[k]); t += b; }
    return t;
}
void D3DAPI L_SetGfxRootSig(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    XSL_TRACE("gfx.cmd", "list %p RootSig %p", (void*)l, (void*)rs);
    pc::list_set_gfx_root_signature(LPC(l), P(rs));
}
// XSL_RT_DEBUG=1: the compute bindings a list makes while a ray-tracing pipeline is selected.
static bool rt_bind_trace(ID3D12GraphicsCommandList* l) {
    static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_RT_DEBUG", b, 4) && b[0] == '1'; }();
    static volatile long n = 0;
    return on && lx(l)->rtPipeline && InterlockedIncrement(&n) <= 400;
}
void D3DAPI L_SetComputeRootSig(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    if (rt_bind_trace(l)) XSL_INFO(CH, "rt bind: list %p SetComputeRootSignature %p", (void*)l, (void*)rs);
    pc::list_set_compute_root_signature(LPC(l), P(rs));
}
void D3DAPI L_SetPipelineState(ID3D12GraphicsCommandList* l, ID3D12PipelineState* p) {
    if (null_graphics()) return;
    if (p && !P(p)) pso_wait_ready(p);
    lx(l)->placeholderPso = p && !P(p);
    lx(l)->placeholderWhy = lx(l)->placeholderPso ? placeholder_reason(p) : -1;
    lx(l)->psoObj = p;
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
    if (draw_dump_now() && v && n) {   // the first vertices, as the title wrote them
        std::string t;
        uint8_t b[96];
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)v[0].BufferLocation, b, sizeof b, &got) && got == sizeof b)
            for (size_t k = 0; k < sizeof b; k++) { char h[4]; snprintf(h, sizeof h, "%02x", b[k]); t += h; if (k % 4 == 3) t += ' '; }
        XSL_INFO(CH, "draw dump: list %p vertex buffer slot %u: %llx, %u bytes, stride %u: %s", (void*)l, s, (unsigned long long)v[0].BufferLocation, v[0].SizeInBytes,
                 v[0].StrideInBytes, t.empty() ? "(not CPU memory)" : t.c_str());
    }
    // The address may be the CPU address of unified memory.
    D3D12_VERTEX_BUFFER_VIEW x[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
    if (v && n <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
        for (UINT i = 0; i < n; i++) {
            x[i] = v[i];
            if (x[i].BufferLocation) x[i].BufferLocation = gpu_va(hdr(l)->device, x[i].BufferLocation);
        }
        v = x;
    }
    pc::list_ia_vbs(LPC(l), s, n, v);
}
void D3DAPI L_IASetIndexBuffer(ID3D12GraphicsCommandList* l, const D3D12_INDEX_BUFFER_VIEW* v) {
    lx(l)->ibValid = v != nullptr;
    if (v) {
        lx(l)->ib = *v;
        if (v->BufferLocation) lx(l)->ib.BufferLocation = gpu_va(hdr(l)->device, v->BufferLocation);   // (unified memory)
        v = &lx(l)->ib;
    }
    XSL_TRACE("gfx.cmd", "list %p IB va=%llx size=%u fmt=%d", (void*)l, v ? (unsigned long long)v->BufferLocation : 0ull, v ? v->SizeInBytes : 0, v ? v->Format : 0);
    pc::list_ia_ib(LPC(l), v);
}
void D3DAPI L_DrawInstanced(ID3D12GraphicsCommandList* l, UINT a, UINT b, UINT c, UINT d) {
    if (null_graphics()) return;
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p Draw(%u vertices, %u instances, first %u), topology %d, pipeline %s%s", (void*)l, a, b, c, lx(l)->topology,
                                  pc::list_pso_name(LPC(l)).c_str(), dump_skipped(l).c_str());
    if (lx(l)->placeholderPso) { placeholder_skip(lx(l)->placeholderWhy); return; }
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
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p DrawIndexed(%u indices, %u instances, first %u, base %d), topology %d, pipeline %s%s", (void*)l, a, b, c, d, lx(l)->topology,
                                  pc::list_pso_name(LPC(l)).c_str(), dump_skipped(l).c_str());
    if (lx(l)->placeholderPso) { placeholder_skip(lx(l)->placeholderWhy); return; }
    XSL_TRACE("gfx.cmd", "list %p DrawIndexed %u x%u", (void*)l, a, b);
    pc::list_draw_indexed(LPC(l), a, b, c, d, e);
}
static void rt_peek_tables(ID3D12GraphicsCommandList* l, const char* what) {
    std::string s;
    for (int k : {0, 1, 3}) {
        uint32_t w[8] = {};
        char b[160];
        bool ok = desc_peek(lx(l)->csTables[k], 0, w);
        snprintf(b, sizeof b, " table %d index %llu%s: %x %x %x %x tag %x %x;", k, (unsigned long long)((lx(l)->csTables[k] - desc_view_gpu_start(0)) / 32), ok ? "" : " (not view memory)",
                 w[0], w[1], w[2], w[3], w[4], w[5]);
        s += b;
    }
    XSL_INFO(CH, "rt bind: list %p %s:%s", (void*)l, what, s.c_str());
}
void D3DAPI L_Dispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
    if (rt_bind_trace(l)) rt_peek_tables(l, "Dispatch");
    static bool diag = [] { char b[4]; return GetEnvironmentVariableA("XSL_D3D12_DEBUG", b, 4) && b[0] != '0'; }();
    if (diag && (uint64_t)x * y * z > (1u << 18)) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 20) XSL_INFO(CH, "large Dispatch(%u, %u, %u) on list %p", x, y, z, (void*)l);
    }
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p Dispatch(%u, %u, %u), pipeline %s%s", (void*)l, x, y, z, pc::list_pso_name(LPC(l)).c_str(), dump_skipped(l).c_str());
    if (lx(l)->placeholderPso) { placeholder_skip(lx(l)->placeholderWhy); return; }
    if (!null_graphics()) pc::list_dispatch(LPC(l), x, y, z);
}
// DispatchX: a dispatch with console options. Only one changes what runs: with
// USE_THREAD_DIMENSIONS the counts are threads, not groups, in each dimension, and the console
// launches no thread past them. A PC dispatch is whole groups: the counts are divided by the
// pipeline's thread-group size, rounded up.
void D3DAPI L_DispatchX(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z, D3D12XBOX_DISPATCH_FLAGS flags) {
    {   // each pipeline's first few dispatches of this kind, with what was asked for (60 lines at most)
        static std::mutex mu;
        static std::map<std::string, int> seen;
        static int lines = 0;
        const std::string pso = pc::list_pso_name(LPC(l));
        std::lock_guard<std::mutex> g(mu);
        if (lines < 60 && seen[pso]++ < 3) {
            lines++;
            HMODULE m = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)__builtin_return_address(0), &m) && m)
                GetModuleFileNameA(m, name, MAX_PATH);
            const char* bn = strrchr(name, '\\');
            XSL_INFO(CH, "DispatchX(%u, %u, %u, flags %#x%s) from %s, pipeline %s%s", x, y, z, (unsigned)flags,
                     (flags & D3D12XBOX_DISPATCH_FLAG_USE_THREAD_DIMENSIONS) ? ": the counts are threads" : "", bn ? bn + 1 : name, pso.c_str(), dump_skipped(l).c_str());
        }
    }
    if (flags & D3D12XBOX_DISPATCH_FLAG_USE_THREAD_DIMENSIONS) {
        // The counts are threads: the groups that hold them are dispatched. The threads of the
        // last group past the count run too (the console launches none of them); the programs
        // seen with this flag compare their thread number with a count of their own first.
        uint32_t tg[3];
        if (pso_thread_group(lx(l)->psoObj, tg)) {
            x = (x + tg[0] - 1) / tg[0];
            y = (y + tg[1] - 1) / tg[1];
            z = (z + tg[2] - 1) / tg[2];
        } else {
            static volatile long w = 0;
            if (InterlockedIncrement(&w) == 1) XSL_WARN(CH, "DispatchX with thread counts (%u, %u, %u) on a pipeline whose thread-group size is not known: dispatched as groups", x, y, z);
        }
    }
    L_Dispatch(l, x, y, z);
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
    if (draw_dump_now()) {   // the table's first slots as they lie in descriptor memory
        std::string t;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t w[8] = {};
            if (!desc_peek(h.ptr, k, w)) { t += " (not view memory)"; break; }
            char b[96];
            snprintf(b, sizeof b, " | %08x %08x %08x %08x %08x %08x %08x %08x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            t += b;
            const uint64_t at = w[0] | (uint64_t)w[1] << 32;   // a buffer view starts with its address
            if (at > 0x10000) t += " [" + dump_bytes(at, 32) + " ]";
        }
        XSL_INFO(CH, "draw dump: list %p root table %u at view slot %llu:%s", (void*)l, i, (unsigned long long)((h.ptr - desc_view_gpu_start(0)) / 32), t.c_str());
    }
    pc::list_gfx_root_table(LPC(l), i, desc_pc_gpu(h.ptr));
}
void D3DAPI L_SetComputeRootTable(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (rt_bind_trace(l)) {
        uint32_t w[8] = {};
        bool ok = desc_peek(h.ptr, 0, w);
        XSL_INFO(CH, "rt bind: list %p SetComputeRootDescriptorTable(%u, %llx)%s index %llu slot 0: %x %x %x %x tag %x %x", (void*)l, i, (unsigned long long)h.ptr,
                 ok ? "" : " (not view memory)", (unsigned long long)((h.ptr - desc_view_gpu_start(0)) / 32), w[0], w[1], w[2], w[3], w[4], w[5]);
    }
    if (i < 16) lx(l)->csTables[i] = h.ptr;
    pc::list_compute_root_table(LPC(l), i, desc_pc_gpu(h.ptr));
}
void D3DAPI L_SetPipelineState1(ID3D12GraphicsCommandList4* l4, ID3D12StateObject* so) {
    lx((ID3D12GraphicsCommandList*)l4)->rtPipeline = so;
    if (rt_bind_trace((ID3D12GraphicsCommandList*)l4)) XSL_INFO(CH, "rt bind: list %p SetPipelineState1 %p", (void*)l4, (void*)so);
}
void D3DAPI L_DispatchRays(ID3D12GraphicsCommandList4* l4, const D3D12_DISPATCH_RAYS_DESC* d) {
    auto* l = (ID3D12GraphicsCommandList*)l4;
    if (null_graphics()) return;
    rt_dispatch(LPC(l), lx(l)->rtPipeline, lx(l)->csTables, d, 0, lx(l)->sampler);
}
void D3DAPI L_DispatchRaysX(ID3D12GraphicsCommandList6* l6, const D3D12XBOX_DISPATCH_RAYS_DESC* d) {
    if (d) L_DispatchRays((ID3D12GraphicsCommandList4*)l6, &d->DispatchArgs);
}
void D3DAPI L_SetGfxRootCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    XSL_TRACE("gfx.cmd", "list %p RootCBV %u %llx", (void*)l, i, (unsigned long long)a);
    pc::list_gfx_root_cbv(LPC(l), i, cbv_fix_root(hdr(l)->device, a));
}
void D3DAPI L_SetComputeRootCbv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    if (rt_bind_trace(l)) XSL_INFO(CH, "rt bind: list %p SetComputeRootConstantBufferView(%u, %llx)", (void*)l, i, (unsigned long long)a);
    pc::list_compute_root_cbv(LPC(l), i, cbv_fix_root(hdr(l)->device, a));
}
// Root SRVs/UAVs may address GPU-visible title memory (XMem graphics allocations).
void D3DAPI L_SetGfxRootSrv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) {
    const uint64_t t = gpu_va(hdr(l)->device, a);
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p root SRV %u at %llx (PC %llx):%s", (void*)l, i, (unsigned long long)a, (unsigned long long)t, dump_bytes(a).c_str());
    pc::list_gfx_root_srv(LPC(l), i, t);
}
void D3DAPI L_SetComputeRootSrv(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_compute_root_srv(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetGfxRootUav(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_gfx_root_uav(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetComputeRootUav(ID3D12GraphicsCommandList* l, UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) { pc::list_compute_root_uav(LPC(l), i, gpu_va(hdr(l)->device, a)); }
void D3DAPI L_SetGfxRoot32(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT o) {
    if (draw_dump_now() && d && n) XSL_INFO(CH, "draw dump: list %p root constants %u (+%u, %u): %08x %08x", (void*)l, i, o, n, ((const uint32_t*)d)[0], n > 1 ? ((const uint32_t*)d)[1] : 0);
    pc::list_gfx_root_constants(LPC(l), i, n, d, o);
}
void D3DAPI L_SetGfxRoot32One(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT o) { pc::list_gfx_root_constants(LPC(l), i, 1, &v, o); }
void D3DAPI L_SetComputeRoot32(ID3D12GraphicsCommandList* l, UINT i, UINT n, const void* d, UINT o) { pc::list_compute_root_constants(LPC(l), i, n, d, o); }
void D3DAPI L_SetComputeRoot32One(ID3D12GraphicsCommandList* l, UINT i, UINT v, UINT o) { pc::list_compute_root_constants(LPC(l), i, 1, &v, o); }
// Diagnostics: a copy on the GPU into a buffer the title placed on its own memory. The buffer has
// a PC buffer of its own there, so what is copied does not reach the memory the title's CPU code
// reads (a title reads small render targets back this way). Counted, and the first ones logged.
static void note_copy_to_placed(ID3D12Resource* d, const char* from, UINT64 off, UINT64 bytes, int format) {
    if (!d || !is_ours(d)) return;
    const uint64_t va = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)d)->titleVa;
    if (!va) return;
    static volatile long n = 0;
    const long k = InterlockedIncrement(&n);
    if (k <= 12 || k % 5000 == 0)
        XSL_INFO(CH, "a copy from a %s into a buffer placed on title memory (at %llx, offset %llu, %llu bytes, format %d): not brought back to that memory (%ld such copies so far)", from,
                 (unsigned long long)va, (unsigned long long)off, (unsigned long long)bytes, format, k);
}
// A texture-to-buffer copy into a buffer the title placed on its own memory: a read-back. The
// buffer has a PC buffer of its own, so the title's CPU code would read memory nothing wrote (a
// title meters its exposure from two tiny float targets it reads back every frame). The texels
// are also copied into a buffer of the layer's own, and stored in the title's memory once the
// list has run. False: not such a copy, or one this does not take (a part of a footprint not at
// its origin, a format whose texel size is not known, more than 64 MB).
// XSL_READ_BACK=0: none are made.
static bool list_read_back(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                           const D3D12_BOX* box) {
    static const bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_READ_BACK", b, 4) && b[0] == '0'); }();
    if (!on || !list_waits_on() || !d->pResource || !is_ours(d->pResource) || src->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) return false;
    const uint64_t va = ex<ID3D12Resource2, ResExtra>((ID3D12Resource2*)d->pResource)->titleVa;
    const D3D12_SUBRESOURCE_FOOTPRINT& f = d->PlacedFootprint.Footprint;
    const uint32_t texel = pc::format_texel_bytes((int)f.Format);
    if (!va || x || y || z || !texel || f.Depth > 1 || !f.RowPitch) return false;
    const uint32_t width = box ? box->right - box->left : f.Width, rows = box ? box->bottom - box->top : f.Height;
    const uint64_t bytes = (uint64_t)f.RowPitch * rows;
    if (!rows || !width || (uint64_t)width * texel > f.RowPitch || bytes > (64ull << 20)) return false;
    PcObj staging = nullptr;
    if (FAILED(pc::create_readback(dev_extra(hdr(l)->device)->pcDevice, bytes + 256, &staging)) || !staging) return false;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = (ID3D12Resource*)staging;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = f;
    to.PlacedFootprint.Footprint.Height = rows;
    to.PlacedFootprint.Footprint.Width = width;
    pc::list_copy_texture_region(LPC(l), &to, 0, 0, 0, src, box);
    ListExtra* e = lx(l);
    if (!e->w) e->w = new ListWaits;
    MemWrite w{va + d->PlacedFootprint.Offset, 0, false};
    w.staging = staging;
    w.rows = rows;
    w.rowBytes = width * texel;
    w.pitch = f.RowPitch;
    e->w->writes.push_back(w);
    static volatile long n = 0;
    if (const long k = InterlockedIncrement(&n); k <= 8 || k % 20000 == 0)
        XSL_INFO(CH, "read-back: %ux%u texels of format %d copied into a buffer placed on title memory go to that memory (%llx) once the list has run (%ld so far)", width, rows,
                 (int)f.Format, (unsigned long long)w.va, k);
    return true;
}
void D3DAPI L_CopyBufferRegion(ID3D12GraphicsCommandList* l, ID3D12Resource* d, UINT64 doff, ID3D12Resource* s, UINT64 soff, UINT64 n) {
    note_copy_to_placed(d, "buffer", doff, n, 0);
    pc::list_copy_buffer_region(LPC(l), P(d), doff, P(s), soff, n);
}
void D3DAPI L_CopyTextureRegion(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* d, UINT x, UINT y, UINT z,
                                const D3D12_TEXTURE_COPY_LOCATION* s, const D3D12_BOX* box) {
    D3D12_TEXTURE_COPY_LOCATION dd = *d, ss = *s;
    dd.pResource = (ID3D12Resource*)P_or_pc(d->pResource);   // decoded video frames are PC resources
    ss.pResource = (ID3D12Resource*)P_or_pc(s->pResource);
    if (!is_ours(s->pResource) || !is_ours(d->pResource) || lx(l)->type == D3D12_COMMAND_LIST_TYPE_COPY) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 8) {
            D3D12_RESOURCE_DESC sd{}, dsd{};
            pc::res_desc((PcObj)ss.pResource, &sd);
            pc::res_desc((PcObj)dd.pResource, &dsd);
            XSL_INFO(CH, "copy (list type %d): src %s %llux%u x%u fmt %d dim %d (type %d sub %u) -> dst %s %llux%u fmt %d dim %d (type %d sub %u, footprint fmt %d %ux%u pitch %u off %llu) at %u,%u box %d",
                     (int)lx(l)->type, is_ours(s->pResource) ? "console" : "PC", (unsigned long long)sd.Width, sd.Height, sd.DepthOrArraySize, (int)sd.Format, (int)sd.Dimension, (int)s->Type,
                     s->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? s->SubresourceIndex : 0, is_ours(d->pResource) ? "console" : "PC", (unsigned long long)dsd.Width,
                     dsd.Height, (int)dsd.Format, (int)dsd.Dimension, (int)d->Type, d->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? d->SubresourceIndex : 0,
                     d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? (int)d->PlacedFootprint.Footprint.Format : -1,
                     d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d->PlacedFootprint.Footprint.Width : 0,
                     d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d->PlacedFootprint.Footprint.Height : 0,
                     d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? d->PlacedFootprint.Footprint.RowPitch : 0,
                     d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ? (unsigned long long)d->PlacedFootprint.Offset : 0ull, x, y, box ? 1 : 0);
        }
    }
    if (!is_ours(s->pResource)) {   // copies out of decoder frames: each distinct kind once
        static std::mutex mu;
        static std::set<uint64_t> seen;
        D3D12_RESOURCE_DESC dsd{};
        pc::res_desc((PcObj)dd.pResource, &dsd);
        uint64_t k = (uint64_t)(s->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? s->SubresourceIndex : 999) << 32 | (uint64_t)dsd.Format << 16 | (uint64_t)d->Type;
        std::lock_guard<std::mutex> g(mu);
        if (seen.insert(k).second)
            XSL_INFO(CH, "decoder copy: src sub %u -> dst fmt %d %llux%u (type %d sub %u) box %s%u,%u-%u,%u", (unsigned)(k >> 32), (int)dsd.Format, (unsigned long long)dsd.Width, dsd.Height,
                     (int)d->Type, d->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? d->SubresourceIndex : 0, box ? "" : "none ", box ? box->left : 0, box ? box->top : 0,
                     box ? box->right : 0, box ? box->bottom : 0);
    }
    if (!is_ours(s->pResource)) {
        std::lock_guard<std::mutex> g(g_alias_mu);
        g_list_frame[l] = (PcObj)ss.pResource;
    } else {
        std::lock_guard<std::mutex> g(g_alias_mu);
        auto a = g_plane_alias.find((PcObj)ss.pResource);
        if (a != g_plane_alias.end()) {
            auto f = g_list_frame.find(l);
            if (f == g_list_frame.end()) return;   // no frame yet: nothing to read
            ss.pResource = (ID3D12Resource*)f->second;
            ss.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            ss.SubresourceIndex = a->second ? 1u : 0u;   // NV12/P010: plane 1 is subresource 1 (one mip, one slice)
        }
    }
    if (!is_ours(s->pResource) && getenv("XSL_SKIP_VIDEO_COPIES")) return;   // experiment: copies out of decoder textures
    if (d->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && !list_read_back(l, d, x, y, z, &ss, box))
        note_copy_to_placed(d->pResource, "texture", d->PlacedFootprint.Offset, (UINT64)d->PlacedFootprint.Footprint.RowPitch * d->PlacedFootprint.Footprint.Height,
                            (int)d->PlacedFootprint.Footprint.Format);
    pc::list_copy_texture_region(LPC(l), &dd, x, y, z, &ss, box);
}
void D3DAPI L_CopyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* d, ID3D12Resource* s) { pc::list_copy_resource(LPC(l), P_or_pc(d), P_or_pc(s)); }
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
    list_note_write(l, va, v, true);
}
void D3DAPI L_Write32BOP(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, UINT v, D3D12XBOX_FLUSH, D3D12XBOX_WRITE_VALUE_BOP_FLAGS) {
    pc::list_write_immediate(LPC(l), gpu_va(hdr(l)->device, va), v, true);
    list_note_write(l, va, v, false);
}
// Top-of-pipe value writes: stored at this point of the list's timeline, low dword then high.
void D3DAPI L_Write64TOP(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, UINT64 v, D3D12XBOX_WRITE_VALUE_TOP_FLAGS flags) {
    Device* dev = hdr(l)->device;
    if (flags & (D3D12XBOX_WRITE_VALUE_TOP_FLAG_GPU_TIMESTAMP | D3D12XBOX_WRITE_VALUE_TOP_FLAG_SYSTEM_TIMESTAMP)) {
        // A timestamp instead of the given value: resolve a timestamp query into the destination.
        UnifiedRef r;
        if (unified_lookup(dev, va, &r)) pc::list_write_timestamp(dev_extra(dev)->pcDevice, LPC(l), r.res, r.offset);
        else XSL_WARN(CH, "timestamp write to %llx: not in GPU-visible title memory", (unsigned long long)va);
        return;
    }
    uint64_t g = gpu_va(dev, va);
    const uint64_t a[2] = {g, g + 4};
    const uint32_t vals[2] = {(uint32_t)v, (uint32_t)(v >> 32)};
    pc::list_write_immediate(LPC(l), 2, a, vals);
    list_note_write(l, va, v, true);
}
void D3DAPI L_Write32TOP(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, UINT v, D3D12XBOX_WRITE_VALUE_TOP_FLAGS) {
    const uint64_t a = gpu_va(hdr(l)->device, va);
    const uint32_t val = v;
    pc::list_write_immediate(LPC(l), 1, &a, &val);
    list_note_write(l, va, v, false);
}
// The console GPU can stand at a point of its command stream until a memory value passes a
// comparison: the list is cut there (list_wait). With XSL_LIST_WAITS=0 the wait is skipped.
void D3DAPI L_Wait64(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, D3D12_COMPARISON_FUNC f, UINT64 ref, D3D12XBOX_WAIT_FLAGS flags) {
    if (list_waits_on()) { list_wait(l, ValueWait{va, ref, ~0ull, (int)f, true}); return; }
    static volatile long n = 0;
    if (InterlockedIncrement(&n) == 1)
        XSL_INFO(CH, "Wait64BitValueX on list %p skipped (XSL_LIST_WAITS=0): address %llx, comparison %d, reference %llx, flags %#x", (void*)l,
                 (unsigned long long)va, (int)f, (unsigned long long)ref, (unsigned)flags);
}
void D3DAPI L_Wait32(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS va, D3D12_COMPARISON_FUNC f, UINT ref, UINT mask, D3D12XBOX_WAIT_FLAGS flags) {
    if (list_waits_on()) { list_wait(l, ValueWait{va, ref, mask, (int)f, false}); return; }
    static volatile long n = 0;
    if (InterlockedIncrement(&n) == 1)
        XSL_INFO(CH, "Wait32BitValueX on list %p skipped (XSL_LIST_WAITS=0): address %llx, comparison %d, reference %x, mask %x, flags %#x", (void*)l,
                 (unsigned long long)va, (int)f, ref, mask, (unsigned)flags);
}
void D3DAPI L_FlushPipelineX(ID3D12GraphicsCommandList* l, D3D12XBOX_FLUSH, D3D12_GPU_VIRTUAL_ADDRESS, SIZE_T) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // global UAV barrier: the closest PC flush
    pc::list_barriers(LPC(l), 1, &b);
}
void D3DAPI L_ExecuteIndirect(ID3D12GraphicsCommandList* l, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args, UINT64 ao,
                              ID3D12Resource* cnt, UINT64 co) {
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p ExecuteIndirect(up to %u), pipeline %s%s", (void*)l, max, pc::list_pso_name(LPC(l)).c_str(), dump_skipped(l).c_str());
    if (lx(l)->placeholderPso) placeholder_skip(lx(l)->placeholderWhy);
    if (null_graphics() || lx(l)->placeholderPso) return;   // placeholder PSO: its work is skipped
    if (sig && args && pc::is_rays_signature(P(sig)) && rt_bind_trace(l)) rt_peek_tables(l, "ExecuteIndirect (rays)");
    if (sig && args && pc::is_rays_signature(P(sig))) {   // indirect rays on a translated pipeline (count buffer ignored)
        uint64_t va = pc::res_gpu_va(P(args));
        {   // diagnostics: the indirect arguments as the CPU sees them now (dimensions may be copied in later on the GPU)
            static volatile long shown = 0;
            if (va && InterlockedIncrement(&shown) <= 6) {
                const uint8_t* m = (const uint8_t*)pc::buffer_try_map(P(args));
                if (m) {
                    D3D12_DISPATCH_RAYS_DESC d;
                    memcpy(&d, m + ao, sizeof d);
                    uint64_t probe[4] = {d.RayGenerationShaderRecord.StartAddress, d.MissShaderTable.StartAddress, d.HitGroupTable.StartAddress, d.CallableShaderTable.StartAddress};
                    char res[4][24];
                    for (int k = 0; k < 4; k++) {
                        PcObj r; uint64_t off, avail;
                        if (probe[k] && pc::gm_resolve(probe[k], &r, &off, &avail)) snprintf(res[k], sizeof res[k], "ok +%llx", (unsigned long long)off);
                        else snprintf(res[k], sizeof res[k], probe[k] ? "NOT in memory" : "-");
                    }
                    XSL_INFO(CH, "indirect rays args (buffer VA %llx+%llu): raygen %llx/%llu (%s) miss %llx/%llu stride %llu (%s) hit %llx/%llu stride %llu (%s) callable %llx (%s) %ux%ux%u",
                             (unsigned long long)va, (unsigned long long)ao, (unsigned long long)probe[0], (unsigned long long)d.RayGenerationShaderRecord.SizeInBytes, res[0],
                             (unsigned long long)probe[1], (unsigned long long)d.MissShaderTable.SizeInBytes, (unsigned long long)d.MissShaderTable.StrideInBytes, res[1],
                             (unsigned long long)probe[2], (unsigned long long)d.HitGroupTable.SizeInBytes, (unsigned long long)d.HitGroupTable.StrideInBytes, res[2],
                             (unsigned long long)probe[3], res[3], d.Width, d.Height, d.Depth);
                } else XSL_INFO(CH, "indirect rays args (buffer VA %llx+%llu): not CPU-readable", (unsigned long long)va, (unsigned long long)ao);
            }
        }
        if (va && lx(l)->rtPipeline) {
            bool ran = false;
            for (UINT k = 0; k < (max ? std::min(max, 1u) : 0u); k++) ran |= rt_dispatch(LPC(l), lx(l)->rtPipeline, lx(l)->csTables, nullptr, va + ao, lx(l)->sampler);
            if (ran || max) return;   // run, or skipped and counted with its reason
        }
    }
    static bool diag = [] { char b[4]; return GetEnvironmentVariableA("XSL_D3D12_DEBUG", b, 4) && b[0] != '0'; }();
    if (diag) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 400) {
            uint32_t* st = tracked_state(lx(l), args, false);
            XSL_INFO(CH, "ExecuteIndirect list %p sig %p max %u args %p+%llu (state in this list: %s%x) count %p", (void*)l, (void*)sig, max, (void*)args,
                     (unsigned long long)ao, st ? "" : "untracked ", st ? *st : 0u, (void*)cnt);
        }
    }
    // Console titles may run ExecuteIndirect on arguments a compute pass just wrote without moving
    // them to INDIRECT_ARGUMENT in the same list (the PC debug layer finds them in UNORDERED_ACCESS).
    // On PC the indirect fetch may then read stale data (garbage group counts hang the GPU), so a
    // buffer this list has not made indirect could be transitioned around the call
    // (XSL_EI_TRANSITION=1; experimental: the state assumed for buffers this list never transitioned
    // can be wrong).
    static bool fix = [] { char b[4]; return GetEnvironmentVariableA("XSL_EI_TRANSITION", b, 4) && b[0] == '1'; }();
    D3D12_RESOURCE_BARRIER bar[2];
    UINT nb = 0;
    if (fix) {
        ID3D12Resource* rs[2] = {args, cnt};
        for (ID3D12Resource* r : rs) {
            if (!r) continue;
            uint32_t* st = tracked_state(lx(l), r, false);
            uint32_t before = st && *st != UINT32_MAX ? *st : (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (before & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) continue;
            if (before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) continue;   // a read state set by the title: leave it
            if (nb && bar[0].Transition.pResource == (ID3D12Resource*)P(r)) continue;
            D3D12_RESOURCE_BARRIER& x = bar[nb++];
            x = {};
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition.pResource = (ID3D12Resource*)P(r);
            x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            x.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            x.Transition.StateAfter = pc::dred_on() ? D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_SOURCE
                                                    : D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        }
        if (nb) pc::list_barriers(LPC(l), nb, bar);
        if (nb && bar[0].Transition.pResource == (ID3D12Resource*)P(args) && pc::dred_on())
            pc::list_record_indirect(LPC(l), P(args), ao, nb == 2 ? P(cnt) : nullptr, co);
    }
    if (!nb && pc::dred_on()) {
        {
            // Arguments already in a read state the title chose: add COPY_SOURCE around the record.
            uint32_t* st = tracked_state(lx(l), args, false);
            uint32_t assumed = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;   // untracked: where a correct title left it
            if (!st || *st == UINT32_MAX) st = &assumed;
            if ((*st & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) && !(*st & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
                D3D12_RESOURCE_BARRIER x{};
                x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                x.Transition.pResource = (ID3D12Resource*)P(args);
                x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                x.Transition.StateBefore = (D3D12_RESOURCE_STATES)*st;
                x.Transition.StateAfter = (D3D12_RESOURCE_STATES)(*st | D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (x.Transition.StateBefore != x.Transition.StateAfter) pc::list_barriers(LPC(l), 1, &x);
                pc::list_record_indirect(LPC(l), P(args), ao, nullptr, 0);
                std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
                if (x.Transition.StateBefore != x.Transition.StateAfter) pc::list_barriers(LPC(l), 1, &x);
            }
        }
    }
    pc::list_execute_indirect(LPC(l), P(sig), max, P(args), ao, P(cnt), co);
    if (nb) {
        for (UINT i = 0; i < nb; i++) std::swap(bar[i].Transition.StateBefore, bar[i].Transition.StateAfter);
        pc::list_barriers(LPC(l), nb, bar);
    }
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
// ---- ray-tracing acceleration structures (built by the PC driver; same structure layouts) ----
void rt_inputs_impl(Device* dev, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* out, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* geo) {
    *out = *in;
    out->Flags = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)(in->Flags & 0x3f);   // console flags (DEHYDRATE) dropped
    auto va = [&](uint64_t a) { return dev && a ? gpu_va(dev, a) : a; };
    if (in->Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        out->InstanceDescs = va(in->InstanceDescs);   // instance contents (BLAS addresses) are GPU data, used as written
        return;
    }
    geo->resize(in->NumDescs);
    for (UINT i = 0; i < in->NumDescs; i++) {
        D3D12_RAYTRACING_GEOMETRY_DESC g = in->DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? in->pGeometryDescs[i] : *in->ppGeometryDescs[i];
        if (g.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
            g.Triangles.Transform3x4 = va(g.Triangles.Transform3x4);
            g.Triangles.IndexBuffer = va(g.Triangles.IndexBuffer);
            g.Triangles.VertexBuffer.StartAddress = va(g.Triangles.VertexBuffer.StartAddress);
        } else {
            g.AABBs.AABBs.StartAddress = va(g.AABBs.AABBs.StartAddress);
        }
        (*geo)[i] = g;
    }
    out->DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    out->pGeometryDescs = geo->data();
}
namespace {
// Postbuild queries the PC knows (the console's offline statistics have no PC equivalent).
bool pc_postbuild(Device* dev, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& d, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* out) {
    if ((int)d.InfoType > 3) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 2) XSL_WARN(CH, "acceleration structure postbuild info type %#x is console-only; skipped", (unsigned)d.InfoType);
        return false;
    }
    *out = d;
    out->DestBuffer = gpu_va(dev, d.DestBuffer);
    return true;
}
}  // namespace
// Acceleration structures stay inert by default: titles ship serialized console structures (copied
// in with DESERIALIZE) that a PC driver cannot read, and the TLAS references them. Builds and
// copies then do nothing and postbuild queries report plausible sizes, until ray tracing is
// translated. XSL_RT_BUILD=1 forwards builds to the PC driver.
bool rt_build_on() {
    static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_RT_BUILD", b, 4) && b[0] == '1'; }();
    return on;
}
void rt_fake_postbuild(ID3D12GraphicsCommandList* l, Device* dev, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC& d, UINT n) {
    // Every info type starts with UINT64 sizes (serialization: size, then BLAS pointer count).
    uint32_t stride = d.InfoType == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION ? 16 : 8;
    if ((int)d.InfoType > 3) return;
    std::vector<uint64_t> a;
    std::vector<uint32_t> v;
    uint64_t base = gpu_va(dev, d.DestBuffer);
    for (UINT i = 0; i < n; i++) {
        uint64_t e = base + (uint64_t)i * stride;
        a.push_back(e); v.push_back(64 * 1024);   // size: 64 KB
        a.push_back(e + 4); v.push_back(0);
        if (stride == 16) { a.push_back(e + 8); v.push_back(0); a.push_back(e + 12); v.push_back(0); }
    }
    pc::list_write_immediate(LPC(l), (uint32_t)a.size(), a.data(), v.data());
}
void D3DAPI L_BuildRtAs(ID3D12GraphicsCommandList4* l4, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT np,
                        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* post) {
    auto* l = (ID3D12GraphicsCommandList*)l4;
    if (null_graphics() || !d) return;
    {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 40)
            XSL_INFO(CH, "AS build: type %d flags %#x, %u descs (geometry type %d), dest %llx, source %llx, scratch %llx", (int)d->Inputs.Type,
                     (unsigned)d->Inputs.Flags, d->Inputs.NumDescs,
                     d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL && d->Inputs.NumDescs &&
                             d->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY ? (int)d->Inputs.pGeometryDescs[0].Type : -1,
                     (unsigned long long)d->DestAccelerationStructureData, (unsigned long long)d->SourceAccelerationStructureData,
                     (unsigned long long)d->ScratchAccelerationStructureData);
    }
    if (!rt_build_on()) {
        if (d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)   // translated ray queries read the console layout
            pc::list_rt_build_tlas(LPC(l), d->DestAccelerationStructureData, d->Inputs.InstanceDescs, d->Inputs.NumDescs,
                                   d->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS ? 1u : 0u, d->ScratchAccelerationStructureData);
        else if (d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            std::vector<pc::BlasGeometry> g;
            Device* dv = hdr(l)->device;
            for (UINT i = 0; i < d->Inputs.NumDescs; i++) {
                const D3D12_RAYTRACING_GEOMETRY_DESC* gd =
                    d->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS ? d->Inputs.ppGeometryDescs[i] : &d->Inputs.pGeometryDescs[i];
                pc::BlasGeometry b{};
                if (gd && gd->Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                    const auto& t = gd->Triangles;
                    b.vb = gpu_va(dv, t.VertexBuffer.StartAddress);
                    b.vbStride = (uint32_t)t.VertexBuffer.StrideInBytes;
                    b.vbFormat = (uint32_t)t.VertexFormat;
                    b.ib = t.IndexBuffer ? gpu_va(dv, t.IndexBuffer) : 0;
                    b.ibFormat = (uint32_t)t.IndexFormat;
                    b.transform = t.Transform3x4 ? gpu_va(dv, t.Transform3x4) : 0;
                    b.triangles = (t.IndexFormat != DXGI_FORMAT_UNKNOWN && t.IndexBuffer ? t.IndexCount : t.VertexCount) / 3;
                    b.flags = (uint32_t)gd->Flags;
                }
                g.push_back(b);   // procedural geometry: no triangles, keeps the geometry index
            }
            pc::list_rt_build_blas(LPC(l), d->DestAccelerationStructureData, d->ScratchAccelerationStructureData, (uint32_t)g.size(), g.data());
        }
        for (UINT i = 0; i < np; i++) rt_fake_postbuild(l, hdr(l)->device, post[i], 1);
        return;
    }
    Device* dev = hdr(l)->device;
    {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 20)
            XSL_INFO(CH, "BuildRaytracingAccelerationStructure type %d flags %#x, %u descs, %u postbuild", (int)d->Inputs.Type, (unsigned)d->Inputs.Flags,
                     d->Inputs.NumDescs, np);
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC x = *d;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geo;
    rt_inputs_impl(dev, &d->Inputs, &x.Inputs, &geo);
    x.DestAccelerationStructureData = gpu_va(dev, d->DestAccelerationStructureData);
    x.SourceAccelerationStructureData = d->SourceAccelerationStructureData ? gpu_va(dev, d->SourceAccelerationStructureData) : 0;
    x.ScratchAccelerationStructureData = gpu_va(dev, d->ScratchAccelerationStructureData);
    std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC> p;
    for (UINT i = 0; i < np; i++) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC t;
        if (pc_postbuild(dev, post[i], &t)) p.push_back(t);
    }
    pc::list_rt_build(LPC(l), &x, (uint32_t)p.size(), p.empty() ? nullptr : p.data());
}
void D3DAPI L_BuildRtAsBatched2X(ID3D12GraphicsCommandList6* l6, UINT n, const D3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2* d) {
    for (UINT i = 0; d && i < n; i++)   // the console stream options (ExtraOptions) have no PC meaning
        L_BuildRtAs((ID3D12GraphicsCommandList4*)l6, &d[i].Desc, d[i].NumPostbuildInfoDescs, d[i].pPostbuildInfoDescs);
}
void D3DAPI L_EmitRtAsPostbuild(ID3D12GraphicsCommandList4* l4, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* d, UINT n,
                                const D3D12_GPU_VIRTUAL_ADDRESS* src) {
    auto* l = (ID3D12GraphicsCommandList*)l4;
    if (null_graphics() || !d || !n) return;
    Device* dev = hdr(l)->device;
    if (!rt_build_on()) { rt_fake_postbuild(l, dev, *d, n); return; }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC t;
    if (!pc_postbuild(dev, *d, &t)) return;
    std::vector<uint64_t> s(n);
    for (UINT i = 0; i < n; i++) s[i] = gpu_va(dev, src[i]);
    pc::list_rt_postbuild(LPC(l), &t, n, s.data());
}
void D3DAPI L_CopyRtAs(ID3D12GraphicsCommandList4* l4, D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
    auto* l = (ID3D12GraphicsCommandList*)l4;
    uint32_t m = (uint32_t)mode;
    {
        static volatile long n = 0;
        MEMORY_BASIC_INFORMATION mbi{};
        bool cpu = VirtualQuery((const void*)src, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
        if (InterlockedIncrement(&n) <= 40)
            XSL_INFO(CH, "AS copy: mode %#x, dest %llx, source %llx (%s)", m, (unsigned long long)dst, (unsigned long long)src, cpu ? "CPU-readable" : "GPU only");
    }
    if (getenv("XSL_AS_DUMP") && m == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE) pc::list_dump_memory(LPC(l), src, "deserialize source");
    if (m == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE && !null_graphics()) {
        pc::list_rt_deserialize(LPC(l), dst, src);   // console-format structure for translated traversal
        if (!rt_build_on()) return;
    }
    if (null_graphics() || !rt_build_on()) return;
    {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 20) XSL_INFO(CH, "CopyRaytracingAccelerationStructure mode %#x", m);
    }
    // Serialized structures are driver data: a console blob cannot be deserialized on PC, and the
    // console's compressed serialization has no PC form. Those copies are skipped.
    if (m == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE || m > 4) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "acceleration structure copy mode %#x (serialized console data) skipped", m);
        return;
    }
    Device* dev = hdr(l)->device;
    pc::list_rt_copy(LPC(l), gpu_va(dev, dst), gpu_va(dev, src), m);
}
void D3DAPI L_DepthBoundsX(ID3D12GraphicsCommandList* l, FLOAT lo, FLOAT hi) { pc::list_depth_bounds(LPC(l), lo, hi); }
void D3DAPI L_DepthBounds(ID3D12GraphicsCommandList1* l, FLOAT lo, FLOAT hi) { pc::list_depth_bounds(LPC((ID3D12GraphicsCommandList*)l), lo, hi); }
// Memory from the list's command allocator, valid until the allocator is reset: titles (and the
// GDK's acceleration structure builder) put data for the GPU there and name it by address. It
// comes from GPU-visible title memory (unified memory), handed out in order from 4 MB blocks that
// are used again after 64 of them (far longer than any allocator stays unreset).
// The PC buffer and offset behind a title address: GPU-visible title memory, a buffer the title
// placed, or page-mapped pool memory.
static bool resolve_memory(Device* dev, uint64_t va, PcObj* res, uint64_t* off) {
    UnifiedRef u;
    if (unified_lookup(dev, va, &u)) { *res = u.res; *off = u.offset; return true; }
    uint64_t avail = 0;
    if (pc::gm_resolve(va, res, off, &avail) && *res) return true;
    uint64_t run = 0;
    if (section_lookup(dev, va, &u, &run)) { *res = u.res; *off = u.offset; return true; }
    return false;
}
// A copy between two GPU addresses on the list's timeline.
void D3DAPI L_CopyMemoryToMemoryX(ID3D12GraphicsCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src, UINT64 n, D3D12XBOX_COPY_FLAGS) {
    Device* dev = hdr(l)->device;
    PcObj dr = nullptr, sr = nullptr;
    uint64_t doff = 0, soff = 0;
    const bool ok = n && resolve_memory(dev, dst, &dr, &doff) && resolve_memory(dev, src, &sr, &soff);
    static volatile long shown = 0;
    if (InterlockedIncrement(&shown) <= 6 || !ok) {
        static volatile long w = 0;
        if (ok || InterlockedIncrement(&w) <= 6)
            XSL_INFO(CH, "CopyMemoryToMemoryX %llx -> %llx (%llu bytes)%s", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n, ok ? "" : ": memory not addressable, skipped");
    }
    if (ok && !null_graphics()) pc::list_copy_buffer_region(LPC(l), dr, doff, sr, soff, n);
}
void* D3DAPI L_AllocateFromCommandAllocatorX(ID3D12GraphicsCommandList8*, UINT size, UINT align) {
    static std::mutex mu;
    static std::vector<uint8_t*> blocks;
    static size_t cur = 0, used = 0;
    constexpr size_t kBlock = 4u << 20, kBlocks = 64;
    if (!align) align = 16;
    if (size > kBlock) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "AllocateFromCommandAllocatorX(%u bytes): more than a block, refused", size);
        return nullptr;
    }
    std::lock_guard<std::mutex> g(mu);
    size_t at = (used + align - 1) / align * align;
    if (blocks.empty() || at + size > kBlock) {
        if (!blocks.empty()) cur = (cur + 1) % kBlocks;
        if (cur >= blocks.size()) {
            typedef PVOID(WINAPI * VAlloc)(PVOID, SIZE_T, DWORD, ULONGLONG, DWORD);
            static auto va = [] { HMODULE m = GetModuleHandleW(L"xmem.dll"); return m ? (VAlloc)GetProcAddress(m, "XMemVirtualAlloc") : nullptr; }();
            void* p = va ? va(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, 0x3ull, PAGE_READWRITE) : nullptr;   // XMEM_GRAPHICS
            if (!p) p = VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!p) return nullptr;
            blocks.push_back((uint8_t*)p);
            if (blocks.size() == 1) XSL_INFO(CH, "AllocateFromCommandAllocatorX: memory from GPU-visible blocks (first at %p)", p);
        }
        at = 0;
    }
    used = at + size;
    return blocks[cur] + at;
}
void D3DAPI L_DispatchMesh(ID3D12GraphicsCommandList6* l, UINT x, UINT y, UINT z) {
    if (draw_dump_now()) XSL_INFO(CH, "draw dump: list %p DispatchMesh(%u, %u, %u)%s", (void*)l, x, y, z, lx((ID3D12GraphicsCommandList*)l)->placeholderPso ? " (placeholder pipeline: skipped)" : "");
    if (!null_graphics() && !lx((ID3D12GraphicsCommandList*)l)->placeholderPso) pc::list_dispatch_mesh(pc_of(l), x, y, z);
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
D3D12DmaCommandList_Function_Table g_dma_tbl;
std::once_flag g_dma_once;
// DMA list memory operations on title addresses (global-memory registry).
void D3DAPI Dma_CopyMemory(ID3D12XboxDmaCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src, UINT64 n, D3D12XBOX_COPY_FLAGS) {
    Device* dev = hdr(l)->device;
    PcObj dr, sr; uint64_t doff, soff;
    uint64_t d2 = gpu_va(dev, dst), s2 = gpu_va(dev, src);
    if ((pc::gm_resolve(dst, &dr, &doff, nullptr) || pc::gm_resolve(d2, &dr, &doff, nullptr)) &&
        (pc::gm_resolve(src, &sr, &soff, nullptr) || pc::gm_resolve(s2, &sr, &soff, nullptr)))
        pc::list_copy_buffer_region(pc_of((ID3D12GraphicsCommandList*)l), dr, doff, sr, soff, n);
    else {
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 3) XSL_WARN(CH, "DMA copy %llx -> %llx (%llu bytes): memory not addressable", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    }
}
void D3DAPI Dma_Fill32(ID3D12XboxDmaCommandList* l, D3D12_GPU_VIRTUAL_ADDRESS dst, UINT64 n, UINT v, D3D12XBOX_COPY_FLAGS) {
    Device* dev = hdr(l)->device;
    uint64_t base = gpu_va(dev, dst);
    std::vector<uint64_t> a;
    std::vector<uint32_t> vals;
    for (uint64_t k = 0; k + 4 <= n && k < (1u << 16); k += 4) { a.push_back(base + k); vals.push_back(v); }   // small fills (larger ones are rare)
    if (!a.empty()) pc::list_write_immediate(pc_of((ID3D12GraphicsCommandList*)l), (uint32_t)a.size(), a.data(), vals.data());
}
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
        g->DispatchX = L_DispatchX;
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
        g->Write64BitValueTopOfPipeX = L_Write64TOP;
        g->Write32BitValueTopOfPipeX = L_Write32TOP;
        g->Wait64BitValueX = L_Wait64;
        g->Wait32BitValueX = L_Wait32;
        g->FlushPipelineX = L_FlushPipelineX;
        g->ExecuteIndirect = L_ExecuteIndirect;
        g->BeginQuery = L_BeginQuery;
        g->EndQuery = L_EndQuery;
        g->ResolveQueryData = L_ResolveQueryData;
        g->ClearUnorderedAccessViewUint = L_ClearUAVUint;
        g->ClearUnorderedAccessViewFloat = L_ClearUAVFloat;
        g->DiscardResource = L_DiscardResource;
        ((D3D12GraphicsCommandList6_Function_Table*)&g_list_tbl)->DispatchMesh = L_DispatchMesh;
        g_list_tbl.AllocateFromCommandAllocatorX = L_AllocateFromCommandAllocatorX;
        g->CopyMemoryToMemoryX = L_CopyMemoryToMemoryX;
        ((D3D12GraphicsCommandList6_Function_Table*)&g_list_tbl)->ResetX = L_ResetX;
        {
            auto* t4 = (D3D12GraphicsCommandList4_Function_Table*)&g_list_tbl;
            t4->BuildRaytracingAccelerationStructure = L_BuildRtAs;
            t4->SetPipelineState1 = L_SetPipelineState1;
            t4->DispatchRays = L_DispatchRays;
            ((D3D12GraphicsCommandList6_Function_Table*)&g_list_tbl)->DispatchRaysX = L_DispatchRaysX;
            t4->EmitRaytracingAccelerationStructurePostbuildInfo = L_EmitRtAsPostbuild;
            t4->CopyRaytracingAccelerationStructure = L_CopyRtAs;
            ((D3D12GraphicsCommandList6_Function_Table*)&g_list_tbl)->BuildRaytracingAccelerationStructureBatched2X = L_BuildRtAsBatched2X;
            ((D3D12GraphicsCommandList1_Function_Table*)&g_list_tbl)->OMSetDepthBounds = L_DepthBounds;
        }
        g->OMSetDepthBoundsX = L_DepthBoundsX;
        g->ResolveSubresource = L_ResolveSubresource;
        g->CopyResourceX = L_CopyResourceX;
        g->OMSetBlendFactor = L_OMSetBlendFactor;
        g->OMSetStencilRef = L_OMSetStencilRef;
        g->BeginEvent = L_Event;
        g->SetMarker = L_Event;
        g->EndEvent = L_EndEvent;
    });
    if (IsEqualGUID(riid, __uuidof(ID3D12XboxDmaCommandList))) {
        // Console copy (DMA) lists are their own interface: same list object, DMA function table.
        std::call_once(g_dma_once, [] {
            xsl_fill_default(&g_dma_tbl);
            install_device_child_base(&g_dma_tbl);
            auto& t = g_dma_tbl;
            t.CommandListFunction.GetType = L_GetType;
            t.Close = reinterpret_cast<decltype(t.Close)>(&L_Close);
            t.Reset = reinterpret_cast<decltype(t.Reset)>(&L_Reset);
            t.CopyBufferRegion = reinterpret_cast<decltype(t.CopyBufferRegion)>(&L_CopyBufferRegion);
            t.CopyTextureRegion = reinterpret_cast<decltype(t.CopyTextureRegion)>(&L_CopyTextureRegion);
            t.CopyResource = reinterpret_cast<decltype(t.CopyResource)>(&L_CopyResource);
            t.ResourceBarrier = reinterpret_cast<decltype(t.ResourceBarrier)>(&L_ResourceBarrier);
            t.BeginQuery = reinterpret_cast<decltype(t.BeginQuery)>(&L_BeginQuery);
            t.EndQuery = reinterpret_cast<decltype(t.EndQuery)>(&L_EndQuery);
            t.ResolveQueryData = reinterpret_cast<decltype(t.ResolveQueryData)>(&L_ResolveQueryData);
            t.SetMarker = reinterpret_cast<decltype(t.SetMarker)>(&L_Event);
            t.BeginEvent = reinterpret_cast<decltype(t.BeginEvent)>(&L_Event);
            t.EndEvent = reinterpret_cast<decltype(t.EndEvent)>(&L_EndEvent);
            t.Write64BitValueBottomOfPipeX = reinterpret_cast<decltype(t.Write64BitValueBottomOfPipeX)>(&L_Write64BOP);
            t.KickoffX = reinterpret_cast<decltype(t.KickoffX)>(&L_KickoffX);
            t.CopyMemoryToMemoryX = &Dma_CopyMemory;
            t.FillMemoryWith32BitValueX = &Dma_Fill32;
        });
        auto* o = make<XList, ListExtra>(Kind::List, &g_dma_tbl, pcl, dev, Iids<ID3D12XboxDmaCommandList>::list, nullptr);
        ex<XList, ListExtra>(o)->type = t;
        return hand_out(o, riid, out);
    }
    auto* o = make<XList, ListExtra>(Kind::List, &g_list_tbl, pcl, dev, Iids<XList>::list, nullptr);
    ex<XList, ListExtra>(o)->type = t;
    list_bind_global_heap(o, pcl);
    return hand_out(o, riid, out);
}

// ============================================================================================
// Command queue, including PresentX.
// ============================================================================================
namespace {
struct QueueExtra { D3D12_COMMAND_QUEUE_DESC desc; Backlog* backlog; };
static Backlog* queue_backlog(ID3D12CommandQueue* q, bool create) {
    QueueExtra* e = ex<ID3D12CommandQueue, QueueExtra>(q);
    if (e->backlog || !create) return e->backlog;
    std::lock_guard<std::mutex> g(g_backlogs_mu);
    if (e->backlog) return e->backlog;
    auto* b = new Backlog;
    b->q = pc_of(q);
    b->dev = dev_extra(hdr(q)->device)->pcDevice;
    static bool started = false;
    if (!started) { started = true; CloseHandle(CreateThread(nullptr, 0, backlog_thread, nullptr, 0, nullptr)); }
    g_backlogs.push_back(b);
    return e->backlog = b;
}
// Gives the queue the submitted lists. Without waits and writes to bring to memory, and with no
// backlog, they run at once (false: the caller runs them as before).
static bool queue_submit(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!list_waits_on()) return false;
    bool parts = false, writes = false;
    for (UINT i = 0; i < n; i++) {
        const ListWaits* w = ex<XList, ListExtra>((XList*)lists[i])->w;
        if (!w) continue;
        parts |= !w->earlier.empty() || !w->gate.empty();
        writes |= !w->writes.empty();
    }
    Backlog* b = queue_backlog(q, parts || writes);
    if (!b) return false;
    std::lock_guard<std::mutex> g(b->mu);
    backlog_pump(b);
    if (!parts && b->items.empty()) {   // nothing to stand at: run now
        std::vector<PcObj> v(n);
        std::vector<MemWrite> wr;
        for (UINT i = 0; i < n; i++) {
            v[i] = P(lists[i]);
            if (ListWaits* w = ex<XList, ListExtra>((XList*)lists[i])->w) { wr.insert(wr.end(), w->writes.begin(), w->writes.end()); w->writes.clear(); }
        }
        backlog_run(b, v, wr);
        return true;
    }
    // Into the backlog, part by part; an entry of lists ends where waits stand before the next part.
    BackItem cur;
    cur.kind = 1;
    auto flush = [&] {
        if (cur.lists.empty()) return;
        b->items.push_back(std::move(cur));
        cur = BackItem();
        cur.kind = 1;
    };
    auto stand = [&](std::vector<ValueWait>& waits) {
        if (waits.empty()) return;
        flush();
        for (const ValueWait& w : waits) { BackItem it; it.wait = w; b->items.push_back(std::move(it)); }
        waits.clear();
    };
    for (UINT i = 0; i < n; i++) {
        ListExtra* e = ex<XList, ListExtra>((XList*)lists[i]);
        if (ListWaits* w = e->w) {
            for (ListPart& part : w->earlier) {
                stand(part.waits);
                cur.lists.push_back(part.list);
                cur.owned.push_back(part.list);
                cur.writes.insert(cur.writes.end(), part.writes.begin(), part.writes.end());
            }
            w->earlier.clear();
            stand(w->gate);
            cur.writes.insert(cur.writes.end(), w->writes.begin(), w->writes.end());
            w->writes.clear();
        }
        // The list's own PC list goes to the backlog for good: the title may reset the list before
        // the queue has run it, so it gets a new one.
        PcObj own = P(lists[i]), fresh = nullptr;
        cur.lists.push_back(own);
        if (own && SUCCEEDED(pc::create_list_closed_like(b->dev, own, &fresh)) && fresh) {
            hdr(lists[i])->pc = fresh;
            cur.owned.push_back(own);
        } else {
            static volatile long said = 0;
            if (InterlockedIncrement(&said) <= 3) XSL_WARN(CH, "a list given to a queue's backlog keeps its PC list: reset before the queue runs it, its work is lost");
        }
    }
    flush();
    backlog_pump(b);
    return true;
}
// A signal or a wait for a fence goes behind the backlog, if there is one (false: done at once by the caller).
static bool queue_behind(ID3D12CommandQueue* q, int kind, PcObj fence, UINT64 value) {
    Backlog* b = queue_backlog(q, false);
    if (!b) return false;
    std::lock_guard<std::mutex> g(b->mu);
    backlog_pump(b);
    if (b->items.empty()) return false;
    BackItem it;
    it.kind = kind;
    it.fence = fence;
    it.value = value;
    pc::addref(fence);
    b->items.push_back(std::move(it));
    return true;
}
// XSL_SUBMIT_TIMES=1 (diagnostics): where the time of submissions goes, every 200 of them.
static bool submit_times() { static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_SUBMIT_TIMES", b, 4) && b[0] == '1'; }(); return on; }
void D3DAPI Q_Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    static std::atomic<uint64_t> us[5], calls{0};
    LARGE_INTEGER f, t0, t1;
    const bool timed = submit_times();
    if (timed) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    auto lap = [&](int k) {
        if (!timed) return;
        QueryPerformanceCounter(&t1);
        us[k] += (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        t0 = t1;
    };
    desc_sync();  // descriptors the title wrote inline since the last submission
    lap(0);
    cbv_refresh();   // constant buffer view copies: the title has written the constants by now
    lap(1);
    detile_flush(dev_extra(hdr(q)->device)->pcDevice, pc_of(q));   // CPU-written console textures first
    lap(2);
    pc::gm_flush(dev_extra(hdr(q)->device)->pcDevice);             // global-memory views and address table
    lap(3);
    if (!queue_submit(q, n, lists)) {   // (lists cut at waits, or a queue standing at one: see queue_submit)
        std::vector<PcObj> v(n);
        for (UINT i = 0; i < n; i++) { v[i] = P(lists[i]); XSL_TRACE("gfx.cmd", "Execute list %p", (void*)lists[i]); }
        pc::queue_execute(pc_of(q), n, v.data());
    }
    lap(4);
    if (timed && ++calls % 200 == 0)
        XSL_INFO(CH, "last 200 submissions: descriptors %llu ms, constant views %llu ms, textures %llu ms, global memory %llu ms, execute %llu ms",
                 (unsigned long long)us[0].exchange(0) / 1000, (unsigned long long)us[1].exchange(0) / 1000, (unsigned long long)us[2].exchange(0) / 1000,
                 (unsigned long long)us[3].exchange(0) / 1000, (unsigned long long)us[4].exchange(0) / 1000);
    if (timed && calls % 200 == 0) {
        uint64_t d[6];
        detile_stats(d);
        XSL_INFO(CH, "  textures: %llu known, %llu MB checked, %llu found changed, %llu uploads (%llu first) of %llu MB", (unsigned long long)d[5],
                 (unsigned long long)(d[0] >> 20), (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[4], (unsigned long long)(d[3] >> 20));
    }
}
HRESULT D3DAPI Q_Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    if (queue_behind(q, 2, P_or_pc(f), v)) return S_OK;
    return pc::queue_signal(pc_of(q), P_or_pc(f), v);
}
// Signal with console cache-flush flags: a PC queue signal already follows completion of all prior
// work, caches included.
HRESULT D3DAPI Q_SignalX(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v, D3D12XBOX_FLUSH) { return Q_Signal(q, f, v); }
HRESULT D3DAPI Q_Wait(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v) {
    if (queue_behind(q, 3, P_or_pc(f), v)) return S_OK;
    return pc::queue_wait(pc_of(q), P_or_pc(f), v);
}
// The X forms add a frame-pipeline token and console sync parameters (cache flushes at the signal
// or wait point); on PC a queue signal/wait already orders all prior work.
void D3DAPI Q_Execute2X(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists, D3D12XBOX_FRAME_PIPELINE_TOKEN, D3D12XBOX_EXECUTE_COMMAND_LIST_FLAGS) {
    Q_Execute(q, n, lists);
}
HRESULT D3DAPI Q_Signal2X(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v, D3D12XBOX_SYNC_PARAMS*, D3D12XBOX_SYNC_PARAMS*) { return Q_Signal(q, f, v); }
HRESULT D3DAPI Q_WaitX(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v, D3D12XBOX_SYNC_PARAMS*) { return Q_Wait(q, f, v); }
void D3DAPI Q_GetDesc(ID3D12CommandQueue* q, D3D12_COMMAND_QUEUE_DESC* d) { *d = ex<ID3D12CommandQueue, QueueExtra>(q)->desc; }
HRESULT D3DAPI Q_SuspendX(ID3D12CommandQueue*, UINT) { return S_OK; }
HRESULT D3DAPI Q_ResumeX(ID3D12CommandQueue*) { return S_OK; }
HRESULT D3DAPI Q_GetTimestampFrequency(ID3D12CommandQueue* q, UINT64* f) { return pc::queue_timestamp_frequency(pc_of(q), f); }
HRESULT D3DAPI Q_GetClockCalibration(ID3D12CommandQueue* q, UINT64* g, UINT64* c) { return pc::queue_clock_calibration(pc_of(q), g, c); }
std::mutex g_keep_mu;
std::vector<std::pair<IUnknown*, uint64_t>> g_keep;   // (PC object, present count when kept)
std::atomic<uint64_t> g_keep_frame{0};
}  // namespace
void keep_foreign(void* o) {
    auto* u = (IUnknown*)o;
    u->AddRef();
    if (getenv("XSL_D3D12_DEBUG")) {   // name PC objects from other components (debug-layer messages)
        void* ob = nullptr;
        if (SUCCEEDED(u->QueryInterface(__uuidof(ID3D12Object), &ob)) && ob) {
            ((ID3D12Object*)ob)->SetName(L"foreign (Media Foundation?)");
            ((ID3D12Object*)ob)->Release();
        }
    }
    std::lock_guard<std::mutex> g(g_keep_mu);
    g_keep.push_back({u, g_keep_frame.load()});
}
namespace {
void release_kept() {   // references older than 8 frames (the frame latency is at most a few)
    std::vector<IUnknown*> done;
    {
        std::lock_guard<std::mutex> g(g_keep_mu);
        uint64_t now = ++g_keep_frame;
        size_t k = 0;
        for (auto& e : g_keep) {
            if (e.second + 8 < now) done.push_back(e.first);
            else g_keep[k++] = e;
        }
        g_keep.resize(k);
    }
    for (auto* u : done) u->Release();
}
HRESULT D3DAPI Q_PresentX(ID3D12CommandQueue* q, UINT planes, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* p, const D3D12XBOX_PRESENT_PARAMETERS*) {
    if (planes == 0 || !p || !p->ResourceCount || !p->ppResources) return S_OK;  // "present blank": nothing to show yet
    backlog_drain(q, wait_timeout_ms());   // (a present is not put behind the queue's backlog: the backlog is given time to empty)
    release_kept();
    xsl_fixed_time_frame();
    {
        // (By time, not by presents: a scene that takes seconds per frame reports too.)
        static ULONGLONG last = GetTickCount64();
        if (const ULONGLONG now = GetTickCount64(); now - last >= 10000) {
            last = now;
            long n = InterlockedExchange(&g_skipped_work, 0);
            XSL_INFO(CH, "%s", pso_pool_stats().c_str());
            if (n) {
                std::string t;
                std::lock_guard<std::mutex> g(g_ph_mu);
                for (auto& [why, k] : g_ph_why) {
                    if (k) t += " [" + std::to_string(k) + ": " + why + "]";
                    k = 0;
                }
                XSL_INFO(CH, "placeholder pipelines skipped %ld draws/dispatches over the last 10 s%s", n, t.c_str());
            }
        }
    }
    LARGE_INTEGER callQpc;
    QueryPerformanceCounter(&callQpc);
    Device* dev = hdr(q)->device;
    DeviceExtra* de = dev_extra(dev);
    if (!de->window || !IsWindow(de->window)) de->window = find_title_window();
    if (!de->window) { XSL_WARN(CH, "PresentX: no title window yet"); return S_OK; }
    XSL_TRACE("gfx.cmd", "PresentX planes=%u res %p", planes, (void*)p->ppResources[0]);
    if (planes > 2) XSL_TRACE(CH, "PresentX: %u planes, presenting the first two", planes);
    {
        static volatile long n = 0;
        if (planes > 1 && InterlockedIncrement(&n) <= 3)
            for (UINT i = 0; i < planes; i++)
                XSL_INFO(CH, "PresentX plane %u: %u resources, first %p, color space %d, flags %#x", i, p[i].ResourceCount,
                         p[i].ResourceCount ? (void*)p[i].ppResources[0] : nullptr, (int)p[i].ColorSpace, (unsigned)p[i].Flags);
    }
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
                                               de->window, 0, (uint32_t)p->ColorSpace,
                                               planes > 1 && p[1].ResourceCount && p[1].ppResources ? P(p[1].ppResources[0]) : nullptr);
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
        g_queue_tbl.ExecuteCommandLists2X = Q_Execute2X;
        g_queue_tbl.Signal2X = Q_Signal2X;
        g_queue_tbl.WaitX = Q_WaitX;
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
    if (list_waits_on()) pc::list_cuts_prepare(false);   // lists record what a continuation after a wait needs, from the first one on
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

void placeholder_note(void* obj, const char* why) {
    std::lock_guard<std::mutex> g(g_ph_mu);
    int idx = -1;
    for (size_t i = 0; i < g_ph_why.size(); i++)
        if (g_ph_why[i].first == why) idx = (int)i;
    if (idx < 0 && g_ph_why.size() < 64) { g_ph_why.emplace_back(why, 0); idx = (int)g_ph_why.size() - 1; }
    g_ph_of[obj] = idx;
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

namespace xs {
void rt_pc_inputs(Device* dev, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* in,
                  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* out, std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* geo) {
    rt_inputs_impl(dev, in, out, geo);
}
}  // namespace xs
