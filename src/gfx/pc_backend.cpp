// SPDX-License-Identifier: AGPL-3.0-or-later
// PC half of d3d12_xs.dll: PC Direct3D 12 (Windows runtime, or Wine/vkd3d).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <deque>
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
ID3D12Device* g_main_device = nullptr;   // for removal reports from calls that do not pass it
struct RsTrack { PcObj gfx = nullptr, cs = nullptr, pso = nullptr; };
std::mutex g_rs_mu;
std::map<PcObj, std::pair<PcObj, bool>> g_pso_rs;
std::map<PcObj, RsTrack> g_list_rs;
// Indirect-argument record ring (readback memory stays readable after device removal).
constexpr uint32_t kEiSlots = 65536, kEiBytes = 48;
ID3D12Resource* g_ei_rb = nullptr;
uint32_t* g_ei_map = nullptr;
volatile long g_ei_next = 0;
struct EiInfo { std::wstring list; void* res; uint64_t off; };
std::vector<EiInfo> g_ei_info;   // per slot, CPU side
}  // namespace
void report_device_removed(ID3D12Device* dev);
namespace {
bool g_dred = false;
// With DRED on: per command list, the pipeline names marked so far (DRED's own breadcrumb contexts
// need Windows 11; the marker count before the failing op indexes this list).
std::mutex g_mark_lock;
std::map<std::wstring, std::vector<std::wstring>> g_marks;   // list debug name -> pipelines marked since Reset
std::map<void*, std::wstring> g_list_name;   // list -> current recording's name
std::deque<std::wstring> g_mark_order;      // oldest recordings are forgotten first
std::wstring list_key(void* l) { auto it = g_list_name.find(l); return it == g_list_name.end() ? std::wstring() : it->second; }
// Any call that reports device removal triggers the DRED report (once).
void note_removed(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DEVICE_RESET) report_device_removed(g_main_device);
}
HRESULT logged(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        XSL_ERR(CH, "%s failed %08lx", what, hr);
        dump_debug_messages();
        if (hr == DXGI_ERROR_DEVICE_REMOVED) report_device_removed(g_main_device);
    }
    return hr;
}
D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint64_t h) { return D3D12_CPU_DESCRIPTOR_HANDLE{(SIZE_T)h}; }
D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint64_t h) { return D3D12_GPU_DESCRIPTOR_HANDLE{h}; }
std::string g_adapter = "unknown";
LUID g_adapter_luid{};
Caps g_caps{};
IDXGIFactory4* g_factory = nullptr;

// Console resource-state encoding -> PC. They differ in two places: the console's PRESENT is
// 0x100000 (PC: 0, i.e. COMMON), and the console's GENERIC_READ (0x8c3) omits INDIRECT_ARGUMENT.
D3D12_RESOURCE_STATES pc_state(uint32_t x) {
    if (x & 0x100000) x &= ~0x100000u;  // PRESENT -> COMMON (PC PRESENT == 0)
    if ((x & 0x8c3) == 0x8c3) x |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    // PC bits valid on graphics and compute lists only: console-only bits (PRESERVE_COMPRESSED_*,
    // 0x10000000) and video states are reserved there and invalidate the list.
    x &= 0x3FFFu | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    return (D3D12_RESOURCE_STATES)x;
}

bool env_on(const char* name) {
    char b[16];
    return GetEnvironmentVariableA(name, b, sizeof b) && b[0] != '0';
}
}  // namespace

// XSL_D3D12_DRED=1: device-removed extended data (automatic breadcrumbs, page faults), logged once
// when the device is found removed.
void report_device_removed(ID3D12Device* dev) {
    static volatile long once = 0;
    if (!dev || InterlockedExchange(&once, 1)) return;
    XSL_ERR(CH, "device removed: reason %08lx", dev->GetDeviceRemovedReason());
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) return;
    ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc{};
    if (SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&dred1))) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc))) {
        // Lists that started but did not finish first (the hang), then a count of those queued
        // behind them. The pipeline names come from the breadcrumb contexts of SetMarker (no calls
        // on the reported list: it may be gone).
        int waiting = 0, n_lists = 0;
        bool shown_waiting = false;
        std::deque<std::wstring> anames;   // names for nodes without a wide one (stable addresses)
        for (int pass = 0; pass < 2; pass++, n_lists = 0)
            for (auto* n0 = bc.pHeadAutoBreadcrumbNode; n0 && n_lists < 4096; n0 = n0->pNext, n_lists++) {
                D3D12_AUTO_BREADCRUMB_NODE1 nn = *n0;
                if (!nn.pCommandListDebugNameW && !nn.pCommandListDebugNameA) {   // match the list pointer (not dereferenced)
                    std::lock_guard<std::mutex> g(g_mark_lock);
                    auto it = g_list_name.find((void*)nn.pCommandList);
                    if (it != g_list_name.end()) { anames.push_back(it->second); nn.pCommandListDebugNameW = anames.back().c_str(); }
                }
                if (!nn.pCommandListDebugNameW && nn.pCommandListDebugNameA) {
                    std::string a = nn.pCommandListDebugNameA;
                    anames.emplace_back(a.begin(), a.end());
                    nn.pCommandListDebugNameW = anames.back().c_str();
                }
                auto* n = &nn;
                UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;   // completed
                if (pass == 0 && done == 0) { waiting++; continue; }
                if (pass == 1 && done != 0) continue;
                if (pass == 1 && shown_waiting) break;    // print one not-started list only
                if (pass == 1) shown_waiting = true;
                XSL_ERR(CH, "DRED: a command list (queue %ls) stopped at op %u of %u", n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"?",
                        done, n->BreadcrumbCount);
                if (n->pCommandListDebugNameW && g_ei_map && !g_ei_info.empty()) {
                    std::wstring nm = n->pCommandListDebugNameW;
                    if (size_t sp = nm.find(L" (debug layer"); sp != std::wstring::npos) nm.resize(sp);
                    std::vector<uint32_t> mine;
                    for (uint32_t k = 0; k < kEiSlots; k++)
                        if (g_ei_info[k].list == nm) mine.push_back(k);
                    // slots are handed out in recording order: the n-th ExecuteIndirect of the list
                    // (counted in its breadcrumbs up to the stop) is the n-th record
                    std::sort(mine.begin(), mine.end());
                    size_t nth = 0;
                    for (UINT q = 0; q <= done && q < n->BreadcrumbCount; q++) nth += n->pCommandHistory[q] == D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT;
                    size_t lo = nth > 2 ? nth - 2 : 0, hi = std::min(mine.size(), nth + 1);
                    if (nth) XSL_ERR(CH, "DRED:   the stop is ExecuteIndirect %zu of the list's %zu", nth, mine.size());
                    for (size_t q = lo; q < hi; q++) {
                        uint32_t k = mine[q];
                        const uint32_t* e = g_ei_map + k * (kEiBytes / 4);
                        XSL_ERR(CH, "DRED:   indirect args of this list (#%u, buffer %p + %llu): %08x %08x %08x %08x %08x", k, g_ei_info[k].res,
                                (unsigned long long)g_ei_info[k].off, e[0], e[1], e[2], e[3], e[4]);
                    }
                }
                if (n->pCommandListDebugNameW) {
                    UINT marks = 0;
                    for (UINT k = 0; k <= done && k < n->BreadcrumbCount; k++) marks += n->pCommandHistory[k] == D3D12_AUTO_BREADCRUMB_OP_SETMARKER;
                    std::lock_guard<std::mutex> g(g_mark_lock);
                    std::wstring nm = n->pCommandListDebugNameW;
                    if (size_t sp = nm.find(L" (debug layer"); sp != std::wstring::npos) nm.resize(sp);
                    auto it = g_marks.find(nm);
                    if (it != g_marks.end() && marks && marks <= it->second.size()) {
                        XSL_ERR(CH, "DRED: pipeline in use: %ls (marker %u of %zu)", it->second[marks - 1].c_str(), marks, it->second.size());
                        for (UINT m = marks > 8 ? marks - 8 : 0; m < marks; m++) XSL_ERR(CH, "DRED:   marker %u: %ls", m + 1, it->second[m].c_str());
                    }
                    else
                        XSL_ERR(CH, "DRED: list %ls: %u markers, %s", n->pCommandListDebugNameW, marks, it == g_marks.end() ? "no history" : "history mismatch");
                }
                std::string hist;
                for (UINT k = done > 40 ? done - 40 : 0; k <= done && k < n->BreadcrumbCount; k++) hist += " " + std::to_string((int)n->pCommandHistory[k]);
                XSL_ERR(CH, "DRED:   ops up to the stop:%s", hist.c_str());
                for (UINT k = done > 4 ? done - 4 : 0; k < n->BreadcrumbCount && k < done + 3; k++) {
                    const wchar_t* ctx = L"";
                    for (UINT c = 0; c < n->BreadcrumbContextsCount; c++)
                        if (n->pBreadcrumbContexts[c].BreadcrumbIndex <= k) ctx = n->pBreadcrumbContexts[c].pContextString;
                    XSL_ERR(CH, "DRED:   %s op %u: %d  [last marker: %ls]", k == done ? ">" : " ", k, (int)n->pCommandHistory[k], ctx ? ctx : L"");
                }
            }
        XSL_ERR(CH, "DRED: %d unfinished lists had not started", waiting);
    }
    if (dred1) dred1->Release();
    if (g_ei_map) {   // last indirect arguments seen by the GPU (0xcdcdcdcd: copy did not run)
        long last = g_ei_next;
        int shown = 0;
        for (long k = last; k > std::max(0L, last - (long)kEiSlots) && shown < 8; k--) {   // newest copies that ran
            const uint32_t* e = g_ei_map + (k % kEiSlots) * (kEiBytes / 4);
            if (e[0] == 0xcdcdcdcdu && e[1] == 0xcdcdcdcdu) continue;
            shown++;
            XSL_ERR(CH, "DRED: indirect args #%ld: %08x %08x %08x %08x %08x %08x %08x %08x count %08x", k, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8]);
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
        XSL_ERR(CH, "DRED: page fault at GPU VA %llx", (unsigned long long)pf.PageFaultVA);
    dred->Release();
}

HRESULT_T device_removed_reason(PcObj dev) {
    HRESULT hr = as<ID3D12Device>(dev)->GetDeviceRemovedReason();
    if (FAILED(hr)) report_device_removed(as<ID3D12Device>(dev));
    return hr;
}

// Diagnostics: unusual exceptions (codes outside the usual debugger/C++ ones) with the module and
// offset of each stack frame, logged when DRED or the debug layer is on.
static LONG CALLBACK note_exception(EXCEPTION_POINTERS* e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == 0xC00000FD) return EXCEPTION_CONTINUE_SEARCH;   // stack overflow: no stack left to log with
    static bool log_av = env_on("XSL_LOG_AV");   // access violations too (titles probe memory; noisy)
    if (c == 0xC0000005 && !log_av) return EXCEPTION_CONTINUE_SEARCH;
    if (c == 0x406D1388 || c == 0x40010006 || c == 0x4001000A || c == 0xE06D7363 || c == 0x80000003 || c == 0x80000004 ||
        c == 0xE0434352 || c == 0x40080201 || c == 0x4000001F)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) > 12) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT k = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string t;
    for (USHORT i = 0; i < k; i++) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m)) {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\');
            char buf[MAX_PATH + 32];
            snprintf(buf, sizeof buf, " %s+%llx", b ? b + 1 : name, (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)m));
            t += buf;
        } else t += " ?";
    }
    XSL_ERR(CH, "exception %08lx (%lu parameters%s%llx):%s", c, e->ExceptionRecord->NumberParameters,
            e->ExceptionRecord->NumberParameters ? ", first " : "", e->ExceptionRecord->NumberParameters ? (unsigned long long)e->ExceptionRecord->ExceptionInformation[0] : 0ull, t.c_str());
    if (c == 0x87d) dump_debug_messages();   // the debug layer's break: show the message behind it
    return EXCEPTION_CONTINUE_SEARCH;
}
HRESULT_T create_device(PcObj* out) {
    if (env_on("XSL_D3D12_DRED") || env_on("XSL_D3D12_DEBUG")) {
        static bool once = false;
        if (!once) { once = true; AddVectoredExceptionHandler(1, note_exception); }
    }
    HRESULT hr;
    if (env_on("XSL_D3D12_DRED")) {
        ID3D12DeviceRemovedExtendedDataSettings* ds = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&ds)))) {
            ds->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            ds->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            ID3D12DeviceRemovedExtendedDataSettings1* ds1 = nullptr;
            if (SUCCEEDED(ds->QueryInterface(IID_PPV_ARGS(&ds1)))) { ds1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON); ds1->Release(); }
            ds->Release();
            g_dred = true;
            XSL_INFO(CH, "DRED enabled (breadcrumbs, page faults)");
        }
    }
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
        g_adapter_luid = d.AdapterLuid;
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
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{5, fls, D3D_FEATURE_LEVEL_11_0};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof o6);
    dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof o1);
    g_caps.waveMin = o1.WaveLaneCountMin ? o1.WaveLaneCountMin : 32;
    g_caps.waveMax = o1.WaveLaneCountMax ? o1.WaveLaneCountMax : 32;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
    set_host_waves(g_caps.waveMin, g_caps.waveMax, sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6);
    g_caps.meshShaders = o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    g_caps.samplerFeedback = o7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    g_caps.raytracing = o5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    g_caps.vrs = o6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    g_caps.featureLevel = fl.MaxSupportedFeatureLevel;
    XSL_INFO(CH, "host caps: feature level %x, shader model %x, waves %u-%u lanes, mesh shaders %s, ray tracing %s, VRS %s, sampler feedback %s", g_caps.featureLevel,
             (unsigned)sm.HighestShaderModel, g_caps.waveMin, g_caps.waveMax, g_caps.meshShaders ? "yes" : "NO", g_caps.raytracing ? "yes" : "NO", g_caps.vrs ? "yes" : "NO", g_caps.samplerFeedback ? "yes" : "NO");
    if (env_on("XSL_D3D12_DEBUG") && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&g_info)))) {
        g_info->SetMessageCountLimit(4096);
        // The debug layer breaks (raises exception 0x87d) on corruption messages by default; titles
        // treat that as a fatal error. Messages are logged instead.
        g_info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        g_info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        g_info->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
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
    g_main_device = (ID3D12Device*)dev;
    return S_OK;
}

const char* adapter_name() { return g_adapter.c_str(); }
uint64_t adapter_luid() { return ((uint64_t)(uint32_t)g_adapter_luid.HighPart << 32) | g_adapter_luid.LowPart; }
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
constexpr uint32_t kGmSlots = 16384, kGmHalf = 8192;   // global-memory view range (end of the global heap)
}  // namespace
PcObj global_view_heap(PcObj dev) {
    std::lock_guard<std::mutex> g(g_gv_mu);
    if (g_gv || g_gv_failed) return g_gv;
    char b[16];
    g_gv_size = GetEnvironmentVariableA("XSL_DESCRIPTOR_HEAP_SIZE", b, sizeof b) ? (uint32_t)atoi(b) : 0;
    // 1,000,000 is the binding tier 1/2 maximum; tier-3 GPUs may allow larger heaps, which titles
    // with several large heaps need (XSL_DESCRIPTOR_HEAP_SIZE; falls back to 1,000,000).
    if (g_gv_size < 4096) g_gv_size = 1000000;
    D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_gv_size, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    g_gv_inc = as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_gv))) && g_gv_size > 1000000) {
        XSL_WARN(CH, "a %u-entry shader-visible descriptor heap is not supported; using 1000000", g_gv_size);
        g_gv_size = d.NumDescriptors = 1000000;
    }
    if (!g_gv && FAILED(logged(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_gv)), "CreateDescriptorHeap(global)"))) {
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
void set_name(PcObj o, const wchar_t* name) {
    if (!o || !name) return;
    if (g_dred) {   // command lists keep the recording names DRED reports are matched by
        std::lock_guard<std::mutex> g(g_mark_lock);
        if (g_list_name.count(o)) return;
    }
    as<ID3D12Object>(o)->SetName(name);
}

// Console copy (DMA) queues and lists take barriers with any resource state; PC copy lists accept
// only copy states (other states remove the device). They run as PC direct queues and lists.
static int pc_list_type(int t) {
    static bool keep = [] { char b[4]; return GetEnvironmentVariableA("XSL_COPY_QUEUES", b, 4) && b[0] == '1'; }();
    // Console compute lists may also use graphics-only states in barriers: compute queues and lists
    // run as direct ones too (XSL_COMPUTE_AS_DIRECT=0 keeps PC compute queues).
    static bool computeDirect = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_COMPUTE_AS_DIRECT", b, 4) && b[0] == '0'); }();
    if (t == D3D12_COMMAND_LIST_TYPE_COMPUTE && computeDirect) return D3D12_COMMAND_LIST_TYPE_DIRECT;
    return t == D3D12_COMMAND_LIST_TYPE_COPY && !keep ? D3D12_COMMAND_LIST_TYPE_DIRECT : t;
}
HRESULT_T create_queue(PcObj dev, const void* desc, PcObj* out) {
    D3D12_COMMAND_QUEUE_DESC d = *static_cast<const D3D12_COMMAND_QUEUE_DESC*>(desc);
    d.Type = (D3D12_COMMAND_LIST_TYPE)pc_list_type(d.Type);
    d.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;  // drop Xbox priority bits
    d.Priority = 0;
    return logged(as<ID3D12Device>(dev)->CreateCommandQueue(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12CommandQueue**>(out))), "CreateCommandQueue");
}
HRESULT_T create_allocator(PcObj dev, int type, PcObj* out) {
    return logged(as<ID3D12Device>(dev)->CreateCommandAllocator((D3D12_COMMAND_LIST_TYPE)pc_list_type(type), IID_PPV_ARGS(reinterpret_cast<ID3D12CommandAllocator**>(out))), "CreateCommandAllocator");
}
void name_recording(PcObj l);
void list_set_pso(PcObj l, PcObj pso);
HRESULT_T create_list(PcObj dev, int type, PcObj alloc, PcObj pso, PcObj* out) {
    HRESULT hr = logged(as<ID3D12Device>(dev)->CreateCommandList(0, (D3D12_COMMAND_LIST_TYPE)pc_list_type(type), as<ID3D12CommandAllocator>(alloc),
                                                           as<ID3D12PipelineState>(pso), IID_PPV_ARGS(reinterpret_cast<ID3D12GraphicsCommandList**>(out))), "CreateCommandList");
    if (SUCCEEDED(hr)) {   // a new list is recording: name it like a Reset
        name_recording(*out);
        if (pso && g_dred) list_set_pso(*out, pso);
    }
    return hr;
}
// Writer tracing: shader-visible view heaps other than the global one (GPU start, CPU start, bytes).
struct SvHeap { uint64_t gpu, cpu, bytes; };
std::vector<SvHeap> g_sv_heaps;
std::mutex g_sv_mu;
HRESULT_T create_descriptor_heap(PcObj dev, const void* desc, PcObj* out) {
    D3D12_DESCRIPTOR_HEAP_DESC d = *static_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(desc);
    d.Flags &= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if ((int)d.Type > 3) d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;  // Xbox "sampler unbounded"
    HRESULT hr = logged(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12DescriptorHeap**>(out))), "CreateDescriptorHeap");
    if (SUCCEEDED(hr) && (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        auto* h = as<ID3D12DescriptorHeap>(*out);
        std::lock_guard<std::mutex> g(g_sv_mu);
        g_sv_heaps.push_back({h->GetGPUDescriptorHandleForHeapStart().ptr, h->GetCPUDescriptorHandleForHeapStart().ptr,
                              (uint64_t)d.NumDescriptors * as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize(d.Type)});
    }
    return hr;
}
// GPU view handle -> CPU handle in the global heap or another shader-visible view heap (0 if none).
uint64_t sv_gpu_to_cpu(uint64_t h) {
    if (g_gv) {
        uint64_t g0 = g_gv->GetGPUDescriptorHandleForHeapStart().ptr;
        if (h >= g0 && h < g0 + (uint64_t)g_gv_size * g_gv_inc) return g_gv->GetCPUDescriptorHandleForHeapStart().ptr + (h - g0);
    }
    std::lock_guard<std::mutex> g(g_sv_mu);
    for (auto& e : g_sv_heaps)
        if (h >= e.gpu && h < e.gpu + e.bytes) return e.cpu + (h - e.gpu);
    return 0;
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

namespace {
struct ByteBlob final : ID3DBlob {
    volatile LONG ref = 1;
    std::vector<uint8_t> data;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, __uuidof(ID3DBlob))) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data.size(); }
};
}  // namespace
PcObj create_blob(const void* data, size_t size) {
    auto* b = new ByteBlob;
    b->data.assign((const uint8_t*)data, (const uint8_t*)data + size);
    return (PcObj)static_cast<ID3DBlob*>(b);
}

// ---- Global memory (docs/gpucode.md, ray tracing R1) ------------------------------------------
// Console code reads title GPU addresses directly. Every buffer the title can address gets raw
// views in a range reserved at the end of the global view heap; slot 0 of the range holds a sorted
// table {start, size, view indices}. Root signatures get one appended descriptor table over the
// range (SRV t0 space901 = table, SRVs t0.. space900, UAVs u0.. space900), bound whenever a root
// signature is set.
namespace {
struct GmBuf { uint64_t size; ID3D12Resource* res; uint64_t resOff; std::vector<std::pair<uint32_t, uint32_t>> views; };   // (SRV, UAV) slot per 512 MB chunk
std::mutex g_gm_mu;
std::map<uint64_t, GmBuf> g_gm;                 // title address -> buffer
bool g_gm_dirty = false;
uint32_t g_gm_next_srv = 1, g_gm_next_uav = kGmHalf;
std::vector<ID3D12Resource*> g_gm_tables;       // table buffers (recent ones kept alive)
std::map<ID3D12Resource*, ID3D12Resource*> g_as_shadow;   // acceleration-structure buffer -> shadow
std::vector<uint32_t> g_gm_free_srv, g_gm_free_uav;   // view slots of destroyed buffers
uint64_t g_gm_built_at = UINT64_MAX;            // present count of the last table rebuild
std::map<PcObj, uint32_t> g_rs_gm;              // root signature -> appended parameter index
std::map<PcObj, bool> g_list_global_heap;       // list -> the global heap is bound
std::map<PcObj, std::pair<PcObj, PcObj>> g_list_gm_rs;   // list -> current (graphics, compute) root signatures
bool gm_enabled() { static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_GLOBAL_MEMORY", b, 4) && b[0] == '0'); }(); return on; }
}  // namespace
bool gm_active() { return gm_enabled() && !getenv("XSL_GM_NO_RS"); }
uint32_t gm_reserved_start() { return gm_enabled() && g_gv_size > kGmSlots ? g_gv_size - kGmSlots : UINT32_MAX; }
void gm_register(PcObj res, uint64_t titleVa, uint64_t size, uint64_t resOff) {
    if (!gm_enabled() || !res || !size) return;
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    if (SUCCEEDED(as<ID3D12Resource>(res)->GetHeapProperties(&hp, &hf)) &&
        (hp.Type == D3D12_HEAP_TYPE_READBACK || (hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK && hp.MemoryPoolPreference == D3D12_MEMORY_POOL_L0 && false)))
        return;   // readback buffers only take copies
    std::lock_guard<std::mutex> g(g_gm_mu);
    auto& e = g_gm[titleVa];
    if (e.res == as<ID3D12Resource>(res) && e.size == size) return;
    e = GmBuf{size, as<ID3D12Resource>(res), resOff};
    g_gm_dirty = true;
}
void gm_forget(PcObj res) {
    if (!gm_enabled() || !res) return;
    ID3D12Resource* shadow = nullptr;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto it = g_as_shadow.find(as<ID3D12Resource>(res));
        if (it != g_as_shadow.end()) { shadow = it->second; g_as_shadow.erase(it); }
    }
    if (shadow) { gm_forget((PcObj)shadow); shadow->Release(); }
    std::lock_guard<std::mutex> g(g_gm_mu);
    for (auto it = g_gm.begin(); it != g_gm.end();) {
        if (it->second.res != as<ID3D12Resource>(res)) { ++it; continue; }
        for (auto& v : it->second.views) {
            if (v.first) g_gm_free_srv.push_back(v.first);
            if (v.second) g_gm_free_uav.push_back(v.second);
        }
        it = g_gm.erase(it);
        g_gm_dirty = true;
    }
}
std::atomic<uint64_t> g_gm_frame{0};   // presents (table rebuilds are limited to one per frame)
// Acceleration-structure buffers cannot have raw views: each gets a shadow buffer of the same size,
// registered at its address, that holds the console-format structure (deserialized or built by the
// layer) for translated traversal code.
static void gm_shadow_as(ID3D12Device* dev, ID3D12Resource* as, uint64_t size) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* sh = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sh)))) return;
    { std::lock_guard<std::mutex> g(g_gm_mu); g_as_shadow[as] = sh; }
    gm_register((PcObj)sh, as->GetGPUVirtualAddress(), size, 0);
}
// DESERIALIZE: the console structure follows the 56-byte serialized header (and its BLAS pointers)
// at a 64-byte boundary; its node pointers are relative, so it is copied as bytes.
void list_rt_deserialize(PcObj l, uint64_t dst, uint64_t src) {
    PcObj sr, dr; uint64_t so, doff, savail, davail;
    if (!gm_resolve(src, &sr, &so, &savail) || !gm_resolve(dst, &dr, &doff, &davail) || savail <= 64) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "acceleration structure deserialize %llx -> %llx: memory not addressable", (unsigned long long)src, (unsigned long long)dst);
        return;
    }
    uint64_t bytes = std::min(savail - 64, davail);
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    L2->CopyBufferRegion(as<ID3D12Resource>(dr), doff, as<ID3D12Resource>(sr), so + 64, bytes);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {as<ID3D12Resource>(dr), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON};
    L2->ResourceBarrier(1, &b);   // promoted by the copy; back to COMMON so later reads promote again
}
// PC buffer holding title address va (global-memory registry): resource and byte offset.
bool gm_resolve(uint64_t va, PcObj* res, uint64_t* off, uint64_t* avail) {
    std::lock_guard<std::mutex> g(g_gm_mu);
    auto it = g_gm.upper_bound(va);
    if (it == g_gm.begin()) return false;
    --it;
    if (va >= it->first + it->second.size) return false;
    *res = (PcObj)it->second.res;
    *off = it->second.resOff + (va - it->first);
    if (avail) *avail = it->first + it->second.size - va;
    return true;
}
// XSL_AS_DUMP=1: the first 512 bytes of serialized acceleration structures, logged a few frames later.
namespace {
struct AsDump { ID3D12Resource* rb; uint64_t va; uint64_t frame; };
std::vector<AsDump> g_as_dumps;
}  // namespace
void list_dump_memory(PcObj l, uint64_t va, const char* what) {
    static int n = 0;
    if (n >= 6 || !g_main_device) return;
    PcObj r; uint64_t off, avail;
    if (!gm_resolve(va, &r, &off, &avail) || avail < 512) return;
    n++;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 512;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) return;
    as<ID3D12GraphicsCommandList>(l)->CopyBufferRegion(rb, 0, as<ID3D12Resource>(r), off, 512);
    std::lock_guard<std::mutex> g(g_gm_mu);
    g_as_dumps.push_back({rb, va, g_gm_frame.load()});
    (void)what;
}
static void print_dumps() {
    std::lock_guard<std::mutex> g(g_gm_mu);
    for (size_t i = 0; i < g_as_dumps.size();) {
        auto& d = g_as_dumps[i];
        if (d.frame + 4 > g_gm_frame.load()) { i++; continue; }
        uint32_t* p = nullptr;
        if (SUCCEEDED(d.rb->Map(0, nullptr, (void**)&p))) {
            for (int row = 0; row < 32; row++)
                XSL_INFO(CH, "AS dump %llx +%03x: %08x %08x %08x %08x", (unsigned long long)d.va, row * 16, p[row * 4], p[row * 4 + 1], p[row * 4 + 2], p[row * 4 + 3]);
            d.rb->Unmap(0, nullptr);
        }
        d.rb->Release();
        g_as_dumps.erase(g_as_dumps.begin() + i);
    }
}
// presents (table rebuilds are limited to one per frame)
void gm_flush(PcObj devObj) {
    if (!gm_enabled() || !g_gv) return;
    std::lock_guard<std::mutex> g(g_gm_mu);
    if (!g_gm_dirty) return;
    if (g_gm_built_at == g_gm_frame.load()) return;   // at most one rebuild per frame
    g_gm_built_at = g_gm_frame.load();
    g_gm_dirty = false;
    auto* dev = as<ID3D12Device>(devObj);
    const uint32_t base = gm_reserved_start();
    auto cpu_at = [&](uint32_t k) { return D3D12_CPU_DESCRIPTOR_HANDLE{g_gv->GetCPUDescriptorHandleForHeapStart().ptr + (uint64_t)(base + k) * g_gv_inc}; };
    std::vector<uint32_t> table(4, 0);
    uint32_t count = 0;
    static volatile long full = 0;
    // A buffer view holds at most 2^27 elements (512 MB raw): larger buffers become several chunks,
    // each with its own views and table entry.
    constexpr uint64_t kChunk = 512ull << 20;
    for (auto& [va, e] : g_gm) {
        D3D12_RESOURCE_DESC rd = e.res->GetDesc();
        uint64_t bytes = std::min<uint64_t>(e.size, rd.Width - e.resOff) & ~3ull;
        if (!bytes) continue;
        uint32_t chunks = (uint32_t)((bytes + kChunk - 1) / kChunk);
        if (e.views.size() != chunks) e.views.assign(chunks, {0, 0});
        for (uint32_t c = 0; c < chunks; c++) {
            uint64_t off = (uint64_t)c * kChunk, len = std::min(kChunk, bytes - off);
            auto& v = e.views[c];
            const bool srvOk = !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
            const bool uavOk = (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
            if (!srvOk && !uavOk) continue;
            if (!v.first && !v.second) {
                auto take = [](std::vector<uint32_t>& fr, uint32_t& next, uint32_t end) -> uint32_t {
                    if (!fr.empty()) { uint32_t k = fr.back(); fr.pop_back(); return k; }
                    return next < end ? next++ : 0u;
                };
                uint32_t sSlot = srvOk ? take(g_gm_free_srv, g_gm_next_srv, kGmHalf) : 0u;
                uint32_t uSlot = uavOk ? take(g_gm_free_uav, g_gm_next_uav, kGmSlots) : 0u;
                if ((srvOk && !sSlot) || (uavOk && !uSlot)) {
                    if (sSlot) g_gm_free_srv.push_back(sSlot);
                    if (uSlot) g_gm_free_uav.push_back(uSlot);
                    if (InterlockedIncrement(&full) == 1) XSL_WARN(CH, "global memory: view range full");
                    continue;
                }
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_R32_TYPELESS;
                sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Buffer.FirstElement = (e.resOff + off) / 4;
                sv.Buffer.NumElements = (UINT)(len / 4);
                sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                if (srvOk) { v.first = sSlot; dev->CreateShaderResourceView(e.res, &sv, cpu_at(v.first)); }
                if (uavOk) {
                    v.second = uSlot;
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
                    uv.Format = DXGI_FORMAT_R32_TYPELESS;
                    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uv.Buffer.FirstElement = sv.Buffer.FirstElement;
                    uv.Buffer.NumElements = sv.Buffer.NumElements;
                    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                    dev->CreateUnorderedAccessView(e.res, nullptr, &uv, cpu_at(v.second));
                }
            }
            // {start lo, start hi, size, SRV index [13:0] | UAV index [27:14] | has UAV [31]}
            uint64_t start = va + off;
            table.push_back(uint32_t(start));
            table.push_back(uint32_t(start >> 32));
            table.push_back((uint32_t)len);
            // [13:0] SRV index, [27:14] UAV index, [30] has SRV, [31] has UAV
            table.push_back((v.first ? (v.first - 1) | 0x40000000u : 0u) | (v.second ? ((v.second - kGmHalf) << 14) | 0x80000000u : 0u));
            count++;
        }
    }
    table[0] = count;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = table.size() * 4;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* t = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&t)))) return;
    void* m = nullptr;
    if (SUCCEEDED(t->Map(0, nullptr, &m))) { memcpy(m, table.data(), table.size() * 4); t->Unmap(0, nullptr); }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Buffer.NumElements = (UINT)(table.size() / 4);
    sv.Buffer.StructureByteStride = 16;
    dev->CreateShaderResourceView(t, &sv, cpu_at(0));
    g_gm_tables.push_back(t);
    while (g_gm_tables.size() > 8) { g_gm_tables.front()->Release(); g_gm_tables.erase(g_gm_tables.begin()); }   // in flight at most a few frames
    static volatile long logged = 0;
    if (InterlockedIncrement(&logged) <= 3) XSL_INFO(CH, "global memory: %u buffers addressable by translated shaders", count);
}
// Appends the global-memory table to a serialized root signature (false: left unchanged).
static bool gm_extend_root_signature(const void* blob, size_t size, std::vector<uint8_t>& out, uint32_t* index) {
    ID3D12VersionedRootSignatureDeserializer* ds = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&ds)))) return false;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    bool ok = false;
    if (SUCCEEDED(ds->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const auto& d = vd->Desc_1_1;
        unsigned cost = 0;
        for (UINT i = 0; i < d.NumParameters; i++) {
            auto t = d.pParameters[i].ParameterType;
            cost += t == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 1 : t == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? d.pParameters[i].Constants.Num32BitValues : 2;
        }
        if (cost < 64) {
            std::vector<D3D12_ROOT_PARAMETER1> params(d.pParameters, d.pParameters + d.NumParameters);
            static const D3D12_DESCRIPTOR_RANGE_FLAGS vol = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
            static const D3D12_DESCRIPTOR_RANGE1 ranges[3] = {
                {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 901, vol, 0},
                {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGmHalf - 1, 0, 900, vol, 1},
                {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kGmSlots - kGmHalf, 0, 900, vol, kGmHalf}};
            D3D12_ROOT_PARAMETER1 p{};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable = {3, ranges};
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params.push_back(p);
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC nd{D3D_ROOT_SIGNATURE_VERSION_1_1};
            nd.Desc_1_1 = d;
            nd.Desc_1_1.NumParameters = (UINT)params.size();
            nd.Desc_1_1.pParameters = params.data();
            ID3DBlob* b = nullptr;
            ID3DBlob* err = nullptr;
            if (SUCCEEDED(D3D12SerializeVersionedRootSignature(&nd, &b, &err))) {
                out.assign((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize());
                *index = d.NumParameters;
                ok = true;
                b->Release();
            }
            if (err) err->Release();
        }
    }
    ds->Release();
    return ok;
}
HRESULT_T create_root_signature(PcObj dev, const void* blob, size_t size, PcObj* out) {
    void* clean = nullptr;
    size_t cleanSize = 0;
    bool owned = sanitize_container(blob, size, &clean, &cleanSize);
    const void* use = owned ? clean : blob;
    size_t useSize = owned ? cleanSize : size;
    std::vector<uint8_t> ext;
    uint32_t gmIndex = 0;
    static bool noRs = getenv("XSL_GM_NO_RS") != nullptr;   // bisecting: views and table only
    bool extended = gm_enabled() && !noRs && gm_extend_root_signature(use, useSize, ext, &gmIndex);
    HRESULT hr = as<ID3D12Device>(dev)->CreateRootSignature(0, extended ? ext.data() : use, extended ? ext.size() : useSize,
                                                            IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(out)));
    if (FAILED(hr) && extended) {   // fall back to the title's own layout
        extended = false;
        hr = as<ID3D12Device>(dev)->CreateRootSignature(0, use, useSize, IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(out)));
    }
    if (owned) free_container(clean);
    if (FAILED(hr)) XSL_ERR(CH, "CreateRootSignature failed %08lx (blob %zu bytes)", hr, size);
    if (SUCCEEDED(hr) && extended) { std::lock_guard<std::mutex> g(g_gm_mu); g_rs_gm[*out] = gmIndex; }
    return hr;
}
static void gm_bind(PcObj l, PcObj rs, bool compute) {
    if (!gm_enabled() || !g_gv || !rs) return;
    uint32_t idx;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto& cur = g_list_gm_rs[l];
        (compute ? cur.second : cur.first) = rs;
        auto it = g_rs_gm.find(rs);
        if (it == g_rs_gm.end()) return;
        auto h = g_list_global_heap.find(l);
        if (h == g_list_global_heap.end() || !h->second) return;   // bound once the global heap is set
        idx = it->second;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gh{g_gv->GetGPUDescriptorHandleForHeapStart().ptr + (uint64_t)gm_reserved_start() * g_gv_inc};
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    if (compute) L2->SetComputeRootDescriptorTable(idx, gh);
    else L2->SetGraphicsRootDescriptorTable(idx, gh);
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

void name_translated(PcObj pso) {   // PC pipelines made from translated shaders are named after them
    const char* k = translated_keys();
    if (!pso || !*k) return;
    wchar_t w[512];
    swprintf(w, 512, L"xsl %hs", k);
    as<ID3D12PipelineState>(pso)->SetName(w);
}
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
    if (FAILED(hr)) { dump_debug_messages(); XSL_ERR(CH, "CreateGraphicsPipelineState failed %08lx", hr); }
    if (SUCCEEDED(hr)) name_translated(*out);
    if (hr == DXGI_ERROR_DEVICE_REMOVED) report_device_removed(as<ID3D12Device>(dev));
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
    if (FAILED(hr)) { dump_debug_messages(); XSL_ERR(CH, "CreateComputePipelineState failed %08lx (translated shaders: %s)", hr, translated_keys()); }
    if (SUCCEEDED(hr)) name_translated(*out);
    if (hr == DXGI_ERROR_DEVICE_REMOVED) report_device_removed(as<ID3D12Device>(dev));
    return hr;
}

namespace {
template <class T> struct alignas(void*) StreamSub { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; T v; };
std::string fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}
bool pso_traced() {   // XSL_TRACE_PSO matches one of the shaders of the pipeline being created
    static char want[512] = "";
    static bool wi = (GetEnvironmentVariableA("XSL_TRACE_PSO", want, sizeof want), true);
    (void)wi;
    if (!want[0]) return false;
    std::string_view k = translated_keys();
    for (const char* t = want; *t;) {
        const char* e = strchr(t, ',');
        size_t n = e ? (size_t)(e - t) : strlen(t);
        if (n && k.find(std::string_view(t, n)) != std::string_view::npos) return true;
        t += n + (e ? 1 : 0);
    }
    return false;
}
// XSL_TRACE_PSO: the output-affecting state of a traced pipeline stream.
void trace_pso_stream(const void* stream, size_t size) {
    const uint8_t* p = (const uint8_t*)stream;
    size_t o = 0;
    std::string out;
    while (o + 4 <= size) {
        auto t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(p + o);
        size_t n = 0;
        switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: n = sizeof(StreamSub<void*>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
            n = sizeof(StreamSub<D3D12_SHADER_BYTECODE>);
            out += fmt(" stage%d:%zu", (int)t, ((const StreamSub<D3D12_SHADER_BYTECODE>*)(p + o))->v.BytecodeLength);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: n = sizeof(StreamSub<D3D12_STREAM_OUTPUT_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: {
            n = sizeof(StreamSub<D3D12_BLEND_DESC>);
            auto& b = ((const StreamSub<D3D12_BLEND_DESC>*)(p + o))->v;
            auto& r = b.RenderTarget[0];
            out += fmt(" blend(a2c %d indep %d rt0 en %d src %d dst %d op %d srcA %d dstA %d mask %#x)", b.AlphaToCoverageEnable, b.IndependentBlendEnable, r.BlendEnable,
                       r.SrcBlend, r.DestBlend, r.BlendOp, r.SrcBlendAlpha, r.DestBlendAlpha, r.RenderTargetWriteMask);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: n = sizeof(StreamSub<UINT>); out += fmt(" samplemask %#x", ((const StreamSub<UINT>*)(p + o))->v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: {
            n = sizeof(StreamSub<D3D12_RASTERIZER_DESC>);
            auto& r = ((const StreamSub<D3D12_RASTERIZER_DESC>*)(p + o))->v;
            out += fmt(" raster(fill %d cull %d ccw %d clip %d)", r.FillMode, r.CullMode, r.FrontCounterClockwise, r.DepthClipEnable);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            n = sizeof(StreamSub<D3D12_DEPTH_STENCIL_DESC>);
            auto& d = ((const StreamSub<D3D12_DEPTH_STENCIL_DESC>*)(p + o))->v;
            out += fmt(" depth(en %d write %d func %d stencil %d)", d.DepthEnable, d.DepthWriteMask, d.DepthFunc, d.StencilEnable);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: n = sizeof(StreamSub<D3D12_INPUT_LAYOUT_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: n = sizeof(StreamSub<UINT>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: n = sizeof(StreamSub<UINT>); out += fmt(" topo %u", ((const StreamSub<UINT>*)(p + o))->v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            n = sizeof(StreamSub<D3D12_RT_FORMAT_ARRAY>);
            auto& f = ((const StreamSub<D3D12_RT_FORMAT_ARRAY>*)(p + o))->v;
            out += fmt(" rtvs %u (%d %d)", f.NumRenderTargets, (int)f.RTFormats[0], (int)f.RTFormats[1]);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: n = sizeof(StreamSub<DXGI_FORMAT>); out += fmt(" dsv %d", (int)((const StreamSub<DXGI_FORMAT>*)(p + o))->v); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: n = sizeof(StreamSub<DXGI_SAMPLE_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: n = sizeof(StreamSub<UINT>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: n = sizeof(StreamSub<D3D12_CACHED_PIPELINE_STATE>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: n = sizeof(StreamSub<D3D12_PIPELINE_STATE_FLAGS>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: n = sizeof(StreamSub<D3D12_VIEW_INSTANCING_DESC>); break;
        default: out += fmt(" (subobject %d: stop)", (int)t); n = size; break;
        }
        o += n;
    }
    XSL_INFO(CH, "pso %s:%s", translated_keys(), out.c_str());
}
}  // namespace
HRESULT_T create_pso_stream(PcObj dev, void* stream, size_t size, PcObj* out) {
    *out = nullptr;
    if (getenv("XSL_TRACE_WRITERS") && pso_traced()) trace_pso_stream(stream, size);
    D3D12_PIPELINE_STATE_STREAM_DESC d{size, stream};
    ID3D12Device2* d2 = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d2));
    if (SUCCEEDED(hr)) {
        hr = d2->CreatePipelineState(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
        d2->Release();
    }
    if (FAILED(hr)) { dump_debug_messages(); XSL_ERR(CH, "CreatePipelineState (stream, %zu bytes) failed %08lx (translated shaders: %s)", size, hr, translated_keys()); note_removed(hr); }
    // XSL_DUMP_FAILED_PSO=<dir>: the shaders of failed pipeline streams (<dir>\pso<n>_s<type>.bin).
    static char dumpDir[MAX_PATH] = "";
    static bool dumpInit = (GetEnvironmentVariableA("XSL_DUMP_FAILED_PSO", dumpDir, sizeof dumpDir), true);
    (void)dumpInit;
    static volatile long dumped = 0;
    if (FAILED(hr) && dumpDir[0] && InterlockedIncrement(&dumped) <= 12) {
        const uint8_t* p = (const uint8_t*)stream;
        for (size_t o = 0; o + sizeof(void*) <= size;) {
            auto t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(p + o);
            size_t n = 0;
            switch (t) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
                n = sizeof(StreamSub<D3D12_SHADER_BYTECODE>);
                auto& bc = ((const StreamSub<D3D12_SHADER_BYTECODE>*)(p + o))->v;
                if (bc.pShaderBytecode && bc.BytecodeLength) {
                    char path[MAX_PATH + 64];
                    snprintf(path, sizeof path, "%s\\pso%ld_s%d.bin", dumpDir, dumped, (int)t);
                    if (FILE* f = fopen(path, "wb")) { fwrite(bc.pShaderBytecode, 1, bc.BytecodeLength, f); fclose(f); }
                }
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: n = sizeof(StreamSub<void*>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: n = sizeof(StreamSub<D3D12_STREAM_OUTPUT_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: n = sizeof(StreamSub<D3D12_BLEND_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: n = sizeof(StreamSub<UINT>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: n = sizeof(StreamSub<D3D12_RASTERIZER_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: n = sizeof(StreamSub<D3D12_DEPTH_STENCIL_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: n = sizeof(StreamSub<D3D12_INPUT_LAYOUT_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: n = sizeof(StreamSub<D3D12_RT_FORMAT_ARRAY>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: n = sizeof(StreamSub<DXGI_FORMAT>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: n = sizeof(StreamSub<DXGI_SAMPLE_DESC>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: n = sizeof(StreamSub<D3D12_CACHED_PIPELINE_STATE>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: n = sizeof(StreamSub<D3D12_PIPELINE_STATE_FLAGS>); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: n = sizeof(StreamSub<D3D12_VIEW_INSTANCING_DESC>); break;
            default: n = size; break;
            }
            o += n;
        }
    }
    if (SUCCEEDED(hr)) name_translated(*out);
    if (hr == DXGI_ERROR_DEVICE_REMOVED) report_device_removed(as<ID3D12Device>(dev));
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

// Console indirect argument types (D3D12XBOX_INDIRECT_ARGUMENT_TYPE_*, 100 and up). Those with a
// PC equivalent are translated; the record layouts match (DISPATCHX and DISPATCH_L2/SINGLE take
// the three thread-group counts of a dispatch, DISPATCH_MESH_X those of a mesh dispatch, and
// only schedule the work differently on the console GPU). Scheduling markers without data
// (DPBB_BREAK_BATCH, FLUSH_PIPELINEX) are dropped when they come last, where removing them moves
// no other argument. The rest (padding, topology, descriptor table, pipeline state) has no PC
// form and still fails.
constexpr int kXboxArgDispatchX = 104, kXboxArgDispatchL2 = 105, kXboxArgBreakBatch = 106, kXboxArgDispatchMeshX = 107,
              kXboxArgFlushPipeline = 108;
bool translate_indirect_args(const D3D12_COMMAND_SIGNATURE_DESC* in, std::vector<D3D12_INDIRECT_ARGUMENT_DESC>* args) {
    args->assign(in->pArgumentDescs, in->pArgumentDescs + in->NumArgumentDescs);
    bool changed = false;
    for (auto& a : *args) {
        int t = (int)a.Type;
        if (t == kXboxArgDispatchX || t == kXboxArgDispatchL2) { a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH; changed = true; }
        else if (t == kXboxArgDispatchMeshX) { a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH; changed = true; }
    }
    while (!args->empty() && ((int)args->back().Type == kXboxArgBreakBatch || (int)args->back().Type == kXboxArgFlushPipeline)) {
        args->pop_back();
        changed = true;
    }
    return changed;
}

std::mutex g_rays_mu;
std::set<PcObj> g_rays_sigs;   // command signatures that dispatch rays (skipped while ray tracing is inert)
// Command signatures whose single argument is a console 1-D dispatch (DISPATCH_SINGLE: X only, Y and
// Z unused) or a DISPATCH_MESH_X (X, then reserved/payload words): PC dispatches read three group
// counts, so ExecuteIndirect runs from a copy of the arguments with Y = Z = 1.
std::map<PcObj, uint32_t> g_fix_sigs;   // signature -> byte stride
HRESULT_T create_command_signature(PcObj dev, const void* desc, PcObj rs, PcObj* out) {
    auto* d = static_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(desc);
    bool rays = false;
    for (UINT i = 0; d && d->pArgumentDescs && i < d->NumArgumentDescs; i++) rays |= d->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    bool fix_args = d && d->pArgumentDescs && d->NumArgumentDescs == 1 &&
                    ((int)d->pArgumentDescs[0].Type == kXboxArgDispatchL2 || (int)d->pArgumentDescs[0].Type == kXboxArgDispatchMeshX);
    uint32_t fix_stride = d ? d->ByteStride : 0;
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> args;
    D3D12_COMMAND_SIGNATURE_DESC pcDesc;
    if (d && d->pArgumentDescs && translate_indirect_args(d, &args)) {
        pcDesc = *d;
        pcDesc.pArgumentDescs = args.data();
        pcDesc.NumArgumentDescs = (UINT)args.size();
        static volatile long once = 0;
        if (!InterlockedExchange(&once, 1)) XSL_INFO(CH, "command signatures with console argument types are translated to PC ones");
        d = &pcDesc;
    }
    // The console accepts a root signature on any command signature; PC D3D12 requires one if and
    // only if an argument changes root parameters (constants, root CBV/SRV/UAV).
    bool rootArgs = false;
    for (UINT i = 0; d && i < d->NumArgumentDescs; i++) {
        D3D12_INDIRECT_ARGUMENT_TYPE t = d->pArgumentDescs[i].Type;
        rootArgs |= t == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT || t == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW ||
                    t == D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW || t == D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    }
    if (!rootArgs) rs = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->CreateCommandSignature(d, as<ID3D12RootSignature>(rs),
                                                               IID_PPV_ARGS(reinterpret_cast<ID3D12CommandSignature**>(out)));
    if (SUCCEEDED(hr) && rays) { std::lock_guard<std::mutex> g(g_rays_mu); g_rays_sigs.insert(*out); }
    if (SUCCEEDED(hr) && fix_args) { std::lock_guard<std::mutex> g(g_rays_mu); g_fix_sigs[*out] = fix_stride; }
    if (FAILED(hr) && d) {
        std::string args;
        for (UINT i = 0; i < d->NumArgumentDescs && i < 16; i++) {
            const D3D12_INDIRECT_ARGUMENT_DESC& a = d->pArgumentDescs[i];
            char b[64];
            snprintf(b, sizeof b, "%s%d(%u,%u)", i ? " " : "", (int)a.Type, a.Constant.RootParameterIndex, a.Constant.Num32BitValuesToSet);
            args += b;
        }
        XSL_ERR(CH, "CreateCommandSignature: stride %u, %u argument(s) [type(param,n)]: %s, root signature %p, node mask %x", d->ByteStride,
                d->NumArgumentDescs, args.c_str(), rs, d->NodeMask);
    }
    return logged(hr, "CreateCommandSignature");
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
    // The console allows unordered access to depth-stencil resources; PC D3D12 does not. The UAV
    // flag is dropped (UAVs onto such resources become null descriptors, see create_uav).
    if ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 2) XSL_WARN(CH, "depth-stencil resource with unordered access (console only): UAV flag dropped");
        d.Flags = (D3D12_RESOURCE_FLAGS)(d.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
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

static void note_texture(PcObj r);
static void note_buffer(PcObj r);
static void note_buffer_fwd(PcObj r) { note_buffer(r); }
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
    if (SUCCEEDED(hr)) note_buffer_fwd(*out);
    if (SUCCEEDED(hr) && g_info) {   // debug layer: name layer resources (others show as unnamed)
        wchar_t nm[96];
        swprintf(nm, 96, L"xsl committed %llux%u fmt %d", (unsigned long long)d.Width, d.Height, (int)d.Format);
        as<ID3D12Resource>(*out)->SetName(nm);
    }
    if (SUCCEEDED(hr)) note_texture(*out);
    if (SUCCEEDED(hr) && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (!(st & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) gm_register(*out, as<ID3D12Resource>(*out)->GetGPUVirtualAddress(), d.Width, 0);
        else if (gm_enabled()) gm_shadow_as(as<ID3D12Device>(dev), as<ID3D12Resource>(*out), d.Width);
    }
    if (SUCCEEDED(hr) && remap) {
        std::lock_guard<std::mutex> g(g_remap_mu);
        g_remapped.insert(*reinterpret_cast<ID3D12Resource**>(out));
    }
    if (FAILED(hr))
        XSL_ERR(CH, "CreateCommittedResource failed %08lx (dim %d %llux%ux%u mips %u fmt %d flags %x layout %d align %llu "
                    "samples %u/%u heap %d cpu %d pool %d heapflags %x state %x)", hr, d.Dimension, (unsigned long long)d.Width, d.Height,
                d.DepthOrArraySize, d.MipLevels, d.Format, d.Flags, d.Layout, (unsigned long long)d.Alignment, d.SampleDesc.Count,
                d.SampleDesc.Quality, hp.Type, hp.CPUPageProperty, hp.MemoryPoolPreference, clean_heap_flags(heapFlags), state);
        note_removed(hr);
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
    // Multisampled textures need 4 MB placement alignment on PC; console heaps that can hold
    // render targets ask for 64 KB. Such heaps get 4 MB alignment when their size allows it.
    constexpr uint64_t kMsaaAlign = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    if (!(d.Flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) && d.Properties.Type == D3D12_HEAP_TYPE_DEFAULT && d.Alignment < kMsaaAlign &&
        d.SizeInBytes % kMsaaAlign == 0)
        d.Alignment = kMsaaAlign;
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
std::map<uint64_t, std::pair<PcObj, uint64_t>> g_buf_va;   // writer tracing: buffer GPU VA -> (resource, size)
std::mutex g_buf_va_mu;
static void note_buffer(PcObj r) {
    if (false) return;
    if (!r || !getenv("XSL_TRACE_BUFFER")) return;
    D3D12_RESOURCE_DESC d = as<ID3D12Resource>(r)->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    std::lock_guard<std::mutex> g(g_buf_va_mu);
    g_buf_va[as<ID3D12Resource>(r)->GetGPUVirtualAddress()] = {r, d.Width};
}
struct Placed { PcObj res, heap; uint64_t offset, size; };
std::mutex g_placed_mu;
std::vector<Placed> g_placed;
HRESULT_T create_placed_resource(PcObj dev, PcObj heap, uint64_t offset, const void* desc, uint32_t state, const void* clear, PcObj* out) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    const D3D12_CLEAR_VALUE* cv = static_cast<const D3D12_CLEAR_VALUE*>(clear);
    if (!(d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) cv = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->CreatePlacedResource(as<ID3D12Heap>(heap), offset, &d, pc_state(state), cv,
                                                             IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out)));
    if (hr == E_INVALIDARG && d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // The PC layout can be larger than the console's (e.g. D32_S8 depth), so a texture placed for
        // the console's size may run past the heap. It gets its own memory instead (no aliasing
        // with the heap's other resources).
        D3D12_HEAP_DESC hd = as<ID3D12Heap>(heap)->GetDesc();
        D3D12_RESOURCE_ALLOCATION_INFO ai = as<ID3D12Device>(dev)->GetResourceAllocationInfo(0, 1, &d);
        D3D12_HEAP_PROPERTIES hp = hd.Properties;
        HRESULT h2 = as<ID3D12Device>(dev)->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, pc_state(state), cv,
                                                                    IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out)));
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5)
            XSL_WARN(CH, "placed texture %llux%u fmt %d at +%llu needs %llu bytes, heap holds %llu: %s", (unsigned long long)d.Width, d.Height, (int)d.Format,
                     (unsigned long long)offset, (unsigned long long)ai.SizeInBytes, (unsigned long long)hd.SizeInBytes,
                     SUCCEEDED(h2) ? "given its own memory" : "failed");
        hr = h2;
    }
    if (FAILED(hr)) logged(hr, "CreatePlacedResource");
    if (SUCCEEDED(hr) && g_info) {
        wchar_t nm[96];
        swprintf(nm, 96, L"xsl placed %llux%u fmt %d", (unsigned long long)d.Width, d.Height, (int)d.Format);
        as<ID3D12Resource>(*out)->SetName(nm);
    }
    if (SUCCEEDED(hr)) note_texture(*out);
    if (SUCCEEDED(hr)) note_buffer(*out);
    if (SUCCEEDED(hr) && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (!(pc_state(state) & D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) gm_register(*out, as<ID3D12Resource>(*out)->GetGPUVirtualAddress(), d.Width, 0);
        else if (gm_enabled()) gm_shadow_as(as<ID3D12Device>(dev), as<ID3D12Resource>(*out), d.Width);
    }
    if (SUCCEEDED(hr) && getenv("XSL_TRACE_WRITERS")) {   // placed resources, for aliasing checks at present
        D3D12_RESOURCE_ALLOCATION_INFO ai = as<ID3D12Device>(dev)->GetResourceAllocationInfo(0, 1, &d);
        std::lock_guard<std::mutex> g(g_placed_mu);
        g_placed.push_back({*out, heap, offset, ai.SizeInBytes});
    }
    return hr;
}
void report_aliases(PcObj r) {   // resources placed over r's memory
    std::lock_guard<std::mutex> g(g_placed_mu);
    const Placed* me = nullptr;
    for (auto& p : g_placed) if (p.res == r) me = &p;
    if (!me) { XSL_INFO(CH, "alias check: %p is not a placed resource", (void*)r); return; }
    unsigned n = 0;
    for (auto& p : g_placed) {
        if (p.res == r || p.heap != me->heap || p.offset >= me->offset + me->size || me->offset >= p.offset + p.size) continue;
        D3D12_RESOURCE_DESC d = as<ID3D12Resource>(p.res)->GetDesc();
        if (n++ < 16) XSL_INFO(CH, "alias check: %p (heap %p +%llx, %llu bytes) overlaps %p (%llux%u fmt %d dim %d flags %#x, +%llx)", (void*)r, (void*)me->heap,
                               (unsigned long long)me->offset, (unsigned long long)me->size, (void*)p.res, (unsigned long long)d.Width, d.Height, (int)d.Format,
                               (int)d.Dimension, (unsigned)d.Flags, (unsigned long long)p.offset);
    }
    XSL_INFO(CH, "alias check: %p placed in heap %p at +%llx: %u overlapping resources", (void*)r, (void*)me->heap, (unsigned long long)me->offset, n);
}

// XSL_TRACE_WRITERS=1: which pipeline last drew into each render target (the presented image's
// writer is logged at present).
static bool trace_writers() { static bool on = env_on("XSL_TRACE_WRITERS"); return on; }
std::mutex g_wr_mu;
std::map<uint64_t, PcObj> g_rtv_res;              // PC RTV handle -> resource
std::map<PcObj, PcObj> g_wr_rt0, g_wr_pso;        // list -> render target 0, pipeline
std::map<PcObj, std::pair<PcObj, uint64_t>> g_wr_last;   // resource -> (pipeline, draws)
std::map<PcObj, uint64_t> g_wr_frame;                     // resource -> present count of its last draw
std::atomic<uint64_t> g_wr_present{0};   // presents so far (writer tracing)
std::vector<PcObj> g_big_textures;        // 2D textures of 1080p and more (writer tracing)
std::map<PcObj, uint32_t> g_last_state;   // last whole-resource transition target (writer tracing)
std::map<uint64_t, PcObj> g_uav_res;      // CPU descriptor handle -> resource of the UAV there (writer tracing)
std::map<PcObj, std::map<uint32_t, uint64_t>> g_cs_tables;   // list -> compute root table GPU handles
std::map<PcObj, std::map<uint32_t, uint64_t>> g_gfx_tables;  // list -> graphics root table GPU handles
std::map<uint64_t, PcObj> g_srv_res;                          // CPU descriptor handle -> resource of the SRV there
std::map<uint64_t, uint64_t> g_cbv_va;                        // CPU descriptor handle -> CBV address
std::map<uint64_t, std::pair<uint64_t, uint8_t*>> g_mapped;   // buffer GPU VA -> (size, CPU pointer)
std::vector<uint64_t> g_traced_cbvs;                          // CBVs of the traced pipeline (dumped again at present)
static void note_reads(PcObj l, bool compute);
std::map<PcObj, std::pair<PcObj, uint64_t>> g_cs_writer;      // big texture -> (compute pipeline, present)
static void note_dispatch(PcObj l);
static void note_dispatch(PcObj l, bool compute);
static uint64_t rt_stats_at() { static uint64_t v = [] { char b[16]; return GetEnvironmentVariableA("XSL_RT_STATS", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }(); return v; }
static void note_texture(PcObj r) {
    if (!trace_writers() || !r) return;
    D3D12_RESOURCE_DESC d = as<ID3D12Resource>(r)->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < 1920 || d.Height < 1080 || d.SampleDesc.Count > 1) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    g_big_textures.push_back(r);
}
static void note_draw(PcObj l) {
    if (!trace_writers()) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    auto rt = g_wr_rt0.find(l);
    if (rt == g_wr_rt0.end() || !rt->second) return;
    auto& w = g_wr_last[rt->second];
    w.first = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
    w.second++;
    g_wr_frame[rt->second] = g_wr_present.load();
}
void create_rtv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_rtv_res[h] = res; }
    D3D12_RENDER_TARGET_VIEW_DESC t;
    const D3D12_RENDER_TARGET_VIEW_DESC* vd = view_fix(res, desc, t);
    if (trace_writers() && res) {
        D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            static volatile long n3 = 0;
            const D3D12_RENDER_TARGET_VIEW_DESC* td = static_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(desc);
            if (InterlockedIncrement(&n3) <= 20)
                XSL_INFO(CH, "RTV %llx on 3D %p (%llux%ux%u fmt %d): title %s dim %d fmt %d mip %u w %u+%u -> PC %s dim %d fmt %d mip %u w %u+%u", (unsigned long long)h,
                         (void*)res, (unsigned long long)rd.Width, rd.Height, rd.DepthOrArraySize, (int)rd.Format, td ? "desc" : "default", td ? (int)td->ViewDimension : -1,
                         td ? (int)td->Format : -1, td ? td->Texture3D.MipSlice : 0, td ? td->Texture3D.FirstWSlice : 0, td ? td->Texture3D.WSize : 0, vd ? "desc" : "default",
                         vd ? (int)vd->ViewDimension : -1, vd ? (int)vd->Format : -1, vd ? vd->Texture3D.MipSlice : 0, vd ? vd->Texture3D.FirstWSlice : 0, vd ? vd->Texture3D.WSize : 0);
        }
        if (rd.Width == 1920 && rd.Height == 1080 && rd.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 40)
                XSL_INFO(CH, "RTV %llx on %p (flags %#x mips %u): %s dim %d fmt %d mip %u", (unsigned long long)h, (void*)res, (unsigned)rd.Flags, rd.MipLevels,
                         vd ? "desc" : "default", vd ? (int)vd->ViewDimension : -1, vd ? (int)vd->Format : -1, vd ? vd->Texture2D.MipSlice : 0);
        }
    }
    as<ID3D12Device>(dev)->CreateRenderTargetView(as<ID3D12Resource>(res), vd, cpu(h));
}
void create_dsv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateDepthStencilView(as<ID3D12Resource>(res), static_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(desc), cpu(h));
}
void create_srv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_srv_res[h] = res; g_uav_res.erase(h); }
    D3D12_SHADER_RESOURCE_VIEW_DESC t;
    const D3D12_SHADER_RESOURCE_VIEW_DESC* vd = view_fix(res, desc, t);
    if (trace_writers() && res) {
        D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width == 1920 && rd.Height == 1080 && rd.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 40)
                XSL_INFO(CH, "SRV %p (%llux%u fmt %d mips %u): %s dim %d fmt %d mips %u..%u swizzle %#x (title desc fmt %d)", (void*)res, (unsigned long long)rd.Width,
                         rd.Height, (int)rd.Format, rd.MipLevels, vd ? "desc" : "default", vd ? (int)vd->ViewDimension : -1, vd ? (int)vd->Format : -1,
                         vd ? vd->Texture2D.MostDetailedMip : 0, vd ? vd->Texture2D.MipLevels : 0, vd ? vd->Shader4ComponentMapping : 0,
                         desc ? (int)static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc)->Format : -1);
        }
    }
    as<ID3D12Device>(dev)->CreateShaderResourceView(as<ID3D12Resource>(res), vd, cpu(h));
}
void create_uav(PcObj dev, PcObj res, PcObj counter, const void* desc, uint64_t h) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_uav_res[h] = res; g_srv_res.erase(h); }
    if (res && !(as<ID3D12Resource>(res)->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        // e.g. a depth-stencil resource whose UAV flag was dropped: null view of the requested dimension
        {
            D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 20)
                XSL_WARN(CH, "UAV on a resource without unordered access (%llux%u format %d flags 0x%x): null view, writes are lost",
                         (unsigned long long)rd.Width, rd.Height, (int)rd.Format, (unsigned)rd.Flags);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC nd{};
        nd.Format = DXGI_FORMAT_R32_FLOAT;
        nd.ViewDimension = desc ? static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(desc)->ViewDimension : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (nd.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) nd.Format = DXGI_FORMAT_R32_TYPELESS, nd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        as<ID3D12Device>(dev)->CreateUnorderedAccessView(nullptr, nullptr, &nd, cpu(h));
        return;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC t;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* vd = view_fix(res, desc, t);
    if (trace_writers() && res) {
        D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.Width >= 1900 && rd.Format == DXGI_FORMAT_R11G11B10_FLOAT) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 60)
                XSL_INFO(CH, "UAV %p (%llux%u fmt %d mips %u): %s dim %d fmt %d mip %u plane %u", (void*)res, (unsigned long long)rd.Width, rd.Height,
                         (int)rd.Format, rd.MipLevels, vd ? "desc" : "default", vd ? (int)vd->ViewDimension : -1, vd ? (int)vd->Format : -1,
                         vd ? vd->Texture2D.MipSlice : 0, vd ? vd->Texture2D.PlaneSlice : 0);
        }
    }
    as<ID3D12Device>(dev)->CreateUnorderedAccessView(as<ID3D12Resource>(res), as<ID3D12Resource>(counter), vd, cpu(h));
}
void create_cbv(PcObj dev, const void* desc, uint64_t h) {
    if (trace_writers() && desc) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        g_cbv_va[h] = static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc)->BufferLocation;
        g_srv_res.erase(h);
        g_uav_res.erase(h);
    }
    // Console constant-buffer views come in 16-byte steps (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_
    // ALIGNMENT is 16 there); PC D3D12 requires sizes in multiples of 256, so the size is rounded
    // up (a larger window onto the same constants). Addresses must also be 256-aligned on PC;
    // unaligned ones cannot be fixed in the view and are reported.
    D3D12_CONSTANT_BUFFER_VIEW_DESC d{};
    const D3D12_CONSTANT_BUFFER_VIEW_DESC* p = static_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(desc);
    if (p) {
        d = *p;
        d.SizeInBytes = (d.SizeInBytes + 255) & ~255u;
        if ((d.BufferLocation & 255) && getenv("XSL_CBV_ALIGN_DOWN")) {   // experiment: legal view, shifted contents
            d.SizeInBytes = UINT((d.SizeInBytes + (d.BufferLocation & 255) + 255) & ~255ull);
            d.BufferLocation &= ~255ull;
        }
        if (d.BufferLocation & 255) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 3)
                XSL_WARN(CH, "constant buffer view at %llx is not 256-byte aligned (PC requirement); the shader may read shifted constants",
                         (unsigned long long)d.BufferLocation);
        }
        p = &d;
    }
    as<ID3D12Device>(dev)->CreateConstantBufferView(p, cpu(h));
}
void create_sampler(PcObj dev, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateSampler(static_cast<const D3D12_SAMPLER_DESC*>(desc), cpu(h));
}
void copy_descriptors_simple(PcObj dev, uint32_t n, uint64_t dst, uint64_t src, int type) {
    if (type > 3) type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (trace_writers() && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        uint32_t inc = g_gv_inc ? g_gv_inc : 32;
        for (uint32_t k = 0; k < n; k++) {
            auto it = g_uav_res.find(src + (uint64_t)k * inc);
            if (it != g_uav_res.end()) g_uav_res[dst + (uint64_t)k * inc] = it->second;
            else g_uav_res.erase(dst + (uint64_t)k * inc);
            auto ct = g_cbv_va.find(src + (uint64_t)k * inc);
            if (ct != g_cbv_va.end()) g_cbv_va[dst + (uint64_t)k * inc] = ct->second;
            else g_cbv_va.erase(dst + (uint64_t)k * inc);
            auto jt = g_srv_res.find(src + (uint64_t)k * inc);
            if (jt != g_srv_res.end()) g_srv_res[dst + (uint64_t)k * inc] = jt->second;
            else g_srv_res.erase(dst + (uint64_t)k * inc);
        }
    }
    if (trace_writers() && type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        uint32_t inc = as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        static volatile long logged = 0;
        for (uint32_t k = 0; k < n; k++) {
            auto it = g_rtv_res.find(src + (uint64_t)k * inc);
            PcObj before = g_rtv_res.count(dst + (uint64_t)k * inc) ? g_rtv_res[dst + (uint64_t)k * inc] : nullptr;
            if (it != g_rtv_res.end()) g_rtv_res[dst + (uint64_t)k * inc] = it->second;
            else g_rtv_res.erase(dst + (uint64_t)k * inc);
            if (InterlockedIncrement(&logged) <= 20)
                XSL_INFO(CH, "RTV copy %llx -> %llx: %p (was %p)", (unsigned long long)(src + k * inc), (unsigned long long)(dst + k * inc),
                         it == g_rtv_res.end() ? nullptr : (void*)it->second, (void*)before);
        }
    }
    as<ID3D12Device>(dev)->CopyDescriptorsSimple(n, cpu(dst), cpu(src), (D3D12_DESCRIPTOR_HEAP_TYPE)type);
}
namespace {
struct UploadJob { ID3D12Resource* buf; ID3D12CommandAllocator* alloc; ID3D12GraphicsCommandList* list; ID3D12Fence* fence; };
std::mutex g_up_mu;
std::vector<UploadJob> g_up_jobs;   // in flight; released once their fence has passed
void reap_uploads(bool all) {
    for (size_t i = 0; i < g_up_jobs.size();) {
        auto& j = g_up_jobs[i];
        if (!all && j.fence->GetCompletedValue() < 1) { i++; continue; }
        j.buf->Release(); j.list->Release(); j.alloc->Release(); j.fence->Release();
        g_up_jobs.erase(g_up_jobs.begin() + i);
    }
}
}  // namespace
uint32_t pc_state_of(uint32_t consoleState) { return (uint32_t)pc_state(consoleState); }
HRESULT_T upload_texture(PcObj devObj, PcObj qObj, PcObj dstObj, uint32_t state, FillSubresource fill, void* ctx) {
    auto* dev = as<ID3D12Device>(devObj);
    auto* q = as<ID3D12CommandQueue>(qObj);
    auto* dst = as<ID3D12Resource>(dstObj);
    D3D12_RESOURCE_DESC rd = dst->GetDesc();
    UINT subs = rd.MipLevels * (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : rd.DepthOrArraySize);
    if (!subs || subs > 4096) return E_INVALIDARG;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> rowBytes(subs);
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, subs, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    if (!total || total == UINT64_MAX) return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    UploadJob j{};
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&j.buf));
    if (FAILED(hr)) return hr;
    uint8_t* p = nullptr;
    if (FAILED(hr = j.buf->Map(0, nullptr, (void**)&p))) { j.buf->Release(); return hr; }
    bool ok = true;
    for (UINT s = 0; s < subs && ok; s++) ok = fill(ctx, s, p + fp[s].Offset, fp[s].Footprint.RowPitch, rows[s], fp[s].Footprint.Depth);
    j.buf->Unmap(0, nullptr);
    if (!ok) { j.buf->Release(); return E_FAIL; }
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&j.alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, j.alloc, nullptr, IID_PPV_ARGS(&j.list));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&j.fence));
    auto st = (D3D12_RESOURCE_STATES)state;
    // COMMON textures are promoted to COPY_DEST by the copy (and decay back afterwards).
    bool barrier = st != D3D12_RESOURCE_STATE_COMMON && st != D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, D3D12_RESOURCE_STATE_COPY_DEST};
    if (barrier) j.list->ResourceBarrier(1, &b);
    for (UINT s = 0; s < subs; s++) {
        D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        d.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION sr{j.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        sr.PlacedFootprint = fp[s];
        j.list->CopyTextureRegion(&d, 0, 0, 0, &sr, nullptr);
    }
    if (barrier) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); j.list->ResourceBarrier(1, &b); }
    j.list->Close();
    ID3D12CommandList* ls[] = {j.list};
    q->ExecuteCommandLists(1, ls);
    q->Signal(j.fence, 1);
    std::lock_guard<std::mutex> g(g_up_mu);
    reap_uploads(false);
    g_up_jobs.push_back(j);
    return S_OK;
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

uint64_t fence_completed(PcObj f) {
    uint64_t v = as<ID3D12Fence>(f)->GetCompletedValue();
    if (v == UINT64_MAX) {   // the device was removed
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(as<ID3D12Fence>(f)->GetDevice(IID_PPV_ARGS(&dev)))) { report_device_removed(dev); dev->Release(); }
    }
    return v;
}
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
    if (SUCCEEDED(hr) && out && *out && trace_writers() && res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        g_mapped[res->GetGPUVirtualAddress()] = {res->GetDesc().Width, (uint8_t*)*out};
    }
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
void list_set_pso(PcObj l, PcObj pso);
void name_recording(PcObj l) {
    if (g_dred) {   // the debug name lets a DRED report find the list's marker history
        static uint64_t gen = 0;
        std::lock_guard<std::mutex> g(g_mark_lock);
        wchar_t b[64];
        swprintf(b, 64, L"xsl list %p #%llu", l, (unsigned long long)++gen);   // one name per recording
        L->SetName(b);
        g_list_name[l] = b;
        // DRED may report another interface pointer of the same list: record those too.
        static const IID* iids[] = {&__uuidof(ID3D12CommandList), &__uuidof(ID3D12GraphicsCommandList), &__uuidof(ID3D12GraphicsCommandList1),
                                    &__uuidof(ID3D12GraphicsCommandList4), &__uuidof(ID3D12GraphicsCommandList6), &__uuidof(IUnknown)};
        for (const IID* iid : iids) {
            void* q = nullptr;
            if (SUCCEEDED(as<IUnknown>(l)->QueryInterface(*iid, &q)) && q) {
                g_list_name[q] = b;
                static_cast<IUnknown*>(q)->Release();
            }
        }
        g_marks[b];
        g_mark_order.push_back(b);
        while (g_mark_order.size() > 8192) { g_marks.erase(g_mark_order.front()); g_mark_order.pop_front(); }
    }
}
HRESULT_T list_reset(PcObj l, PcObj a, PcObj pso) {
    if (gm_enabled()) { std::lock_guard<std::mutex> g(g_gm_mu); g_list_global_heap.erase(l); g_list_gm_rs.erase(l); }
    name_recording(l);
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs.erase(l); }
    HRESULT hr = logged(L->Reset(as<ID3D12CommandAllocator>(a), as<ID3D12PipelineState>(pso)), "CommandList::Reset");
    if (pso && SUCCEEDED(hr)) list_set_pso(l, pso);   // initial pipeline: marked like SetPipelineState
    return hr;
}
static bool plane_ops_now();
extern std::set<PcObj> g_planes;
void list_barriers(PcObj l, uint32_t n, const void* b) {
    if (!n) return;
    D3D12_RESOURCE_BARRIER tmp[64];
    auto* in = static_cast<const D3D12_RESOURCE_BARRIER*>(b);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        tmp[k] = in[i];
        if (tmp[k].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && trace_writers() && plane_ops_now()) {
            std::lock_guard<std::mutex> g(g_wr_mu);
            if (g_planes.count(tmp[k].Transition.pResource))
                XSL_INFO(CH, "plane op: list %p barrier on %p: console %#x -> %#x (sub %#x flags %#x), PC %#x -> %#x", (void*)l, (void*)tmp[k].Transition.pResource,
                         (unsigned)in[i].Transition.StateBefore, (unsigned)in[i].Transition.StateAfter, tmp[k].Transition.Subresource, (unsigned)tmp[k].Flags,
                         (unsigned)pc_state(in[i].Transition.StateBefore), (unsigned)pc_state(in[i].Transition.StateAfter));
        }
        if (tmp[k].Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            // Committed resources (e.g. textures that could not be placed) cannot take part in
            // aliasing barriers; a null side means "any resource".
            auto committed = [](ID3D12Resource* r) { D3D12_HEAP_PROPERTIES hp; D3D12_HEAP_FLAGS hf; return r && SUCCEEDED(r->GetHeapProperties(&hp, &hf)); };
            if (committed(tmp[k].Aliasing.pResourceBefore)) tmp[k].Aliasing.pResourceBefore = nullptr;
            if (committed(tmp[k].Aliasing.pResourceAfter)) tmp[k].Aliasing.pResourceAfter = nullptr;
        }
        if (tmp[k].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            tmp[k].Transition.StateBefore = pc_state(tmp[k].Transition.StateBefore);
            tmp[k].Transition.StateAfter = pc_state(tmp[k].Transition.StateAfter);
            if (trace_writers() && (tmp[k].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || tmp[k].Transition.Subresource == 0) &&
                !(tmp[k].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)) {
                std::lock_guard<std::mutex> g(g_wr_mu);
                g_last_state[tmp[k].Transition.pResource] = tmp[k].Transition.StateAfter;
            }
            if (tmp[k].Transition.StateBefore == tmp[k].Transition.StateAfter) continue;  // e.g. PRESENT <-> COMMON
        }
        k++;
    }
    if (k) L->ResourceBarrier(k, tmp);
}
std::map<PcObj, uint64_t> g_wr_rtv0;   // writer tracing: RTV handle 0 per list
void list_om_set_render_targets(PcObj l, uint32_t n, const uint64_t* rtvs, int single, const uint64_t* dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE r[8];
    uint32_t cnt = single ? (n ? 1 : 0) : n;
    for (uint32_t i = 0; i < cnt && i < 8; i++) r[i] = cpu(rtvs[i]);
    if (trace_writers()) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        auto it = cnt ? g_rtv_res.find(rtvs[0]) : g_rtv_res.end();
        g_wr_rt0[l] = it == g_rtv_res.end() ? nullptr : it->second;
        g_wr_rtv0[l] = cnt ? rtvs[0] : 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE d = dsv ? cpu(*dsv) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    L->OMSetRenderTargets(n, n ? r : nullptr, single, dsv ? &d : nullptr);
}
extern std::set<PcObj> g_plane_lists;
static bool plane_ops_now();
static void plane_op(PcObj l, PcObj r, const char* what);
void list_clear_rtv(PcObj l, uint64_t rtv, const float* c, uint32_t n, const void* rects) {
    if (trace_writers() && plane_ops_now()) {
        PcObj r = nullptr;
        { std::lock_guard<std::mutex> g(g_wr_mu); auto it = g_rtv_res.find(rtv); if (it != g_rtv_res.end()) r = it->second; }
        if (r) plane_op(l, r, n ? "clear (rects)" : "clear");
    }
    L->ClearRenderTargetView(cpu(rtv), c, n, static_cast<const D3D12_RECT*>(rects)); }
void list_clear_dsv(PcObj l, uint64_t dsv, uint32_t f, float d, uint8_t s, uint32_t n, const void* rects) {
    L->ClearDepthStencilView(cpu(dsv), (D3D12_CLEAR_FLAGS)f, d, s, n, static_cast<const D3D12_RECT*>(rects));
}
std::map<PcObj, D3D12_VIEWPORT> g_wr_vp;   // writer tracing: viewport 0 and scissor 0 per list
std::map<PcObj, D3D12_RECT> g_wr_sc;
void list_viewports(PcObj l, uint32_t n, const void* v) {
    if (trace_writers() && n) { std::lock_guard<std::mutex> g(g_wr_mu); g_wr_vp[l] = *static_cast<const D3D12_VIEWPORT*>(v); }
    L->RSSetViewports(n, static_cast<const D3D12_VIEWPORT*>(v));
}
void list_scissors(PcObj l, uint32_t n, const void* r) {
    if (trace_writers() && n) { std::lock_guard<std::mutex> g(g_wr_mu); g_wr_sc[l] = *static_cast<const D3D12_RECT*>(r); }
    L->RSSetScissorRects(n, static_cast<const D3D12_RECT*>(r));
}
void note_pso_rs(PcObj pso, PcObj rs, bool compute) {
    if (!g_info || !pso) return;
    std::lock_guard<std::mutex> g(g_rs_mu);
    g_pso_rs[pso] = {rs, compute};
}
void list_set_gfx_root_signature(PcObj l, PcObj rs) {
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].gfx = rs; }
    L->SetGraphicsRootSignature(as<ID3D12RootSignature>(rs));
    gm_bind(l, rs, false);
}
void list_set_compute_root_signature(PcObj l, PcObj rs) {
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].cs = rs; }
    L->SetComputeRootSignature(as<ID3D12RootSignature>(rs));
    gm_bind(l, rs, true);
}
static void check_rs(PcObj l, const char* what) {
    std::lock_guard<std::mutex> g(g_rs_mu);
    auto t = g_list_rs.find(l);
    if (t == g_list_rs.end()) return;
    auto p = g_pso_rs.find(t->second.pso);
    if (p == g_pso_rs.end()) return;
    PcObj bound = p->second.second ? t->second.cs : t->second.gfx;
    if (bound == p->second.first) return;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) > 12) return;
    wchar_t name[128] = L"?";
    UINT sz = sizeof name;
    as<ID3D12PipelineState>(t->second.pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, name);
    XSL_WARN(CH, "%s: %s pipeline %ls made with root signature %p, list has %p bound", what, p->second.second ? "compute" : "graphics", name,
             p->second.first, bound);
}
void list_set_pso(PcObj l, PcObj pso) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_wr_pso[l] = pso; }
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].pso = pso; }
    L->SetPipelineState(as<ID3D12PipelineState>(pso));
    if (g_dred && pso) {   // breadcrumb context: which pipeline the following work uses
        wchar_t name[128];
        UINT n = sizeof name;
        if (SUCCEEDED(as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &n, name)) && n > 2)
        {
            L->SetMarker(0, name, n);
            std::lock_guard<std::mutex> g(g_mark_lock);
            auto it = g_marks.find(list_key(l));
            if (it != g_marks.end()) it->second.push_back(name);
        }
    }
}
void list_ia_topology(PcObj l, int t) { L->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)t); }
void list_ia_vbs(PcObj l, uint32_t s, uint32_t n, const void* v) { L->IASetVertexBuffers(s, n, static_cast<const D3D12_VERTEX_BUFFER_VIEW*>(v)); }
void list_ia_ib(PcObj l, const void* v) { L->IASetIndexBuffer(static_cast<const D3D12_INDEX_BUFFER_VIEW*>(v)); }
static bool trace_dispatch_size(PcObj l, const char* how, uint32_t x, uint32_t y, uint32_t z, std::string* name);
// XSL_PLANE_OPS=<present> (with XSL_TRACE_WRITERS=1): every draw, clear and copy that writes a
// presented image during the frame before that present, in recording order.
std::set<PcObj> g_planes;   // resources presented so far
static bool plane_ops_now() {
    static uint64_t at = [] { char b[16]; return GetEnvironmentVariableA("XSL_PLANE_OPS", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
    uint64_t now = g_wr_present.load();
    return at && now + 1 == at;
}
static void plane_op(PcObj l, PcObj r, const char* what) {
    if (!trace_writers() || !plane_ops_now()) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    if (!g_planes.count(r)) return;
    PcObj pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
    wchar_t wn[128] = L"-";
    UINT sz = sizeof wn;
    if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
    static int n = 0;
    g_plane_lists.insert(l);
    if (n++ < 200) XSL_INFO(CH, "plane op: list %p %s on %p (pipeline %ls)", (void*)l, what, (void*)r, wn);
}
extern std::map<PcObj, uint64_t> g_wr_rtv0;
static void trace_draw_target(PcObj l, uint32_t n) {
    if (trace_writers() && plane_ops_now()) {
        PcObj r = nullptr;
        { std::lock_guard<std::mutex> g(g_wr_mu); auto rt = g_wr_rt0.find(l); if (rt != g_wr_rt0.end()) r = rt->second; }
        if (r) plane_op(l, r, "draw");
    }
    // XSL_PLANE_CLEAR_TEST=1: clear presented images to red through the bound RTV before each draw.
    static bool ct = getenv("XSL_PLANE_CLEAR_TEST") != nullptr;
    if (ct && trace_writers()) {
        uint64_t h = 0;
        bool plane = false;
        {
            std::lock_guard<std::mutex> g(g_wr_mu);
            auto rt = g_wr_rt0.find(l);
            plane = rt != g_wr_rt0.end() && g_planes.count(rt->second);
            h = g_wr_rtv0.count(l) ? g_wr_rtv0[l] : 0;
        }
        if (plane && h) {
            static const float red[4] = {1, 0, 0, 1};
            as<ID3D12GraphicsCommandList>(l)->ClearRenderTargetView(cpu(h), red, 0, nullptr);
        }
    }
    std::string tn;
    if (!trace_dispatch_size(l, "Draw", n, 0, 0, &tn)) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    auto rt = g_wr_rt0.find(l);
    PcObj r = rt == g_wr_rt0.end() ? nullptr : rt->second;
    static int shown = 0;
    if (shown++ < 12) {
        if (r) {
            D3D12_RESOURCE_DESC d = as<ID3D12Resource>(r)->GetDesc();
            D3D12_VIEWPORT vp = g_wr_vp.count(l) ? g_wr_vp[l] : D3D12_VIEWPORT{};
            D3D12_RECT sc = g_wr_sc.count(l) ? g_wr_sc[l] : D3D12_RECT{};
            XSL_INFO(CH, "pso %s: draws into %p (%llux%u x%u fmt %d dim %d), viewport %g %g %g %g depth %g..%g, scissor %ld %ld %ld %ld", tn.c_str(), (void*)r,
                     (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, (int)d.Format, (int)d.Dimension, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
                     vp.MinDepth, vp.MaxDepth, sc.left, sc.top, sc.right, sc.bottom);
        } else XSL_INFO(CH, "pso %s: draws with no render target 0 bound", tn.c_str());
    }
}
void list_draw(PcObj l, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { trace_draw_target(l, a * b); if (getenv("XSL_TRACE_BUFFER")) note_dispatch(l, false); note_draw(l); note_reads(l, false); L->DrawInstanced(a, b, c, d); }
void list_draw_indexed(PcObj l, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { trace_draw_target(l, a * b); if (getenv("XSL_TRACE_BUFFER")) note_dispatch(l, false); note_draw(l); note_reads(l, false); L->DrawIndexedInstanced(a, b, c, d, e); }
struct EiTrace { long slot; std::string name; uint64_t present; };
static std::vector<EiTrace> g_ei_trace;   // traced indirect dispatches whose arguments are printed later
static bool trace_dispatch_size(PcObj l, const char* how, uint32_t x, uint32_t y, uint32_t z, std::string* name) {
    static char want[512] = "";
    static bool wi = (GetEnvironmentVariableA("XSL_TRACE_PSO", want, sizeof want), true);
    (void)wi;
    if (!want[0] || !trace_writers()) return false;
    std::lock_guard<std::mutex> g(g_wr_mu);
    PcObj pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
    wchar_t wn[128] = L"?";
    UINT sz = sizeof wn;
    if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
    char nm[128];
    snprintf(nm, sizeof nm, "%ls", wn);
    // want: comma-separated name substrings
    bool match = false;
    for (const char* t = want; *t;) {
        const char* e = strchr(t, ',');
        size_t n = e ? (size_t)(e - t) : strlen(t);
        if (n && std::string_view(nm).find(std::string_view(t, n)) != std::string_view::npos) match = true;
        t += n + (e ? 1 : 0);
    }
    static int shown = 0;
    if (match && shown++ < 60) XSL_INFO(CH, "pso %s: %s %u x %u x %u (present %llu)", nm, how, x, y, z, (unsigned long long)g_wr_present);
    if (match && name) *name = nm;
    return match;
}
void list_dispatch(PcObj l, uint32_t x, uint32_t y, uint32_t z) {
    trace_dispatch_size(l, "Dispatch", x, y, z, nullptr);
    note_dispatch(l);
    note_reads(l, true);
    L->Dispatch(x, y, z);
}
// A dispatch writes the big textures whose UAVs are in the first slots of its compute tables.
static void note_dispatch(PcObj l) { note_dispatch(l, true); }
static void note_dispatch(PcObj l, bool compute) {
    if (!trace_writers() || !g_gv) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    auto& tabs = compute ? g_cs_tables : g_gfx_tables;
    auto t = tabs.find(l);
    if (t == tabs.end()) return;
    uint64_t gpu0 = g_gv->GetGPUDescriptorHandleForHeapStart().ptr, cpu0 = g_gv->GetCPUDescriptorHandleForHeapStart().ptr;
    for (auto& [idx, h] : t->second) {
        uint64_t c = sv_gpu_to_cpu(h);
        if (!c) continue;
        (void)gpu0; (void)cpu0;
        for (uint32_t k = 0; k < 64; k++) {
            auto it = g_uav_res.find(c + (uint64_t)k * g_gv_inc);
            if (it == g_uav_res.end()) continue;
            // XSL_TRACE_BUFFER=<size in bytes>: every compute writer of buffers of that size in the examined frame
            static uint64_t tb = [] { char b[24]; return GetEnvironmentVariableA("XSL_TRACE_BUFFER", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
            if (tb) {
                D3D12_RESOURCE_DESC d = as<ID3D12Resource>(it->second)->GetDesc();
                static std::set<std::pair<PcObj, PcObj>> seen;   // (buffer, pipeline) pairs already logged
                PcObj pso0 = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Width == tb && seen.insert({it->second, pso0}).second) {
                    PcObj pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
                    wchar_t wn[128] = L"?";
                    UINT sz = sizeof wn;
                    if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
                    XSL_INFO(CH, "frame writer of buffer %p: %ls (table %u slot %u, present %llu)", (void*)it->second, wn, idx, k, (unsigned long long)g_wr_present.load());
                }
            }
            // XSL_TRACE_TEX=<width>x<height>: every (texture, pipeline) pair writing textures of that size
            static uint32_t tw = 0, th = 0;
            static bool ti = [] { char b[32]; if (GetEnvironmentVariableA("XSL_TRACE_TEX", b, sizeof b)) sscanf(b, "%ux%u", &tw, &th); return true; }();
            (void)ti;
            if (tw) {
                D3D12_RESOURCE_DESC d = as<ID3D12Resource>(it->second)->GetDesc();
                static std::set<std::pair<PcObj, PcObj>> seen_t;
                PcObj pso0 = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && d.Width == tw && d.Height == th && seen_t.insert({it->second, pso0}).second) {
                    wchar_t wn[128] = L"?";
                    UINT sz = sizeof wn;
                    if (pso0) as<ID3D12PipelineState>(pso0)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
                    XSL_INFO(CH, "writer of texture %p (%ux%u fmt %d): %ls (table %u slot %u, present %llu)", (void*)it->second, tw, th, (int)d.Format, wn, idx, k,
                             (unsigned long long)g_wr_present.load());
                }
            }
            for (PcObj b : g_big_textures)
                if (b == it->second) {
                    PcObj pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
                    g_cs_writer[b] = {pso, g_wr_present.load()};
                    static uint64_t logged_at = 0;
                    if (g_wr_present.load() + 1 == rt_stats_at() && pso) {   // the frame being examined: every writer
                        wchar_t wn[128] = L"?";
                        UINT sz = sizeof wn;
                        as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
                        XSL_INFO(CH, "frame writer of texture %p: %ls (table %u slot %u)", b, wn, idx, k);
                    }
                    (void)logged_at;
                }
        }
    }
}
void list_dispatch_mesh(PcObj l, uint32_t x, uint32_t y, uint32_t z) {
    if (!g_caps.meshShaders) return;
    ID3D12GraphicsCommandList6* l6 = nullptr;
    if (SUCCEEDED(L->QueryInterface(IID_PPV_ARGS(&l6)))) { l6->DispatchMesh(x, y, z); l6->Release(); }
}
bool dred_on() { return g_dred; }
static long record_indirect(PcObj l, PcObj args, uint64_t ao, PcObj cnt, uint64_t co);
void list_record_indirect(PcObj l, PcObj args, uint64_t ao, PcObj cnt, uint64_t co) {
    if (g_dred) record_indirect(l, args, ao, cnt, co);
}
static long record_indirect(PcObj l, PcObj args, uint64_t ao, PcObj cnt, uint64_t co) {
    if (!g_main_device) return -1;
    static std::once_flag once;
    std::call_once(once, [] {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)kEiSlots * kEiBytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_ei_rb))))
            g_ei_rb->Map(0, nullptr, (void**)&g_ei_map);
    });
    if (!g_ei_map) return -1;
    uint32_t slot = (uint32_t)InterlockedIncrement(&g_ei_next) % kEiSlots;
    {
        std::lock_guard<std::mutex> g(g_mark_lock);
        if (g_ei_info.empty()) g_ei_info.resize(kEiSlots);
        g_ei_info[slot] = {list_key(l), args, ao};
    }
    memset(g_ei_map + slot * (kEiBytes / 4), 0xcd, kEiBytes);
    ID3D12Resource* a = as<ID3D12Resource>(args);
    uint64_t n = std::min<uint64_t>(32, a->GetDesc().Width - std::min(ao, a->GetDesc().Width));
    if (n) L->CopyBufferRegion(g_ei_rb, (uint64_t)slot * kEiBytes, a, ao, n);
    if (cnt) L->CopyBufferRegion(g_ei_rb, (uint64_t)slot * kEiBytes + 32, as<ID3D12Resource>(cnt), co, 4);
    return (long)slot;
}
void rt_prebuild_info(PcObj dev, const void* inputs, void* info) {
    ID3D12Device5* d5 = nullptr;
    if (FAILED(as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d5)))) return;
    d5->GetRaytracingAccelerationStructurePrebuildInfo(static_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS*>(inputs),
                                                       static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO*>(info));
    d5->Release();
}
static ID3D12GraphicsCommandList4* list4(PcObj l) {
    ID3D12GraphicsCommandList4* l4 = nullptr;
    if (FAILED(L->QueryInterface(IID_PPV_ARGS(&l4)))) return nullptr;
    l4->Release();   // the list keeps itself alive
    return l4;
}
void list_rt_build(PcObj l, const void* desc, uint32_t npost, const void* post) {
    if (auto* l4 = list4(l))
        l4->BuildRaytracingAccelerationStructure(static_cast<const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*>(desc), npost,
                                                 static_cast<const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*>(post));
}
void list_rt_postbuild(PcObj l, const void* desc, uint32_t n, const uint64_t* src) {
    if (auto* l4 = list4(l))
        l4->EmitRaytracingAccelerationStructurePostbuildInfo(static_cast<const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*>(desc), n, src);
}
void list_write_immediate(PcObj l, uint32_t n, const uint64_t* addrs, const uint32_t* values) {
    ID3D12GraphicsCommandList2* l2 = nullptr;
    if (FAILED(L->QueryInterface(IID_PPV_ARGS(&l2)))) return;
    std::vector<D3D12_WRITEBUFFERIMMEDIATE_PARAMETER> p(n);
    for (uint32_t i = 0; i < n; i++) p[i] = {addrs[i], values[i]};
    l2->WriteBufferImmediate(n, p.data(), nullptr);
    l2->Release();
}
HRESULT_T create_rs_deserializer(const void* data, size_t size, bool versioned, PcObj* out) {
    *out = nullptr;
    if (versioned)
        return D3D12CreateVersionedRootSignatureDeserializer(data, size, IID_PPV_ARGS(reinterpret_cast<ID3D12VersionedRootSignatureDeserializer**>(out)));
    return D3D12CreateRootSignatureDeserializer(data, size, IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignatureDeserializer**>(out)));
}
const void* rs_deserializer_desc(PcObj d) { return as<ID3D12RootSignatureDeserializer>(d)->GetRootSignatureDesc(); }
HRESULT_T rs_vdeserializer_desc(PcObj d, int version, const void** out) {
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* p = nullptr;
    HRESULT hr = as<ID3D12VersionedRootSignatureDeserializer>(d)->GetRootSignatureDescAtVersion((D3D_ROOT_SIGNATURE_VERSION)version, &p);
    *out = p;
    return hr;
}
const void* rs_vdeserializer_unconverted(PcObj d) { return as<ID3D12VersionedRootSignatureDeserializer>(d)->GetUnconvertedRootSignatureDesc(); }
void list_depth_bounds(PcObj l, float lo, float hi) {
    ID3D12GraphicsCommandList1* l1 = nullptr;
    if (SUCCEEDED(L->QueryInterface(IID_PPV_ARGS(&l1)))) { l1->OMSetDepthBounds(lo, hi); l1->Release(); }
}
void list_rt_copy(PcObj l, uint64_t dst, uint64_t src, uint32_t mode) {
    if (auto* l4 = list4(l)) l4->CopyRaytracingAccelerationStructure(dst, src, (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE)mode);
}
// Scratch ring for rewritten indirect arguments (default heap, COPY_DEST between uses).
struct ArgRing { ID3D12Resource* res = nullptr; uint64_t head = 0; std::mutex mu; } g_arg_ring;
constexpr uint64_t kArgRing = 16ull << 20;
static bool fixed_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t ao, PcObj cnt, uint64_t co, uint32_t stride) {
    if (!max || !stride || max > 4096 || !g_main_device) return false;
    uint64_t bytes = ((uint64_t)max * stride + 255) & ~255ull, off;
    {
        std::lock_guard<std::mutex> g(g_arg_ring.mu);
        if (!g_arg_ring.res) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = kArgRing;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(&g_arg_ring.res))))
                return false;
            XSL_INFO(CH, "console 1-D and mesh-X indirect dispatches run from rewritten arguments (Y = Z = 1)");
        }
        if (g_arg_ring.head + bytes > kArgRing) g_arg_ring.head = 0;
        off = g_arg_ring.head;
        g_arg_ring.head += bytes;
    }
    ID3D12Resource* a = as<ID3D12Resource>(args);
    if (ao + (uint64_t)max * stride > a->GetDesc().Width) return false;
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES x, D3D12_RESOURCE_STATES y) {
        D3D12_RESOURCE_BARRIER b{};
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = x;
        b.Transition.StateAfter = y;
        L->ResourceBarrier(1, &b);
    };
    // The arguments are in INDIRECT_ARGUMENT, where the title leaves them for ExecuteIndirect.
    const auto ind = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    barrier(a, ind, (D3D12_RESOURCE_STATES)(ind | D3D12_RESOURCE_STATE_COPY_SOURCE));
    L->CopyBufferRegion(g_arg_ring.res, off, a, ao, (uint64_t)max * stride);
    barrier(a, (D3D12_RESOURCE_STATES)(ind | D3D12_RESOURCE_STATE_COPY_SOURCE), ind);
    std::vector<uint64_t> wa;
    std::vector<uint32_t> wv;
    uint64_t base = g_arg_ring.res->GetGPUVirtualAddress() + off;
    for (uint32_t i = 0; i < max; i++) {
        wa.push_back(base + (uint64_t)i * stride + 4); wv.push_back(1);
        wa.push_back(base + (uint64_t)i * stride + 8); wv.push_back(1);
    }
    list_write_immediate(l, (uint32_t)wa.size(), wa.data(), wv.data());
    barrier(g_arg_ring.res, D3D12_RESOURCE_STATE_COPY_DEST, ind);
    L->ExecuteIndirect(as<ID3D12CommandSignature>(sig), max, g_arg_ring.res, off, as<ID3D12Resource>(cnt), co);
    barrier(g_arg_ring.res, ind, D3D12_RESOURCE_STATE_COPY_DEST);
    return true;
}
void list_execute_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t ao, PcObj cnt, uint64_t co) {
    std::string tn;
    if (trace_writers()) note_reads(l, true);   // the views of indirect (compute) dispatches too
    if (trace_dispatch_size(l, "ExecuteIndirect max", max, cnt ? 1 : 0, 0, &tn)) {
        static int recorded = 0;
        if (recorded++ < 24) {
            long slot = record_indirect(l, args, ao, cnt, co);
            std::lock_guard<std::mutex> g(g_wr_mu);
            if (slot >= 0) g_ei_trace.push_back({slot, tn, g_wr_present.load()});
        }
    }
    note_dispatch(l);
    {   // ray dispatches need a ray-tracing pipeline, which is not translated yet (inert state objects)
        std::lock_guard<std::mutex> g(g_rays_mu);
        if (g_rays_sigs.count(sig)) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 2) XSL_WARN(CH, "indirect ray dispatch skipped (ray tracing is not translated yet)");
            return;
        }
    }
    uint32_t fix_stride = 0;
    {
        std::lock_guard<std::mutex> g(g_rays_mu);
        auto it = g_fix_sigs.find(sig);
        if (it != g_fix_sigs.end()) fix_stride = it->second;
    }
    if (fix_stride && fixed_indirect(l, sig, max, args, ao, cnt, co, fix_stride)) return;
    if (g_info) check_rs(l, "ExecuteIndirect");
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
void list_set_heaps(PcObj l, uint32_t n, PcObj* h) {
    if (gm_enabled()) {
        bool global = false;
        for (uint32_t i = 0; i < n; i++) global |= as<ID3D12DescriptorHeap>(h[i]) == g_gv;
        std::pair<PcObj, PcObj> cur{};
        {
            std::lock_guard<std::mutex> g(g_gm_mu);
            g_list_global_heap[l] = global;
            cur = g_list_gm_rs[l];
        }
        L->SetDescriptorHeaps(n, reinterpret_cast<ID3D12DescriptorHeap* const*>(h));
        if (global) { gm_bind(l, cur.first, false); gm_bind(l, cur.second, true); }   // root signatures set before the heap
        return;
    }
    L->SetDescriptorHeaps(n, reinterpret_cast<ID3D12DescriptorHeap* const*>(h));
}
void list_gfx_root_table(PcObj l, uint32_t i, uint64_t h) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_gfx_tables[l][i] = h; }
    L->SetGraphicsRootDescriptorTable(i, gpu(h));
}
// During the examined frame: every pass that reads one of the frame's large render targets (or a
// 1080p texture) through an SRV in its first 64 table slots.
static void note_reads(PcObj l, bool compute) {
    uint64_t at = rt_stats_at(), now = g_wr_present.load();
    static volatile long dbg = 0;
    if (trace_writers() && InterlockedIncrement(&dbg) % 20000 == 1) XSL_INFO(CH, "reader check: now %llu at %llu gv %p", (unsigned long long)now, (unsigned long long)at, (void*)g_gv);
    if (!trace_writers() || !g_gv || !at || now + 6 < at || now >= at) return;
    std::lock_guard<std::mutex> g(g_wr_mu);
    static uint64_t calls = 0, with_tables = 0, srvs = 0;
    if (++calls % 500 == 0) XSL_INFO(CH, "reader scan: %llu passes, %llu with tables, %llu SRVs seen, %zu SRV handles known", (unsigned long long)calls,
                                     (unsigned long long)with_tables, (unsigned long long)srvs, g_srv_res.size());
    auto& tabs = compute ? g_cs_tables : g_gfx_tables;
    auto t = tabs.find(l);
    if (t == tabs.end()) return;
    with_tables++;
    uint64_t gpu0 = g_gv->GetGPUDescriptorHandleForHeapStart().ptr, cpu0 = g_gv->GetCPUDescriptorHandleForHeapStart().ptr;
    PcObj pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr;
    wchar_t wn[128] = L"?";
    UINT sz = sizeof wn;
    if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
    // XSL_TRACE_PSO=<name substring>: every view in that pipeline's tables, with its format.
    static char want[512] = "";
    static bool wi = (GetEnvironmentVariableA("XSL_TRACE_PSO", want, sizeof want), true);
    (void)wi;
    if (want[0]) {
        char nm[128];
        snprintf(nm, sizeof nm, "%ls", wn);
        if (strstr(nm, want)) {
            static int shown = 0;
            if (shown++ < 3)
                for (auto& [idx, h] : t->second) {
                    uint64_t c = sv_gpu_to_cpu(h);
                    if (!c) continue;
                    (void)gpu0; (void)cpu0;
                    for (uint32_t k = 0; k < 16; k++) {
                        if (auto cb = g_cbv_va.find(c + (uint64_t)k * g_gv_inc); cb != g_cbv_va.end()) {
                            const float* f = nullptr;
                            auto m = g_mapped.upper_bound(cb->second);
                            if (m != g_mapped.begin() && (--m, cb->second < m->first + m->second.first)) f = (const float*)(m->second.second + (cb->second - m->first));
                            g_traced_cbvs.push_back(cb->second);
                            if (f)
                                XSL_INFO(CH, "pso %s table %u slot %u: CBV %llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", nm, idx, k,
                                         (unsigned long long)cb->second, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
                            else XSL_INFO(CH, "pso %s table %u slot %u: CBV %llx (memory not mapped)", nm, idx, k, (unsigned long long)cb->second);
                            continue;
                        }
                        auto it = g_srv_res.find(c + (uint64_t)k * g_gv_inc);
                        auto ju = g_uav_res.find(c + (uint64_t)k * g_gv_inc);
                        PcObj r = it != g_srv_res.end() ? it->second : ju != g_uav_res.end() ? ju->second : nullptr;
                        if (!r) continue;
                        D3D12_RESOURCE_DESC d = as<ID3D12Resource>(r)->GetDesc();
                        D3D12_HEAP_PROPERTIES hp{};
                        D3D12_HEAP_FLAGS hf{};
                        bool committed = SUCCEEDED(as<ID3D12Resource>(r)->GetHeapProperties(&hp, &hf));
                        XSL_INFO(CH, "pso %s table %u slot %u: %s %p %llux%u fmt %d (%s heap type %d cpu page %d) va %llx", nm, idx, k, it != g_srv_res.end() ? "SRV" : "UAV", r,
                                 (unsigned long long)d.Width, d.Height, (int)d.Format, committed ? "committed" : "placed", (int)hp.Type, (int)hp.CPUPageProperty,
                                 d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? (unsigned long long)as<ID3D12Resource>(r)->GetGPUVirtualAddress() : 0ull);
                    }
                }
        }
    }
    for (auto& [idx, h] : t->second) {
        uint64_t c = sv_gpu_to_cpu(h);
        if (!c) continue;
        (void)gpu0; (void)cpu0;
        for (uint32_t k = 0; k < 64; k++) {
            auto it = g_srv_res.find(c + (uint64_t)k * g_gv_inc);
            if (it == g_srv_res.end() || !it->second) continue;
            srvs++;
            D3D12_RESOURCE_DESC d = as<ID3D12Resource>(it->second)->GetDesc();
            if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width < 1024) continue;
            XSL_INFO(CH, "frame reader: %s %ls reads %p (%llux%u fmt %d)", compute ? "compute" : "draw", wn, it->second,
                     (unsigned long long)d.Width, d.Height, (int)d.Format);
        }
    }
}
void list_compute_root_table(PcObj l, uint32_t i, uint64_t h) {
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_cs_tables[l][i] = h; }
    L->SetComputeRootDescriptorTable(i, gpu(h));
}
void list_gfx_root_cbv(PcObj l, uint32_t i, uint64_t va) { L->SetGraphicsRootConstantBufferView(i, va); }
void list_compute_root_cbv(PcObj l, uint32_t i, uint64_t va) { L->SetComputeRootConstantBufferView(i, va); }
void list_gfx_root_srv(PcObj l, uint32_t i, uint64_t va) { L->SetGraphicsRootShaderResourceView(i, va); }
void list_compute_root_srv(PcObj l, uint32_t i, uint64_t va) { L->SetComputeRootShaderResourceView(i, va); }
// XSL_TRACE_BUFFER=<size>: root UAVs into buffers of that size during the examined frame.
static void trace_root_uav(PcObj l, uint32_t i, uint64_t va, const char* kind) {
    static uint64_t tb = [] { char b[24]; return GetEnvironmentVariableA("XSL_TRACE_BUFFER", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
    if (!tb || !trace_writers()) return;
    std::lock_guard<std::mutex> g(g_buf_va_mu);
    auto m = g_buf_va.upper_bound(va);
    if (m == g_buf_va.begin() || (--m, va >= m->first + m->second.second) || m->second.second != tb) return;
    PcObj pso = nullptr;
    { std::lock_guard<std::mutex> g2(g_wr_mu); pso = g_wr_pso.count(l) ? g_wr_pso[l] : nullptr; }
    wchar_t wn[128] = L"?";
    UINT sz = sizeof wn;
    if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
    XSL_INFO(CH, "frame %s root UAV %u of buffer %p +%llu: %ls", kind, i, (void*)m->second.first, (unsigned long long)(va - m->first), wn);
}
void list_gfx_root_uav(PcObj l, uint32_t i, uint64_t va) { trace_root_uav(l, i, va, "graphics"); L->SetGraphicsRootUnorderedAccessView(i, va); }
void list_compute_root_uav(PcObj l, uint32_t i, uint64_t va) { trace_root_uav(l, i, va, "compute"); L->SetComputeRootUnorderedAccessView(i, va); }
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
    if (!ok) return false;
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
    static uint64_t tb = [] { char b[24]; return GetEnvironmentVariableA("XSL_TRACE_BUFFER", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
    if (tb && trace_writers() && as<ID3D12Resource>(dst)->GetDesc().Width == tb)
        XSL_INFO(CH, "frame copy into buffer %p +%llu: %llu bytes from %p (%llu bytes) +%llu", (void*)dst, (unsigned long long)doff, (unsigned long long)n, (void*)src,
                 (unsigned long long)as<ID3D12Resource>(src)->GetDesc().Width, (unsigned long long)soff);
    L->CopyBufferRegion(as<ID3D12Resource>(dst), doff, as<ID3D12Resource>(src), soff, n);
}
void warn_unaligned_copy(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& p) {
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 4)
        XSL_WARN(CH, "texture copy with a buffer footprint at offset %llu (format %d, %ux%u, pitch %u) cannot be realigned: skipped",
                 (unsigned long long)p.Offset, (int)p.Footprint.Format, p.Footprint.Width, p.Footprint.Height, p.Footprint.RowPitch);
}

// Staging for source footprints that cannot be rebased (the rows fill their pitch, so there is no
// room to shift into): the footprint's bytes are copied on the CPU, when the copy is recorded,
// from the (CPU-visible) source buffer to a 512-aligned spot in a layer-owned upload ring, and
// the copy reads from there. Titles write upload data before recording the copy, as for the
// constant-buffer re-alignment. One ring segment per presented frame.
std::atomic<uint64_t> g_stage_frames{0};
struct StageRing {
    static constexpr uint64_t kSegment = 64ull << 20;
    static constexpr uint32_t kSegments = 4;
    std::mutex mu;
    ID3D12Resource* res = nullptr;
    uint8_t* cpu = nullptr;
    uint64_t frame = UINT64_MAX, head = 0;
} g_stage;
bool stage_footprint(ID3D12GraphicsCommandList* l, D3D12_TEXTURE_COPY_LOCATION* src) {
    auto* buf = src->pResource;
    D3D12_RESOURCE_DESC bd = buf->GetDesc();
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& p = src->PlacedFootprint;
    FormatSize fs = format_size(p.Footprint.Format);
    if (!fs.bytes) return false;
    uint64_t rows = (p.Footprint.Height + fs.block - 1) / fs.block;
    uint64_t bytes = (uint64_t)p.Footprint.RowPitch * rows * p.Footprint.Depth;
    if (p.Offset + bytes > bd.Width) bytes = bd.Width - p.Offset;  // the last row may end short of the pitch
    if (bytes > StageRing::kSegment) return false;
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(buf->Map(0, &none, &mapped))) return false;  // not CPU-visible
    std::lock_guard<std::mutex> g(g_stage.mu);
    if (!g_stage.res) {
        ID3D12Device* dev = nullptr;
        l->GetDevice(IID_PPV_ARGS(&dev));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = StageRing::kSegment * StageRing::kSegments;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&g_stage.res));
        dev->Release();
        if (FAILED(hr) || FAILED(g_stage.res->Map(0, &none, (void**)&g_stage.cpu))) {
            buf->Unmap(0, &none);
            return false;
        }
        XSL_INFO(CH, "texture copies from unaligned buffer footprints are staged through a %llu MB upload ring",
                 (unsigned long long)(rd.Width >> 20));
    }
    uint64_t frame = g_stage_frames.load();
    if (frame != g_stage.frame) { g_stage.frame = frame; g_stage.head = 0; }
    if (g_stage.head + bytes > StageRing::kSegment) { buf->Unmap(0, &none); return false; }
    uint64_t off = (frame % StageRing::kSegments) * StageRing::kSegment + g_stage.head;
    memcpy(g_stage.cpu + off, (uint8_t*)mapped + p.Offset, bytes);
    buf->Unmap(0, &none);
    g_stage.head += (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    src->pResource = g_stage.res;
    src->PlacedFootprint.Offset = off;
    return true;
}

void list_copy_texture_region(PcObj l, const void* dst, uint32_t x, uint32_t y, uint32_t z, const void* src, const void* box) {
    plane_op(l, (PcObj) static_cast<const D3D12_TEXTURE_COPY_LOCATION*>(dst)->pResource, "copy region");
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
        if (!rebase_footprint(&d.PlacedFootprint, &x0, &y0)) { warn_unaligned_copy(d.PlacedFootprint); return; }
        x += x0;
        y += y0;
    }
    if (misaligned_footprint(s)) {
        const D3D12_SUBRESOURCE_FOOTPRINT f = s.PlacedFootprint.Footprint;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT orig = s.PlacedFootprint;
        if (rebase_footprint(&s.PlacedFootprint, &x0, &y0)) {
            b = pb ? *pb : D3D12_BOX{0, 0, 0, f.Width, f.Height, f.Depth};
            b.left += x0;
            b.right += x0;
            b.top += y0;
            b.bottom += y0;
            pb = &b;
        } else if (!stage_footprint(L, &s)) {
            warn_unaligned_copy(orig);
            return;
        }
    }
    L->CopyTextureRegion(&d, x, y, z, &s, pb);
}
void list_copy_resource(PcObj l, PcObj dst, PcObj src) {
    static uint64_t tb = [] { char b[24]; return GetEnvironmentVariableA("XSL_TRACE_BUFFER", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
    if (tb && trace_writers() && g_wr_present.load() + 1 == rt_stats_at() && as<ID3D12Resource>(dst)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        as<ID3D12Resource>(dst)->GetDesc().Width == tb)
        XSL_INFO(CH, "frame copy of resource %p into buffer %p", (void*)src, (void*)dst);
    plane_op(l, dst, "copy resource"); L->CopyResource(as<ID3D12Resource>(dst), as<ID3D12Resource>(src)); }
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

std::set<PcObj> g_plane_lists;   // XSL_PLANE_OPS: lists that wrote a presented image this frame
void queue_execute(PcObj q, uint32_t n, PcObj* lists) {
    if (trace_writers() && plane_ops_now()) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        for (uint32_t i = 0; i < n; i++)
            XSL_INFO(CH, "plane op: queue %p executes list %p%s", (void*)q, (void*)lists[i], g_plane_lists.count(lists[i]) ? " (wrote the plane)" : "");
    }
    as<ID3D12CommandQueue>(q)->ExecuteCommandLists(n, reinterpret_cast<ID3D12CommandList* const*>(lists));
}
HRESULT_T queue_signal(PcObj q, PcObj f, uint64_t v) {
    if (g_dred) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 400) XSL_INFO(CH, "queue %p signals fence %p = %llu", q, f, (unsigned long long)v);
    }
    return as<ID3D12CommandQueue>(q)->Signal(as<ID3D12Fence>(f), v);
}
HRESULT_T queue_wait(PcObj q, PcObj f, uint64_t v) {
    if (g_dred && f) {   // diagnostics: GPU waits that are not satisfied yet when submitted
        uint64_t done = as<ID3D12Fence>(f)->GetCompletedValue();
        if (done < v) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 400) XSL_INFO(CH, "queue %p waits for fence %p >= %llu (completed %llu)", q, f, (unsigned long long)v, (unsigned long long)done);
        }
    }
    return as<ID3D12CommandQueue>(q)->Wait(as<ID3D12Fence>(f), v);
}

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
void capture_plane(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12Resource* res, const char* path,
                   D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT) {
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
    if (HRESULT hrr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)); FAILED(hrr)) {
        XSL_WARN(CH, "capture: readback buffer (%llu bytes) failed %08lx", (unsigned long long)total, hrr);
        return;
    }
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {res, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE};   // only subresource 0 is copied (its state is the one tracked)
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
    DWORD waited = WaitForSingleObject(e, 2000);
    CloseHandle(e);
    if (waited != WAIT_OBJECT_0) {
        XSL_WARN(CH, "capture: copy did not finish (device %08lx)", dev->GetDeviceRemovedReason());
        f->Release(); l->Release(); a->Release(); rb->Release();
        return;
    }
    void* p = nullptr;
    D3D12_RANGE r{0, (SIZE_T)total};
    HRESULT mapHr = rb->Map(0, &r, &p);
    if (FAILED(mapHr)) XSL_WARN(CH, "capture: map failed %08lx (device %08lx)", mapHr, dev->GetDeviceRemovedReason());
    if (SUCCEEDED(mapHr) && !path) {   // statistics only: share of non-zero texels
        UINT w = fp.Footprint.Width, h = fp.Footprint.Height;
        size_t bpt = fp.Footprint.RowPitch / (w ? w : 1) >= 8 ? 8 : 4, nz = 0, n = 0;
        uint32_t mx = 0;
        // R11G11B10_FLOAT: per-channel mean of finite values and counts of NaN/infinite texels
        const bool f111110 = rd.Format == DXGI_FORMAT_R11G11B10_FLOAT;
        double sum[3] = {};
        size_t nan = 0, inf = 0;
        auto small_float = [](uint32_t bits, int mant, bool& isnan, bool& isinf) {
            uint32_t e = bits >> mant, m = bits & ((1u << mant) - 1);
            isnan = e == 31 && m;
            isinf = e == 31 && !m;
            if (e == 31) return 0.0;
            return e ? std::ldexp(1.0 + m / double(1u << mant), (int)e - 15) : std::ldexp(m / double(1u << mant), -14);
        };
        for (UINT y = 0; y < h; y += 7)
            for (UINT x = 0; x < w; x += 7, n++) {
                auto* t = (uint8_t*)p + fp.Offset + (size_t)y * fp.Footprint.RowPitch + x * bpt;
                uint32_t v = bpt == 4 ? (t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24) : (t[0] | t[1] << 8 | t[2] << 16 | t[3] << 24 | t[4] | t[5] | t[6] | t[7]);
                if (v) nz++;
                mx = std::max<uint32_t>(mx, t[0] | t[1] | t[2] | (bpt == 8 || f111110 ? t[3] : 0));
                if (f111110) {
                    bool a, b, c2[3] = {};
                    bool any_nan = false, any_inf = false;
                    const uint32_t ch[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22};
                    for (int k = 0; k < 3; k++) {
                        sum[k] += small_float(ch[k], k < 2 ? 6 : 5, a, b);
                        any_nan |= a;
                        any_inf |= b;
                        (void)c2;
                    }
                    nan += any_nan;
                    inf += any_inf;
                }
            }
        if (f111110 && n)
            XSL_INFO(CH, "%s stats (present %llu, %llux%u format %d): %.1f%% non-zero, mean %g %g %g, NaN %.1f%%, infinite %.1f%%", state == D3D12_RESOURCE_STATE_PRESENT ? "plane" : "target",
                     (unsigned long long)g_present_count, (unsigned long long)rd.Width, rd.Height, rd.Format, 100.0 * nz / n, sum[0] / n, sum[1] / n, sum[2] / n,
                     100.0 * nan / n, 100.0 * inf / n);
        else
        XSL_INFO(CH, "%s stats (present %llu, %llux%u format %d): %.1f%% non-zero, max byte %u", state == D3D12_RESOURCE_STATE_PRESENT ? "plane" : "target",
                 (unsigned long long)g_present_count, (unsigned long long)rd.Width, rd.Height, rd.Format, n ? 100.0 * nz / n : 0.0, mx);
        rb->Unmap(0, nullptr);
    } else if (SUCCEEDED(mapHr)) {
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
                if (rd.Format == DXGI_FORMAT_R10G10B10A2_UNORM || rd.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
                    for (UINT x = 0; x < w; x++) {   // 10-bit channels to 8-bit BGR
                        uint32_t v;
                        memcpy(&v, row + x * 4, 4);
                        row[x * 4 + 0] = uint8_t(((v >> 20) & 0x3ff) >> 2);
                        row[x * 4 + 1] = uint8_t(((v >> 10) & 0x3ff) >> 2);
                        row[x * 4 + 2] = uint8_t((v & 0x3ff) >> 2);
                    }
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
    if (trace_writers()) {
        static uint64_t n = 0;
        if (++n % 60 == 0) {
            std::lock_guard<std::mutex> g(g_wr_mu);
            auto it = g_wr_last.find(resObj);
            wchar_t name[128] = L"(none)";
            if (it != g_wr_last.end() && it->second.first) {
                UINT sz = sizeof name;
                if (FAILED(as<ID3D12PipelineState>(it->second.first)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, name))) wcscpy(name, L"(unnamed)");
            }
            XSL_INFO(CH, "present %llu: image %p last drawn by %ls (%llu draws so far)", (unsigned long long)n, resObj, name,
                     it == g_wr_last.end() ? 0ull : (unsigned long long)it->second.second);
        }
    }
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
        ++g_gm_frame;
        if (getenv("XSL_AS_DUMP")) print_dumps();
        if (g_info && g_present_count % 30 == 0) dump_debug_messages();   // debug layer: what it reported since the last dump
        if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_planes.insert((PcObj)res); }
        if (trace_writers() && plane_ops_now()) { XSL_INFO(CH, "plane op: present %llu of %p on queue %p", (unsigned long long)g_present_count, (void*)res, (void*)q); report_aliases((PcObj)res); }
        g_wr_present = g_present_count;
        if (trace_writers() && g_ei_map) {   // traced indirect dispatches: their arguments, once the GPU has written them
            std::lock_guard<std::mutex> g(g_wr_mu);
            for (size_t i = 0; i < g_ei_trace.size();) {
                auto& e = g_ei_trace[i];
                if (e.present + 3 > g_present_count) { i++; continue; }
                const uint32_t* w = g_ei_map + e.slot * (kEiBytes / 4);
                XSL_INFO(CH, "pso %s: indirect arguments %08x %08x %08x %08x %08x %08x %08x %08x, count %08x (present %llu)", e.name.c_str(), w[0], w[1], w[2], w[3],
                         w[4], w[5], w[6], w[7], w[8], (unsigned long long)e.present);
                g_ei_trace.erase(g_ei_trace.begin() + i);
            }
        }
        // XSL_PLANE_STATS=<n>: every n presents, log how much of the title's plane is non-black.
        static uint64_t statsEvery = [] { char b[16]; return GetEnvironmentVariableA("XSL_PLANE_STATS", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
        if (statsEvery && g_present_count % statsEvery == 0) capture_plane(dev, q, res, nullptr);
        // XSL_RT_STATS=<present> (with XSL_TRACE_WRITERS=1): every 2D render target drawn during
        // the frame before that present, with the pipeline that drew it last.
        static uint64_t rtAt = [] { char b[16]; return GetEnvironmentVariableA("XSL_RT_STATS", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
        if (rtAt && g_present_count == rtAt) {
            std::vector<std::pair<PcObj, PcObj>> targets;
            {
                std::lock_guard<std::mutex> g(g_wr_mu);
                for (auto& [r, f] : g_wr_frame)
                    if (f + 1 >= g_present_count) targets.push_back({r, g_wr_last[r].first});
            }
            {   // the traced pipeline's constants, after the submissions that refresh constant copies
                std::lock_guard<std::mutex> g(g_wr_mu);
                for (uint64_t va : g_traced_cbvs) {
                    auto m = g_mapped.upper_bound(va);
                    if (m == g_mapped.begin() || (--m, va >= m->first + m->second.first)) continue;
                    const float* f = (const float*)(m->second.second + (va - m->first));
                    XSL_INFO(CH, "at present: CBV %llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", (unsigned long long)va, f[0], f[1], f[2],
                             f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
                }
            }
            std::vector<PcObj> big;
            {
                std::lock_guard<std::mutex> g(g_wr_mu);
                big = g_big_textures;
                for (auto& [r, w] : g_cs_writer) {   // compute-written textures that were not noted at creation
                    if (std::find(big.begin(), big.end(), r) != big.end()) continue;
                    D3D12_RESOURCE_DESC d = as<ID3D12Resource>(r)->GetDesc();
                    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= 1920 && d.Height >= 1080) big.push_back(r);
                }
            }
            for (PcObj r : big) {   // 1080p textures (written by compute passes too), in their last transitioned state
                if (as<ID3D12Resource>(r) == res) continue;
                uint32_t st;
                { std::lock_guard<std::mutex> g(g_wr_mu); auto it = g_last_state.find(r); if (it == g_last_state.end()) continue; st = it->second; }
                if (st == D3D12_RESOURCE_STATE_COPY_SOURCE) continue;
                switch (as<ID3D12Resource>(r)->GetDesc().Format) {   // colour targets only
                    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
                    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R16_FLOAT: break;
                    default: continue;
                }
                wchar_t wn[128] = L"(no compute writer seen)";
                {
                    std::lock_guard<std::mutex> g(g_wr_mu);
                    auto w = g_cs_writer.find(r);
                    UINT sz = sizeof wn;
                    if (w != g_cs_writer.end() && w->second.first) as<ID3D12PipelineState>(w->second.first)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, wn);
                }
                XSL_INFO(CH, "texture %p (format %d, state %#x), last compute writer %ls:", r, (int)as<ID3D12Resource>(r)->GetDesc().Format, st, wn);
                capture_plane(dev, q, as<ID3D12Resource>(r), nullptr, (D3D12_RESOURCE_STATES)st);
            }
            for (auto& [r, pso] : targets) {
                auto* tr = as<ID3D12Resource>(r);
                if (tr == res || tr->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || tr->GetDesc().SampleDesc.Count > 1) continue;
                wchar_t name[128] = L"(unnamed)";
                UINT sz = sizeof name;
                if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, name);
                XSL_INFO(CH, "render target %p last drawn by %ls:", r, name);
                capture_plane(dev, q, tr, nullptr, D3D12_RESOURCE_STATE_RENDER_TARGET);
                // XSL_RT_DUMP=<dir>: also write the larger 4-byte targets as BMPs (raw bytes).
                static char dumpDir[MAX_PATH] = "";
                static bool dumpInit = (GetEnvironmentVariableA("XSL_RT_DUMP", dumpDir, sizeof dumpDir), true);
                (void)dumpInit;
                D3D12_RESOURCE_DESC td = tr->GetDesc();
                DXGI_FORMAT f = td.Format;
                bool four = f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
                            f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                            f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
                if (dumpDir[0] && four && td.Width >= 512) {
                    char path[MAX_PATH];
                    snprintf(path, sizeof path, "%s\\rt_%p_f%d.bmp", dumpDir, r, (int)f);
                    capture_plane(dev, q, tr, path, D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }
        }
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
    g_stage_frames++;
    update_fps_title(hwnd);
    q->Signal(s.fence, ++s.fenceValue);
    s.slotValue[s.slot] = s.fenceValue;
    s.slot = (s.slot + 1) % 3;
    if (FAILED(hr)) XSL_ERR(CH, "Present failed %08lx", hr);
    return hr;
}

}  // namespace pc
