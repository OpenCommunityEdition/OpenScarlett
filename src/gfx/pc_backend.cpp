// SPDX-License-Identifier: AGPL-3.0-or-later
// PC half of d3d12_xs.dll: PC Direct3D 12 (Windows runtime, or Wine/vkd3d).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <unordered_map>
#include <set>
#include <psapi.h>
#include <thread>
#include <windows.h>
#include <tlhelp32.h>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.h"
#include "rtso.h"
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
// Diagnostics: the first queue-level call after which the device is seen removed (an invalid call
// removes it without any failing HRESULT at the call itself).
// XSL_REMOVED_TRACE=1 turns it on: the check takes the device's lock, and on every view a title
// creates that holds up its render threads (one stopped presenting for good).
static void removed_after(const char* what, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    static volatile long seen = 0;
    static const bool on = [] { char e[4]; return GetEnvironmentVariableA("XSL_REMOVED_TRACE", e, 4) && e[0] == '1'; }();
    if (!on || seen || !g_main_device) return;
    HRESULT r = g_main_device->GetDeviceRemovedReason();
    if (r == S_OK || InterlockedExchange(&seen, 1)) return;
    XSL_ERR(CH, "device first seen removed (%08lx) after %s (%llx %llx %llx)", r, what, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}
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
static void prof_report_now();
void report_device_removed(ID3D12Device* dev) {
    xsl::log_arm(false);
    static volatile long once = 0;
    if (!dev || InterlockedExchange(&once, 1)) return;
    XSL_ERR(CH, "device removed: reason %08lx", dev->GetDeviceRemovedReason());
    prof_report_now();   // (a profiled frame's times, when the device went before their report)
    dump_debug_messages();
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
// XSL_CRASH_DUMP=<file> (diagnostics): on the first access violation or breakpoint in the title's
// executable (XSL_CRASH_DUMP_AT=<rva hex>: only at that address), a minidump with the modules' data
// segments and the memory the stacks point at; titles' own crash dumps leave their globals out.
static void crash_dump(EXCEPTION_POINTERS* e) {
    static char path[MAX_PATH] = "";
    static bool init = false;
    static volatile long done = 0;
    if (!init) { init = true; GetEnvironmentVariableA("XSL_CRASH_DUMP", path, sizeof path); }
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (!path[0] || (c != 0xC0000005 && c != 0x80000003)) return;
    auto* exe = (const uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionAddress, b = (uintptr_t)exe;
    if (a < b || a >= b + nt->OptionalHeader.SizeOfImage) return;
    char at[32];
    if (GetEnvironmentVariableA("XSL_CRASH_DUMP_AT", at, sizeof at) && _strtoui64(at, nullptr, 16) != a - b) return;
    if (InterlockedExchange(&done, 1)) return;
    // XSL_CRASH_PEEK=<item>,...: bytes of the title's memory at that moment, logged. An item is
    // rva:len (address in the executable) or *rva:off:len (the pointer stored at rva, plus off).
    char peek[1024];
    if (GetEnvironmentVariableA("XSL_CRASH_PEEK", peek, sizeof peek)) {
        for (char* it = strtok(peek, ","); it; it = strtok(nullptr, ",")) {
            // leading '*'s: pointers followed; then rva, one offset per pointer, and the length
            int levels = 0;
            char* q = it;
            while (*q == '*') { levels++; q++; }
            uint64_t vals[8] = {};
            int nv = 0;
            vals[nv++] = _strtoui64(q, &q, 16);
            while (*q == ':' && nv < 8) vals[nv++] = _strtoui64(q + 1, &q, 16);
            uint64_t len = nv > levels + 1 ? vals[levels + 1] : 8;
            uintptr_t at = b + vals[0];
            bool bad = false;
            for (int l = 0; l < levels && !bad; l++) {
                uintptr_t ptr = 0;
                if (IsBadReadPtr((void*)at, 8)) { bad = true; break; }
                memcpy(&ptr, (void*)at, 8);
                at = ptr + (l + 1 < nv ? vals[l + 1] : 0);
            }
            if (bad) at = 0;
            len = std::min<uint64_t>(len ? len : 8, 64);
            std::string hex;
            if (!at || IsBadReadPtr((void*)at, (UINT_PTR)len)) hex = "(unreadable)";
            else for (uint64_t k = 0; k < len; k++) { char h[4]; snprintf(h, sizeof h, "%02x", ((const uint8_t*)at)[k]); hex += h; }
            XSL_ERR(CH, "peek %s -> %llx: %s", it, (unsigned long long)at, hex.c_str());
        }
    }
    typedef BOOL(WINAPI * Write)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    HMODULE dh = LoadLibraryW(L"dbghelp.dll");
    auto write = dh ? (Write)GetProcAddress(dh, "MiniDumpWriteDump") : nullptr;
    if (!strcmp(path, "peek")) return;   // XSL_CRASH_DUMP=peek: the peeks only
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (!write || f == INVALID_HANDLE_VALUE) return;
    struct { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; } xi{GetCurrentThreadId(), e, FALSE};
    // MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x40 | 0x1000 | 0x20, &xi, nullptr, nullptr);
    CloseHandle(f);
    XSL_ERR(CH, "exception %08lx at exe+%llx: dump %s %s", c, (unsigned long long)(a - b), path, ok ? "written" : "failed");
}
// XSL_TRAP=<rva>,... (diagnostics, at most four): hardware execution breakpoints (debug registers;
// the title's code is not modified) on every thread of the process. Each hit logs the registers and
// the XSL_TRAP_PEEK items, [*]reg+off:len (a leading * reads the pointer stored there first);
// XSL_TRAP_MAX hits per address (default 20) are logged, after which the trap stays silent.
static uintptr_t g_trap[4];
static int g_ntrap = 0;
// XSL_TRAP_WATCH=<reg>[+off]: at the first hit of the first trap, the page of that address becomes
// read-only; each later write by code to the first 64 bytes there logs the writer and a stack
// (first 16), and file reads into the page fail.
static volatile uintptr_t g_watch = 0;
static volatile long g_watch_gen = 0;
static volatile long g_trap_hits[4];
static void trap_arm(HANDLE th) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    DWORD64* dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
    for (int i = 0; i < g_ntrap; i++) { *dr[i] = g_trap[i]; c.Dr7 |= 1ull << (2 * i); c.Dr7 &= ~(0xFull << (16 + 4 * i)); }
    SetThreadContext(th, &c);
}
// The call stack of a suspended thread, innermost first: its program counter and the return
// addresses its unwind tables give. Nothing here allocates or takes a lock of ours, because the
// suspended thread may hold the heap's lock.
static int unwind_suspended(HANDLE th, uintptr_t* pcs, int max) {
    alignas(16) CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(th, &c)) return 0;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)c.Rsp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    const uintptr_t stackLo = (uintptr_t)mbi.BaseAddress, stackHi = stackLo + mbi.RegionSize;
    int n = 0;
    while (n < max && c.Rip > 0x10000) {
        pcs[n++] = (uintptr_t)c.Rip;
        if (c.Rsp < stackLo || c.Rsp + 8 > stackHi) break;
        const DWORD64 before = c.Rsp;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fn) {   // a leaf function: its return address is at the stack pointer
            c.Rip = *(const DWORD64*)c.Rsp;
            c.Rsp += 8;
        } else {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler, &frame, nullptr);
        }
        if (c.Rsp <= before) break;   // no progress: not a stack the tables describe
    }
    return n;
}
// A code address as text: L+offset in this module, the bare offset in the title's image,
// module+offset elsewhere. (Takes the loader lock: never while a thread is suspended.)
static std::string code_address(uintptr_t pc) {
    static const HMODULE exe = GetModuleHandleW(nullptr);
    static const HMODULE self = [] {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&unwind_suspended, &m);
        return m;
    }();
    char b[96];
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m) || !m) {
        snprintf(b, sizeof b, "?%llx", (unsigned long long)pc);
        return b;
    }
    const unsigned long long off = pc - (uintptr_t)m;
    if (m == self) snprintf(b, sizeof b, "L+%llx", off);
    else if (m == exe) snprintf(b, sizeof b, "%llx", off);
    else {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* bn = strrchr(name, '\\');
        snprintf(b, sizeof b, "%.24s+%llx", bn ? bn + 1 : name, off);
    }
    return b;
}
std::atomic<DWORD> g_present_tid{0};   // the thread that presented last
// XSL_PROFILE=<from>-<to>[:<period ms>] (diagnostics): between those seconds after start-up every
// thread of the process is sampled (default every 20 ms) and, at the end, the stacks each thread
// was seen in most are logged with their share: where a title's threads spend a stall. Threads
// that only wait in the title's own code are counted, not listed.
static void profile_run(unsigned from, unsigned to, unsigned period) {
    struct Stack { uint32_t count = 0; int n = 0; uintptr_t pc[24]; };
    const ULONGLONG t0 = GetTickCount64();
    Sleep(from * 1000);
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&unwind_suspended, &self);
    uintptr_t slo = (uintptr_t)self, shi = slo;
    if (self) {
        auto* sd = (const IMAGE_DOS_HEADER*)self;
        shi = slo + ((const IMAGE_NT_HEADERS*)((const uint8_t*)self + sd->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    std::map<DWORD, std::unordered_map<uint64_t, Stack>> seen;
    std::vector<std::pair<DWORD, HANDLE>> threads;
    ULONGLONG listed = 0;
    uint32_t rounds = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)to * 1000) {
        if (GetTickCount64() - listed >= 1000) {   // the threads there are now
            listed = GetTickCount64();
            for (auto& t : threads) CloseHandle(t.second);
            threads.clear();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{sizeof te};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
                    if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID))
                        threads.emplace_back(te.th32ThreadID, th);
                }
                CloseHandle(snap);
            }
        }
        for (auto& t : threads) {
            uintptr_t pcs[24];
            if (SuspendThread(t.second) == (DWORD)-1) continue;
            const int n = unwind_suspended(t.second, pcs, 24);
            ResumeThread(t.second);
            if (!n) continue;
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < n; i++) h = (h ^ pcs[i]) * 1099511628211ull;
            Stack& s = seen[t.first][h];
            if (!s.count) { s.n = n; memcpy(s.pc, pcs, n * sizeof *pcs); }
            s.count++;
        }
        rounds++;
        Sleep(period);
    }
    for (auto& t : threads) CloseHandle(t.second);
    const HMODULE exe = GetModuleHandleW(nullptr);
    const uintptr_t elo = (uintptr_t)exe;
    const uintptr_t ehi = elo + ((const IMAGE_NT_HEADERS*)((const uint8_t*)exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew))->OptionalHeader.SizeOfImage;
    const HMODULE sysmods[] = {GetModuleHandleW(L"ntdll.dll"), GetModuleHandleW(L"KERNELBASE.dll"), GetModuleHandleW(L"KERNEL32.DLL"), GetModuleHandleW(L"win32u.dll"),
                               GetModuleHandleW(L"user32.dll")};
    // A stack that waits in a system call (the two bytes before the program counter are the
    // syscall instruction) and holds only frames of the system and of the title: the title's own
    // wait, as an idle worker's.
    auto own_wait = [&](const Stack& st) {
        const uint8_t* q = (const uint8_t*)st.pc[0];
        if (!(q[-2] == 0x0F && q[-1] == 0x05)) return false;
        for (int i = 0; i < st.n; i++) {
            if (st.pc[i] >= elo && st.pc[i] < ehi) continue;
            HMODULE m = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)st.pc[i], &m)) return false;
            if (std::find(std::begin(sysmods), std::end(sysmods), m) == std::end(sysmods)) return false;
        }
        return true;
    };
    auto text = [&](const Stack& st) {
        std::string t;
        for (int k = 0; k < st.n; k++) { t += ' '; t += code_address(st.pc[k]); }
        return t;
    };
    XSL_INFO(CH, "profile %u-%u s: %u rounds, %zu threads (the title presents on thread %lu)", from, to, rounds, seen.size(), (unsigned long)g_present_tid.load());
    std::map<std::string, std::pair<uint32_t, uint32_t>> waits;   // the title's own waits: stack -> threads, samples
    for (auto& th : seen) {
        uint32_t busy = 0, total = 0;
        std::vector<const Stack*> v;
        for (auto& kv : th.second) {
            total += kv.second.count;
            if (own_wait(kv.second)) continue;
            busy += kv.second.count;
            v.push_back(&kv.second);
        }
        const bool presents = th.first == g_present_tid.load();
        if (!presents && busy * 50 < rounds) {   // under 2 % outside its own waits: counted with the others that wait alike
            const Stack* top = nullptr;
            for (auto& kv : th.second) if (!top || kv.second.count > top->count) top = &kv.second;
            if (top) { auto& w = waits[text(*top)]; w.first++; w.second += top->count; }
            continue;
        }
        if (presents) { v.clear(); for (auto& kv : th.second) v.push_back(&kv.second); }   // the presenting thread: its waits too
        std::sort(v.begin(), v.end(), [](const Stack* a, const Stack* b) { return a->count > b->count; });
        XSL_INFO(CH, "profile: thread %lu%s: %u of %u samples outside its own waits", (unsigned long)th.first, presents ? " (presents)" : "", busy, total);
        // A busy thread is seldom seen twice in one stack: also where its samples are by the module
        // of the innermost frame, by the innermost frame of the layer and by that of the title.
        std::map<std::string, uint32_t> by[3];
        for (const Stack* st : v) {
            std::string leaf = code_address(st->pc[0]);
            if (size_t plus = leaf.find('+'); plus != std::string::npos && leaf[0] != 'L') leaf.resize(plus);
            else if (leaf[0] == 'L') leaf = "layer";
            else leaf = "title";
            by[0][leaf] += st->count;
            bool l = false, e = false;
            for (int k = 0; k < st->n && !(l && e); k++) {
                if (!l && st->pc[k] >= slo && st->pc[k] < shi) { l = true; by[1][code_address(st->pc[k])] += st->count; }
                if (!e && st->pc[k] >= elo && st->pc[k] < ehi) { e = true; by[2][code_address(st->pc[k])] += st->count; }
            }
        }
        static const char* const what[3] = {"innermost module", "innermost layer frame", "innermost title frame"};
        for (int w = 0; w < 3; w++) {
            std::vector<std::pair<uint32_t, std::string>> r;
            for (auto& kv : by[w]) r.emplace_back(kv.second, kv.first);
            std::sort(r.rbegin(), r.rend());
            std::string t;
            for (size_t i = 0; i < r.size() && i < 10; i++) { char c[96]; snprintf(c, sizeof c, " %s=%u", r[i].second.c_str(), r[i].first); t += c; }
            if (!t.empty()) XSL_INFO(CH, "profile: thread %lu: by %s:%s", (unsigned long)th.first, what[w], t.c_str());
        }
        for (size_t i = 0; i < v.size() && i < 14; i++)
            XSL_INFO(CH, "profile: thread %lu: %u (%u %%):%s", (unsigned long)th.first, v[i]->count, v[i]->count * 100 / rounds, text(*v[i]).c_str());
    }
    for (auto& w : waits) XSL_INFO(CH, "profile: waits: %u threads (%u samples):%s", w.second.first, w.second.second, w.first.c_str());
}
static void profile_start() {
    char b[64];
    if (!GetEnvironmentVariableA("XSL_PROFILE", b, sizeof b) || !b[0]) return;
    unsigned from = 0, to = 0, period = 20;
    if (sscanf(b, "%u-%u:%u", &from, &to, &period) < 2 || to <= from) return;
    std::thread([=] { profile_run(from, to, period ? period : 20); }).detach();
}
// XSL_STACKS=<seconds>[,<seconds>...] (diagnostics): at those times after start-up, the stack of
// every thread of the title is logged as the return addresses into the title's image found on it
// (what a title that shows nothing is waiting for).
static void log_thread_stacks(const char* when) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (const IMAGE_DOS_HEADER*)exe;
    auto* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)exe + dos->e_lfanew);
    const uintptr_t lo = (uintptr_t)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    // ... and into this module (shown as L+offset): what the layer is doing for the title.
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&log_thread_stacks, &self);
    uintptr_t slo = 0, shi = 0;
    if (self) {
        auto* sd = (const IMAGE_DOS_HEADER*)self;
        auto* sn = (const IMAGE_NT_HEADERS*)((const uint8_t*)self + sd->e_lfanew);
        slo = (uintptr_t)self; shi = slo + sn->OptionalHeader.SizeOfImage;
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            std::string t;
            if (GetThreadContext(th, &c)) {
                char b[48];
                HMODULE m = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c.Rip, &m) && m) {
                    GetModuleFileNameA(m, name, MAX_PATH);
                    const char* bn = strrchr(name, '\\');
                    snprintf(b, sizeof b, "%.20s+%llx", bn ? bn + 1 : name, (unsigned long long)(c.Rip - (uintptr_t)m));
                } else snprintf(b, sizeof b, "%llx", (unsigned long long)c.Rip);
                t = b;
                MEMORY_BASIC_INFORMATION mbi{};
                int frames = 0;
                if (VirtualQuery((void*)c.Rsp, &mbi, sizeof mbi)) {
                    const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                    for (uintptr_t p = c.Rsp & ~7ull; p + 8 <= end && frames < 56; p += 8) {
                        uintptr_t v = *(const uintptr_t*)p;
                        if (v >= slo + 0x1000 && v < shi) {
                            const uint8_t* q = (const uint8_t*)v;
                            if (q[-5] == 0xE8 || (q[-2] == 0xFF && ((q[-1] >> 3) & 7) == 2) || (q[-3] == 0xFF && ((q[-2] >> 3) & 7) == 2) ||
                                (q[-6] == 0xFF && ((q[-5] >> 3) & 7) == 2)) {
                                snprintf(b, sizeof b, " L+%llx", (unsigned long long)(v - slo));
                                t += b;
                                frames++;
                            }
                            continue;
                        }
                        if (v < lo + 0x1000 || v >= hi) continue;
                        const uint8_t* q = (const uint8_t*)v;   // a return address follows a call
                        bool call = q[-5] == 0xE8 || (q[-2] == 0xFF && ((q[-1] >> 3) & 7) == 2) || (q[-3] == 0xFF && ((q[-2] >> 3) & 7) == 2) ||
                                    (q[-6] == 0xFF && ((q[-5] >> 3) & 7) == 2) || (q[-7] == 0xFF && ((q[-6] >> 3) & 7) == 2);
                        if (!call) continue;
                        snprintf(b, sizeof b, " %llx", (unsigned long long)(v - lo));
                        t += b;
                        frames++;
                    }
                }
            }
            ResumeThread(th);
            XSL_INFO(CH, "stacks %s: thread %lu: %s", when, te.th32ThreadID, t.c_str());
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
}
static void stacks_start() {
    char b[128];
    if (!GetEnvironmentVariableA("XSL_STACKS", b, sizeof b) || !b[0]) return;
    std::string list = b;
    std::thread([list] {
        DWORD t0 = GetTickCount();
        for (const char* p = list.c_str(); *p;) {
            unsigned long at = strtoul(p, (char**)&p, 10);
            while (*p == ',') p++;
            DWORD now = GetTickCount() - t0;
            if (at * 1000 > now) Sleep(at * 1000 - now);
            char w[24];
            snprintf(w, sizeof w, "at %lu s", at);
            log_thread_stacks(w);
        }
    }).detach();
}
static void trap_watch() {
    std::set<DWORD> armed;
    long gen = 0;
    for (;;) {
        if (gen != g_watch_gen) { gen = g_watch_gen; armed.clear(); }   // a watch was set: every thread again
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{sizeof te};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId() || armed.count(te.th32ThreadID)) continue;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
                if (!th) continue;
                if (SuspendThread(th) != (DWORD)-1) { trap_arm(th); ResumeThread(th); }
                CloseHandle(th);
                armed.insert(te.th32ThreadID);
            }
            CloseHandle(snap);
        }
        Sleep(20);
    }
}
static void trap_start() {
    char b[256];
    if (!GetEnvironmentVariableA("XSL_TRAP", b, sizeof b)) return;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    for (char* it = strtok(b, ","); it && g_ntrap < 3; it = strtok(nullptr, ",")) g_trap[g_ntrap++] = base + _strtoui64(it, nullptr, 16);
    if (g_ntrap) std::thread(trap_watch).detach();
    XSL_INFO(CH, "%d hardware trap(s) armed on the title's threads", g_ntrap);
}
static bool trap_hit(EXCEPTION_POINTERS* e) {
    static thread_local bool stepping = false;
    if (g_watch && e->ExceptionRecord->ExceptionCode == 0xC0000005 && e->ExceptionRecord->NumberParameters >= 2 && e->ExceptionRecord->ExceptionInformation[0] == 1 &&
        (e->ExceptionRecord->ExceptionInformation[1] & ~0xFFFull) == (g_watch & ~0xFFFull)) {
        uintptr_t wa = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
        static volatile long n = 0;
        if (wa >= g_watch && wa < g_watch + 64 && InterlockedIncrement(&n) <= 16) {
            void* frames[24];
            USHORT k = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
            std::string t;
            for (USHORT f = 0; f < k; f++) {
                HMODULE m = nullptr;
                char name[MAX_PATH] = "?", buf[MAX_PATH + 32];
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[f], &m)) {
                    GetModuleFileNameA(m, name, MAX_PATH);
                    const char* bn = strrchr(name, '\\');
                    snprintf(buf, sizeof buf, " %s+%llx", bn ? bn + 1 : name, (unsigned long long)((uint8_t*)frames[f] - (uint8_t*)m));
                    t += buf;
                }
            }
            XSL_ERR(CH, "watch: write to %llx (thread %lu), rip %llx, rcx %llx rdx %llx r8 %llx; stack:%s", (unsigned long long)wa, GetCurrentThreadId(),
                    (unsigned long long)e->ContextRecord->Rip, e->ContextRecord->Rcx, e->ContextRecord->Rdx, e->ContextRecord->R8, t.c_str());
        }
        DWORD old;
        VirtualProtect((void*)(g_watch & ~0xFFFull), 4096, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;   // one instruction, then read-only again
        stepping = true;
        return true;
    }
    if (stepping && e->ExceptionRecord->ExceptionCode == 0x80000004) {
        stepping = false;
        DWORD old;
        VirtualProtect((void*)(g_watch & ~0xFFFull), 4096, PAGE_READONLY, &old);
        return true;
    }
    if (e->ExceptionRecord->ExceptionCode != 0x80000004 || !g_ntrap) return false;
    CONTEXT* c = e->ContextRecord;
    uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    int i = 0;
    while (i < g_ntrap && g_trap[i] != at) i++;
    if (i == g_ntrap) return false;
    if (i == 0 && !g_watch) {
        char w[32];
        if (GetEnvironmentVariableA("XSL_TRAP_WATCH", w, sizeof w)) {
            const char* names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
            DWORD64 vals[] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
            for (int k = 15; k >= 0; k--) {
                size_t L = strlen(names[k]);
                if (_strnicmp(w, names[k], L) || isalnum((unsigned char)w[L])) continue;
                g_watch = (uintptr_t)(vals[k] + (w[L] == '+' ? _strtoui64(w + L + 1, nullptr, 16) : 0));
                // The page becomes read-only: writes by code fault (logged below, then allowed for
                // one instruction), writes by the kernel (file reads) fail with ERROR_NOACCESS.
                DWORD old;
                VirtualProtect((void*)(g_watch & ~0xFFFull), 4096, PAGE_READONLY, &old);
                XSL_ERR(CH, "write watch set on %llx (page read-only; first bytes %016llx)", (unsigned long long)g_watch, *(unsigned long long*)g_watch);
                break;
            }
        }
    }
    c->EFlags |= 0x10000;   // RF: resume without hitting the breakpoint again
    static long maxHits = [] { char b[16]; return GetEnvironmentVariableA("XSL_TRAP_MAX", b, sizeof b) ? atol(b) : 20L; }();
    if (InterlockedIncrement(&g_trap_hits[i]) > maxHits) return true;
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    XSL_ERR(CH, "trap exe+%llx (thread %lu): rax %llx rcx %llx rdx %llx rbx %llx rsp %llx rbp %llx rsi %llx rdi %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
            (unsigned long long)(at - base), GetCurrentThreadId(), c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    char pk[512];
    if (GetEnvironmentVariableA("XSL_TRAP_PEEK", pk, sizeof pk)) {
        const char* names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        DWORD64 vals[] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
        char* save = nullptr;
        for (char* it = strtok_s(pk, ",", &save); it; it = strtok_s(nullptr, ",", &save)) {
            bool deref = *it == '*';
            char* q = it + (deref ? 1 : 0);
            int r = -1;
            for (int k = 0; k < 16; k++) if (!_strnicmp(q, names[k], strlen(names[k])) && !isalnum((unsigned char)q[strlen(names[k])])) { r = k; q += strlen(names[k]); break; }
            if (r < 0) continue;
            uint64_t off = *q == '+' ? _strtoui64(q + 1, &q, 16) : 0, len = *q == ':' ? _strtoui64(q + 1, &q, 16) : 16;
            uintptr_t a = vals[r] + off;
            if (deref) { if (IsBadReadPtr((void*)a, 8)) continue; a = *(uintptr_t*)a; }
            len = std::min<uint64_t>(len, 64);
            std::string hex;
            if (!a || IsBadReadPtr((void*)a, (UINT_PTR)len)) hex = "(unreadable)";
            else for (uint64_t k = 0; k < len; k++) { char h[4]; snprintf(h, sizeof h, "%02x", ((const uint8_t*)a)[k]); hex += h; }
            XSL_ERR(CH, "  %s -> %llx: %s", it, (unsigned long long)a, hex.c_str());
        }
    }
    return true;
}
// Pipelines the GPU driver's compiler gives up on. It does so with a breakpoint in its own code,
// and a breakpoint nobody handles ends the process: 27 s into one compute pipeline of a title
// (3 MB of translated HLSL), in every run. The shaders of such a pipeline are noted beside the
// pipeline cache (bad-pipelines.txt, one line of cache keys each) and it is not given to the
// driver again: it stays a placeholder whose work is skipped. The breakpoint itself is stepped
// over, which lets the compiler fail in its own way where it can; a compiler that keeps hitting
// breakpoints has its thread parked for good (the pipeline never completes).
static wchar_t g_bad_path[MAX_PATH] = L"";
static std::mutex g_bad_mu;
static std::set<std::string> g_bad;
static thread_local const char* t_driver_keys = nullptr;   // the shaders of the pipeline this thread has in the driver
static thread_local unsigned t_driver_breaks = 0;
std::wstring rt_cache_dir();
static void bad_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const std::wstring p = rt_cache_dir();
        if (p.empty() || p.size() + 24 >= MAX_PATH) return;
        memcpy(g_bad_path, (p + L"bad-pipelines.txt").c_str(), (p.size() + 18) * sizeof(wchar_t));
        if (FILE* f = _wfopen(g_bad_path, L"r")) {
            char l[512];
            while (fgets(l, sizeof l, f)) {
                l[strcspn(l, "\r\n")] = 0;
                if (l[0]) g_bad.insert(l);
            }
            fclose(f);
        }
        if (!g_bad.empty()) XSL_INFO(CH, "%zu pipelines the GPU driver's compiler gave up on in earlier runs stay placeholders (%ls)", g_bad.size(), g_bad_path);
    });
}
static bool driver_refused(const char* keys) {
    bad_init();
    if (!keys || !*keys) return false;
    std::lock_guard<std::mutex> g(g_bad_mu);
    if (!g_bad.count(keys)) return false;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 5) XSL_INFO(CH, "pipeline of %s: the GPU driver's compiler gave up on it in an earlier run: placeholder", keys);
    return true;
}
struct DriverCall {
    explicit DriverCall(const char* keys) { bad_init(); t_driver_keys = keys && *keys ? keys : nullptr; t_driver_breaks = 0; }
    ~DriverCall() { t_driver_keys = nullptr; }
};
static bool driver_break(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0x80000003 || !t_driver_keys) return false;
    if (t_driver_breaks++ == 0) {
        if (g_bad_path[0]) {   // (no heap here: the thread is somewhere inside the compiler)
            HANDLE f = CreateFileW(g_bad_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(f, t_driver_keys, (DWORD)strlen(t_driver_keys), &w, nullptr);
                WriteFile(f, "\r\n", 2, &w, nullptr);
                CloseHandle(f);
            }
        }
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)e->ExceptionRecord->ExceptionAddress, &m))
            GetModuleFileNameA(m, name, MAX_PATH);
        const char* b = strrchr(name, '\\');
        XSL_ERR(CH, "the GPU driver's compiler stopped at a breakpoint of its own (%s+%llx) on the pipeline of %s: noted, a placeholder from the next run on; stepping over it",
                b ? b + 1 : name, (unsigned long long)((uint8_t*)e->ExceptionRecord->ExceptionAddress - (uint8_t*)m), t_driver_keys);
    }
    if (t_driver_breaks > 256) {
        XSL_ERR(CH, "the GPU driver's compiler keeps stopping on the pipeline of %s: its thread is parked", t_driver_keys);
        Sleep(INFINITE);
    }
    e->ContextRecord->Rip += 1;
    return true;
}
static LONG CALLBACK note_exception(EXCEPTION_POINTERS* e) {
    if (trap_hit(e)) return EXCEPTION_CONTINUE_EXECUTION;
    if (driver_break(e)) return EXCEPTION_CONTINUE_EXECUTION;
    DWORD c = e->ExceptionRecord->ExceptionCode;
    crash_dump(e);
    if (c == 0xC00000FD) return EXCEPTION_CONTINUE_SEARCH;   // stack overflow: no stack left to log with
    if (c == 0x406D1388 || c == 0x40010006 || c == 0x4001000A || c == 0xE06D7363 || c == 0x80000004 ||
        c == 0xE0434352 || c == 0x40080201 || c == 0x4000001F)
        return EXCEPTION_CONTINUE_SEARCH;
    if (c == 0xC0000005 || c == 0x80000003) {
        // Access violations and breakpoints: titles probe memory and handle these themselves, so
        // each code address is reported once (the last one before a silent exit is the crash).
        static std::atomic<void*> seen[24];
        void* at = e->ExceptionRecord->ExceptionAddress;
        bool fresh = false;
        for (auto& sl : seen) {
            void* cur = sl.load();
            if (cur == at) break;
            if (!cur) { void* z = nullptr; if (sl.compare_exchange_strong(z, at)) { fresh = true; break; } if (z == at) break; }
        }
        if (!fresh) return EXCEPTION_CONTINUE_SEARCH;
    } else {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) > 12) return EXCEPTION_CONTINUE_SEARCH;
    }
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
    XSL_ERR(CH, "exception %08lx (%lu parameters%s%llx, address %llx):%s", c, e->ExceptionRecord->NumberParameters,
            e->ExceptionRecord->NumberParameters ? ", first " : "", e->ExceptionRecord->NumberParameters ? (unsigned long long)e->ExceptionRecord->ExceptionInformation[0] : 0ull,
            (unsigned long long)(e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0), t.c_str());
    if (c == 0x87d) dump_debug_messages();   // the debug layer's break: show the message behind it
    return EXCEPTION_CONTINUE_SEARCH;
}
void rt_warm_start();
// D3D12 Agility SDK runtime: when xsl-run placed one in D3D12\ next to the layer
// (tools/get-agility.sh), select it before the first device. Titles compile shader model 6.6+ DXIL
// at run time, which the Windows 10 system runtime rejects. XSL_AGILITY=0 keeps the system runtime.
// A title can need more memory than the system has left to commit (its textures exist twice: in
// its own memory and on the GPU). At the commit limit every allocation of every process fails:
// other programs die, remote sessions cannot start a command. So the process ends itself, with a
// line in the log, when less than XSL_MIN_COMMIT_MB (default 2048; 0: never) can still be committed.
static void commit_guard_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        char b[16];
        const uint64_t floorMb = GetEnvironmentVariableA("XSL_MIN_COMMIT_MB", b, sizeof b) ? (uint64_t)_strtoui64(b, nullptr, 10) : 2048;
        if (!floorMb) return;
        std::thread([floorMb] {
            for (;;) {
                Sleep(250);
                MEMORYSTATUSEX ms{};
                ms.dwLength = sizeof ms;
                if (!GlobalMemoryStatusEx(&ms) || (ms.ullAvailPageFile >> 20) >= floorMb) continue;
                PROCESS_MEMORY_COUNTERS_EX pm{};
                pm.cb = sizeof pm;
                GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
                XSL_ERR(CH, "the system can commit only %llu MB more (limit %llu MB; this process holds %llu MB): ending the process before allocations fail everywhere (XSL_MIN_COMMIT_MB)",
                        (unsigned long long)(ms.ullAvailPageFile >> 20), (unsigned long long)(ms.ullTotalPageFile >> 20), (unsigned long long)(pm.PrivateUsage >> 20));
                Sleep(100);   // (let the log line out)
                TerminateProcess(GetCurrentProcess(), 0xC000012D);   // STATUS_COMMITMENT_LIMIT
            }
        }).detach();
    });
}
static void select_agility_runtime() {
    static bool done = false;
    if (done) return;
    done = true;
    { char b[4]; if (GetEnvironmentVariableA("XSL_AGILITY", b, 4) && b[0] == '0') return; }
    wchar_t dir[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&select_agility_runtime, &self);
    DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\') n--;
    dir[n] = 0;
    std::wstring sdk = std::wstring(dir) + L"D3D12\\";
    FILE* vf = _wfopen((sdk + L"VERSION").c_str(), L"r");
    if (!vf) return;
    unsigned major = 0, minor = 0, patch = 0;
    int got = fscanf(vf, "%u.%u.%u", &major, &minor, &patch);
    fclose(vf);
    if (got < 2) return;
    ID3D12SDKConfiguration* cfg = nullptr;
    // CLSID_D3D12SDKConfiguration (d3d12.h; defined here, the import library does not carry it)
    static const GUID kSdkConfig = {0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
    HRESULT hr = D3D12GetInterface(kSdkConfig, IID_PPV_ARGS(&cfg));
    if (FAILED(hr)) { XSL_WARN(CH, "D3D12 Agility SDK: no SDK configuration interface (%08lx); system runtime in use", hr); return; }
    // The path is taken relative to the executable (the layer sits in the executable's directory).
    const char* path = ".\\D3D12\\";
    hr = cfg->SetSDKVersion(minor, path);
    cfg->Release();
    if (FAILED(hr)) XSL_WARN(CH, "D3D12 Agility SDK %u.%u.%u at %s not selected (%08lx; needs Windows Developer Mode); system runtime in use", major, minor, patch, path, hr);
    else XSL_INFO(CH, "D3D12 Agility SDK %u.%u.%u selected (%s)", major, minor, patch, path);
}
HRESULT_T create_device(PcObj* out) {
    select_agility_runtime();
    commit_guard_start();
    { static bool once = false; if (!once) { once = true; profile_start(); } }
    if (env_on("XSL_D3D12_DRED") || env_on("XSL_D3D12_DEBUG")) {
        static bool once = false;
        if (!once) { once = true; AddVectoredExceptionHandler(1, note_exception); trap_start(); stacks_start(); }
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
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            // XSL_D3D12_DEBUG=2: GPU-based validation too (descriptors and resource states as shaders
            // use them; slow).
            char b[4];
            ID3D12Debug1* d1 = nullptr;
            if (GetEnvironmentVariableA("XSL_D3D12_DEBUG", b, 4) && b[0] == '2' && SUCCEEDED(dbg->QueryInterface(IID_PPV_ARGS(&d1)))) {
                d1->SetEnableGPUBasedValidation(TRUE);
                d1->Release();
                XSL_INFO(CH, "GPU-based validation on");
            }
            dbg->Release();
        }
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
    rt_warm_start();
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
uint32_t g_gv_virtual = 0;   // view slots handed to title heaps (>= g_gv_size; see global_view_virtual_size)
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
    // Title heaps may together need more view slots than a PC heap can hold (one title asks for
    // 1,245,184 in one heap; the PC limit is about 1,000,000). Slots are numbered up to
    // XSL_VIEW_SLOTS (default 2,097,152): the console-layout memory holds all of them, the PC heap
    // the first g_gv_size; later ones are kept but not visible to PC shaders.
    g_gv_virtual = GetEnvironmentVariableA("XSL_VIEW_SLOTS", b, sizeof b) ? (uint32_t)atoi(b) : 2097152u;
    g_gv_virtual = std::max(g_gv_virtual, g_gv_size);
    g_gv_free[0] = g_gv_virtual;
    XSL_INFO(CH, "descriptor heaps: %u-entry shader-visible CBV/SRV/UAV heap shared by all title heaps (%u-byte descriptors; console 32)",
             g_gv_size, g_gv_inc);
    return g_gv;
}
PcObj global_view_heap_if_created() { return g_gv; }
uint32_t global_view_virtual_size() { return g_gv_virtual; }
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
static std::atomic<long> g_n_allocators{0}, g_n_lists{0}, g_n_oneoff{0};   // XSL_MEM_STATS: what has been created so far
HRESULT_T create_allocator(PcObj dev, int type, PcObj* out) {
    if (const long n = ++g_n_allocators; getenv("XSL_MEM_STATS") && (n <= 4 || n % 50 == 0)) XSL_INFO(CH, "mem: %ld command allocators created by the title so far (%ld lists, %ld one-off uploads)", n, g_n_lists.load(), g_n_oneoff.load());
    return logged(as<ID3D12Device>(dev)->CreateCommandAllocator((D3D12_COMMAND_LIST_TYPE)pc_list_type(type), IID_PPV_ARGS(reinterpret_cast<ID3D12CommandAllocator**>(out))), "CreateCommandAllocator");
}
void name_recording(PcObj l);
void list_set_pso(PcObj l, PcObj pso);
HRESULT_T create_list(PcObj dev, int type, PcObj alloc, PcObj pso, PcObj* out) {
    ++g_n_lists;
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
// Sampler heaps and which of their slots hold a sampler: a shader that samples through a slot the
// title never wrote reads whatever the heap's memory held (see the table check).
struct SampHeap { uint64_t cpu, gpu; uint32_t n, inc; std::vector<uint8_t> init; };
std::mutex g_samp_mu;
std::vector<SampHeap> g_samp_heaps;
uint64_t g_samp_fixed = 0;
static SampHeap* samp_heap_cpu(uint64_t h) {   // caller holds g_samp_mu
    for (auto& s : g_samp_heaps)
        if (h >= s.cpu && h < s.cpu + (uint64_t)s.n * s.inc) return &s;
    return nullptr;
}
HRESULT_T create_descriptor_heap(PcObj dev, const void* desc, PcObj* out) {
    D3D12_DESCRIPTOR_HEAP_DESC d = *static_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(desc);
    d.Flags &= D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if ((int)d.Type > 3) d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;  // Xbox "sampler unbounded"
    HRESULT hr = logged(as<ID3D12Device>(dev)->CreateDescriptorHeap(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12DescriptorHeap**>(out))), "CreateDescriptorHeap");
    if (SUCCEEDED(hr) && d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        auto* h = as<ID3D12DescriptorHeap>(*out);
        std::lock_guard<std::mutex> g(g_samp_mu);
        const uint64_t c = h->GetCPUDescriptorHandleForHeapStart().ptr;
        g_samp_heaps.erase(std::remove_if(g_samp_heaps.begin(), g_samp_heaps.end(), [&](const SampHeap& s) { return s.cpu == c; }), g_samp_heaps.end());
        g_samp_heaps.push_back({c, (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ? h->GetGPUDescriptorHandleForHeapStart().ptr : 0, d.NumDescriptors,
                                as<ID3D12Device>(dev)->GetDescriptorHandleIncrementSize(d.Type), std::vector<uint8_t>(d.NumDescriptors, 0)});
    }
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
std::atomic<uint64_t> g_gm_frame{0};   // presents (table rebuilds are limited to one per frame)
std::map<uint64_t, GmBuf> g_gm;                 // title address -> buffer
struct GmRetired { uint64_t frame; ID3D12Resource* res; std::vector<std::pair<uint32_t, uint32_t>> views; };
std::vector<GmRetired> g_gm_retired;            // released buffers and unused view slots, dropped a few frames later (gm_retire_due)
bool g_gm_dirty = false;
uint32_t g_gm_next_srv = 1, g_gm_next_uav = kGmHalf;
std::vector<ID3D12Resource*> g_gm_tables;       // table buffers (recent ones kept alive)
std::map<ID3D12Resource*, ID3D12Resource*> g_as_shadow;   // acceleration-structure buffer -> shadow
std::vector<uint32_t> g_gm_free_srv, g_gm_free_uav;   // view slots of destroyed buffers
std::map<std::tuple<ID3D12Resource*, uint64_t, uint64_t>, std::pair<uint32_t, uint32_t>> g_gm_synth;   // views of segments inside buffers (resource, first byte, bytes)
uint64_t g_gm_built_at = UINT64_MAX;            // present count of the last table rebuild
bool g_gm_force = false;                         // rebuild at the next submission even if one ran this frame
std::vector<std::pair<uint64_t, uint64_t>> g_gm_built;   // address ranges in the current GPU table (sorted)
static bool gm_in_table(uint64_t va, uint64_t bytes) {   // caller holds g_gm_mu
    auto it = std::upper_bound(g_gm_built.begin(), g_gm_built.end(), std::make_pair(va, UINT64_MAX));
    if (it == g_gm_built.begin()) return false;
    --it;
    return va >= it->first && va + bytes <= it->first + it->second;
}
std::map<PcObj, uint32_t> g_rs_gm;              // root signature -> appended parameter index (| kRsHeaps: the heap tables follow)
constexpr uint32_t kRsHeaps = 0x80000000u, kHeapTables = 2;   // the view heap's table, the sampler heap's
std::map<PcObj, PcObj> g_list_sampler_heap;    // list -> the sampler heap the title bound
std::map<PcObj, bool> g_list_global_heap;       // list -> the global heap is bound
std::map<PcObj, std::pair<PcObj, PcObj>> g_list_gm_rs;   // list -> current (graphics, compute) root signatures
bool gm_enabled() { static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_GLOBAL_MEMORY", b, 4) && b[0] == '0'); }(); return on; }
}  // namespace
bool gm_active() { return gm_enabled() && !getenv("XSL_GM_NO_RS"); }
uint32_t gm_reserved_start() { return gm_enabled() && g_gv_size > kGmSlots ? g_gv_size - kGmSlots : UINT32_MAX; }
// Layer memory at fixed low addresses (translated ray-tracing code holds some pointers in 32 bits):
// 0xF0000000 upward. A title buffer registered in that range would be ambiguous: warned.
constexpr uint64_t kLowBase = 0xF0000000ull, kLowEnd = 0x100000000ull;
uint64_t present_count();
// XSL_GM_WATCH=<hex address> (diagnostics): log buffers created, registered or forgotten that cover it.
static uint64_t gm_watch() { static uint64_t v = [] { char b[32]; return GetEnvironmentVariableA("XSL_GM_WATCH", b, sizeof b) ? _strtoui64(b, nullptr, 16) : 0ull; }(); return v; }
static void gm_watch_note(const char* what, PcObj res, uint64_t va, uint64_t size) {
    uint64_t w = gm_watch();
    if (!w || w < va || w >= va + size) return;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) > 40) return;
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    D3D12_RESOURCE_DESC d = as<ID3D12Resource>(res)->GetDesc();
    bool heap = SUCCEEDED(as<ID3D12Resource>(res)->GetHeapProperties(&hp, &hf));
    XSL_INFO(CH, "gm watch: %s %p at %llx+%llx (width %llx, flags %#x, heap type %d%s, present %llu)", what, res, (unsigned long long)va, (unsigned long long)size,
             (unsigned long long)d.Width, (unsigned)d.Flags, heap ? (int)hp.Type : -1, heap ? "" : " (reserved)", (unsigned long long)present_count());
}
void gm_register(PcObj res, uint64_t titleVa, uint64_t size, uint64_t resOff) {
    if (res && size) gm_watch_note("register", res, titleVa, size);
    if (!gm_enabled() || !res || !size) return;
    if (titleVa < kLowEnd && titleVa + size > kLowBase && titleVa < kLowBase) {
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 3) XSL_WARN(CH, "buffer at %llx overlaps the layer's low global-memory range", (unsigned long long)titleVa);
    }
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    if (SUCCEEDED(as<ID3D12Resource>(res)->GetHeapProperties(&hp, &hf)) &&
        (hp.Type == D3D12_HEAP_TYPE_READBACK || (hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK && hp.MemoryPoolPreference == D3D12_MEMORY_POOL_L0 && false)))
        return;   // readback buffers only take copies
    std::lock_guard<std::mutex> g(g_gm_mu);
    auto& e = g_gm[titleVa];
    if (e.res == as<ID3D12Resource>(res) && e.size == size) return;
    if (!e.views.empty()) g_gm_retired.push_back({g_gm_frame.load(), nullptr, std::move(e.views)});   // another buffer was here: its views go later
    e = GmBuf{size, as<ID3D12Resource>(res), resOff};
    g_gm_dirty = true;
}
// Buffers the title released, and view slots no longer used, stay as they are for a few frames:
// lists recorded with them may still be waiting for the GPU, and the table and views are shared by
// every list (titles free per-frame buffers such as shader tables right after recording).
constexpr uint64_t kGmKeepFrames = 6;
static void gm_forget_now(PcObj res);
void gm_forget(PcObj res) {
    if (!gm_enabled() || !res) return;
    as<ID3D12Resource>(res)->AddRef();
    std::lock_guard<std::mutex> g(g_gm_mu);
    g_gm_retired.push_back({g_gm_frame.load(), as<ID3D12Resource>(res), {}});
}
static void gm_retire_due() {   // from gm_flush, outside g_gm_mu
    std::vector<GmRetired> due;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        uint64_t now = g_gm_frame.load();
        size_t k = 0;
        for (auto& e : g_gm_retired) {
            if (e.frame + kGmKeepFrames <= now) due.push_back(std::move(e));
            else g_gm_retired[k++] = std::move(e);
        }
        g_gm_retired.resize(k);
        for (auto& e : due)
            for (auto& v : e.views) {
                if (v.first) g_gm_free_srv.push_back(v.first);
                if (v.second) g_gm_free_uav.push_back(v.second);
            }
    }
    for (auto& e : due)
        if (e.res) { gm_forget_now((PcObj)e.res); e.res->Release(); }
}
static void gm_forget_now(PcObj res) {
    if (!gm_enabled() || !res) return;
    ID3D12Resource* shadow = nullptr;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto it = g_as_shadow.find(as<ID3D12Resource>(res));
        if (it != g_as_shadow.end()) { shadow = it->second; g_as_shadow.erase(it); }
    }
    if (shadow) { gm_forget_now((PcObj)shadow); shadow->Release(); }
    std::lock_guard<std::mutex> g(g_gm_mu);
    for (auto it = g_gm_synth.begin(); it != g_gm_synth.end();) {
        if (std::get<0>(it->first) != as<ID3D12Resource>(res)) { ++it; continue; }
        if (it->second.first) g_gm_free_srv.push_back(it->second.first);
        if (it->second.second) g_gm_free_uav.push_back(it->second.second);
        it = g_gm_synth.erase(it);
        g_gm_dirty = true;
    }
    for (auto it = g_gm.begin(); it != g_gm.end();) {
        if (it->second.res != as<ID3D12Resource>(res)) { ++it; continue; }
        gm_watch_note("forget", res, it->first, it->second.size);
        for (auto& v : it->second.views) {
            if (v.first) g_gm_free_srv.push_back(v.first);
            if (v.second) g_gm_free_uav.push_back(v.second);
        }
        it = g_gm.erase(it);
        g_gm_dirty = true;
    }
}
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
// ---- Top-level acceleration structures in the console format ----------------------------------
// Translated ray-query code traverses console BVHs itself (image_bvh*_intersect_ray plus memory
// loads), so a TLAS build writes the console layout into the structure's shadow:
//   header (64 B: flags = version 2008, numElements, sceneBounds, rootNodePtr at +48, offsets)
//   box32 nodes (128 B: 4 child pointers, 4 x {min, max}), bottom level first, root last
//   instance nodes (128 B: ObjectToWorld, ID|mask, contribution|flags, BLAS root pointer at +56,
//                   BLAS address >> 8 at +60, WorldToObject at +64, instance index at +112)
// Node pointers are (byte offset / 64) << 3 | type (5 box32, 6 instance). Instances are sorted by
// the Morton code of their bounds' centre (up to 2048 per build; more stay in input order). An
// instance whose BLAS has no console header (a BLAS this layer did not provide) gets an invalid
// root pointer and empty bounds, so rays skip it. One 1024-thread group does the whole build.
static const char kTlasBuilder[] = R"(
StructuredBuffer<uint4> GMT : register(t0, space901);
ByteAddressBuffer GMS[8191] : register(t0, space900);
globallycoherent RWByteAddressBuffer GMU[8192] : register(u0, space900);
cbuffer C : register(b0) { uint dstLo, dstHi, instLo, instHi, scrLo, scrHi, N, layout, instOff, pad0, pad1, pad2; };
uint3 gm_find(uint lo, uint hi) {
  uint a = 0u, b = GMT[0].x;
  while (a < b) { uint m = (a + b) >> 1; uint4 e = GMT[1u + m]; if (e.y < hi || (e.y == hi && e.x <= lo)) a = m + 1u; else b = m; }
  if (a == 0u) return uint3(0u, 0u, 0u);
  uint4 e = GMT[a];
  uint off = lo - e.x, ohi = hi - e.y - (lo < e.x ? 1u : 0u);
  return (ohi == 0u && off < e.z) ? uint3(1u, e.w, off) : uint3(0u, 0u, 0u);
}
uint2 at(uint lo, uint hi, uint o) { uint r = lo + o; return uint2(r, hi + (r < lo ? 1u : 0u)); }
uint ld(uint2 a) {   // title memory (read-only use)
  uint3 f = gm_find(a.x, a.y);
  if (f.x == 0u) return 0u;
  if (f.y & 0x80000000u) return GMU[NonUniformResourceIndex((f.y >> 14) & 0x3fffu)].Load(f.z & ~3u);
  if (f.y & 0x40000000u) return GMS[NonUniformResourceIndex(f.y & 0x3fffu)].Load(f.z & ~3u);
  return 0u;
}
void st(uint2 a, uint v) {
  uint3 f = gm_find(a.x, a.y);
  if (f.x != 0u && (f.y & 0x80000000u)) GMU[NonUniformResourceIndex((f.y >> 14) & 0x3fffu)].Store(f.z & ~3u, v);
}
float ldf(uint2 a) { return asfloat(ld(a)); }
void stf(uint2 a, float v) { st(a, asuint(v)); }
groupshared uint keys[2048];
groupshared uint idxs[2048];
groupshared uint smin[3], smax[3];
uint ford(float f) { uint u = asuint(f); return (u & 0x80000000u) ? ~u : (u | 0x80000000u); }
float unord(uint u) { return asfloat((u & 0x80000000u) ? (u & 0x7fffffffu) : ~u); }
uint spread(uint x) { x &= 0x3ffu; x = (x | (x << 16)) & 0x030000FFu; x = (x | (x << 8)) & 0x0300F00Fu; x = (x | (x << 4)) & 0x030C30C3u; x = (x | (x << 2)) & 0x09249249u; return x; }
void box_of(uint2 a, out float3 mn, out float3 mx) {
  mn = float3(ldf(a), ldf(at(a.x, a.y, 4u)), ldf(at(a.x, a.y, 8u)));
  mx = float3(ldf(at(a.x, a.y, 12u)), ldf(at(a.x, a.y, 16u)), ldf(at(a.x, a.y, 20u)));
}
void put_box(uint2 a, float3 mn, float3 mx) {
  stf(a, mn.x); stf(at(a.x, a.y, 4u), mn.y); stf(at(a.x, a.y, 8u), mn.z);
  stf(at(a.x, a.y, 12u), mx.x); stf(at(a.x, a.y, 16u), mx.y); stf(at(a.x, a.y, 20u), mx.z);
}
[numthreads(1024, 1, 1)]
void main(uint t : SV_GroupIndex) {
  if (t < 3u) { smin[t] = 0xffffffffu; smax[t] = 0u; }
  GroupMemoryBarrierWithGroupSync();
  const float BIG = 3.0e38;
  for (uint i = t; i < N; i += 1024u) {
    uint2 da;
    if (layout == 0u) da = at(instLo, instHi, i * 64u);
    else { uint2 pa = at(instLo, instHi, i * 8u); da = uint2(ld(pa), ld(at(pa.x, pa.y, 4u))); }
    float w[12];
    [unroll] for (uint k = 0u; k < 12u; k++) w[k] = ldf(at(da.x, da.y, 4u * k));
    uint idm = ld(at(da.x, da.y, 48u)), cfl = ld(at(da.x, da.y, 52u));
    uint2 blas = uint2(ld(at(da.x, da.y, 56u)), ld(at(da.x, da.y, 60u)));
    bool valid = false;
    uint root = 0x3fffffffu;
    float3 bmn = 0, bmx = 0;
    if ((blas.x | blas.y) != 0u && (blas.x & 0xffu) == 0u) {
      uint fl = ld(blas);
      if ((fl & 0x8000ffffu) == 0x80000004u) {
        uint r = ld(at(blas.x, blas.y, 48u));
        if ((r & 7u) != 6u && r != 0xffffffffu) {
          valid = true;
          root = r & 0x3fffffffu;
          box_of(at(blas.x, blas.y, 16u), bmn, bmx);
        }
      }
    }
    uint2 node = at(dstLo, dstHi, instOff + i * 128u);
    [unroll] for (uint k2 = 0u; k2 < 12u; k2++) stf(at(node.x, node.y, 4u * k2), w[k2]);
    st(at(node.x, node.y, 48u), idm);
    st(at(node.x, node.y, 52u), cfl);
    st(at(node.x, node.y, 56u), root);
    st(at(node.x, node.y, 60u), (blas.x >> 8) | (blas.y << 24));
    float3x3 R = float3x3(w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9], w[10]);
    float3 tr = float3(w[3], w[7], w[11]);
    float det = determinant(R);
    float3x3 Ri = float3x3(0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (abs(det) > 1e-30) {
      float id = 1.0 / det;
      Ri[0] = float3(R[1][1] * R[2][2] - R[1][2] * R[2][1], R[0][2] * R[2][1] - R[0][1] * R[2][2], R[0][1] * R[1][2] - R[0][2] * R[1][1]) * id;
      Ri[1] = float3(R[1][2] * R[2][0] - R[1][0] * R[2][2], R[0][0] * R[2][2] - R[0][2] * R[2][0], R[0][2] * R[1][0] - R[0][0] * R[1][2]) * id;
      Ri[2] = float3(R[1][0] * R[2][1] - R[1][1] * R[2][0], R[0][1] * R[2][0] - R[0][0] * R[2][1], R[0][0] * R[1][1] - R[0][1] * R[1][0]) * id;
    }
    float3 ti = -mul(Ri, tr);
    [unroll] for (uint r2 = 0u; r2 < 3u; r2++) {
      stf(at(node.x, node.y, 64u + r2 * 16u), Ri[r2].x);
      stf(at(node.x, node.y, 68u + r2 * 16u), Ri[r2].y);
      stf(at(node.x, node.y, 72u + r2 * 16u), Ri[r2].z);
      stf(at(node.x, node.y, 76u + r2 * 16u), ti[r2]);
    }
    st(at(node.x, node.y, 112u), i);
    st(at(node.x, node.y, 116u), 0u);
    st(at(node.x, node.y, 120u), 0x007F7F7Fu);
    st(at(node.x, node.y, 124u), 0u);
    float3 wmn = float3(BIG, BIG, BIG), wmx = float3(-BIG, -BIG, -BIG);
    if (valid) {
      [unroll] for (uint c = 0u; c < 8u; c++) {
        float3 p = float3((c & 1u) ? bmx.x : bmn.x, (c & 2u) ? bmx.y : bmn.y, (c & 4u) ? bmx.z : bmn.z);
        float3 q = mul(R, p) + tr;
        wmn = min(wmn, q); wmx = max(wmx, q);
      }
      float3 ce = (wmn + wmx) * 0.5;
      uint o;
      InterlockedMin(smin[0], ford(ce.x), o); InterlockedMin(smin[1], ford(ce.y), o); InterlockedMin(smin[2], ford(ce.z), o);
      InterlockedMax(smax[0], ford(ce.x), o); InterlockedMax(smax[1], ford(ce.y), o); InterlockedMax(smax[2], ford(ce.z), o);
    }
    put_box(at(scrLo, scrHi, i * 32u), wmn, wmx);
  }
  DeviceMemoryBarrierWithGroupSync();
  bool sortIt = N <= 2048u;
  uint P = 1u;
  while (P < N) P <<= 1;
  if (sortIt) {
    float3 lo = float3(unord(smin[0]), unord(smin[1]), unord(smin[2]));
    float3 hi = float3(unord(smax[0]), unord(smax[1]), unord(smax[2]));
    float3 sc = 1023.0 / max(hi - lo, 1e-20);
    for (uint i2 = t; i2 < P; i2 += 1024u) {
      uint key = 0xffffffffu;
      if (i2 < N) {
        float3 mn, mx;
        box_of(at(scrLo, scrHi, i2 * 32u), mn, mx);
        if (mn.x <= mx.x) {
          uint3 q = uint3(clamp(((mn + mx) * 0.5 - lo) * sc, 0.0, 1023.0));
          key = (spread(q.x) << 2) | (spread(q.y) << 1) | spread(q.z);
        }
      }
      keys[i2] = key;
      idxs[i2] = i2;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k3 = 2u; k3 <= P; k3 <<= 1) {
      for (uint j = k3 >> 1; j > 0u; j >>= 1) {
        for (uint i3 = t; i3 < P; i3 += 1024u) {
          uint l = i3 ^ j;
          if (l > i3) {
            bool up = (i3 & k3) == 0u;
            if ((keys[i3] > keys[l]) == up) { uint a = keys[i3]; keys[i3] = keys[l]; keys[l] = a; a = idxs[i3]; idxs[i3] = idxs[l]; idxs[l] = a; }
          }
        }
        GroupMemoryBarrierWithGroupSync();
      }
    }
  }
  uint cnt = N, base = 0u, prev = 0u, level = 0u;
  while (cnt > 0u) {
    uint lc = (cnt + 3u) / 4u;
    for (uint j2 = t; j2 < lc; j2 += 1024u) {
      uint2 nd = at(dstLo, dstHi, 64u + (base + j2) * 128u);
      float3 umn = float3(BIG, BIG, BIG), umx = float3(-BIG, -BIG, -BIG);
      [unroll] for (uint k4 = 0u; k4 < 4u; k4++) {
        uint c2 = j2 * 4u + k4;
        uint ptr = 0xffffffffu;
        float3 mn = float3(BIG, BIG, BIG), mx = float3(-BIG, -BIG, -BIG);
        if (c2 < cnt) {
          if (level == 0u) {
            uint ii = sortIt ? idxs[c2] : c2;
            ptr = (((instOff + ii * 128u) >> 6) << 3) | 6u;
            box_of(at(scrLo, scrHi, ii * 32u), mn, mx);
          } else {
            uint bi = prev + c2;
            ptr = (((64u + bi * 128u) >> 6) << 3) | 5u;
            box_of(at(scrLo, scrHi, N * 32u + bi * 32u), mn, mx);
          }
          if (mn.x <= mx.x) { umn = min(umn, mn); umx = max(umx, mx); }
        }
        st(at(nd.x, nd.y, 4u * k4), ptr);
        put_box(at(nd.x, nd.y, 16u + 24u * k4), mn, mx);
      }
      st(at(nd.x, nd.y, 112u), 0u); st(at(nd.x, nd.y, 116u), 0u); st(at(nd.x, nd.y, 120u), 0u); st(at(nd.x, nd.y, 124u), 0u);
      put_box(at(scrLo, scrHi, N * 32u + (base + j2) * 32u), umn, umx);
    }
    DeviceMemoryBarrierWithGroupSync();
    prev = base;
    base += lc;
    level++;
    cnt = lc == 1u ? 0u : lc;
  }
  if (t == 0u) {
    uint2 h = uint2(dstLo, dstHi);
    float3 mn = float3(0, 0, 0), mx = float3(0, 0, 0);
    uint root = 0xffffffffu;
    if (N > 0u) { root = (((64u + (base - 1u) * 128u) >> 6) << 3) | 5u; box_of(at(scrLo, scrHi, N * 32u + (base - 1u) * 32u), mn, mx); }
    st(h, 0x00000004u);
    st(at(h.x, h.y, 4u), (instOff + N * 128u) / 64u);
    st(at(h.x, h.y, 8u), 0u);
    st(at(h.x, h.y, 12u), N);
    put_box(at(h.x, h.y, 16u), mn, mx);
    st(at(h.x, h.y, 40u), 64u);
    st(at(h.x, h.y, 44u), 0u);
    st(at(h.x, h.y, 48u), root);
    st(at(h.x, h.y, 52u), instOff);
    st(at(h.x, h.y, 56u), 0u);
    st(at(h.x, h.y, 60u), 0x007F7F7Fu);
  }
}
)";
// Scratch for the console-layout builders: a layer-owned ring registered in the global-memory table
// at 0x7E0000000000 (titles' scratch can be transient memory the table does not know yet).
constexpr uint64_t kRtScratchVa = 0x7E0000000000ull, kRtScratchSize = 64ull << 20;
struct RtScratch { ID3D12Resource* res = nullptr; uint64_t head = 0; std::mutex mu; } g_rt_scratch;
static uint64_t rt_scratch(uint64_t bytes) {   // address of a slice of at least `bytes`, 0 when it cannot be had
    bytes = (bytes + 255) & ~255ull;
    if (bytes > kRtScratchSize || !g_main_device) return 0;
    bool fresh = false;
    uint64_t at;
    {
        std::lock_guard<std::mutex> g(g_rt_scratch.mu);
        if (!g_rt_scratch.res) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = kRtScratchSize;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rt_scratch.res))))
                return 0;
            fresh = true;
        }
        if (g_rt_scratch.head + bytes > kRtScratchSize) g_rt_scratch.head = 0;
        at = g_rt_scratch.head;
        g_rt_scratch.head += bytes;
    }
    if (fresh) gm_register((PcObj)g_rt_scratch.res, kRtScratchVa, kRtScratchSize, 0);
    return kRtScratchVa + at;
}
struct AsDump { ID3D12Resource* rb; uint64_t va; uint64_t frame; };
std::vector<AsDump> g_as_dumps;   // XSL_AS_DUMP / XSL_TLAS_DUMP readbacks
bool compile_layer_shader(const std::string& hlsl, const char* profile, std::vector<uint8_t>& out, std::string& log);
struct TlasBuilder { ID3D12RootSignature* rs = nullptr; ID3D12PipelineState* pso = nullptr; bool failed = false; std::mutex mu; } g_tlas;
static bool tlas_init(ID3D12Device* dev) {
    std::lock_guard<std::mutex> g(g_tlas.mu);
    if (g_tlas.pso) return true;
    if (g_tlas.failed) return false;
    g_tlas.failed = true;
    std::vector<uint8_t> cs;
    std::string log;
    if (!compile_layer_shader(std::string(kTlasBuilder, sizeof kTlasBuilder - 1), "cs_6_0", cs, log)) {
        XSL_ERR(CH, "TLAS builder: compile failed: %s", log.c_str());
        return false;
    }
    ID3DBlob* err = nullptr;
    static const D3D12_DESCRIPTOR_RANGE ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 901, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGmHalf - 1, 0, 900, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kGmSlots - kGmHalf, 0, 900, kGmHalf}};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 12;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {3, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rsb = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&g_tlas.rs)))) {
        XSL_ERR(CH, "TLAS builder: root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_tlas.rs;
    pd.CS = {cs.data(), cs.size()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_tlas.pso)))) { XSL_ERR(CH, "TLAS builder: pipeline failed"); return false; }
    g_tlas.failed = false;
    XSL_INFO(CH, "top-level acceleration structures are built in the console layout");
    return true;
}
uint64_t tlas_console_size(uint32_t n) {   // bytes of the layout above for n instances
    uint64_t boxes = 0;
    for (uint64_t c = n; c > 0;) { uint64_t lc = (c + 3) / 4; boxes += lc; c = lc == 1 ? 0 : lc; }
    return 64 + boxes * 128 + (uint64_t)n * 128;
}
uint64_t tlas_scratch_size(uint32_t n) { return (uint64_t)n * 32 + (tlas_console_size(n) / 128) * 32 + 256; }
static void restore_compute_state(PcObj l);
bool list_rt_build_tlas(PcObj l, uint64_t dst, uint64_t inst, uint32_t n, uint32_t layout, uint64_t scratch) {
    if (!gm_enabled() || !g_gv || !g_main_device) return false;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto h = g_list_global_heap.find(l);
        if (h == g_list_global_heap.end() || !h->second) {
            static volatile long w = 0;
            if (InterlockedIncrement(&w) <= 3) XSL_WARN(CH, "TLAS build on a list without the global heap bound: skipped");
            return false;
        }
    }
    {   // memory made this frame (transient instance buffers) may not be in the GPU table yet
        std::lock_guard<std::mutex> g(g_gm_mu);
        if (!gm_in_table(inst, (uint64_t)n * (layout ? 8 : 64)) || !gm_in_table(dst, 64) || !gm_in_table(scratch, 32)) { g_gm_force = true; g_gm_dirty = true; }
    }
    PcObj dr, sr; uint64_t doff, davail, soff, savail;
    uint64_t boxes = (tlas_console_size(n) - 64 - (uint64_t)n * 128) / 128;
    uint64_t instOff = 64 + boxes * 128;
    if (uint64_t own = rt_scratch((uint64_t)n * 32 + boxes * 32)) scratch = own;
    { std::lock_guard<std::mutex> g(g_gm_mu); if (!gm_in_table(scratch, 32)) { g_gm_force = true; g_gm_dirty = true; } }
    if (!gm_resolve(dst, &dr, &doff, &davail) || davail < tlas_console_size(n) || !gm_resolve(scratch, &sr, &soff, &savail) ||
        savail < (uint64_t)n * 32 + boxes * 32) {
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 5)
            XSL_WARN(CH, "TLAS build (%u instances) at %llx: destination %s (%llu of %llu bytes), scratch %llx %s (%llu of %llu bytes)", n, (unsigned long long)dst,
                     gm_resolve(dst, &dr, &doff, &davail) ? "ok" : "NOT addressable", (unsigned long long)davail, (unsigned long long)tlas_console_size(n),
                     (unsigned long long)scratch, gm_resolve(scratch, &sr, &soff, &savail) ? "ok" : "NOT addressable", (unsigned long long)savail,
                     (unsigned long long)((uint64_t)n * 32 + boxes * 32));
        return false;
    }
    if (!tlas_init(g_main_device)) return false;
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    L2->ResourceBarrier(1, &ub);   // instance data written just before
    L2->SetPipelineState(g_tlas.pso);
    L2->SetComputeRootSignature(g_tlas.rs);
    uint32_t c[12] = {(uint32_t)dst, (uint32_t)(dst >> 32), (uint32_t)inst, (uint32_t)(inst >> 32), (uint32_t)scratch, (uint32_t)(scratch >> 32),
                      n, layout, (uint32_t)instOff, 0, 0, 0};
    L2->SetComputeRoot32BitConstants(0, 12, c, 0);
    L2->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{g_gv->GetGPUDescriptorHandleForHeapStart().ptr + (uint64_t)gm_reserved_start() * g_gv_inc});
    L2->Dispatch(1, 1, 1);
    L2->ResourceBarrier(1, &ub);
    static volatile long dumped = 0;
    if (n && getenv("XSL_TLAS_DUMP") && InterlockedIncrement(&dumped) == 1) {   // header, first box nodes, first instance nodes
        auto tr = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER t{};
            t.Transition.pResource = as<ID3D12Resource>(dr);
            t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            t.Transition.StateBefore = a;
            t.Transition.StateAfter = b;
            L2->ResourceBarrier(1, &t);
        };
        tr(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (uint64_t part : {0ull, instOff}) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 512;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* rb = nullptr;
            if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) continue;
            L2->CopyBufferRegion(rb, 0, as<ID3D12Resource>(dr), doff + part, 512);
            std::lock_guard<std::mutex> g(g_gm_mu);
            g_as_dumps.push_back({rb, dst + part, g_gm_frame.load()});
        }
        tr(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    restore_compute_state(l);
    static volatile long logged = 0;
    if (InterlockedIncrement(&logged) <= 3) {
        PcObj ir; uint64_t io = 0, ia = 0;
        bool ok = gm_resolve(inst, &ir, &io, &ia);
        D3D12_HEAP_PROPERTIES ihp{};
        if (ok) as<ID3D12Resource>(ir)->GetHeapProperties(&ihp, nullptr);
        XSL_INFO(CH, "TLAS build: %u instances at %llx (console layout, %llu bytes), instance descs %llx %s (heap type %d, %llu bytes there)", n,
                 (unsigned long long)dst, (unsigned long long)tlas_console_size(n), (unsigned long long)inst, ok ? "addressable" : "NOT addressable",
                 (int)ihp.Type, (unsigned long long)ia);
    }
    return true;
}
// ---- Bottom-level acceleration structures (triangles) in the console layout ---------------------
//   header (flags 0x80000004: bottom level, version 2008; rootNodePtr at +48; sceneBounds)
//   box32 nodes (bottom level first, root last), then one 64-byte leaf per triangle:
//   v0, v1, v2, v2 (the unused 4th vertex), (geometry index << 8) | geometry flags, primitive index,
//   0xffffffff (no second triangle), barycentric remap 0x09 (identity for TRI0). Leaf pointers have
//   type TRI0. Leaves keep the index order; inactive triangles (NaN x) are ignored leaves with
//   empty bounds. Builds and updates are full rebuilds; procedural (AABB) geometry is not built.
static const char kBlasBuilder[] = R"(
StructuredBuffer<uint4> GMT : register(t0, space901);
ByteAddressBuffer GMS[8191] : register(t0, space900);
globallycoherent RWByteAddressBuffer GMU[8192] : register(u0, space900);
cbuffer C : register(b0) {
  uint dstLo, dstHi, scrLo, scrHi, mode, count, base, prev;
  uint vbLo, vbHi, vbStride, vbFormat, ibLo, ibHi, ibFormat, trLo;
  uint trHi, geomWord, leafOff, leafBase, total, boxTotal, pad0, pad1;
};
uint3 gm_find(uint lo, uint hi) {
  uint a = 0u, b = GMT[0].x;
  while (a < b) { uint m = (a + b) >> 1; uint4 e = GMT[1u + m]; if (e.y < hi || (e.y == hi && e.x <= lo)) a = m + 1u; else b = m; }
  if (a == 0u) return uint3(0u, 0u, 0u);
  uint4 e = GMT[a];
  uint off = lo - e.x, ohi = hi - e.y - (lo < e.x ? 1u : 0u);
  return (ohi == 0u && off < e.z) ? uint3(1u, e.w, off) : uint3(0u, 0u, 0u);
}
uint2 at(uint lo, uint hi, uint o) { uint r = lo + o; return uint2(r, hi + (r < lo ? 1u : 0u)); }
uint ld(uint2 a) {
  uint3 f = gm_find(a.x, a.y);
  if (f.x == 0u) return 0u;
  if (f.y & 0x80000000u) return GMU[NonUniformResourceIndex((f.y >> 14) & 0x3fffu)].Load(f.z & ~3u);
  if (f.y & 0x40000000u) return GMS[NonUniformResourceIndex(f.y & 0x3fffu)].Load(f.z & ~3u);
  return 0u;
}
void st(uint2 a, uint v) {
  uint3 f = gm_find(a.x, a.y);
  if (f.x != 0u && (f.y & 0x80000000u)) GMU[NonUniformResourceIndex((f.y >> 14) & 0x3fffu)].Store(f.z & ~3u, v);
}
float ldf(uint2 a) { return asfloat(ld(a)); }
void stf(uint2 a, float v) { st(a, asuint(v)); }
uint ld16(uint2 a) { uint w = ld(uint2(a.x & ~3u, a.y)); return (a.x & 2u) ? (w >> 16) : (w & 0xffffu); }
float snorm16(uint v) { int s = int(v << 16) >> 16; return max(float(s) / 32767.0, -1.0); }
void box_of(uint2 a, out float3 mn, out float3 mx) {
  mn = float3(ldf(a), ldf(at(a.x, a.y, 4u)), ldf(at(a.x, a.y, 8u)));
  mx = float3(ldf(at(a.x, a.y, 12u)), ldf(at(a.x, a.y, 16u)), ldf(at(a.x, a.y, 20u)));
}
void put_box(uint2 a, float3 mn, float3 mx) {
  stf(a, mn.x); stf(at(a.x, a.y, 4u), mn.y); stf(at(a.x, a.y, 8u), mn.z);
  stf(at(a.x, a.y, 12u), mx.x); stf(at(a.x, a.y, 16u), mx.y); stf(at(a.x, a.y, 20u), mx.z);
}
float3 vertex(uint i) {
  uint2 a = at(vbLo, vbHi, i * vbStride);
  float3 p;
  if (vbFormat == 2u || vbFormat == 6u) p = float3(ldf(a), ldf(at(a.x, a.y, 4u)), ldf(at(a.x, a.y, 8u)));
  else if (vbFormat == 16u) p = float3(ldf(a), ldf(at(a.x, a.y, 4u)), 0.0);
  else if (vbFormat == 10u) p = float3(f16tof32(ld16(a)), f16tof32(ld16(at(a.x, a.y, 2u))), f16tof32(ld16(at(a.x, a.y, 4u))));
  else if (vbFormat == 34u) p = float3(f16tof32(ld16(a)), f16tof32(ld16(at(a.x, a.y, 2u))), 0.0);
  else if (vbFormat == 13u) p = float3(snorm16(ld16(a)), snorm16(ld16(at(a.x, a.y, 2u))), snorm16(ld16(at(a.x, a.y, 4u))));
  else if (vbFormat == 37u) p = float3(snorm16(ld16(a)), snorm16(ld16(at(a.x, a.y, 2u))), 0.0);
  else p = asfloat(0x7fc00000u).xxx;   // unknown format: inactive
  if ((trLo | trHi) != 0u) {
    float4 r0 = float4(ldf(uint2(trLo, trHi)), ldf(at(trLo, trHi, 4u)), ldf(at(trLo, trHi, 8u)), ldf(at(trLo, trHi, 12u)));
    float4 r1 = float4(ldf(at(trLo, trHi, 16u)), ldf(at(trLo, trHi, 20u)), ldf(at(trLo, trHi, 24u)), ldf(at(trLo, trHi, 28u)));
    float4 r2 = float4(ldf(at(trLo, trHi, 32u)), ldf(at(trLo, trHi, 36u)), ldf(at(trLo, trHi, 40u)), ldf(at(trLo, trHi, 44u)));
    float4 q = float4(p, 1.0);
    p = float3(dot(r0, q), dot(r1, q), dot(r2, q));
  }
  return p;
}
uint index_of(uint k) {
  if (ibFormat == 42u) return ld(at(ibLo, ibHi, k * 4u));       // R32_UINT
  if (ibFormat == 57u) return ld16(at(ibLo, ibHi, k * 2u));     // R16_UINT
  return k;                                                     // no index buffer
}
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  const float BIG = 3.0e38;
  uint i = tid.x;
  if (mode == 0u) {   // leaves of one geometry: triangles [0, count) -> leaves [leafBase, leafBase + count)
    if (i >= count) return;
    float3 a = vertex(index_of(i * 3u)), b = vertex(index_of(i * 3u + 1u)), c = vertex(index_of(i * 3u + 2u));
    bool active = !(a.x != a.x || b.x != b.x || c.x != c.x);
    uint L = leafBase + i;
    uint2 lf = at(dstLo, dstHi, leafOff + L * 64u);
    stf(lf, a.x); stf(at(lf.x, lf.y, 4u), a.y); stf(at(lf.x, lf.y, 8u), a.z);
    stf(at(lf.x, lf.y, 12u), b.x); stf(at(lf.x, lf.y, 16u), b.y); stf(at(lf.x, lf.y, 20u), b.z);
    stf(at(lf.x, lf.y, 24u), c.x); stf(at(lf.x, lf.y, 28u), c.y); stf(at(lf.x, lf.y, 32u), c.z);
    stf(at(lf.x, lf.y, 36u), c.x); stf(at(lf.x, lf.y, 40u), c.y); stf(at(lf.x, lf.y, 44u), c.z);
    st(at(lf.x, lf.y, 48u), geomWord);
    st(at(lf.x, lf.y, 52u), active ? i : 0xffffffffu);
    st(at(lf.x, lf.y, 56u), 0xffffffffu);
    st(at(lf.x, lf.y, 60u), 0x09u);
    float3 mn = active ? min(a, min(b, c)) : float3(BIG, BIG, BIG);
    float3 mx = active ? max(a, max(b, c)) : float3(-BIG, -BIG, -BIG);
    put_box(at(scrLo, scrHi, L * 32u), mn, mx);
  } else if (mode == 1u) {   // one box level: nodes [base, base + count), children = leaves (prev == ~0) or boxes from prev
    if (i >= count) return;
    uint nchild = prev == 0xffffffffu ? total : (base - prev);
    uint2 nd = at(dstLo, dstHi, 64u + (base + i) * 128u);
    float3 umn = float3(BIG, BIG, BIG), umx = float3(-BIG, -BIG, -BIG);
    [unroll] for (uint k = 0u; k < 4u; k++) {
      uint c2 = i * 4u + k;
      uint ptr = 0xffffffffu;
      float3 mn = float3(BIG, BIG, BIG), mx = float3(-BIG, -BIG, -BIG);
      if (c2 < nchild) {
        if (prev == 0xffffffffu) { ptr = ((leafOff + c2 * 64u) >> 6) << 3; box_of(at(scrLo, scrHi, c2 * 32u), mn, mx); }
        else { uint bi = prev + c2; ptr = (((64u + bi * 128u) >> 6) << 3) | 5u; box_of(at(scrLo, scrHi, total * 32u + bi * 32u), mn, mx); }
        if (mn.x <= mx.x) { umn = min(umn, mn); umx = max(umx, mx); }
      }
      st(at(nd.x, nd.y, 4u * k), ptr);
      put_box(at(nd.x, nd.y, 16u + 24u * k), mn, mx);
    }
    st(at(nd.x, nd.y, 112u), 0u); st(at(nd.x, nd.y, 116u), 0u); st(at(nd.x, nd.y, 120u), 0u); st(at(nd.x, nd.y, 124u), 0u);
    put_box(at(scrLo, scrHi, total * 32u + (base + i) * 32u), umn, umx);
  } else if (i == 0u) {   // header; base = root box index (or ~0 when empty)
    uint2 h = uint2(dstLo, dstHi);
    float3 mn = float3(0, 0, 0), mx = float3(0, 0, 0);
    uint root = 0xffffffffu;
    if (base != 0xffffffffu) { root = (((64u + base * 128u) >> 6) << 3) | 5u; box_of(at(scrLo, scrHi, total * 32u + base * 32u), mn, mx); }
    st(h, 0x80000004u);
    st(at(h.x, h.y, 4u), (leafOff + total * 64u) / 64u);
    st(at(h.x, h.y, 8u), 0u);
    st(at(h.x, h.y, 12u), total);
    put_box(at(h.x, h.y, 16u), mn, mx);
    st(at(h.x, h.y, 40u), 64u);
    st(at(h.x, h.y, 44u), 0u);
    st(at(h.x, h.y, 48u), root);
    st(at(h.x, h.y, 52u), leafOff);
    st(at(h.x, h.y, 56u), 0u);
    st(at(h.x, h.y, 60u), 0x007F7F7Fu);
  }
}
)";
struct BlasBuilder { ID3D12RootSignature* rs = nullptr; ID3D12PipelineState* pso = nullptr; bool failed = false; std::mutex mu; } g_blas;
bool compile_layer_shader(const std::string& hlsl, const char* profile, std::vector<uint8_t>& out, std::string& log);
static bool blas_init(ID3D12Device* dev) {
    std::lock_guard<std::mutex> g(g_blas.mu);
    if (g_blas.pso) return true;
    if (g_blas.failed) return false;
    g_blas.failed = true;
    std::vector<uint8_t> cs;
    std::string log;
    if (!compile_layer_shader(std::string(kBlasBuilder, sizeof kBlasBuilder - 1), "cs_6_0", cs, log)) {
        XSL_ERR(CH, "BLAS builder: compile failed: %s", log.c_str());
        return false;
    }
    static const D3D12_DESCRIPTOR_RANGE ranges[3] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 901, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGmHalf - 1, 0, 900, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kGmSlots - kGmHalf, 0, 900, kGmHalf}};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 24;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {3, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *rsb = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&g_blas.rs)))) {
        XSL_ERR(CH, "BLAS builder: root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_blas.rs;
    pd.CS = {cs.data(), cs.size()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_blas.pso)))) { XSL_ERR(CH, "BLAS builder: pipeline failed"); return false; }
    g_blas.failed = false;
    XSL_INFO(CH, "bottom-level acceleration structures (triangles) are built in the console layout");
    return true;
}
static uint64_t blas_boxes(uint64_t n) {
    uint64_t boxes = 0;
    for (uint64_t c = n; c > 0;) { uint64_t lc = (c + 3) / 4; boxes += lc; c = lc == 1 ? 0 : lc; }
    return boxes;
}
uint64_t blas_console_size(uint64_t tris) { return 64 + blas_boxes(tris) * 128 + tris * 64; }
uint64_t blas_scratch_size(uint64_t tris) { return (tris + blas_boxes(tris)) * 32 + 256; }
bool list_rt_build_blas(PcObj l, uint64_t dst, uint64_t scratch, uint32_t ngeo, const BlasGeometry* geo) {
    if (!gm_enabled() || !g_gv || !g_main_device) return false;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto h = g_list_global_heap.find(l);
        if (h == g_list_global_heap.end() || !h->second) return false;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < ngeo; i++) total += geo[i].triangles;
    if (total >= (1u << 26)) return false;
    uint64_t boxes = blas_boxes(total);
    uint32_t leafOff = (uint32_t)(64 + boxes * 128);
    if (uint64_t own = rt_scratch((total + boxes) * 32)) scratch = own;
    { std::lock_guard<std::mutex> g(g_gm_mu); if (!gm_in_table(scratch, 32)) { g_gm_force = true; g_gm_dirty = true; } }
    PcObj dr, sr; uint64_t doff, davail, soff, savail;
    {   // memory made this frame may not be in the GPU table yet
        std::lock_guard<std::mutex> g(g_gm_mu);
        bool missing = !gm_in_table(dst, 64) || !gm_in_table(scratch, 32);
        for (uint32_t i = 0; i < ngeo && !missing; i++) missing = !gm_in_table(geo[i].vb, 4) || (geo[i].ib && !gm_in_table(geo[i].ib, 2));
        if (missing) { g_gm_force = true; g_gm_dirty = true; }
    }
    if (!gm_resolve(dst, &dr, &doff, &davail) || davail < blas_console_size(total) || !gm_resolve(scratch, &sr, &soff, &savail) ||
        savail < (total + boxes) * 32) {
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 5)
            XSL_WARN(CH, "BLAS build (%llu triangles) at %llx: destination %s (%llu of %llu bytes), scratch %llx %s (%llu of %llu bytes)", (unsigned long long)total,
                     (unsigned long long)dst, gm_resolve(dst, &dr, &doff, &davail) ? "ok" : "NOT addressable", (unsigned long long)davail,
                     (unsigned long long)blas_console_size(total), (unsigned long long)scratch, gm_resolve(scratch, &sr, &soff, &savail) ? "ok" : "NOT addressable",
                     (unsigned long long)savail, (unsigned long long)((total + boxes) * 32));
        return false;
    }
    if (!blas_init(g_main_device)) return false;
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    L2->ResourceBarrier(1, &ub);
    L2->SetPipelineState(g_blas.pso);
    L2->SetComputeRootSignature(g_blas.rs);
    L2->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{g_gv->GetGPUDescriptorHandleForHeapStart().ptr + (uint64_t)gm_reserved_start() * g_gv_inc});
    uint32_t c[24] = {(uint32_t)dst, (uint32_t)(dst >> 32), (uint32_t)scratch, (uint32_t)(scratch >> 32)};
    c[18] = leafOff;
    c[20] = (uint32_t)total;
    c[21] = (uint32_t)boxes;
    uint64_t leafBase = 0;
    for (uint32_t gi = 0; gi < ngeo; gi++) {   // leaves
        const BlasGeometry& gm = geo[gi];
        if (!gm.triangles) continue;
        c[4] = 0; c[5] = gm.triangles;
        c[8] = (uint32_t)gm.vb; c[9] = (uint32_t)(gm.vb >> 32); c[10] = gm.vbStride; c[11] = gm.vbFormat;
        c[12] = (uint32_t)gm.ib; c[13] = (uint32_t)(gm.ib >> 32); c[14] = gm.ib ? gm.ibFormat : 0;
        c[15] = (uint32_t)gm.transform; c[16] = (uint32_t)(gm.transform >> 32);
        c[17] = (gi << 8) | (gm.flags & 0xff);
        c[19] = (uint32_t)leafBase;
        L2->SetComputeRoot32BitConstants(0, 24, c, 0);
        L2->Dispatch((gm.triangles + 63) / 64, 1, 1);
        leafBase += gm.triangles;
    }
    L2->ResourceBarrier(1, &ub);
    uint32_t base = 0, prev = 0xffffffffu;
    for (uint64_t cnt = total; cnt > 0;) {   // box levels, bottom up
        uint32_t lc = (uint32_t)((cnt + 3) / 4);
        c[4] = 1; c[5] = lc; c[6] = base; c[7] = prev;
        L2->SetComputeRoot32BitConstants(0, 24, c, 0);
        L2->Dispatch((lc + 63) / 64, 1, 1);
        L2->ResourceBarrier(1, &ub);
        prev = base;
        base += lc;
        cnt = lc == 1 ? 0 : lc;
    }
    c[4] = 2; c[6] = total ? base - 1 : 0xffffffffu;
    L2->SetComputeRoot32BitConstants(0, 24, c, 0);
    L2->Dispatch(1, 1, 1);
    L2->ResourceBarrier(1, &ub);
    restore_compute_state(l);
    static volatile long logged = 0;
    if (InterlockedIncrement(&logged) <= 3)
        XSL_INFO(CH, "BLAS build: %u geometries, %llu triangles at %llx (console layout, %llu bytes)", ngeo, (unsigned long long)total, (unsigned long long)dst,
                 (unsigned long long)blas_console_size(total));
    return true;
}
// ---- Console ray-tracing pipelines (translated literally: gpucode/rtso.h) -------------------------
// Root signature of a linked pipeline's program: 8 root constants (runtime table address, group
// counts, stack bytes per wave), the global-memory table, and the bindless global-heap arrays of
// image access through descriptor memory (spaces 910..918, SRVs and UAVs, unbounded from slot 0).
static ID3D12RootSignature* rt_root_signature(ID3D12Device* dev) {
    static ID3D12RootSignature* rs = nullptr;
    static std::once_flag once;
    std::call_once(once, [dev] {
        const D3D12_DESCRIPTOR_RANGE_FLAGS vol = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        static D3D12_DESCRIPTOR_RANGE1 gm[3];
        gm[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 901, vol, 0};
        gm[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kGmHalf - 1, 0, 900, vol, 1};
        gm[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kGmSlots - kGmHalf, 0, 900, vol, kGmHalf};
        static D3D12_DESCRIPTOR_RANGE1 heap[18];
        std::vector<D3D12_ROOT_PARAMETER1> params;
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants = {0, 0, 8};
        params.push_back(p);
        p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = {3, gm};
        params.push_back(p);
        for (uint32_t k = 0; k < 9; k++) {
            heap[2 * k] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 910 + k, vol, 0};
            heap[2 * k + 1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, 910 + k, vol, 0};
            for (int j = 0; j < 2; j++) {
                p = {};
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                p.DescriptorTable = {1, &heap[2 * k + j]};
                params.push_back(p);
            }
        }
        // parameter 20: cube SRVs (space 919); 21: the title's sampler heap (space 920)
        static D3D12_DESCRIPTOR_RANGE1 cube = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 919, vol, 0};
        static D3D12_DESCRIPTOR_RANGE1 smp = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 920, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
        p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = {1, &cube};
        params.push_back(p);
        p.DescriptorTable = {1, &smp};
        params.push_back(p);
        // parameters 22..24: parked-lane state (u0 space930), the discovery bitmap (u0 space931),
        // debug counters (u0 space932)
        for (UINT sp : {930u, 931u, 932u}) {
            p = {};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p.Descriptor = {0, sp, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
            params.push_back(p);
        }
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{D3D_ROOT_SIGNATURE_VERSION_1_1};
        d.Desc_1_1 = {(UINT)params.size(), params.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *b = nullptr, *e = nullptr;
        if (FAILED(D3D12SerializeVersionedRootSignature(&d, &b, &e)) ||
            FAILED(dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
            XSL_ERR(CH, "ray-tracing root signature failed: %s", e ? (const char*)e->GetBufferPointer() : "?");
            rs = nullptr;
        }
    });
    return rs;
}
// XSL_RT_WARM=1: create (and release) a pipeline for every cached ray-tracing program on all
// processors at device creation, so the driver's own shader cache holds them (the driver takes tens
// of seconds per unoptimized program the first time).
std::wstring rt_cache_dir();
void rt_warm_start() {
    char b[4];
    if (!GetEnvironmentVariableA("XSL_RT_WARM", b, sizeof b) || b[0] != '1') return;
    auto files = std::make_shared<std::vector<std::wstring>>();
    WIN32_FIND_DATAW fd;
    std::wstring dir = rt_cache_dir();
    wchar_t wd[MAX_PATH];
    if (GetEnvironmentVariableW(L"XSL_RT_WARM_DIR", wd, MAX_PATH)) dir = std::wstring(wd) + L"\\";   // diagnostics: another directory
    HANDLE h = FindFirstFileW((dir + L"rt_*.dxil").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do files->push_back(dir + fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    auto next = std::make_shared<std::atomic<size_t>>(0);
    auto done = std::make_shared<std::atomic<size_t>>(0);
    DWORD t0 = GetTickCount();
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    XSL_INFO(CH, "ray-tracing warm-up: %zu programs on %u threads", files->size(), n);
    for (unsigned t = 0; t < n; t++)
        std::thread([files, next, done, t0] {
            for (size_t i; (i = (*next)++) < files->size();) {
                HANDLE f = CreateFileW((*files)[i].c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) continue;
                std::vector<uint8_t> d(GetFileSize(f, nullptr));
                DWORD got = 0;
                ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr);
                CloseHandle(f);
                DWORD s = GetTickCount();
                if (PcObj p = rt_create_pso((PcObj)g_main_device, d)) as<ID3D12PipelineState>(p)->Release();
                size_t k = ++*done;
                if (k % 16 == 0 || k == files->size() || files->size() <= 16)
                    XSL_INFO(CH, "ray-tracing warm-up: %zu/%zu programs (last %ls: %lu ms, %lu s so far)", k, files->size(),
                             (*files)[i].substr((*files)[i].find_last_of(L"\\") + 1).c_str(), (unsigned long)(GetTickCount() - s),
                             (unsigned long)((GetTickCount() - t0) / 1000));
            }
        }).detach();
}
PcObj rt_create_pso(PcObj devObj, const std::vector<uint8_t>& dxil);
// Offline warm-up (tools/rtwarm): the driver's compiled form of every cached program whose index
// modulo `shards` is `shard`, on a device of the caller's. The driver compiles one pipeline at a
// time per process, so several processes (one per shard) are what makes this fast.
int rt_warm_shard(void* device, unsigned shard, unsigned shards) {
    auto* dev = (ID3D12Device*)device;
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    std::wstring dir = rt_cache_dir();
    HANDLE h = FindFirstFileW((dir + L"rt_*.dxil").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do files.push_back(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(files.begin(), files.end());
    int made = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (shards && i % shards != shard) continue;
        std::vector<uint8_t> d;
        HANDLE f = CreateFileW((dir + files[i]).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        d.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr);
        CloseHandle(f);
        DWORD t0 = GetTickCount();
        if (PcObj p = rt_create_pso((PcObj)dev, d)) {
            ((ID3D12PipelineState*)p)->Release();
            made++;
            fprintf(stderr, "%ls: %lu ms\n", files[i].c_str(), (unsigned long)(GetTickCount() - t0));
        } else fprintf(stderr, "%ls: failed\n", files[i].c_str());
    }
    return made;
}
// The driver's compiled form of a ray-tracing program is kept next to its DXIL (rtpso_<hash>.bin,
// from GetCachedBlob): the driver needs 15 s to minutes per program and its own cache does not
// reliably keep programs this large. A blob from another driver or adapter is refused and rebuilt.
static bool read_all(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out.resize(GetFileSize(f, nullptr));
    DWORD got = 0;
    bool ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(f);
    return ok && !out.empty();
}
static void write_all(const std::wstring& path, const void* d, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, d, (DWORD)n, &put, nullptr) && put == n;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}
PcObj rt_create_pso(PcObj devObj, const std::vector<uint8_t>& dxil) {
    auto* dev = as<ID3D12Device>(devObj);
    ID3D12RootSignature* rs = rt_root_signature(dev);
    if (!rs) return nullptr;
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : dxil) h = (h ^ c) * 1099511628211ull;
    wchar_t name[64];
    swprintf(name, 64, L"rtpso_%016llx.bin", (unsigned long long)h);
    std::wstring path = rt_cache_dir() + name;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rs;
    pd.CS = {dxil.data(), dxil.size()};
    ID3D12PipelineState* pso = nullptr;
    std::vector<uint8_t> blob;
    if (read_all(path, blob)) {
        pd.CachedPSO = {blob.data(), blob.size()};
        HRESULT ch = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(ch)) {   // another driver: rebuild
            pso = nullptr;
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 5) XSL_WARN(CH, "ray-tracing pipeline: stored driver blob refused (%08lx, %zu bytes); rebuilding", ch, blob.size());
        }
        pd.CachedPSO = {};
    }
    if (!pso) {
        HRESULT hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) { XSL_ERR(CH, "ray-tracing pipeline: CreateComputePipelineState failed %08lx", hr); return nullptr; }
        ID3DBlob* cb = nullptr;
        if (SUCCEEDED(pso->GetCachedBlob(&cb)) && cb) {
            write_all(path, cb->GetBufferPointer(), cb->GetBufferSize());
            cb->Release();
        }
    }
    return (PcObj)pso;
}
// Per-wave ray stacks: kRtStackSlots slots of `perWave` bytes in one layer buffer.
uint64_t rt_stack_buffer(uint64_t bytes) {
    static ID3D12Resource* buf = nullptr;
    static uint64_t size = 0;
    static std::mutex mu;
    std::lock_guard<std::mutex> g(mu);
    if (buf && size >= bytes) return buf->GetGPUVirtualAddress();
    if (!g_main_device) return 0;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) return 0;
    buf = r;   // an earlier, smaller buffer stays alive (in-flight work may use it)
    size = bytes;
    gm_register((PcObj)r, r->GetGPUVirtualAddress(), bytes, 0);
    XSL_INFO(CH, "ray stacks: %llu MiB", (unsigned long long)(bytes >> 20));
    return r->GetGPUVirtualAddress();
}
// Ray dispatches: a one-thread prep pass writes the scheduler's runtime table (and the group counts)
// into a slice of a GPU ring registered in global memory below 4 GB, from the dispatch arguments
// (root constants for DispatchRays, the 104-byte argument record for indirect dispatches); then
// the translated program runs as an indirect dispatch of those counts.
static const char kRtPrep[] = R"(
RWByteAddressBuffer T : register(u0);   // table slice
ByteAddressBuffer A : register(t0);     // indirect arguments (D3D12_DISPATCH_RAYS_DESC)
RWByteAddressBuffer DBG : register(u1); // debug counters (slots 32..: the raw arguments of the last dispatch)
ByteAddressBuffer RGR : register(t1);   // direct dispatches: the ray-generation record, first miss and hit records (diagnostics)
ByteAddressBuffer MSR : register(t2);
ByteAddressBuffer HGR : register(t3);
cbuffer C : register(b0) {
  uint indirect, srv, cbv, smp, uav, stackLo, stackHi, perLane;
  uint trav0, trav1, trav2, tableLo, tableHi, w, h, d;
  uint rgLo, rgHi, missLo, missHi, missStride, hitLo, hitHi, hitStride;
  uint callLo, callHi, pad0, pad1;
};
[numthreads(1, 1, 1)] void main() {
  uint W = w, H = h, D = d, RG0 = rgLo, RG1 = rgHi, M0 = missLo, M1 = missHi, MS = missStride, H0 = hitLo, H1 = hitHi, HS = hitStride, C0 = callLo, C1 = callHi;
  if (indirect != 0u) {
    RG0 = A.Load(0); RG1 = A.Load(4); M0 = A.Load(16); M1 = A.Load(20); MS = A.Load(32);
    H0 = A.Load(40); H1 = A.Load(44); HS = A.Load(56); C0 = A.Load(64); C1 = A.Load(68);
    W = A.Load(88); H = A.Load(92); D = A.Load(96);
  }
  DBG.Store4(pad0 + 128u, uint4(RG0, RG1, M0, M1));
  DBG.Store4(pad0 + 144u, uint4(H0, H1, MS, HS));
  DBG.Store4(pad0 + 160u, uint4(W, H, D, indirect));
  if (pad1 != 0u) { DBG.Store4(pad0 + 176u, RGR.Load4(0)); DBG.Store4(pad0 + 192u, MSR.Load4(0)); DBG.Store4(pad0 + 208u, HGR.Load4(0)); }
  T.Store4(0, uint4(srv, cbv, smp, uav));
  uint trav = tableLo + 0xC0u;
  T.Store2(0x18, uint2(trav, tableHi + (trav < tableLo ? 1u : 0u)));
  T.Store3(0x20, uint3(W, H, D));
  T.Store(0x2C, ((HS / 32u) & 0xffu) << 21 | ((MS / 32u) & 0xffu) << 5);
  // Launch index packing (read by the scheduler and, through s13 >> 16, the shaders): x in the low
  // bits, y from bit ysh, z from bit zsh; s18 = ysh << 8 | (32 - zsh) & 31.
  uint ysh = W > 1u ? firstbithigh(W - 1u) + 1u : 0u, zsh = ysh + (H > 1u ? firstbithigh(H - 1u) + 1u : 0u);
  T.Store(0x30, (ysh << 8) | ((32u - zsh) & 31u));
  T.Store4(0x34, uint4((M0 >> 6) | (M1 << 26), (H0 >> 6) | (H1 << 26), (RG0 >> 6) | (RG1 << 26), (C0 >> 6) | (C1 << 26)));
  T.Store(0x44, 0u);
  T.Store4(0x48, uint4(perLane, 0u, 0u, 0u));
  T.Store4(0x90, uint4(stackLo, stackHi & 0xffffu, 0xffffffffu, (20u << 12) | (1u << 23)));   // ADD_TID_ENABLE: a stack per lane
  for (uint k = 0u; k < 32u; k++) T.Store(0xC0u + 4u * k, k == 0u ? trav0 : k == 1u ? trav1 : trav2);
  uint gx, gy, gz = max(D, 1u);
  if (H < 2u) { gx = (W + 31u) / 32u; gy = max(H, 1u); } else { gx = (W + 7u) / 8u; gy = (H + 3u) / 4u; }
  if (W == 0u || gx > 65535u || gy > 65535u || gz > 65535u) gx = gy = gz = 0u;   // garbage arguments: no work (no GPU hang)
  T.Store2(0x1B8, uint2(gx, gy));   // root constants 2..3 of the programs (indirect signature)
  T.Store3(0x1C0, uint3(gx, gy, gz));
}
)";
struct RtRuntime {
    ID3D12RootSignature* prepRs = nullptr;
    ID3D12PipelineState* prep = nullptr;
    ID3D12CommandSignature* dispatchSig = nullptr;
    ID3D12Resource* ring = nullptr;
    uint64_t ringGm = 0, head = 0;
    bool failed = false;
    std::mutex mu;
} g_rtrt;
constexpr uint64_t kRtRing = 4u << 20, kRtSlice = 512;
bool compile_layer_shader(const std::string& hlsl, const char* profile, std::vector<uint8_t>& out, std::string& log);
static bool rtrt_init() {
    if (g_rtrt.prep) return true;
    if (g_rtrt.failed || !g_main_device) return false;
    g_rtrt.failed = true;
    auto* dev = g_main_device;
    std::vector<uint8_t> cs;
    std::string log;
    if (!compile_layer_shader(std::string(kRtPrep, sizeof kRtPrep - 1), "cs_6_0", cs, log)) { XSL_ERR(CH, "ray prep: %s", log.c_str()); return false; }
    D3D12_ROOT_PARAMETER params[7] = {};
    for (int k = 4; k < 7; k++) { params[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; params[k].Descriptor.ShaderRegister = (UINT)(k - 3); }
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 28;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd{7, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *b = nullptr, *e = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e)) ||
        FAILED(dev->CreateRootSignature(0, b->GetBufferPointer(), b->GetBufferSize(), IID_PPV_ARGS(&g_rtrt.prepRs))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_rtrt.prepRs;
    pd.CS = {cs.data(), cs.size()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_rtrt.prep)))) return false;
    D3D12_INDIRECT_ARGUMENT_DESC ad[2] = {};
    ad[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    ad[0].Constant = {0, 2, 2};   // groups x, y -> root constants 2..3 (lane and stack-slot indices)
    ad[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sd{20, 2, ad, 0};
    if (FAILED(dev->CreateCommandSignature(&sd, rt_root_signature(dev), IID_PPV_ARGS(&g_rtrt.dispatchSig)))) return false;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kRtRing;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rtrt.ring)))) return false;
    g_rtrt.ringGm = kLowBase + (64ull << 20);   // after the descriptor memory
    gm_register((PcObj)g_rtrt.ring, g_rtrt.ringGm, kRtRing, 0);
    g_rtrt.failed = false;
    return true;
}
volatile long g_rays_run = 0, g_rays_skipped = 0;   // ray dispatches per frame-rate period
volatile long g_rays_why[5] = {};   // per reason: no pipeline, start program building, failed, a reached program building, indirect path
void note_rays_skipped(int why) { InterlockedIncrement(&g_rays_skipped); InterlockedIncrement(&g_rays_why[why >= 0 && why < 5 ? why : 0]); }
static std::string fmt_rays(long run, long skipped) {
    char b[96];
    snprintf(b, sizeof b, "; ray dispatches: %ld run, %ld skipped", run, skipped);
    return b;
}
// Sampler descriptor memory for translated ray-tracing code: samplers are not shadowed, so a table
// handle maps to an identity array (entry k = {k, 0, 0, 0}, 16 bytes) at the heap index of the handle.
uint64_t sampler_memory_address(PcObj heap, uint64_t gpuHandle) {
    static uint64_t base = 0;
    static std::once_flag once;
    constexpr uint32_t kN = 4096;
    std::call_once(once, [] {
        if (!g_main_device) return;
        auto* m = (uint32_t*)gm_low_buffer((PcObj)g_main_device, 68ull << 20, kN * 16, &base);
        if (!m) { base = 0; return; }
        for (uint32_t k = 0; k < kN; k++) { m[4 * k] = k; m[4 * k + 1] = m[4 * k + 2] = m[4 * k + 3] = 0; }
    });
    if (!base || !heap || !g_main_device) return 0;
    uint64_t start = as<ID3D12DescriptorHeap>(heap)->GetGPUDescriptorHandleForHeapStart().ptr;
    uint32_t inc = g_main_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (gpuHandle < start || !inc) return 0;
    uint64_t i = (gpuHandle - start) / inc;
    return i < kN ? base + i * 16 : 0;
}
// Lane state of parked rays (TranslateOptions::raw_park) and per-pipeline discovery bitmaps.
constexpr uint32_t kRtDiscRegions = 64, kRtDiscBytes = 128u << 10;
struct RtPark {
    ID3D12Resource* state = nullptr;
    ID3D12Resource* disc = nullptr;
    uint8_t* discCpu = nullptr;
    ID3D12Resource* dbg = nullptr;   // debug counters: 256 bytes per pipeline region
    uint32_t* dbgCpu = nullptr;
    uint32_t lanes = 0;
    bool failed = false;
} g_rtpark;
static bool rtpark_init() {
    if (g_rtpark.state) return true;
    if (g_rtpark.failed || !g_main_device) return false;
    g_rtpark.failed = true;
    char b[16];
    g_rtpark.lanes = GetEnvironmentVariableA("XSL_RT_LANES", b, sizeof b) ? (uint32_t)atoi(b) : (1u << 20);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (uint64_t)g_rtpark.lanes * gpucode::kRtParkStride;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rtpark.state)))) return false;
    D3D12_HEAP_PROPERTIES cp{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
    bd.Width = (uint64_t)kRtDiscRegions * kRtDiscBytes;
    if (FAILED(g_main_device->CreateCommittedResource(&cp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rtpark.disc)))) return false;
    void* m = nullptr;
    if (FAILED(g_rtpark.disc->Map(0, nullptr, &m))) return false;
    g_rtpark.discCpu = (uint8_t*)m;
    memset(m, 0, (size_t)bd.Width);
    bd.Width = (uint64_t)kRtDiscRegions * 512;
    if (FAILED(g_main_device->CreateCommittedResource(&cp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_rtpark.dbg)))) return false;
    if (FAILED(g_rtpark.dbg->Map(0, nullptr, &m))) return false;
    g_rtpark.dbgCpu = (uint32_t*)m;
    memset(m, 0, (size_t)bd.Width);
    g_rtpark.failed = false;
    XSL_INFO(CH, "parked-ray state: %u lanes (%llu MiB)", g_rtpark.lanes, (unsigned long long)((uint64_t)g_rtpark.lanes * gpucode::kRtParkStride >> 20));
    return true;
}
// XSL_RT_DEBUG=1: the counters translated ray-tracing programs keep (TranslateOptions::raw_stats),
// logged per pipeline region with the frame-rate line and cleared.
// Where an address stands among the buffers registered in global memory (diagnostics).
static std::string gm_describe(uint64_t a) {
    std::lock_guard<std::mutex> g(g_gm_mu);
    char b[256];
    auto it = g_gm.upper_bound(a);
    uint64_t pv = 0, ps = 0, nv = 0;
    if (it != g_gm.end()) nv = it->first;
    if (it != g_gm.begin()) { auto p = std::prev(it); pv = p->first; ps = p->second.size; }
    bool in = ps && a < pv + ps;
    snprintf(b, sizeof b, "%llx %s (before: %llx+%llx, after: %llx; %zu buffers)", (unsigned long long)a, in ? "registered" : "NOT registered", (unsigned long long)pv,
             (unsigned long long)ps, (unsigned long long)nv, g_gm.size());
    std::string out = b;
    int shown = 0;
    for (auto& [va, e] : g_gm)   // every buffer holding the address (buffers may overlap)
        if (a >= va && a < va + e.size && shown++ < 4) {
            D3D12_RESOURCE_DESC d = e.res->GetDesc();
            D3D12_HEAP_PROPERTIES hp{};
            D3D12_HEAP_FLAGS hf{};
            bool heap = SUCCEEDED(e.res->GetHeapProperties(&hp, &hf));
            snprintf(b, sizeof b, "; inside %llx+%llx (res %p, PC VA %llx, width %llx, flags %#x, heap type %d)", (unsigned long long)va, (unsigned long long)e.size, (void*)e.res,
                     (unsigned long long)e.res->GetGPUVirtualAddress(), (unsigned long long)d.Width, (unsigned)d.Flags, heap ? (int)hp.Type : -1);
            out += b;
        }
    if (!shown) out += "; inside no buffer";
    return out;
}
std::string gm_describe_address(uint64_t a) { return gm_describe(a); }
void rt_debug_dump() {
    if (!g_rtpark.dbgCpu) return;
    for (uint32_t r = 0; r < kRtDiscRegions; r++) {
        uint32_t c[128];
        memcpy(c, g_rtpark.dbgCpu + r * 128, sizeof c);
        if (!c[0] && !c[9]) continue;
        memset(g_rtpark.dbgCpu + r * 128, 0, 512);
        if (c[66] | c[67])
            XSL_INFO(CH, "rt debug region %u: that lane's lookup of the raygen record: found %u, view %08x, offset %x, left %x, value %08x; table rows %u, generation %u; pointer %08x", r,
                     c[88], c[89], c[90], c[91], c[92], c[93], c[94], c[95]);
        if (c[66] | c[67])
            XSL_INFO(CH, "rt debug region %u: a lane without a pointer saw table s12.. %x %x | %u %u %u | %x %x | %x %x %x %x %x | %u; group %u %u %u; v0..3 %x %x %x %x; s0 s1 s2 %x %x %x", r,
                     c[64], c[65], c[66], c[67], c[68], c[69], c[70], c[71], c[72], c[73], c[74], c[75], c[76], c[77], c[78], c[79], c[80], c[81], c[82], c[83], c[84], c[85], c[86]);
        XSL_INFO(CH, "rt debug region %u: last dispatch%s raygen %08x'%08x miss %08x'%08x (stride %u) hit %08x'%08x (stride %u) size %ux%ux%u", r, c[43] ? " (indirect)" : "",
                 c[33], c[32], c[35], c[34], c[38], c[37], c[36], c[39], c[40], c[41], c[42]);
        XSL_INFO(CH, "rt debug region %u: at the scheduler: %u lanes, %u with a shader pointer (last without: %08x, round tag %x), %u entered code here; per round %u %u %u %u %u %u", r,
                 c[24], c[23], c[25], c[26], c[27], c[56], c[57], c[58], c[59], c[60], c[61]);
        if (c[44] | c[48] | c[52])
            XSL_INFO(CH, "rt debug region %u: records raygen %08x %08x %08x %08x; miss[0] %08x %08x %08x %08x; hit[0] %08x %08x %08x %08x", r, c[44], c[45], c[46], c[47],
                     c[48], c[49], c[50], c[51], c[52], c[53], c[54], c[55]);
        XSL_INFO(CH, "rt debug region %u: raygen record %s; hit table %s", r, gm_describe((uint64_t)c[33] << 32 | c[32]).c_str(), gm_describe((uint64_t)c[37] << 32 | c[36]).c_str());
        if (c[12]) XSL_INFO(CH, "rt debug region %u: last missed load %s", r, gm_describe((uint64_t)c[14] << 32 | c[13]).c_str());
        XSL_INFO(CH, "rt debug region %u: lanes %u, endpgm %u, unknown jump %u (last %08x'%08x), guard %u (block %u), parked %u (last %08x'%08x), resumed %u, bvh %u (results %u), "
                 "loads %u (missed %u, last %08x'%08x), stores %u (missed %u, last %08x'%08x)", r, c[0], c[1], c[2], c[4], c[3], c[5], c[19], c[6], c[8], c[7], c[9], c[10], c[11],
                 c[21], c[12], c[14], c[13], c[22], c[15], c[17], c[16]);
    }
}
const uint8_t* rt_discovery(uint32_t region) {
    std::lock_guard<std::mutex> g(g_rtrt.mu);
    if (!rtpark_init() || region >= kRtDiscRegions) return nullptr;
    return g_rtpark.discCpu + (size_t)region * kRtDiscBytes;
}
bool list_rays(PcObj l, const std::vector<RtRun>& runs, const RtRaysArgs& a) {
    if (!gm_enabled() || !g_gv || runs.empty() || !g_main_device) return false;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto h = g_list_global_heap.find(l);
        if (h == g_list_global_heap.end() || !h->second) return false;
    }
    ID3D12RootSignature* rs = rt_root_signature(g_main_device);
    uint64_t slice;
    {
        std::lock_guard<std::mutex> g(g_rtrt.mu);
        if (!rs || !rtrt_init() || !rtpark_init()) return false;
        if (g_rtrt.head + kRtSlice > kRtRing) g_rtrt.head = 0;
        slice = g_rtrt.head;
        g_rtrt.head += kRtSlice;
    }
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        if (!gm_in_table(g_rtrt.ringGm, 64) || !gm_in_table(a.stack, 64)) { g_gm_force = true; g_gm_dirty = true; }
        // Shader tables are often made just before the dispatch (a new buffer every frame): any
        // buffer registered since the last table build must be in the table this list runs with.
        if (g_gm_dirty) g_gm_force = true;
    }
    static volatile long serial = 0;
    uint32_t ser = (uint32_t)InterlockedIncrement(&serial) & 0xffffff;
    uint64_t table = g_rtrt.ringGm + slice;
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    auto tr = [&](D3D12_RESOURCE_STATES x, D3D12_RESOURCE_STATES y) {
        D3D12_RESOURCE_BARRIER t{};
        t.Transition.pResource = g_rtrt.ring;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = x;
        t.Transition.StateAfter = y;
        L2->ResourceBarrier(1, &t);
    };
    D3D12_RESOURCE_BARRIER ub{};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    L2->ResourceBarrier(1, &ub);
    // prep: the runtime table, the group counts and (for the indirect signature) groups x/y as root constants
    L2->SetPipelineState(g_rtrt.prep);
    L2->SetComputeRootSignature(g_rtrt.prepRs);
    uint32_t c[28] = {a.argsVa ? 1u : 0u, a.tables[0], a.tables[1], a.tables[2], a.tables[3], (uint32_t)a.stack, (uint32_t)(a.stack >> 32), a.stackPerLane,
                      a.traversal[0], a.traversal[1], a.traversal[2], (uint32_t)table, (uint32_t)(table >> 32), a.width, a.height, a.depth,
                      (uint32_t)a.raygen, (uint32_t)(a.raygen >> 32), (uint32_t)a.miss, (uint32_t)(a.miss >> 32), a.missStride,
                      (uint32_t)a.hit, (uint32_t)(a.hit >> 32), a.hitStride, (uint32_t)a.callable, (uint32_t)(a.callable >> 32),
                      std::min(a.discRegion, kRtDiscRegions - 1) * 512, (!a.argsVa && a.raygen && a.debugRecords) ? 1u : 0u};
    L2->SetComputeRoot32BitConstants(0, 28, c, 0);
    L2->SetComputeRootUnorderedAccessView(1, g_rtrt.ring->GetGPUVirtualAddress() + slice);
    L2->SetComputeRootShaderResourceView(2, a.argsVa ? a.argsVa : g_rtrt.ring->GetGPUVirtualAddress());
    L2->SetComputeRootUnorderedAccessView(3, g_rtpark.dbg->GetGPUVirtualAddress());
    {
        const bool rec = !a.argsVa && a.raygen && a.debugRecords;
        const uint64_t fb = g_rtrt.ring->GetGPUVirtualAddress();
        L2->SetComputeRootShaderResourceView(4, rec ? a.raygen : fb);
        L2->SetComputeRootShaderResourceView(5, rec && a.miss ? a.miss : fb);
        L2->SetComputeRootShaderResourceView(6, rec && a.hit ? a.hit : fb);
    }
    L2->Dispatch(1, 1, 1);
    L2->ResourceBarrier(1, &ub);
    tr(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    // the translated programs, in rounds (parked lanes continue in the program holding their target)
    L2->SetComputeRootSignature(rs);
    uint64_t gpu0 = g_gv->GetGPUDescriptorHandleForHeapStart().ptr;
    L2->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu0 + (uint64_t)gm_reserved_start() * g_gv_inc});
    for (UINT k = 2; k < 21; k++) L2->SetComputeRootDescriptorTable(k, D3D12_GPU_DESCRIPTOR_HANDLE{gpu0});
    if (a.samplerHeap) L2->SetComputeRootDescriptorTable(21, as<ID3D12DescriptorHeap>(a.samplerHeap)->GetGPUDescriptorHandleForHeapStart());
    L2->SetComputeRootUnorderedAccessView(22, g_rtpark.state->GetGPUVirtualAddress());
    L2->SetComputeRootUnorderedAccessView(23, g_rtpark.disc->GetGPUVirtualAddress() + (uint64_t)std::min(a.discRegion, kRtDiscRegions - 1) * kRtDiscBytes);
    L2->SetComputeRootUnorderedAccessView(24, g_rtpark.dbg->GetGPUVirtualAddress());
    for (auto& r : runs) {
        if (!r.pso) continue;
        L2->SetPipelineState(as<ID3D12PipelineState>(r.pso));
        uint32_t rc[8] = {(uint32_t)table, (uint32_t)(table >> 32), 0, 0, gpucode::rt_wave_stack_bytes(a.stackPerLane), std::min(a.discRegion, kRtDiscRegions - 1) * 512,
                          (ser << 8) | (r.round & 0xff), g_rtpark.lanes};
        L2->SetComputeRoot32BitConstants(0, 8, rc, 0);
        L2->ExecuteIndirect(g_rtrt.dispatchSig, 1, g_rtrt.ring, slice + 0x1B8, nullptr, 0);   // root constants 2..3 = groups x, y; then the dispatch
        L2->ResourceBarrier(1, &ub);
        InterlockedIncrement(&g_rays_run);
    }
    tr((D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    L2->ResourceBarrier(1, &ub);
    restore_compute_state(l);
    return true;
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
    for (int back = 0;; back++) {   // buffers may overlap: the nearest ones below, until one holds the address
        if (it == g_gm.begin() || back >= 64) return false;
        --it;
        if (va < it->first + it->second.size) break;
    }
    *res = (PcObj)it->second.res;
    *off = it->second.resOff + (va - it->first);
    if (avail) *avail = it->first + it->second.size - va;
    return true;
}
void res_heap_props(PcObj r, void* heapProps) {
    D3D12_HEAP_FLAGS f;
    *static_cast<D3D12_HEAP_PROPERTIES*>(heapProps) = {};
    as<ID3D12Resource>(r)->GetHeapProperties(static_cast<D3D12_HEAP_PROPERTIES*>(heapProps), &f);
}
// Diagnostics: the title address of a registered buffer's memory at a PC GPU address (0: none).
uint64_t gm_title_of_gpu(uint64_t gpu) {
    std::lock_guard<std::mutex> g(g_gm_mu);
    for (auto& [va, b] : g_gm) {
        if (!b.res) continue;
        const uint64_t base = b.res->GetGPUVirtualAddress() + b.resOff;
        if (gpu >= base && gpu < base + b.size) return va + (gpu - base);
    }
    return 0;
}
// XSL_AS_DUMP=1: the first 512 bytes of serialized acceleration structures, logged a few frames later.
namespace {
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
// A CPU-mapped GPU buffer of `bytes` registered in global memory at a fixed low address (kLowBase +
// `offset`): returns the mapping, or nullptr.
void* gm_low_buffer(PcObj devObj, uint64_t offset, uint64_t bytes, uint64_t* address) {
    if (!gm_enabled() || kLowBase + offset + bytes > kLowEnd) return nullptr;
    auto* dev = as<ID3D12Device>(devObj);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    void* m = nullptr;
    if (FAILED(r->Map(0, nullptr, &m))) { r->Release(); return nullptr; }
    gm_register((PcObj)r, kLowBase + offset, bytes, 0);
    if (address) *address = kLowBase + offset;
    return m;
}
// XSL_PROBE=<present> (diagnostics for hand-edited shaders): a 64 KiB CPU-readable buffer registered
// in the global-memory table at address 0x7F0000000000 (find it like gm_store does: binary search of
// GMT, then GMU[index]). It is cleared two presents before that present (dword 15 = 0x5a5a5a5a as a
// CPU-side marker) and its first 256 dwords are logged eight presents after.
struct Probe { ID3D12Resource* res = nullptr; uint32_t* cpu = nullptr; bool placed = false; } g_probe;
bool g_probe_register = false;   // registered in the address table at the next opportunity (outside g_gm_mu)
static uint64_t probe_at() { static uint64_t v = [] { char b[16]; return GetEnvironmentVariableA("XSL_PROBE", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }(); return v; }
static void probe_place(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!probe_at() || g_probe.placed) return;
    g_probe.placed = true;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 64 << 10;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_probe.res)))) {
        XSL_ERR(CH, "probe: buffer not created");
        return;
    }
    g_probe.res->Map(0, nullptr, (void**)&g_probe.cpu);
    if (g_probe.cpu) memset(g_probe.cpu, 0, 64 << 10);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
    uv.Format = DXGI_FORMAT_R32_TYPELESS;
    uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uv.Buffer.NumElements = (64 << 10) / 4;
    uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(g_probe.res, nullptr, &uv, h);
    XSL_INFO(CH, "probe buffer in global-memory UAV slot 8191 and at global address 0x7F0000000000");
    g_probe_register = true;
}
static void probe_present(uint64_t present) {
    uint64_t at = probe_at();
    if (!at || !g_probe.cpu) return;
    if (present + 2 == at) { memset(g_probe.cpu, 0, 64 << 10); g_probe.cpu[15] = 0x5a5a5a5au; }   // dword 15: CPU-side marker
    if (present == at + 8) {
        for (int r = 0; r < 32; r++) {
            const uint32_t* w = g_probe.cpu + r * 8;
            XSL_INFO(CH, "probe[%3d]: %08x %08x %08x %08x %08x %08x %08x %08x", r * 8, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
}
void gm_flush(PcObj devObj) {
    if (!gm_enabled() || !g_gv) return;
    gm_retire_due();
    {
        const uint32_t pb = gm_reserved_start();
        probe_place(as<ID3D12Device>(devObj), D3D12_CPU_DESCRIPTOR_HANDLE{g_gv->GetCPUDescriptorHandleForHeapStart().ptr + (uint64_t)(pb + kGmSlots - 1) * g_gv_inc});
        if (g_probe_register) { g_probe_register = false; gm_register((PcObj)g_probe.res, 0x7F0000000000ull, 64 << 10, 0); }
    }
    std::lock_guard<std::mutex> g(g_gm_mu);
    if (!g_gm_dirty) return;
    if (g_gm_built_at == g_gm_frame.load() && !g_gm_force) return;   // at most one rebuild per frame (unless needed)
    g_gm_built_at = g_gm_frame.load();
    g_gm_force = false;
    g_gm_built.clear();
    for (auto& [va, e] : g_gm) g_gm_built.push_back({va, e.size});
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
    auto take = [](std::vector<uint32_t>& fr, uint32_t& next, uint32_t end) -> uint32_t {
        if (!fr.empty()) { uint32_t k = fr.back(); fr.pop_back(); return k; }
        return next < end ? next++ : 0u;
    };
    // Views of `len` bytes of a buffer from byte `first`; false when the view range is full or the
    // buffer allows neither kind of view.
    auto make_views = [&](ID3D12Resource* res, const D3D12_RESOURCE_DESC& rd, uint64_t first, uint64_t len, std::pair<uint32_t, uint32_t>& v) -> bool {
        const bool srvOk = !(rd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        const bool uavOk = (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        if (!srvOk && !uavOk) return false;
        if (v.first || v.second) return true;
        uint32_t sSlot = srvOk ? take(g_gm_free_srv, g_gm_next_srv, kGmHalf) : 0u;
        uint32_t uSlot = uavOk ? take(g_gm_free_uav, g_gm_next_uav, kGmSlots - 1) : 0u;   // last slot: XSL_PROBE
        if ((srvOk && !sSlot) || (uavOk && !uSlot)) {
            if (sSlot) g_gm_free_srv.push_back(sSlot);
            if (uSlot) g_gm_free_uav.push_back(uSlot);
            if (InterlockedIncrement(&full) == 1) XSL_WARN(CH, "global memory: view range full");
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R32_TYPELESS;
        sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.FirstElement = first / 4;
        sv.Buffer.NumElements = (UINT)(len / 4);
        sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        if (srvOk) { v.first = sSlot; dev->CreateShaderResourceView(res, &sv, cpu_at(v.first)); }
        if (uavOk) {
            v.second = uSlot;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_R32_TYPELESS;
            uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uv.Buffer.FirstElement = sv.Buffer.FirstElement;
            uv.Buffer.NumElements = sv.Buffer.NumElements;
            uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            dev->CreateUnorderedAccessView(res, nullptr, &uv, cpu_at(v.second));
        }
        return true;
    };
    struct Row { uint64_t start; uint32_t len, view; };
    std::vector<Row> rows;
    auto add_row = [&](uint64_t start, uint64_t len, const std::pair<uint32_t, uint32_t>& v) {
        // [13:0] SRV index, [27:14] UAV index, [30] has SRV, [31] has UAV
        rows.push_back({start, (uint32_t)len, (v.first ? (v.first - 1) | 0x40000000u : 0u) | (v.second ? ((v.second - kGmHalf) << 14) | 0x80000000u : 0u)});
    };
    // Titles place buffers at overlapping addresses (placed resources sharing heap memory). The
    // table is searched for the last start at or below an address, so the part of an enclosing
    // buffer that follows the end of a buffer inside it gets a segment of its own (with views that
    // start at that offset; kept in g_gm_synth while used).
    struct Open { uint64_t start, end; GmBuf* e; };
    std::vector<Open> open;
    std::set<std::tuple<ID3D12Resource*, uint64_t, uint64_t>> used;
    auto synth = [&](const Open& t, uint64_t from, uint64_t to) {   // [from, to) of buffer t
        D3D12_RESOURCE_DESC rd = t.e->res->GetDesc();
        // raw views start at multiples of 16 bytes of the buffer: the segment starts at the next one
        from = t.start + ((t.e->resOff + (from - t.start) + 15) & ~15ull) - t.e->resOff;
        for (uint64_t at = from; at < to;) {
            uint64_t len = std::min(kChunk, to - at) & ~3ull;
            if (!len) break;
            auto key = std::make_tuple(t.e->res, t.e->resOff + (at - t.start), len);
            auto& v = g_gm_synth[key];
            if (make_views(t.e->res, rd, std::get<1>(key), len, v)) { add_row(at, len, v); used.insert(key); }
            at += len;
        }
    };
    auto close_until = [&](uint64_t s) {   // buffers ending at or before s; s = UINT64_MAX closes all
        while (!open.empty() && open.back().end <= s) {
            uint64_t e = open.back().end;
            open.pop_back();
            while (!open.empty() && open.back().end <= e) open.pop_back();   // ended inside the one just closed
            if (!open.empty() && e < s) synth(open.back(), e, std::min(open.back().end, s));
        }
    };
    for (auto& [va, e] : g_gm) {
        D3D12_RESOURCE_DESC rd = e.res->GetDesc();
        uint64_t bytes = std::min<uint64_t>(e.size, rd.Width - e.resOff) & ~3ull;
        if (!bytes) continue;
        close_until(va);
        uint32_t chunks = (uint32_t)((bytes + kChunk - 1) / kChunk);
        if (e.views.size() != chunks) e.views.assign(chunks, {0, 0});
        bool any = false;
        for (uint32_t c = 0; c < chunks; c++) {
            uint64_t off = (uint64_t)c * kChunk, len = std::min(kChunk, bytes - off);
            if (!make_views(e.res, rd, e.resOff + off, len, e.views[c])) continue;
            add_row(va + off, len, e.views[c]);
            any = true;
        }
        if (any) open.push_back({va, va + bytes, &e});
    }
    close_until(UINT64_MAX);
    for (auto it = g_gm_synth.begin(); it != g_gm_synth.end();) {   // segments no longer needed: their views go later
        if (used.count(it->first)) { ++it; continue; }
        g_gm_retired.push_back({g_gm_frame.load(), nullptr, {it->second}});
        it = g_gm_synth.erase(it);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.start < y.start; });
    for (auto& r : rows) {   // {start lo, start hi, size, views}
        table.push_back(uint32_t(r.start));
        table.push_back(uint32_t(r.start >> 32));
        table.push_back(r.len);
        table.push_back(r.view);
        count++;
    }
    table[0] = count;
    static uint32_t generation = 0;
    table[1] = ++generation;   // diagnostics (XSL_RT_DEBUG shows which table a program ran with)

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
// With `heaps`, the bindless arrays of image access through descriptor memory follow the
// global-memory table as two tables. One holds the whole view heap seen through every array (the
// ranges all start at the heap's first descriptor): SRVs and UAVs of 2D, 2D array and 3D textures
// in spaces 910..918 (float, uint, int each), cube SRVs in 919, 1D textures in 921..923. The other
// holds the title's sampler heap as samplers (space 920) and as comparison samplers (925).
static bool gm_extend_root_signature(const void* blob, size_t size, std::vector<uint8_t>& out, uint32_t* index, bool heaps) {
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
        if (cost + (heaps ? kHeapTables : 0u) < 64) {
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
            static D3D12_DESCRIPTOR_RANGE1 views[32], smps[2];
            static UINT nviews = 0;
            static std::once_flag once;
            std::call_once(once, [] {
                for (uint32_t sp : {910u, 911u, 912u, 913u, 914u, 915u, 916u, 917u, 918u, 921u, 922u, 923u}) {
                    views[nviews++] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, sp, vol, 0};
                    views[nviews++] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, sp, vol, 0};
                }
                views[nviews++] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 919, vol, 0};
                smps[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 920, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
                smps[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 925, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};
            });
            if (heaps) {
                p.DescriptorTable = {nviews, views};
                params.push_back(p);
                p.DescriptorTable = {2, smps};
                params.push_back(p);
            }
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
// The parameter types of the root signatures made here. A title sets root arguments for the
// signature it has in mind; the one bound on the PC list may be another pipeline's (bound lazily),
// and an argument for a parameter it does not have, or of another type, crashes the driver.
std::mutex g_rs_par_mu;
std::unordered_map<PcObj, std::vector<uint8_t>> g_rs_params;
static void note_rs_params(PcObj rs, const void* blob, size_t size) {
    ID3D12VersionedRootSignatureDeserializer* ds = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&ds)))) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d = nullptr;
    std::vector<uint8_t> kinds;
    if (SUCCEEDED(ds->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &d)) && d)
        for (UINT i = 0; i < d->Desc_1_1.NumParameters; i++) kinds.push_back((uint8_t)d->Desc_1_1.pParameters[i].ParameterType);
    ds->Release();
    std::lock_guard<std::mutex> g(g_rs_par_mu);
    g_rs_params[rs] = std::move(kinds);
}
// kind: 0 table, 1 CBV, 2 SRV, 3 UAV, 4 constants (root-argument kinds of the lazy binding).
static bool rs_param_ok(PcObj rs, uint32_t i, int kind) {
    static const uint8_t want[5] = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, D3D12_ROOT_PARAMETER_TYPE_CBV, D3D12_ROOT_PARAMETER_TYPE_SRV, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
    std::lock_guard<std::mutex> g(g_rs_par_mu);
    auto it = g_rs_params.find(rs);
    if (it == g_rs_params.end()) return true;   // not made here: as the title set it
    return i < it->second.size() && it->second[i] == want[kind];
}
HRESULT_T create_root_signature(PcObj dev, const void* blob, size_t size, PcObj* out) { return create_root_signature_ex(dev, blob, size, out, false); }
HRESULT_T create_root_signature_ex(PcObj dev, const void* blob, size_t size, PcObj* out, bool heaps) {
    void* clean = nullptr;
    size_t cleanSize = 0;
    bool owned = sanitize_container(blob, size, &clean, &cleanSize);
    const void* use = owned ? clean : blob;
    size_t useSize = owned ? cleanSize : size;
    std::vector<uint8_t> ext;
    uint32_t gmIndex = 0;
    static bool noRs = getenv("XSL_GM_NO_RS") != nullptr;   // bisecting: views and table only
    bool extended = gm_enabled() && !noRs && gm_extend_root_signature(use, useSize, ext, &gmIndex, heaps);
    if (!extended && heaps && gm_enabled() && !noRs) { heaps = false; extended = gm_extend_root_signature(use, useSize, ext, &gmIndex, false); }
    HRESULT hr = as<ID3D12Device>(dev)->CreateRootSignature(0, extended ? ext.data() : use, extended ? ext.size() : useSize,
                                                            IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(out)));
    if (FAILED(hr) && extended) {   // fall back to the title's own layout
        extended = false;
        hr = as<ID3D12Device>(dev)->CreateRootSignature(0, use, useSize, IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(out)));
    }
    if (SUCCEEDED(hr)) note_rs_params(*out, extended ? ext.data() : use, extended ? ext.size() : useSize);
    if (owned) free_container(clean);
    if (FAILED(hr)) XSL_ERR(CH, "CreateRootSignature failed %08lx (blob %zu bytes)", hr, size);
    if (SUCCEEDED(hr) && extended) { std::lock_guard<std::mutex> g(g_gm_mu); g_rs_gm[*out] = gmIndex | (heaps ? kRsHeaps : 0u); }
    if (SUCCEEDED(hr) && !extended && gm_enabled() && !noRs) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 10) XSL_WARN(CH, "root signature (%zu bytes) without the global-memory table (no room, or the extended one was rejected)", size);
    }
    return hr;
}
std::atomic<uint64_t> g_gm_bind_stats[4];   // bound, root signature without table, global heap not set, no heap yet
static bool root_is_bound(PcObj l, bool compute, PcObj rs);
static void gm_bind(PcObj l, PcObj rs, bool compute) {
    if (!gm_enabled() || !g_gv || !rs) return;
    if (!root_is_bound(l, compute, rs)) return;   // another signature is on the list now: bound with this one later
    uint32_t idx;
    PcObj samp = nullptr;
    {
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto& cur = g_list_gm_rs[l];
        (compute ? cur.second : cur.first) = rs;
        auto it = g_rs_gm.find(rs);
        if (it == g_rs_gm.end()) { g_gm_bind_stats[1]++; return; }
        auto h = g_list_global_heap.find(l);
        if (h == g_list_global_heap.end() || !h->second) { g_gm_bind_stats[h == g_list_global_heap.end() ? 3 : 2]++; return; }   // bound once the global heap is set
        idx = it->second;
        auto sh = g_list_sampler_heap.find(l);
        samp = sh == g_list_sampler_heap.end() ? nullptr : sh->second;
    }
    g_gm_bind_stats[0]++;
    const bool heaps = (idx & kRsHeaps) != 0;
    idx &= ~kRsHeaps;
    const D3D12_GPU_DESCRIPTOR_HANDLE g0 = g_gv->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gh{g0.ptr + (uint64_t)gm_reserved_start() * g_gv_inc};
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    if (compute) L2->SetComputeRootDescriptorTable(idx, gh);
    else L2->SetGraphicsRootDescriptorTable(idx, gh);
    if (!heaps) return;
    if (compute) L2->SetComputeRootDescriptorTable(idx + 1, g0);
    else L2->SetGraphicsRootDescriptorTable(idx + 1, g0);
    if (samp) {
        D3D12_GPU_DESCRIPTOR_HANDLE sg = as<ID3D12DescriptorHeap>(samp)->GetGPUDescriptorHandleForHeapStart();
        if (compute) L2->SetComputeRootDescriptorTable(idx + 2, sg);
        else L2->SetGraphicsRootDescriptorTable(idx + 2, sg);
    }
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

// XSL_PSO_TIMES=1 (diagnostics): every creation and selection of a pipeline that takes 20 ms or
// more is logged with the pipeline's address.
static bool pso_times() { static const bool on = env_on("XSL_PSO_TIMES"); return on; }
void name_translated(PcObj pso) {   // PC pipelines made from translated shaders are named after them
    const char* k = translated_keys();
    if (!pso || !*k) return;
    wchar_t w[512];
    swprintf(w, 512, L"xsl %hs", k);
    as<ID3D12PipelineState>(pso)->SetName(w);
}
// Compiled pipelines kept on disk (pso_<hash>.bin from GetCachedBlob, next to the shader cache):
// translated shaders can take the driver seconds to minutes, every run. A blob the driver refuses
// (other driver, other root signature) is rebuilt. XSL_PSO_CACHE=0 turns it off.
static bool pso_cache_on() { static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_PSO_CACHE", b, 4) && b[0] == '0'); }(); return on; }
static uint64_t pso_hash(uint64_t h, const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static std::wstring pso_cache_path(uint64_t h) {
    wchar_t name[64];
    swprintf(name, 64, L"pso_%016llx.bin", (unsigned long long)h);
    return rt_cache_dir() + name;
}
template <class Create>
static HRESULT cached_pso(uint64_t key, const void*& cachedPtr, SIZE_T& cachedSize, ID3D12PipelineState** out, Create create, bool heavy = false) {
    std::vector<uint8_t> blob;
    std::wstring path = pso_cache_on() ? pso_cache_path(key) : std::wstring();
    if (!path.empty() && read_all(path, blob)) {
        cachedPtr = blob.data();
        cachedSize = blob.size();
        const ULONGLONG t0 = GetTickCount64();
        HRESULT hr = create();
        cachedPtr = nullptr;
        cachedSize = 0;
        if (pso_times()) XSL_INFO(CH, "pso %p: created from its stored blob (%zu bytes) in %llu ms: %08lx", SUCCEEDED(hr) ? (void*)*out : nullptr, blob.size(), (unsigned long long)(GetTickCount64() - t0), hr);
        if (SUCCEEDED(hr)) return hr;
    }
    // Not stored yet: the driver compiles it. From a large translation that takes it long enough
    // to keep a worker for minutes, so a thread that must stay quick hands such a pipeline on.
    if (heavy && deferring()) { defer_now(); return E_FAIL; }
    const ULONGLONG t1 = GetTickCount64();
    HRESULT hr = create();
    if (pso_times()) XSL_INFO(CH, "pso %p: created by the driver in %llu ms: %08lx", SUCCEEDED(hr) ? (void*)*out : nullptr, (unsigned long long)(GetTickCount64() - t1), hr);
    if (SUCCEEDED(hr) && !path.empty() && *out) {
        ID3DBlob* cb = nullptr;
        if (SUCCEEDED((*out)->GetCachedBlob(&cb)) && cb) { write_all(path, cb->GetBufferPointer(), cb->GetBufferSize()); cb->Release(); }
    }
    return hr;
}
void table_note_stage(PcObj pso, const void* code, size_t size);
void table_forget(PcObj pso);
static bool no_blend_on_integers(D3D12_BLEND_DESC& b, const DXGI_FORMAT* formats, UINT n);
HRESULT_T create_graphics_pso(PcObj dev, const void* desc, PcObj* out) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc);
    for (UINT i = 0; i < 8; i++)
        if (DXGI_FORMAT sf = stand_in(d.RTVFormats[i]); sf != DXGI_FORMAT_UNKNOWN) d.RTVFormats[i] = sf;
    no_blend_on_integers(d.BlendState, d.RTVFormats, d.NumRenderTargets);
    ShaderFix fx;
    fx.fix(d.VS, 0); fx.fix(d.PS, 1); fx.fix(d.DS, 2); fx.fix(d.HS, 3); fx.fix(d.GS, 4);
    if (had_untranslated()) return E_FAIL;   // console code the PC runtime cannot take (see create_compute_pso)
    d.Flags = (D3D12_PIPELINE_STATE_FLAGS)(d.Flags & D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG);
    d.CachedPSO = {};
    d.NodeMask = 0;
    auto** pso = reinterpret_cast<ID3D12PipelineState**>(out);
    uint64_t key = pso_hash(1469598103934665603ull, "gfx1", 4);
    for (auto* bc : {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS}) key = pso_hash(pso_hash(key, &bc->BytecodeLength, 8), bc->pShaderBytecode, bc->BytecodeLength);
    key = pso_hash(key, d.RTVFormats, sizeof d.RTVFormats);
    key = pso_hash(key, &d.DSVFormat, sizeof d.DSVFormat);
    key = pso_hash(key, &d.NumRenderTargets, sizeof d.NumRenderTargets);
    key = pso_hash(key, &d.PrimitiveTopologyType, sizeof d.PrimitiveTopologyType);
    key = pso_hash(key, &d.BlendState, sizeof d.BlendState);
    key = pso_hash(key, &d.RasterizerState, sizeof d.RasterizerState);
    key = pso_hash(key, &d.DepthStencilState, sizeof d.DepthStencilState);
    key = pso_hash(key, &d.SampleDesc, sizeof d.SampleDesc);
    key = pso_hash(key, &d.SampleMask, sizeof d.SampleMask);
    for (UINT i = 0; i < d.InputLayout.NumElements; i++) {
        auto& e = d.InputLayout.pInputElementDescs[i];
        key = pso_hash(key, e.SemanticName, strlen(e.SemanticName));
        key = pso_hash(key, &e.SemanticIndex, sizeof e - offsetof(D3D12_INPUT_ELEMENT_DESC, SemanticIndex));
    }
    if (driver_refused(translated_keys())) return E_FAIL;
    DriverCall inDriver(translated_keys());
    size_t translated = 0;
    for (auto* bc : {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS}) translated += bc->BytecodeLength;
    HRESULT hr = cached_pso(key, d.CachedPSO.pCachedBlob, d.CachedPSO.CachedBlobSizeInBytes, pso,
                            [&] { return as<ID3D12Device>(dev)->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(pso)); }, translated >= heavy_pipeline_bytes());
    if (SUCCEEDED(hr) && *pso) { table_forget(*pso); for (auto* bc : {&d.VS, &d.PS, &d.DS, &d.HS, &d.GS}) table_note_stage(*pso, bc->pShaderBytecode, bc->BytecodeLength); }
    if (FAILED(hr) && compile_deferred()) return hr;
    if (FAILED(hr)) { dump_debug_messages(); XSL_ERR(CH, "CreateGraphicsPipelineState failed %08lx", hr); }
    if (SUCCEEDED(hr)) name_translated(*out);
    if (hr == DXGI_ERROR_DEVICE_REMOVED) report_device_removed(as<ID3D12Device>(dev));
    return hr;
}
static void table_check_note(PcObj pso, const void* code, size_t size, bool compute = true);
HRESULT_T create_compute_pso(PcObj dev, const void* desc, PcObj* out) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc);
    ShaderFix fx;
    fx.fix(d.CS, 0);
    // A console program that did not translate is never handed to the PC runtime: the GPU driver's
    // compiler faults on such a container (it catches the fault and reports "out of memory", but
    // a thousand of those per run are not harmless: the driver's heap was seen corrupted after).
    // The caller makes a placeholder pipeline, as it does for the failure the runtime would report.
    if (had_untranslated()) return E_FAIL;
    d.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    d.CachedPSO = {};
    d.NodeMask = 0;
    auto** pso = reinterpret_cast<ID3D12PipelineState**>(out);
    uint64_t key = pso_hash(pso_hash(1469598103934665603ull, "cs1", 3), d.CS.pShaderBytecode, d.CS.BytecodeLength);
    const ULONGLONG t0c = GetTickCount64();
    if (driver_refused(translated_keys())) return E_FAIL;
    DriverCall inDriver(translated_keys());
    HRESULT hr = cached_pso(key, d.CachedPSO.pCachedBlob, d.CachedPSO.CachedBlobSizeInBytes, pso,
                            [&] { return as<ID3D12Device>(dev)->CreateComputePipelineState(&d, IID_PPV_ARGS(pso)); }, d.CS.BytecodeLength >= heavy_pipeline_bytes());
    if (FAILED(hr) && compile_deferred()) return hr;
    if (SUCCEEDED(hr)) table_check_note(*pso, d.CS.pShaderBytecode, d.CS.BytecodeLength);
    if (const ULONGLONG dt = GetTickCount64() - t0c; dt >= 5000) {
        static volatile long shown = 0;
        if (InterlockedIncrement(&shown) <= 40) XSL_WARN(CH, "CreateComputePipelineState took %llu ms in the driver (shaders %s)", (unsigned long long)dt, translated_keys());
    }
    if (FAILED(hr)) {
        dump_debug_messages();
        XSL_ERR(CH, "CreateComputePipelineState failed %08lx (translated shaders: %s)", hr, translated_keys());
        // What the PC runtime was given, for a pipeline that is not a translated console program:
        // the container's parts, and the container itself with XSL_DUMP_SHADERS.
        static volatile long shown = 0;
        const uint8_t* c = (const uint8_t*)d.CS.pShaderBytecode;
        const size_t n = d.CS.BytecodeLength;
        if (!*translated_keys() && !had_untranslated() && c && n >= 32 && InterlockedIncrement(&shown) <= 12) {
            std::string parts;
            uint32_t count = 0;
            memcpy(&count, c + 28, 4);
            for (uint32_t i = 0; !memcmp(c, "DXBC", 4) && i < count && 32 + 4ull * (i + 1) <= n; i++) {
                uint32_t off = 0, sz = 0;
                memcpy(&off, c + 32 + 4 * i, 4);
                if (off + 8ull > n) break;
                memcpy(&sz, c + off + 4, 4);
                char t[40];
                snprintf(t, sizeof t, " %.4s(%u)", (const char*)c + off, sz);
                parts += t;
            }
            const uint64_t h = pso_hash(1469598103934665603ull, c, n);
            XSL_ERR(CH, "  its shader: %zu bytes, starts %02x %02x %02x %02x, parts:%s, hash %016llx, root signature %p", n, c[0], c[1], c[2], c[3], parts.c_str(),
                    (unsigned long long)h, (void*)d.pRootSignature);
            if (const char* dir = getenv("XSL_DUMP_SHADERS"); dir && *dir) {
                char path[600];
                snprintf(path, sizeof path, "%s\\failed_cs_%016llx.cso", dir, (unsigned long long)h);
                if (FILE* f = fopen(path, "wb")) { fwrite(c, 1, n, f); fclose(f); }
            }
        }
    }
    if (SUCCEEDED(hr)) name_translated(*out);
    if (SUCCEEDED(hr) && pso_times()) {
        const uint64_t h = pso_hash(1469598103934665603ull, d.CS.pShaderBytecode, d.CS.BytecodeLength);
        XSL_INFO(CH, "pso %p: compute shader %016llx of %zu bytes (%s)", *out, (unsigned long long)h, (size_t)d.CS.BytecodeLength, *translated_keys() ? translated_keys() : "the title's DXIL");
        if (const char* dir = getenv("XSL_DUMP_SHADERS"); dir && *dir && !*translated_keys()) {
            char path[600];
            snprintf(path, sizeof path, "%s\\title_cs_%016llx.cso", dir, (unsigned long long)h);
            if (FILE* f = fopen(path, "wb")) { fwrite(d.CS.pShaderBytecode, 1, d.CS.BytecodeLength, f); fclose(f); }
        }
    }
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
// A render target of an integer format cannot be blended on a PC, and a pipeline that says so is
// refused; the console ignores the blend state of such a target (a title leaves one blend state
// on for all its targets, a mask of integers among them). Blending is turned off for those.
static bool integer_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT: return true;
    default: return false;
    }
}
static bool no_blend_on_integers(D3D12_BLEND_DESC& b, const DXGI_FORMAT* formats, UINT n) {
    bool any = false;
    for (UINT k = 0; k < n && k < 8; k++) {
        const auto& r = b.RenderTarget[b.IndependentBlendEnable ? k : 0];
        any |= integer_format(formats[k]) && (r.BlendEnable || r.LogicOpEnable);
    }
    if (!any) return false;
    if (!b.IndependentBlendEnable) {
        for (int k = 1; k < 8; k++) b.RenderTarget[k] = b.RenderTarget[0];
        b.IndependentBlendEnable = TRUE;
    }
    for (UINT k = 0; k < n && k < 8; k++)
        if (integer_format(formats[k])) b.RenderTarget[k].BlendEnable = b.RenderTarget[k].LogicOpEnable = FALSE;
    static volatile long said = 0;
    if (InterlockedIncrement(&said) == 1) XSL_INFO(CH, "a pipeline blends into a render target of an integer format: blending is off for that target (the console ignores it there)");
    return true;
}
// The same for a pipeline stream (its blend and render-target-format parts, wherever they are).
static void stream_no_blend_on_integers(void* stream, size_t size) {
    uint8_t* p = (uint8_t*)stream;
    D3D12_BLEND_DESC* blend = nullptr;
    const D3D12_RT_FORMAT_ARRAY* rts = nullptr;
    for (size_t o = 0; o + 4 <= size;) {
        const auto t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(p + o);
        size_t n = 0;
        switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: n = sizeof(StreamSub<void*>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: n = sizeof(StreamSub<D3D12_SHADER_BYTECODE>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: n = sizeof(StreamSub<D3D12_INPUT_LAYOUT_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: n = sizeof(StreamSub<D3D12_BLEND_DESC>); blend = &((StreamSub<D3D12_BLEND_DESC>*)(p + o))->v; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: n = sizeof(StreamSub<D3D12_RT_FORMAT_ARRAY>); rts = &((const StreamSub<D3D12_RT_FORMAT_ARRAY>*)(p + o))->v; break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: n = sizeof(StreamSub<UINT>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: n = sizeof(StreamSub<D3D12_RASTERIZER_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: n = sizeof(StreamSub<D3D12_DEPTH_STENCIL_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: n = sizeof(StreamSub<DXGI_FORMAT>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: n = sizeof(StreamSub<DXGI_SAMPLE_DESC>); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: n = sizeof(StreamSub<D3D12_PIPELINE_STATE_FLAGS>); break;
        default: return;   // (a part this does not know: left as it is)
        }
        o += n;
    }
    if (blend && rts) no_blend_on_integers(*blend, rts->RTFormats, rts->NumRenderTargets);
}
static bool own_rs_blob(PcObj rs, std::vector<uint8_t>* out);
// What a pipeline stream describes, as a key for its stored blob: the shaders' bytes, the root
// signature's serialized form (one made from a shader's description), the input elements and the
// state as it stands. false: the stream holds something this does not know (it is not stored).
static bool stream_key(const void* stream, size_t size, uint64_t* key) {
    const uint8_t* p = (const uint8_t*)stream;
    uint64_t h = pso_hash(1469598103934665603ull, "stream1", 7);
    for (size_t o = 0; o + 4 <= size;) {
        const auto t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(p + o);
        size_t n = 0;
        h = pso_hash(h, &t, sizeof t);
#define payload_of(T) (n = sizeof(StreamSub<T>), h = pso_hash(h, &((const StreamSub<T>*)(p + o))->v, sizeof(T)))
        switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            n = sizeof(StreamSub<void*>);
            std::vector<uint8_t> b;
            if (own_rs_blob(((const StreamSub<void*>*)(p + o))->v, &b)) h = pso_hash(h, b.data(), b.size());
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
            n = sizeof(StreamSub<D3D12_SHADER_BYTECODE>);
            auto& bc = ((const StreamSub<D3D12_SHADER_BYTECODE>*)(p + o))->v;
            h = pso_hash(h, &bc.BytecodeLength, sizeof bc.BytecodeLength);
            if (bc.pShaderBytecode && bc.BytecodeLength) h = pso_hash(h, bc.pShaderBytecode, bc.BytecodeLength);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            n = sizeof(StreamSub<D3D12_INPUT_LAYOUT_DESC>);
            auto& il = ((const StreamSub<D3D12_INPUT_LAYOUT_DESC>*)(p + o))->v;
            for (UINT k = 0; k < il.NumElements && il.pInputElementDescs; k++) {
                const D3D12_INPUT_ELEMENT_DESC& e = il.pInputElementDescs[k];
                if (e.SemanticName) h = pso_hash(h, e.SemanticName, strlen(e.SemanticName));
                h = pso_hash(h, &e.SemanticIndex, (const uint8_t*)(&e + 1) - (const uint8_t*)&e.SemanticIndex);
            }
            break;
        }
        // (Field by field where a description has padding: a title's stream holds whatever was there.)
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: {
            n = sizeof(StreamSub<D3D12_BLEND_DESC>);
            auto& b = ((const StreamSub<D3D12_BLEND_DESC>*)(p + o))->v;
            uint32_t w[2 + 8 * 10] = {(uint32_t)b.AlphaToCoverageEnable, (uint32_t)b.IndependentBlendEnable};
            for (int k = 0; k < 8; k++) {
                auto& r = b.RenderTarget[k];
                const uint32_t f[10] = {(uint32_t)r.BlendEnable, (uint32_t)r.LogicOpEnable, (uint32_t)r.SrcBlend, (uint32_t)r.DestBlend, (uint32_t)r.BlendOp,
                                        (uint32_t)r.SrcBlendAlpha, (uint32_t)r.DestBlendAlpha, (uint32_t)r.BlendOpAlpha, (uint32_t)r.LogicOp, (uint32_t)r.RenderTargetWriteMask};
                memcpy(w + 2 + k * 10, f, sizeof f);
            }
            h = pso_hash(h, w, sizeof w);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
            n = sizeof(StreamSub<D3D12_DEPTH_STENCIL_DESC>);
            auto& d = ((const StreamSub<D3D12_DEPTH_STENCIL_DESC>*)(p + o))->v;
            const uint32_t w[14] = {(uint32_t)d.DepthEnable, (uint32_t)d.DepthWriteMask, (uint32_t)d.DepthFunc, (uint32_t)d.StencilEnable, d.StencilReadMask, d.StencilWriteMask,
                                    (uint32_t)d.FrontFace.StencilFailOp, (uint32_t)d.FrontFace.StencilDepthFailOp, (uint32_t)d.FrontFace.StencilPassOp, (uint32_t)d.FrontFace.StencilFunc,
                                    (uint32_t)d.BackFace.StencilFailOp, (uint32_t)d.BackFace.StencilDepthFailOp, (uint32_t)d.BackFace.StencilPassOp, (uint32_t)d.BackFace.StencilFunc};
            h = pso_hash(h, w, sizeof w);
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: payload_of(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: payload_of(D3D12_RASTERIZER_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: payload_of(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: payload_of(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: payload_of(D3D12_RT_FORMAT_ARRAY); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: payload_of(DXGI_FORMAT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: payload_of(DXGI_SAMPLE_DESC); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: payload_of(UINT); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: payload_of(D3D12_PIPELINE_STATE_FLAGS); break;
        default: return false;   // (a stream output, a cached blob of the title's, view instancing, anything newer)
        }
        o += n;
    }
    *key = h;
    return true;
}
HRESULT_T create_pso_stream(PcObj dev, void* stream, size_t size, PcObj* out) {
    *out = nullptr;
    if (getenv("XSL_TRACE_WRITERS") && pso_traced()) trace_pso_stream(stream, size);
    stream_no_blend_on_integers(stream, size);
    D3D12_PIPELINE_STATE_STREAM_DESC d{size, stream};
    ID3D12Device2* d2 = nullptr;
    if (driver_refused(translated_keys())) return E_FAIL;
    DriverCall inDriver(translated_keys());
    HRESULT hr = as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d2));
    // The driver's compiled form of the pipeline is kept beside the shader cache and handed back
    // on later runs: a scene's mesh pipelines take the driver's compiler 5 to 45 s each, and a
    // title asks for a thousand of them again at every start.
    uint64_t key = 0;
    std::wstring path;
    if (SUCCEEDED(hr) && pso_cache_on() && stream_key(stream, size, &key)) path = pso_cache_path(key);
    std::vector<uint8_t> blob;
    if (SUCCEEDED(hr) && !path.empty() && read_all(path, blob) && !blob.empty()) {
        std::vector<uint8_t> with((size + 7) & ~(size_t)7);
        memcpy(with.data(), stream, size);
        StreamSub<D3D12_CACHED_PIPELINE_STATE> c{};
        c.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO;
        c.v = {blob.data(), blob.size()};
        with.insert(with.end(), (const uint8_t*)&c, (const uint8_t*)&c + sizeof c);
        D3D12_PIPELINE_STATE_STREAM_DESC dc{with.size(), with.data()};
        const ULONGLONG t0 = GetTickCount64();
        HRESULT hc = d2->CreatePipelineState(&dc, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
        if (pso_times()) XSL_INFO(CH, "pso %p (stream): created from its stored blob (%zu bytes) in %llu ms: %08lx", SUCCEEDED(hc) ? *out : nullptr, blob.size(),
                                  (unsigned long long)(GetTickCount64() - t0), hc);
        if (SUCCEEDED(hc)) { d2->Release(); return hc; }
        *out = nullptr;   // (another driver, or another pipeline under the same key: compiled anew and stored again)
    }
    if (SUCCEEDED(hr)) {
        const ULONGLONG t0 = GetTickCount64();
        hr = d2->CreatePipelineState(&d, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(out)));
        if (SUCCEEDED(hr) && !path.empty() && *out) {
            ID3DBlob* cb = nullptr;
            if (SUCCEEDED(as<ID3D12PipelineState>(*out)->GetCachedBlob(&cb)) && cb) { write_all(path, cb->GetBufferPointer(), cb->GetBufferSize()); cb->Release(); }
        }
        d2->Release();
        if (const ULONGLONG dt = GetTickCount64() - t0; dt >= 5000) {
            static volatile long shown = 0;
            if (InterlockedIncrement(&shown) <= 40) XSL_WARN(CH, "CreatePipelineState took %llu ms in the driver (shaders %s)", (unsigned long long)dt, translated_keys());
        }
    }
    if (FAILED(hr)) {
        dump_debug_messages();
        XSL_ERR(CH, "CreatePipelineState (stream, %zu bytes) failed %08lx (translated shaders: %s)", size, hr, translated_keys());
        note_removed(hr);
        static volatile long described = 0;
        if (InterlockedIncrement(&described) <= 6) trace_pso_stream(stream, size);   // what the stream holds
    }
    // XSL_DUMP_FAILED_PSO=<dir>: the shaders of failed pipeline streams (<dir>\pso<n>_s<type>.bin).
    static char dumpDir[MAX_PATH] = "";
    static bool dumpInit = (GetEnvironmentVariableA("XSL_DUMP_FAILED_PSO", dumpDir, sizeof dumpDir), true);
    (void)dumpInit;
    static volatile long dumped = 0;
    if (FAILED(hr) && dumpDir[0] && InterlockedIncrement(&dumped) <= 12) {
        const uint8_t* p = (const uint8_t*)stream;
        if (auto& rs = fallback_root_signature(); !rs.empty()) {   // the pipeline's root signature (<dir>\pso<n>_rs.bin)
            char path[MAX_PATH + 64];
            snprintf(path, sizeof path, "%s\\pso%ld_rs.bin", dumpDir, dumped);
            if (FILE* f = fopen(path, "wb")) { fwrite(rs.data(), 1, rs.size(), f); fclose(f); }
        }
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

// The same for memory that is a section (file mapping): a buffer over the whole section.
// The section is not imported as such (OpenExistingHeapFromFileMapping): on Windows 10 19044 with
// an RTX 5090 (driver 591.59) a section imported that way stays committed after the process is gone, however the
// process ends and even when the heap was released first (tests/unit/section_heap_check.cpp; two
// days of runs of a title left 200 GB committed and nothing could allocate any more). A view of
// the section imported by address is the same memory to the GPU and does not stay behind.
HRESULT_T import_file_mapping(PcObj dev, void* section, uint64_t size, PcObj* res, uint64_t* gpuVa) {
    ID3D12Device3* d3 = nullptr;
    HRESULT hr = as<ID3D12Device>(dev)->QueryInterface(IID_PPV_ARGS(&d3));
    if (FAILED(hr)) return logged(hr, "QueryInterface(ID3D12Device3)");
    void* view = MapViewOfFile((HANDLE)section, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);   // kept for the life of the process
    if (!view) { d3->Release(); return logged(HRESULT_FROM_WIN32(GetLastError()), "MapViewOfFile(section)"); }
    ID3D12Heap* heap = nullptr;
    hr = d3->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap));
    d3->Release();
    if (FAILED(hr)) { UnmapViewOfFile(view); return logged(hr, "OpenExistingHeapFromAddress(view of a section)"); }
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* r = nullptr;
    hr = as<ID3D12Device>(dev)->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    heap->Release();
    if (FAILED(hr)) { UnmapViewOfFile(view); return logged(hr, "CreatePlacedResource(section)"); }
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

// Compute bindings of each list, so the layer's own dispatches (indirect-argument checks) can put
// the title's state back afterwards.
struct CsShadow {
    PcObj rs = nullptr, pso = nullptr;
    std::map<uint32_t, std::pair<int, uint64_t>> params;   // index -> (0 table, 1 CBV, 2 SRV, 3 UAV, address/handle)
    std::map<uint32_t, std::vector<uint32_t>> consts;      // index -> 32-bit constants
};
std::mutex g_cs_mu;
std::unordered_map<PcObj, CsShadow> g_cs_shadow;
std::mutex g_rays_mu;
static bool guard_on() { static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_INDIRECT_GUARD", b, 4) && b[0] == '0'); }(); return on; }
// Signatures the guard handles: byte offset of the dispatch/draw argument in a record, and its kind.
struct GuardSig { uint32_t argOff; uint32_t kind; uint32_t stride; };
std::map<PcObj, GuardSig> g_guard_sigs;
std::set<PcObj> g_rays_sigs;   // command signatures that dispatch rays (skipped while ray tracing is inert)
// Command signatures whose single argument is a console 1-D dispatch (DISPATCH_SINGLE: X only, Y and
// Z unused) or a DISPATCH_MESH_X (X, then reserved/payload words): PC dispatches read three group
// counts, so ExecuteIndirect runs from a copy of the arguments with Y = Z = 1.
std::map<PcObj, uint32_t> g_fix_sigs;   // signature -> byte stride
bool is_rays_signature(PcObj sig) {
    std::lock_guard<std::mutex> g(g_rays_mu);
    return g_rays_sigs.count(sig) != 0;
}

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
    if (SUCCEEDED(hr) && d && guard_on()) {   // where the dispatch/draw argument sits in a record (PC layout, console stride)
        uint32_t o = 0;
        for (UINT i = 0; i < d->NumArgumentDescs; i++) {
            const auto& a = d->pArgumentDescs[i];
            const int ct = i < static_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(desc)->NumArgumentDescs
                               ? (int)static_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(desc)->pArgumentDescs[i].Type : -1;
            switch (a.Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
                { std::lock_guard<std::mutex> g(g_rays_mu); g_guard_sigs[*out] = {o, 0, d->ByteStride}; }
                o += ct == kXboxArgDispatchX ? 16 : 12; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
                { std::lock_guard<std::mutex> g(g_rays_mu); g_guard_sigs[*out] = {o, 1, d->ByteStride}; }
                o += a.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ? 16 : 20; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: o += 16; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: o += 4 * a.Constant.Num32BitValuesToSet; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: o += 8; break;
            default: o += 1u << 20; break;   // unknown layout: no guard
            }
        }
        if (o >= (1u << 20)) { std::lock_guard<std::mutex> g(g_rays_mu); g_guard_sigs.erase(*out); }
    }
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
    // The console's 16-bit depth with stencil (D16_UNORM_S8_UINT and its two view formats) does not
    // exist on a PC: the 24-bit family stands in.
    switch ((int)f) {
    case 118: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case 119: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case 120: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    // 16-bit packed colour as a render target with unordered access: not on a PC.
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: break;
    }
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
// A depth-stencil resource the console starts in the unordered-access state (it allows such
// views there) starts writable as depth on a PC, which has no such state for it.
D3D12_RESOURCE_STATES depth_state(const D3D12_RESOURCE_DESC& d, D3D12_RESOURCE_STATES st) {
    if ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && (st & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return st;
}
D3D12_HEAP_FLAGS clean_heap_flags(uint32_t f) {
    // Xbox reuses 0x2 (ALLOW_INDIRECT_BUFFERS) where PC has SHARED; ALLOW_DISPLAY has no PC meaning here.
    return (D3D12_HEAP_FLAGS)(f & ~(0x2u | 0x8u) & 0xFFFFu);
}
}  // namespace

// Console memory is unified: titles Map DEFAULT-heap buffers (FramePacing reads its query buffer that
// way). Such buffers get CPU-visible memory chosen once per device: the GPU-upload heap (resizable BAR)
// where supported, else write-back system memory on UMA GPUs. Discrete GPUs on runtimes without the
// GPU upload heap (Windows 10's) keep DEFAULT buffers in video memory: in system memory every GPU
// access crosses the bus, which made GPU-heavy passes (character deformation over ~1M elements)
// 50x slower and real-time scenes dark. XSL_MAPPABLE_DEFAULT=0 never maps, =1 forces system memory.
enum class MappableDefault { None, GpuUpload, UmaCustom, SystemCustom, SmallSystem };
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
        if (GetEnvironmentVariableA("XSL_MAPPABLE_DEFAULT", b, 4) && b[0] == '1') {
            XSL_WARN(CH, "DEFAULT-heap buffers live in system memory (XSL_MAPPABLE_DEFAULT=1: CPU-mappable, slower GPU access)");
            return MappableDefault::SystemCustom;
        }
        XSL_INFO(CH, "no GPU upload heap on this runtime: DEFAULT-heap buffers up to 1 MiB are CPU-mappable system memory, larger ones stay in "
                     "video memory (XSL_MAPPABLE_DEFAULT=1 maps all)");
        return MappableDefault::SmallSystem;
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
// XSL_MEM_STATS=1 (diagnostics): every buffer or heap of 4 MB or more the title has the layer make,
// with the heap type it asked for (what a process's memory is made of when it grows past the
// title's own).
static bool mem_stats() { static const bool on = getenv("XSL_MEM_STATS") != nullptr; return on; }
static void mem_note(const char* what, uint64_t bytes, int type, int dim) {
    if (!mem_stats() || bytes < (4u << 20)) return;
    static std::atomic<uint64_t> total{0};
    static volatile long n = 0;
    const uint64_t t = total += bytes;
    if (InterlockedIncrement(&n) <= 1500) XSL_INFO(CH, "mem: %s of %llu KB, heap type %d, dimension %d (%.2f GB of these so far)", what, (unsigned long long)(bytes >> 10), type, dim, t / 1073741824.0);
}
HRESULT_T create_committed_resource(PcObj dev, const void* heapProps, uint32_t heapFlags, const void* desc,
                                    uint32_t state, const void* clear, PcObj* out) {
    D3D12_RESOURCE_DESC d = clean_desc(desc);
    D3D12_HEAP_PROPERTIES hp = *static_cast<const D3D12_HEAP_PROPERTIES*>(heapProps);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) mem_note("committed buffer", d.Width, (int)hp.Type, (int)d.Dimension);
    hp.CreationNodeMask = hp.VisibleNodeMask = 0;
    if (hp.Type == D3D12_HEAP_TYPE_DEFAULT && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        switch (mappable_default(as<ID3D12Device>(dev))) {
            case MappableDefault::GpuUpload: hp.Type = D3D12_HEAP_TYPE_GPU_UPLOAD; break;
            case MappableDefault::SmallSystem:   // small buffers (queries, constants) are the ones titles map
                if (d.Width > (1u << 20)) break;
                [[fallthrough]];
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
                 (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
    if (remap) {
        XSL_INFO(CH, "format %d is not renderable on PC; using %d for this resource", d.Format, standIn);
        d.Format = standIn;
        if (cv) { cvTmp = *cv; cvTmp.Format = standIn; cv = &cvTmp; }
    }
    D3D12_RESOURCE_STATES st = depth_state(d, pc_state(state));
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
    HRESULT hr = logged(as<ID3D12Device>(dev)->CreateReservedResource(&d, depth_state(d, pc_state(state)), cv,
                                                                     IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out))), "CreateReservedResource");
    if (FAILED(hr)) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 40)
            XSL_ERR(CH, "CreateReservedResource failed %08lx (dim %d %llux%ux%u mips %u fmt %d flags %x samples %u state %x)", hr, d.Dimension,
                    (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.Flags, d.SampleDesc.Count, state);
    }
    if (SUCCEEDED(hr) && remap) {
        std::lock_guard<std::mutex> g(g_remap_mu);
        g_remapped.insert(*reinterpret_cast<ID3D12Resource**>(out));
    }
    if (SUCCEEDED(hr) && d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {   // buffers the title places itself: addressable by translated code too
        note_buffer_fwd(*out);
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5)
            XSL_INFO(CH, "reserved buffer %p: %llu bytes at %llx (registered in global memory)", *out, (unsigned long long)d.Width,
                     (unsigned long long)as<ID3D12Resource>(*out)->GetGPUVirtualAddress());
        gm_register(*out, as<ID3D12Resource>(*out)->GetGPUVirtualAddress(), d.Width, 0);
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
    removed_after("UpdateTileMappings");
}
HRESULT_T create_heap(PcObj dev, const void* desc, PcObj* out) {
    D3D12_HEAP_DESC d = *static_cast<const D3D12_HEAP_DESC*>(desc);
    mem_note("heap", d.SizeInBytes, (int)d.Properties.Type, 0);
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
    if (r && gm_watch()) {
        D3D12_RESOURCE_DESC wd = as<ID3D12Resource>(r)->GetDesc();
        if (wd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) gm_watch_note("created", r, as<ID3D12Resource>(r)->GetGPUVirtualAddress(), wd.Width);
    }
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
    HRESULT hr = as<ID3D12Device>(dev)->CreatePlacedResource(as<ID3D12Heap>(heap), offset, &d, depth_state(d, pc_state(state)), cv,
                                                             IID_PPV_ARGS(reinterpret_cast<ID3D12Resource**>(out)));
    if (hr == E_INVALIDARG && d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        // The PC layout can be larger than the console's (e.g. D32_S8 depth), so a texture placed for
        // the console's size may run past the heap. It gets its own memory instead (no aliasing
        // with the heap's other resources).
        D3D12_HEAP_DESC hd = as<ID3D12Heap>(heap)->GetDesc();
        D3D12_RESOURCE_ALLOCATION_INFO ai = as<ID3D12Device>(dev)->GetResourceAllocationInfo(0, 1, &d);
        D3D12_HEAP_PROPERTIES hp = hd.Properties;
        HRESULT h2 = as<ID3D12Device>(dev)->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, depth_state(d, pc_state(state)), cv,
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
std::atomic<uint64_t> g_wr_present{0};   // presents so far (writer tracing)
uint64_t present_count() { return g_wr_present.load(); }
static uint64_t env_u64(const char* n) { char b[16]; return GetEnvironmentVariableA(n, b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }
// Writer tracing: always with XSL_TRACE_WRITERS=1; with only XSL_RT_STATS / XSL_PLANE_OPS, during the
// 30 presents before the requested one (tracing slows every draw). RTV handles are always recorded.
static bool trace_rtvs() {
    static bool on = env_on("XSL_TRACE_WRITERS") || env_u64("XSL_RT_STATS") || env_u64("XSL_PLANE_OPS") || getenv("XSL_DRAW_DUMP") || getenv("XSL_RT_DUMP");
    return on;
}
static bool trace_writers() {
    static bool all = env_on("XSL_TRACE_WRITERS");
    static uint64_t at = std::max(env_u64("XSL_RT_STATS"), env_u64("XSL_PLANE_OPS"));
    if (all) return true;
    uint64_t now = g_wr_present.load();
    return at && now + 30 >= at && now <= at;
}
std::mutex g_wr_mu;
std::map<uint64_t, PcObj> g_rtv_res;              // PC RTV handle -> resource
std::map<PcObj, PcObj> g_wr_rt0, g_wr_pso;        // list -> render target 0, pipeline
std::map<PcObj, std::pair<PcObj, uint64_t>> g_wr_last;   // resource -> (pipeline, draws)
std::map<PcObj, uint64_t> g_wr_frame;                     // resource -> present count of its last draw
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
    if (trace_rtvs()) { std::lock_guard<std::mutex> g(g_wr_mu); g_rtv_res[h] = res; }
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
    removed_after("CreateRenderTargetView", (uint64_t)res);
}
void create_dsv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    D3D12_DEPTH_STENCIL_VIEW_DESC t;
    as<ID3D12Device>(dev)->CreateDepthStencilView(as<ID3D12Resource>(res), view_fix(res, desc, t), cpu(h));
    removed_after("CreateDepthStencilView", (uint64_t)res);
}
UINT format_bytes_of(DXGI_FORMAT f);
// A buffer view reaching past its resource is an invalid call that removes a PC device (the
// console accepts it). Returns the element size; first/count are brought inside the buffer.
static bool clamp_buffer_view(ID3D12Resource* res, DXGI_FORMAT fmt, UINT stride, UINT64& first, UINT& count, const char* kind) {
    const UINT64 width = res->GetDesc().Width;
    UINT64 es = stride ? stride : (fmt == DXGI_FORMAT_UNKNOWN || fmt == DXGI_FORMAT_R32_TYPELESS) ? 4 : format_bytes_of(fmt);
    if (!es) es = 4;
    const UINT64 room = width / es;
    if (first < room && count && first + count <= room) return false;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 12)
        XSL_WARN(CH, "buffer %s past its resource: first %llu count %u of %llu-byte elements on %llu bytes (format %d): brought inside the buffer",
                 kind, (unsigned long long)first, count, (unsigned long long)es, (unsigned long long)width, (int)fmt);
    if (first >= room) first = 0;
    if (!count || first + count > room) count = (UINT)std::min<UINT64>(room - first, UINT_MAX);
    if (!count) count = 1;
    return true;
}
// Textures made without their largest level (XSL_TEXTURE_SKIP_MIP): a view's level range is the
// console texture's, one level off.
static std::mutex g_reduced_mu;
static std::set<PcObj> g_reduced;
void note_reduced(PcObj res, bool on) {
    std::lock_guard<std::mutex> g(g_reduced_mu);
    if (on) g_reduced.insert(res);
    else if (!g_reduced.empty()) g_reduced.erase(res);
}
static bool is_reduced(PcObj res) {
    std::lock_guard<std::mutex> g(g_reduced_mu);
    return !g_reduced.empty() && g_reduced.count(res) != 0;
}
void create_srv(PcObj dev, PcObj res, const void* desc, uint64_t h) {
    if (trace_writers() || (trace_rtvs() && res && as<ID3D12Resource>(res)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)) {
        std::lock_guard<std::mutex> g(g_wr_mu); g_srv_res[h] = res; g_uav_res.erase(h);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC t;
    const D3D12_SHADER_RESOURCE_VIEW_DESC* vd = view_fix(res, desc, t);
    if (res && as<ID3D12Resource>(res)->GetDesc().Format == DXGI_FORMAT_NV12) {   // views of decoded video frames
        static volatile long n = 0;
        auto* td = static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(desc);
        if (InterlockedIncrement(&n) <= 12)
            XSL_INFO(CH, "NV12 SRV: title %s dim %d fmt %d plane %u swizzle %#x -> PC %s dim %d fmt %d plane %u", td ? "desc" : "default", td ? (int)td->ViewDimension : -1,
                     td ? (int)td->Format : -1, td ? td->Texture2D.PlaneSlice : 0, td ? td->Shader4ComponentMapping : 0, vd ? "desc" : "default", vd ? (int)vd->ViewDimension : -1,
                     vd ? (int)vd->Format : -1, vd ? vd->Texture2D.PlaneSlice : 0);
    }
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
    // A mip range past the resource's levels (titles name "all levels" with more than there are,
    // which the console accepts) is an invalid call that removes a PC device: it is clamped.
    D3D12_SHADER_RESOURCE_VIEW_DESC shifted;
    if (vd && res && vd->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D && is_reduced(res)) {
        shifted = *vd;
        auto& t2 = shifted.Texture2D;
        if (t2.MostDetailedMip > 0) t2.MostDetailedMip--;
        else if (t2.MipLevels != UINT(-1) && t2.MipLevels > 1) t2.MipLevels--;
        t2.ResourceMinLODClamp = t2.ResourceMinLODClamp > 1.0f ? t2.ResourceMinLODClamp - 1.0f : 0.0f;
        vd = &shifted;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC clamped;
    if (vd && res) {
        const UINT mips = as<ID3D12Resource>(res)->GetDesc().MipLevels;
        UINT *first = nullptr, *count = nullptr;
        clamped = *vd;
        switch (clamped.ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D: first = &clamped.Texture1D.MostDetailedMip; count = &clamped.Texture1D.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY: first = &clamped.Texture1DArray.MostDetailedMip; count = &clamped.Texture1DArray.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURE2D: first = &clamped.Texture2D.MostDetailedMip; count = &clamped.Texture2D.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY: first = &clamped.Texture2DArray.MostDetailedMip; count = &clamped.Texture2DArray.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURE3D: first = &clamped.Texture3D.MostDetailedMip; count = &clamped.Texture3D.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: first = &clamped.TextureCube.MostDetailedMip; count = &clamped.TextureCube.MipLevels; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY: first = &clamped.TextureCubeArray.MostDetailedMip; count = &clamped.TextureCubeArray.MipLevels; break;
        default: break;
        }
        if (first && mips && (*first >= mips || (*count != UINT_MAX && *first + *count > mips))) {
            if (*first >= mips) *first = mips - 1;
            *count = UINT_MAX;
            vd = &clamped;
        }
        // The same for array ranges ("all slices" as a count past the array).
        const UINT slices = as<ID3D12Resource>(res)->GetDesc().DepthOrArraySize;
        UINT *slice0 = nullptr, *sliceN = nullptr;
        if (clamped.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1DARRAY) { slice0 = &clamped.Texture1DArray.FirstArraySlice; sliceN = &clamped.Texture1DArray.ArraySize; }
        else if (clamped.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) { slice0 = &clamped.Texture2DArray.FirstArraySlice; sliceN = &clamped.Texture2DArray.ArraySize; }
        if (slice0 && slices && (*slice0 >= slices || (*sliceN != UINT_MAX && *slice0 + *sliceN > slices))) {
            if (*slice0 >= slices) *slice0 = slices - 1;
            *sliceN = UINT_MAX;
            vd = &clamped;
        }
        if (clamped.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY && slices >= 6) {
            auto& c = clamped.TextureCubeArray;
            if (c.First2DArrayFace + 6 > slices) { c.First2DArrayFace = 0; vd = &clamped; }
            const UINT room = (slices - c.First2DArrayFace) / 6;
            if (c.NumCubes == 0 || c.NumCubes > room) { c.NumCubes = room; vd = &clamped; }
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC inBuf;
    if (vd && res && vd->ViewDimension == D3D12_SRV_DIMENSION_BUFFER && as<ID3D12Resource>(res)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        inBuf = *vd;
        if (clamp_buffer_view(as<ID3D12Resource>(res), inBuf.Format, inBuf.Buffer.StructureByteStride, inBuf.Buffer.FirstElement, inBuf.Buffer.NumElements, "SRV")) vd = &inBuf;
    }
    as<ID3D12Device>(dev)->CreateShaderResourceView(as<ID3D12Resource>(res), vd, cpu(h));
    removed_after("CreateShaderResourceView", (uint64_t)res, vd ? (uint64_t)vd->Format | ((uint64_t)vd->ViewDimension << 32) : 0, vd ? vd->Texture2D.MostDetailedMip | ((uint64_t)vd->Texture2D.MipLevels << 32) : 0);
}
struct FormatSize;
UINT format_bytes_of(DXGI_FORMAT f);
void create_uav(PcObj dev, PcObj res, PcObj counter, const void* desc, uint64_t h) {
    if (trace_writers() || (trace_rtvs() && res && as<ID3D12Resource>(res)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)) {
        std::lock_guard<std::mutex> g(g_wr_mu); g_uav_res[h] = res; g_srv_res.erase(h);
    }
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
    if (res && desc && getenv("XSL_TRACE_BIG_VIEWS")) {   // buffer views reaching past their resource
        auto* td = static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(desc);
        D3D12_RESOURCE_DESC rd = as<ID3D12Resource>(res)->GetDesc();
        if (td->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
            uint64_t es = td->Buffer.StructureByteStride ? td->Buffer.StructureByteStride : td->Format == DXGI_FORMAT_UNKNOWN || td->Format == DXGI_FORMAT_R32_TYPELESS ? 4 : format_bytes_of(td->Format);
            uint64_t end = (td->Buffer.FirstElement + td->Buffer.NumElements) * (es ? es : 4);
            static volatile long n = 0;
            if (end > rd.Width && InterlockedIncrement(&n) <= 20)
                XSL_WARN(CH, "buffer UAV past its resource: first %llu count %u stride %u fmt %d (%llu bytes) on %llu bytes; PC view %s count %u",
                         (unsigned long long)td->Buffer.FirstElement, td->Buffer.NumElements, td->Buffer.StructureByteStride, (int)td->Format, (unsigned long long)end,
                         (unsigned long long)rd.Width, vd ? "desc" : "default", vd ? vd->Buffer.NumElements : 0);
        }
    }
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
    D3D12_UNORDERED_ACCESS_VIEW_DESC inBuf;
    if (vd && res && vd->ViewDimension == D3D12_UAV_DIMENSION_BUFFER && as<ID3D12Resource>(res)->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        inBuf = *vd;
        if (clamp_buffer_view(as<ID3D12Resource>(res), inBuf.Format, inBuf.Buffer.StructureByteStride, inBuf.Buffer.FirstElement, inBuf.Buffer.NumElements, "UAV")) vd = &inBuf;
    }
    as<ID3D12Device>(dev)->CreateUnorderedAccessView(as<ID3D12Resource>(res), as<ID3D12Resource>(counter), vd, cpu(h));
    removed_after("CreateUnorderedAccessView", (uint64_t)res, vd ? (uint64_t)vd->Format | ((uint64_t)vd->ViewDimension << 32) : 0, vd ? vd->Buffer.FirstElement : 0);
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
        if (d.BufferLocation & 255) {
            // An unaligned view is an invalid call that removes the device: the view starts at the
            // 256-byte boundary below (legal, but the shader reads its constants shifted).
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 3)
                XSL_WARN(CH, "constant buffer view at %llx is not 256-byte aligned (PC requirement); the shader reads shifted constants",
                         (unsigned long long)d.BufferLocation);
            d.SizeInBytes = UINT((d.SizeInBytes + (d.BufferLocation & 255) + 255) & ~255ull);
            if (d.SizeInBytes > 65536) d.SizeInBytes = 65536;
            d.BufferLocation &= ~255ull;
        }
        p = &d;
    }
    as<ID3D12Device>(dev)->CreateConstantBufferView(p, cpu(h));
    removed_after("CreateConstantBufferView", p ? p->BufferLocation : 0, p ? p->SizeInBytes : 0);
}
void create_sampler(PcObj dev, const void* desc, uint64_t h) {
    as<ID3D12Device>(dev)->CreateSampler(static_cast<const D3D12_SAMPLER_DESC*>(desc), cpu(h));
    { std::lock_guard<std::mutex> g(g_samp_mu); if (SampHeap* s = samp_heap_cpu(h)) s->init[(h - s->cpu) / s->inc] = 1; }
    removed_after("CreateSampler");
}
void copy_descriptors_simple(PcObj dev, uint32_t n, uint64_t dst, uint64_t src, int type) {
    if (type > 3) type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        std::lock_guard<std::mutex> g(g_samp_mu);
        SampHeap *d = samp_heap_cpu(dst), *s = samp_heap_cpu(src);
        for (uint32_t k = 0; d && k < n && (dst - d->cpu) / d->inc + k < d->n; k++)
            d->init[(dst - d->cpu) / d->inc + k] = s && (src - s->cpu) / s->inc + k < s->n ? s->init[(src - s->cpu) / s->inc + k] : 1;
    }
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
    removed_after("CopyDescriptorsSimple", n, type);
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
// One reusable uploader per queue. Creating an upload buffer, an allocator, a list and a fence for
// every upload can each wait hundreds of milliseconds on the video memory manager while a title
// streams textures, so these are kept: mapped upload buffers ("chunks") filled from the front, each
// with an allocator of its own, one list and one fence that counts up. A chunk is used again from
// its start once the fence shows that every copy recorded from it has run. Nothing here waits
// for the queue: the uploads run before the lists the title submits next, and those may be held
// back by a wait of the title's own that only this thread's return ends (a 500 ms wait here, per
// upload, was most of a 4 s frame). When no chunk is free another is made, up to a limit past
// which an upload goes the one-off way.
struct UpChunk {
    ID3D12Resource* buf = nullptr;
    uint8_t* map = nullptr;
    uint64_t size = 0, head = 0;   // head: the first byte no upload in flight uses
    ID3D12CommandAllocator* alloc = nullptr;
    uint64_t lastUse = 0;          // the fence value of the last upload recorded from it
};
struct Uploader {
    ID3D12Device* dev = nullptr;
    std::vector<UpChunk> chunks;
    size_t cur = 0;                // the chunk being filled
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t value = 0;            // the last value signalled
    bool bad = false;              // could not be made: every upload goes the one-off way
    // (what up_begin hands out for the upload being recorded)
    ID3D12Resource* buf = nullptr;
    uint8_t* map = nullptr;
};
std::map<ID3D12CommandQueue*, Uploader> g_uploaders;   // under g_up_mu
constexpr uint64_t kUpChunkBytes = 64ull << 20;
constexpr size_t kUpChunkLimit = 48;   // 3 GB of upload buffers in flight at most
static bool up_new_chunk(Uploader& u, uint64_t bytes) {
    UpChunk c;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = std::max(kUpChunkBytes, (bytes + 0xffffull) & ~0xffffull);
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE none{0, 0};   // nothing is read back through the mapping
    bool ok = SUCCEEDED(u.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&c.buf))) &&
              SUCCEEDED(c.buf->Map(0, &none, (void**)&c.map)) && c.map &&
              SUCCEEDED(u.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc)));
    if (!ok) {
        if (c.alloc) c.alloc->Release();
        if (c.buf) c.buf->Release();
        return false;
    }
    c.size = bd.Width;
    u.chunks.push_back(c);
    return true;
}
// Room for `bytes` in the queue's uploader (at *base of u->buf / u->map, a multiple of 512) and its
// list reset for recording. Null: the caller uses the one-off path. Called under g_up_mu.
Uploader* up_begin(ID3D12Device* dev, ID3D12CommandQueue* q, uint64_t bytes, uint64_t* base, int* slot) {
    // XSL_UPLOADER=0: every upload makes its own buffer, allocator and list, as before
    static const bool off = [] { char b[4]; return GetEnvironmentVariableA("XSL_UPLOADER", b, 4) && b[0] == '0'; }();
    if (off) return nullptr;
    Uploader& u = g_uploaders[q];
    if (u.bad) return nullptr;
    if (u.dev != dev) {
        // First use. (A new device and queue at an old address: the old objects are left alone,
        // their device may be gone.)
        u = Uploader{};
        u.dev = dev;
        bool ok = up_new_chunk(u, 0);
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, u.chunks[0].alloc, nullptr, IID_PPV_ARGS(&u.list))) && SUCCEEDED(u.list->Close());
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&u.fence)));
        if (!ok) {
            if (u.list) u.list->Release();
            if (u.fence) u.fence->Release();
            for (auto& c : u.chunks) { c.alloc->Release(); c.buf->Release(); }
            u = Uploader{};
            u.bad = true;
            return nullptr;
        }
    }
    const uint64_t done = u.fence->GetCompletedValue();
    auto fits = [&](UpChunk& c) {
        if (done >= c.lastUse && c.head) {   // every copy from it has run: the whole chunk, and its allocator, are free
            if (FAILED(c.alloc->Reset())) return false;
            c.head = 0;
        }
        return ((c.head + 511) & ~511ull) + bytes <= c.size;
    };
    size_t k = u.cur;
    if (!fits(u.chunks[k])) {
        k = u.chunks.size();
        for (size_t i = 0; i < u.chunks.size() && k == u.chunks.size(); i++)
            if (i != u.cur && fits(u.chunks[i])) k = i;
        if (k == u.chunks.size()) {
            if (u.chunks.size() >= kUpChunkLimit || !up_new_chunk(u, bytes)) return nullptr;
            static volatile long n = 0;
            const long made = InterlockedIncrement(&n);
            if (made == 1 || made % 8 == 0) XSL_INFO(CH, "upload buffers in use on a queue: %zu (%llu MB the newest)", u.chunks.size(), (unsigned long long)(u.chunks.back().size >> 20));
        }
    }
    UpChunk& c = u.chunks[k];
    if (FAILED(u.list->Reset(c.alloc, nullptr))) return nullptr;
    const uint64_t at = (c.head + 511) & ~511ull;
    c.head = at + bytes;
    u.cur = k;
    u.buf = c.buf;
    u.map = c.map;
    *base = at;
    *slot = (int)k;
    return &u;
}
// Runs what was recorded on the queue (before the lists the title submits next) and marks it
// with the next fence value.
void up_end(Uploader& u, ID3D12CommandQueue* q, int slot) {
    u.list->Close();
    ID3D12CommandList* ls[] = {u.list};
    q->ExecuteCommandLists(1, ls);
    removed_after("an internal upload/copy list");
    q->Signal(u.fence, ++u.value);
    u.chunks[(size_t)slot].lastUse = u.value;
}
// The copies of a texture upload: every subresource from its footprint in `buf` (fp offsets are
// from `base`), between the barriers its state needs.
void record_texture_copies(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* buf, uint64_t base, D3D12_RESOURCE_STATES st,
                           const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& fp) {
    // COMMON textures are promoted to COPY_DEST by the copy (and decay back afterwards).
    bool barrier = st != D3D12_RESOURCE_STATE_COMMON && st != D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, st, D3D12_RESOURCE_STATE_COPY_DEST};
    if (barrier) list->ResourceBarrier(1, &b);
    for (UINT s = 0; s < (UINT)fp.size(); s++) {
        D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        d.SubresourceIndex = s;
        D3D12_TEXTURE_COPY_LOCATION sr{buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        sr.PlacedFootprint = fp[s];
        sr.PlacedFootprint.Offset += base;
        list->CopyTextureRegion(&d, 0, 0, 0, &sr, nullptr);
    }
    if (barrier) { std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b); }
}
}  // namespace
uint32_t pc_state_of(uint32_t consoleState) { return (uint32_t)pc_state(consoleState); }
// Bytes into a buffer in video memory, through an upload buffer on the queue (before the lists the
// title submits next). The buffer's state is left to the implicit promotion of buffers.
HRESULT_T upload_buffer(PcObj devObj, PcObj qObj, PcObj dstObj, uint64_t offset, const void* data, uint64_t size) {
    auto* dev = as<ID3D12Device>(devObj);
    auto* q = as<ID3D12CommandQueue>(qObj);
    auto* dst = as<ID3D12Resource>(dstObj);
    if (!size) return S_OK;
    {   // The queue's reusable uploader; what it cannot take goes the one-off way below.
        std::lock_guard<std::mutex> g(g_up_mu);
        uint64_t base = 0;
        int slot = 0;
        if (Uploader* u = up_begin(dev, q, size, &base, &slot)) {
            memcpy(u->map + base, data, (size_t)size);
            u->list->CopyBufferRegion(dst, offset, u->buf, base, size);
            up_end(*u, q, slot);
            return S_OK;
        }
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    UploadJob j{};
    ++g_n_oneoff;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&j.buf));
    if (FAILED(hr)) return hr;
    uint8_t* p = nullptr;
    if (FAILED(hr = j.buf->Map(0, nullptr, (void**)&p))) { j.buf->Release(); return hr; }
    memcpy(p, data, (size_t)size);
    j.buf->Unmap(0, nullptr);
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&j.alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, j.alloc, nullptr, IID_PPV_ARGS(&j.list));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&j.fence));
    j.list->CopyBufferRegion(dst, offset, j.buf, 0, size);
    j.list->Close();
    ID3D12CommandList* ls[] = {j.list};
    q->ExecuteCommandLists(1, ls);
    removed_after("an internal upload/copy list");
    q->Signal(j.fence, 1);
    std::lock_guard<std::mutex> g(g_up_mu);
    reap_uploads(false);
    g_up_jobs.push_back(j);
    return S_OK;
}
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
    {   // The queue's reusable uploader; what it cannot take goes the one-off way below.
        std::lock_guard<std::mutex> g(g_up_mu);
        uint64_t base = 0;
        int slot = 0;
        if (Uploader* u = up_begin(dev, q, total, &base, &slot)) {
            bool ok = true;
            for (UINT s = 0; s < subs && ok; s++) ok = fill(ctx, s, u->map + base + fp[s].Offset, fp[s].Footprint.RowPitch, rows[s], fp[s].Footprint.Depth);
            if (!ok) { u->list->Close(); return E_FAIL; }   // (nothing recorded; the list must be closed before its next reset)
            record_texture_copies(u->list, dst, u->buf, base, (D3D12_RESOURCE_STATES)state, fp);
            up_end(*u, q, slot);
            return S_OK;
        }
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    UploadJob j{};
    ++g_n_oneoff;
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
    record_texture_copies(j.list, dst, j.buf, 0, (D3D12_RESOURCE_STATES)state, fp);
    j.list->Close();
    ID3D12CommandList* ls[] = {j.list};
    q->ExecuteCommandLists(1, ls);
    removed_after("an internal upload/copy list");
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

void* map_shadow_base(PcObj r) {
    std::lock_guard<std::mutex> l(g_shadow_lock);
    auto it = g_shadows.find(r);
    return it == g_shadows.end() ? nullptr : it->second.base;
}
HRESULT_T res_map(PcObj r, uint32_t sub, const void* range, void** out) {
    auto* res = as<ID3D12Resource>(r);
    // A texture in video memory cannot be mapped on a PC, and for a reserved one the attempt alone
    // removes the device: only textures in a CPU-visible custom heap are mapped for real.
    bool tryMap = true;
    if (out && res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        D3D12_HEAP_PROPERTIES hp{};
        tryMap = SUCCEEDED(res->GetHeapProperties(&hp, nullptr)) && hp.Type == D3D12_HEAP_TYPE_CUSTOM && hp.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    }
    HRESULT hr = tryMap ? res->Map(sub, static_cast<const D3D12_RANGE*>(range), out) : E_INVALIDARG;
    removed_after("Map", (uint64_t)r, sub, (uint64_t)hr);
    if (SUCCEEDED(hr) && out && *out && trace_writers() && res->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::lock_guard<std::mutex> g(g_wr_mu);
        g_mapped[res->GetGPUVirtualAddress()] = {res->GetDesc().Width, (uint8_t*)*out};
    }
    if (SUCCEEDED(hr) || !out) return hr;
    D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        return hr;   // the caller hands out the resource's console memory instead
    }
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
        // Write-watched: a title may keep the mapping for good and write through it every frame
        // (movie planes its decoder fills), so writes are what tells that the texture changed.
        void* base = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!base) return E_OUTOFMEMORY;
        MapShadow sh{base, size, {}};
        for (auto& f : fp) sh.offsets.push_back(f.Offset * 2);
        it = g_shadows.emplace(r, std::move(sh)).first;
        static volatile long logged = 0;
        if (InterlockedIncrement(&logged) <= 6)
            XSL_INFO(CH, "Map of a texture in video memory (%llux%u x%u, %u mips, format %d): CPU copy of %llu KiB (uploaded when the title unmaps it, where the layout is known)",
                     (unsigned long long)d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, (unsigned long long)(size >> 10));
    }
    const MapShadow& sh = it->second;
    *out = (uint8_t*)sh.base + (sub < sh.offsets.size() ? sh.offsets[sub] : 0);
    return S_OK;
}
// Whether the title wrote into a mapped texture's CPU copy since the last call.
bool map_shadow_written(PcObj r) {
    void* base = nullptr;
    uint64_t size = 0;
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        auto it = g_shadows.find(r);
        if (it == g_shadows.end()) return false;
        base = it->second.base;
        size = it->second.size;
    }
    void* pages[4];
    ULONG_PTR n = 4;
    DWORD gran = 0;
    if (GetWriteWatch(0, base, (SIZE_T)size, pages, &n, &gran) != 0 || !n) return false;
    ResetWriteWatch(base, (SIZE_T)size);
    return true;
}
void res_unmap(PcObj r, uint32_t sub, const void* range) {
    {
        std::lock_guard<std::mutex> l(g_shadow_lock);
        if (g_shadows.count(r)) return;
    }
    as<ID3D12Resource>(r)->Unmap(sub, static_cast<const D3D12_RANGE*>(range));
    removed_after("Unmap", (uint64_t)r, sub);
}
void res_desc(PcObj r, void* out) { *static_cast<D3D12_RESOURCE_DESC*>(out) = as<ID3D12Resource>(r)->GetDesc(); }
uint64_t res_gpu_va(PcObj r) { return as<ID3D12Resource>(r)->GetGPUVirtualAddress(); }

HRESULT_T alloc_reset(PcObj a) { return logged(as<ID3D12CommandAllocator>(a)->Reset(), "CommandAllocator::Reset"); }

#define L as<ID3D12GraphicsCommandList>(l)
static void rt_dump_pass_end(PcObj l);
HRESULT_T list_close(PcObj l) {
    static const bool rtDump = getenv("XSL_RT_DUMP") != nullptr;
    if (rtDump) rt_dump_pass_end(l);   // (the list's last pass)
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
void list_shadow_reset(PcObj l);
HRESULT_T list_reset(PcObj l, PcObj a, PcObj pso) {
    list_shadow_reset(l);
    if (gm_enabled()) { std::lock_guard<std::mutex> g(g_gm_mu); g_list_global_heap.erase(l); g_list_gm_rs.erase(l); g_list_sampler_heap.erase(l); }
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
// XSL_RT_DUMP=<present> (diagnostics): the colour targets of every pass of the frame before that
// present, as they are when the pass ends (when a list binds other targets, or closes). Each is
// copied inside the title's own list, where its state is known, and written at the present to
// rt_<pass>_<slot>_<width>x<height>_f<format>.bmp beside the shader cache (float formats tone-
// mapped, one-channel formats grey). Multisampled targets and levels other than the first are
// not copied.
struct RtDump { ID3D12Resource* copy; D3D12_RESOURCE_DESC desc; unsigned pass, slot; };
static std::mutex g_rtd_mu;
static std::vector<RtDump> g_rtd;
static std::map<PcObj, std::vector<ID3D12Resource*>> g_rtd_bound;   // list -> colour targets bound now
static unsigned g_rtd_pass = 0;
// (XSL_RT_DUMP=t<seconds>: the first frame presented that long after start-up.)
std::atomic<uint64_t> g_rtd_at{0};
static double rt_dump_after() {
    static const double v = [] { char b[16]; return GetEnvironmentVariableA("XSL_RT_DUMP", b, sizeof b) && b[0] == 't' ? atof(b + 1) : 0.0; }();
    return v;
}
static bool rt_dump_now() {
    static const uint64_t fixed = env_u64("XSL_RT_DUMP");
    const uint64_t at = fixed ? fixed : g_rtd_at.load();
    return at && g_wr_present.load() + 1 == at;
}
static void rt_dump_pass_end(PcObj l) {
    std::vector<ID3D12Resource*> bound;
    {
        std::lock_guard<std::mutex> g(g_rtd_mu);
        auto it = g_rtd_bound.find(l);
        if (it == g_rtd_bound.end()) return;
        bound.swap(it->second);
        g_rtd_bound.erase(it);
    }
    if (bound.empty() || !rt_dump_now() || !g_main_device) return;
    unsigned pass;
    { std::lock_guard<std::mutex> g(g_rtd_mu); pass = g_rtd_pass++; }
    for (unsigned slot = 0; slot < bound.size(); slot++) {
        ID3D12Resource* res = bound[slot];
        if (!res) continue;
        D3D12_RESOURCE_DESC d = res->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || g_rtd.size() >= 400) continue;
        D3D12_RESOURCE_DESC cd = d;
        cd.MipLevels = 1;
        cd.DepthOrArraySize = 1;
        cd.Flags = D3D12_RESOURCE_FLAG_NONE;
        cd.Alignment = 0;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        ID3D12Resource* copy = nullptr;
        if (FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy)))) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {res, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        L->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst{copy, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, src{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        L->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        L->ResourceBarrier(1, &b);
        std::lock_guard<std::mutex> g(g_rtd_mu);
        g_rtd.push_back({copy, cd, pass, slot});
    }
}
void list_om_set_render_targets(PcObj l, uint32_t n, const uint64_t* rtvs, int single, const uint64_t* dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE r[8];
    uint32_t cnt = single ? (n ? 1 : 0) : n;
    for (uint32_t i = 0; i < cnt && i < 8; i++) r[i] = cpu(rtvs[i]);
    static const bool rtDumpOn = getenv("XSL_RT_DUMP") != nullptr;
    if (rtDumpOn) {
        rt_dump_pass_end(l);
        if (rt_dump_now()) {
            std::lock_guard<std::mutex> g(g_wr_mu);
            std::vector<ID3D12Resource*> now;
            for (uint32_t i = 0; i < cnt && i < 8; i++) {
                auto it = g_rtv_res.find(rtvs[i]);
                now.push_back(it == g_rtv_res.end() ? nullptr : as<ID3D12Resource>(it->second));
            }
            std::lock_guard<std::mutex> g2(g_rtd_mu);
            g_rtd_bound[l] = now;
        }
    }
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
// ---- Root signatures described by the shaders themselves ----------------------------------------
// Some engines keep a compact description of the root signature in the private data of every
// shader container ("PRIV", bytes DC E2 ...) and create root-signature objects only once per
// shape (number and kinds of parameters): on the console the ranges inside a descriptor table do
// not matter, each shader loads its descriptors from fixed table offsets. On a PC the pipeline's
// root signature must name every register its shaders use, so a pipeline of such shaders gets a
// root signature of its own, built from that description; a list binds it in place of the title's
// object when the pipeline is drawn with, and the title's root arguments are applied again.
// XSL_TABLE_CHECK=1 (diagnostics): before each dispatch, every resource the compute shader declares
// (its container's PSV0 part) is looked up in the tables the title bound, through the pipeline's
// own root signature: a slot the title left empty, a view of another kind than the shader declares
// (a buffer where it reads a 3D texture) or a table that continues in a page which is not behind
// its first in the PC heap is logged, once per pipeline. A PC GPU reads such a descriptor as what
// the shader says it is; what follows is not defined, and a hung device is one of the outcomes.
struct TcRes { uint32_t type, space, lo, hi, kind; };
struct TcRange { uint32_t param, type, base, space, num, offset; };
std::mutex g_tc_mu;
struct TcPso { bool compute = true; std::vector<TcRes> res; };
std::unordered_map<PcObj, TcPso> g_tc_pso;                  // pipeline -> what its shaders declare
std::unordered_map<PcObj, std::vector<TcRange>> g_tc_rs;    // own root signature -> its table ranges
std::unordered_map<PcObj, int> g_tc_done;                   // pipeline -> reports made
bool (*g_tc_read)(uint64_t, uint32_t, uint32_t*, uint32_t*, bool*) = nullptr;
int (*g_tc_fix)(uint64_t, uint32_t, uint32_t) = nullptr;
uint64_t (*g_tc_fresh)(uint64_t, uint32_t) = nullptr;
bool (*g_tc_dump)(uint64_t, uint32_t, const char*) = nullptr;
void set_table_slot_reader(bool (*read)(uint64_t, uint32_t, uint32_t*, uint32_t*, bool*), int (*fix)(uint64_t, uint32_t, uint32_t), uint64_t (*fresh)(uint64_t, uint32_t),
                           bool (*dump)(uint64_t, uint32_t, const char*)) {
    g_tc_read = read;
    g_tc_fix = fix;
    g_tc_fresh = fresh;
    g_tc_dump = dump;
}
// The same walk puts two cases right in the PC heap unless XSL_TABLE_FIX=0 (see table_slot_fix in
// xs_descriptors.cpp): empty slots and constant buffer views where the shader reads a buffer.
static bool table_log_on() { static const bool on = env_on("XSL_TABLE_CHECK"); return on; }
static bool table_check_on() { static const bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_TABLE_FIX", b, 4) && b[0] == '0'); }() || table_log_on(); return on; }
static bool table_fix_on() { static const bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_TABLE_FIX", b, 4) && b[0] == '0'); }(); return on; }
static void table_check_note(PcObj pso, const void* code, size_t size, bool compute) {
    const uint8_t* c = (const uint8_t*)code;
    if (!table_check_on() || !pso || !c || size < 32 || memcmp(c, "DXBC", 4)) return;
    uint32_t parts = 0;
    memcpy(&parts, c + 28, 4);
    std::vector<TcRes> out;
    for (uint32_t i = 0; i < parts && 32 + 4ull * (i + 1) <= size; i++) {
        uint32_t off = 0, len = 0;
        memcpy(&off, c + 32 + 4 * i, 4);
        if (off + 8ull > size || memcmp(c + off, "PSV0", 4)) continue;
        memcpy(&len, c + off + 4, 4);
        const uint8_t *p = c + off + 8, *end = p + len;
        if (end > c + size || len < 8) break;
        uint32_t info = 0, n = 0, each = 0;
        memcpy(&info, p, 4);
        p += 4 + info;
        if (p + 4 > end) break;
        memcpy(&n, p, 4);
        p += 4;
        if (!n || p + 4 > end) break;
        memcpy(&each, p, 4);
        p += 4;
        for (uint32_t k = 0; k < n && each >= 16 && p + each <= end; k++, p += each) {
            TcRes r{};
            memcpy(&r, p, 16);
            if (each >= 20) memcpy(&r.kind, p + 16, 4);
            out.push_back(r);
        }
        break;
    }
    std::lock_guard<std::mutex> g(g_tc_mu);
    TcPso& e = g_tc_pso[pso];
    e.compute = compute;
    if (compute) e.res.clear();
    for (auto& r : out) {   // (the stages of a graphics pipeline name many of the same registers)
        bool have = false;
        for (auto& x : e.res) have |= x.type == r.type && x.space == r.space && x.lo == r.lo;
        if (!have) e.res.push_back(r);
    }
}
void table_note_stage(PcObj pso, const void* code, size_t size) { table_check_note(pso, code, size, false); }
void table_forget(PcObj pso) { std::lock_guard<std::mutex> g(g_tc_mu); g_tc_pso.erase(pso); }
namespace {
struct CompactRs {
    std::vector<D3D12_ROOT_PARAMETER1> params;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    uint32_t flags = 0;
};
// The description: parameter count, flags (16 bits), two bytes naming the shape, sampler count,
// then the parameters (tables as four bit-packed register lists: SRV, UAV, CBV, sampler) and the
// static samplers.
bool decode_compact_rs(const uint8_t* a, const uint8_t* end, CompactRs& o, size_t* used) {
    const uint8_t* p = a;
    if (end - p < 6) return false;
    const unsigned n = p[0], ns = p[5];
    o.flags = p[1] | p[2] << 8;
    p += 6;
    o.params.resize(n);
    o.ranges.resize(n);
    for (unsigned i = 0; i < n; i++) {
        if (end - p < 2) return false;
        D3D12_ROOT_PARAMETER1& q = o.params[i];
        q = {};
        q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        switch (p[0] & 3) {
        case 0: {
            const uint32_t space = (p[0] >> 2) & 3;
            p += 2;
            for (int kind = 0; kind < 4; kind++) {
                if (p >= end) return false;
                if (!*p) { p++; continue; }
                if (end - p < 2) return false;
                unsigned count = (p[0] >> 4) | (p[1] << 4), bits = (p[0] & 15) + 3, have = 0;
                uint64_t acc = 0;
                p += 2;
                for (unsigned k = 0; k < count; k++) {
                    while (have < bits) {
                        if (p >= end) return false;
                        acc = (acc << 8) | *p++;
                        have += 8;
                    }
                    D3D12_DESCRIPTOR_RANGE1 r{};
                    r.RangeType = (D3D12_DESCRIPTOR_RANGE_TYPE)kind;
                    r.NumDescriptors = 1;
                    r.BaseShaderRegister = (uint32_t)((acc >> (have - bits)) & ((1ull << bits) - 1));
                    r.RegisterSpace = space;
                    r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
                    r.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                    // Registers that follow each other in the list and in number are one range: a
                    // shader that indexes them as an array needs a single range to cover it.
                    if (!o.ranges[i].empty()) {
                        D3D12_DESCRIPTOR_RANGE1& last = o.ranges[i].back();
                        if (last.RangeType == r.RangeType && last.RegisterSpace == r.RegisterSpace && last.BaseShaderRegister + last.NumDescriptors == r.BaseShaderRegister) {
                            last.NumDescriptors++;
                            have -= bits;
                            acc &= (1ull << have) - 1;
                            continue;
                        }
                    }
                    o.ranges[i].push_back(r);
                    have -= bits;
                    acc &= (1ull << have) - 1;
                }
            }
            q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            break;
        }
        case 1:
            q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            q.Constants.Num32BitValues = (p[0] >> 2) & 31;
            q.Constants.ShaderRegister = (p[0] >> 7) | (p[1] << 1);
            p += 2;
            break;
        default:
            q.ParameterType = (p[0] & 3) == 2 ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_UAV;
            q.Descriptor.RegisterSpace = (p[0] >> 4) & 3;
            q.Descriptor.ShaderRegister = (p[0] >> 6) | (p[1] << 2);
            p += 2;
            break;
        }
    }
    for (unsigned i = 0; i < n; i++)
        if (o.params[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            o.params[i].DescriptorTable = {(UINT)o.ranges[i].size(), o.ranges[i].data()};
    auto lod = [&](float& v) {   // one byte in eighths from -3, or FF and the float itself
        if (p >= end) return false;
        if (*p != 0xFF) { v = *p++ * 0.125f - 3.0f; return true; }
        if (end - p < 5) return false;
        memcpy(&v, p + 1, 4);
        p += 5;
        return true;
    };
    for (unsigned i = 0; i < ns; i++) {
        if (end - p < 6) return false;
        D3D12_STATIC_SAMPLER_DESC d{};
        d.Filter = (D3D12_FILTER)(p[0] | p[1] << 8);
        d.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)(p[2] & 15);
        d.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)(p[2] >> 4);
        d.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)(p[3] & 15);
        d.ComparisonFunc = (D3D12_COMPARISON_FUNC)(p[3] >> 4);
        d.MaxAnisotropy = p[4] & 31;
        d.BorderColor = (D3D12_STATIC_BORDER_COLOR)(p[4] >> 5);
        p += 5;
        if (!lod(d.MipLODBias) || !lod(d.MinLOD) || !lod(d.MaxLOD) || p >= end) return false;
        d.ShaderRegister = *p & 63;
        d.RegisterSpace = *p >> 6;
        d.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p++;
        o.samplers.push_back(d);
    }
    *used = (size_t)(p - a);
    return true;
}
// The description inside a shader container, if it has one.
bool find_compact_rs(const void* code, size_t n, const uint8_t** a, const uint8_t** end) {
    const uint8_t* d = (const uint8_t*)code;
    if (!d || n < 36 || memcmp(d, "DXBC", 4)) return false;
    uint32_t count;
    memcpy(&count, d + 28, 4);
    for (uint32_t i = 0; i < count && 32 + 4ull * (i + 1) <= n; i++) {
        uint32_t off, sz;
        memcpy(&off, d + 32 + 4 * i, 4);
        if (off + 8ull > n) return false;
        memcpy(&sz, d + off + 4, 4);
        if (memcmp(d + off, "PRIV", 4) || off + 8ull + sz > n || sz < 8 || d[off + 8] != 0xDC || d[off + 9] != 0xE2) continue;
        *a = d + off + 10;
        *end = d + off + 8 + sz;
        return true;
    }
    return false;
}
std::atomic<bool> g_own_rs_any{false};
std::mutex g_own_mu;
std::map<std::vector<uint8_t>, std::pair<PcObj, std::vector<uint8_t>>> g_own_rs;   // description -> (root signature, serialized)
std::unordered_map<PcObj, std::pair<PcObj, bool>> g_pso_own_rs;                    // pipeline -> (root signature, compute)
// What the title bound at one bind point of a list, and what the PC list has.
struct RootBind {
    PcObj title = nullptr, own = nullptr, bound = nullptr;
    std::map<uint32_t, std::pair<int, uint64_t>> params;   // index -> (0 table, 1 CBV, 2 SRV, 3 UAV, address/handle)
    std::map<uint32_t, std::vector<uint32_t>> consts;
};
std::mutex g_bind_mu;
std::unordered_map<PcObj, std::array<RootBind, 2>> g_root_bind;   // list -> [graphics, compute]
}  // namespace
PcObj own_root_signature(PcObj dev, const void* const* code, const size_t* size, size_t count, std::vector<uint8_t>* blob) {
    const uint8_t *a = nullptr, *end = nullptr;
    CompactRs best;
    size_t bestRanges = 0, bestUsed = 0;
    const uint8_t* bestA = nullptr;
    bool found = false, differ = false;
    uint32_t flags = 0;
    for (size_t i = 0; i < count; i++) {
        if (!code[i] || !find_compact_rs(code[i], size[i], &a, &end)) continue;
        CompactRs c;
        size_t used = 0;
        if (!decode_compact_rs(a, end, c, &used)) {
            static volatile long w = 0;
            if (InterlockedIncrement(&w) <= 3) XSL_WARN(CH, "a shader's own root-signature description (%zu bytes) cannot be read", (size_t)(end - a));
            continue;
        }
        size_t nr = 0;
        for (auto& r : c.ranges) nr += r.size();
        // Stages of one pipeline describe the same signature; should they differ, the fullest wins.
        // (The flags differ by stage, input-assembler use for one: all of them apply.)
        if (found && (used != bestUsed || a[0] != bestA[0] || memcmp(a + 3, bestA + 3, used - 3))) differ = true;
        flags |= c.flags;
        if (!found || nr > bestRanges) { best = std::move(c); bestRanges = nr; bestUsed = used; bestA = a; }
        for (unsigned k = 0; k < best.params.size(); k++)
            if (best.params[k].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                best.params[k].DescriptorTable = {(UINT)best.ranges[k].size(), best.ranges[k].data()};
        found = true;
    }
    if (!found) return nullptr;
    static const bool trace = getenv("XSL_RS_TRACE") != nullptr;   // what each stage of the first pipelines carries
    static volatile long traced = 0;
    if (trace && bestRanges && InterlockedIncrement(&traced) <= 16) {
        std::string t;
        for (size_t i = 0; i < count; i++) {
            char b[40];
            snprintf(b, sizeof b, " | stage %zu (%zu bytes):", i, size[i]);
            t += b;
            if (!code[i] || !find_compact_rs(code[i], size[i], &a, &end)) { t += memcmp(code[i], "DXBC", 4) ? " not a container" : " none"; continue; }
            for (const uint8_t* q = a; q < end; q++) { snprintf(b, sizeof b, " %02x", *q); t += b; }
        }
        XSL_INFO(CH, "root signature descriptions:%s", t.c_str());
    }
    if (differ) {
        static volatile long w = 0;
        if (InterlockedIncrement(&w) <= 12) {
            std::string t;
            for (size_t i = 0; i < count; i++) {
                if (!code[i] || !find_compact_rs(code[i], size[i], &a, &end)) continue;
                t += " |";
                for (const uint8_t* q = a; q < end; q++) { char b[4]; snprintf(b, sizeof b, " %02x", *q); t += b; }
            }
            XSL_WARN(CH, "the stages of a pipeline describe different root signatures: the fullest one is used (%s)", t.c_str());
        }
    }
    best.flags = flags;
    std::vector<uint8_t> key(bestA, bestA + bestUsed);
    key[1] = (uint8_t)flags;
    key[2] = (uint8_t)(flags >> 8);
    std::lock_guard<std::mutex> g(g_own_mu);
    auto it = g_own_rs.find(key);
    if (it == g_own_rs.end()) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d{D3D_ROOT_SIGNATURE_VERSION_1_1};
        d.Desc_1_1.NumParameters = (UINT)best.params.size();
        d.Desc_1_1.pParameters = best.params.data();
        d.Desc_1_1.NumStaticSamplers = (UINT)best.samplers.size();
        d.Desc_1_1.pStaticSamplers = best.samplers.data();
        d.Desc_1_1.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)best.flags;
        ID3DBlob *b = nullptr, *e = nullptr;
        PcObj rs = nullptr;
        std::vector<uint8_t> ser;
        HRESULT hr = D3D12SerializeVersionedRootSignature(&d, &b, &e);
        if (SUCCEEDED(hr)) {
            ser.assign((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize());
            hr = create_root_signature_ex(dev, ser.data(), ser.size(), &rs, true);
        }
        if (SUCCEEDED(hr) && table_check_on()) {
            std::vector<TcRange> rr;
            for (uint32_t k = 0; k < best.params.size(); k++) {
                if (best.params[k].ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;
                uint32_t at = 0;
                for (auto& q : best.ranges[k]) {
                    if (q.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) at = q.OffsetInDescriptorsFromTableStart;
                    rr.push_back({k, (uint32_t)q.RangeType, q.BaseShaderRegister, q.RegisterSpace, q.NumDescriptors, at});
                    at += q.NumDescriptors;
                }
            }
            std::lock_guard<std::mutex> g2(g_tc_mu);
            g_tc_rs[rs] = std::move(rr);
        }
        static volatile long n = 0;
        if (FAILED(hr)) XSL_ERR(CH, "root signature from a shader's own description failed %08lx: %s", hr, e ? (const char*)e->GetBufferPointer() : "");
        else if (InterlockedIncrement(&n) <= 4)
            XSL_INFO(CH, "pipelines get the root signature their shaders describe (%zu parameters, %zu table ranges, %zu static samplers)", best.params.size(),
                     bestRanges, best.samplers.size());
        if (b) b->Release();
        if (e) e->Release();
        it = g_own_rs.emplace(std::move(key), std::make_pair(rs, std::move(ser))).first;
    }
    if (it->second.first && blob) *blob = it->second.second;
    return it->second.first;
}
static bool own_rs_blob(PcObj rs, std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> g(g_own_mu);
    for (auto& [desc, v] : g_own_rs)
        if (v.first == rs) { *out = v.second; return true; }
    return false;
}
void note_pso_own_rs(PcObj pso, PcObj rs, bool compute) {
    if (!pso || (!rs && !g_own_rs_any)) return;
    std::lock_guard<std::mutex> g(g_own_mu);
    if (rs) { g_pso_own_rs[pso] = {rs, compute}; g_own_rs_any = true; }
    else g_pso_own_rs.erase(pso);
}
static void gm_bind(PcObj l, PcObj rs, bool compute);
// The root signature a bind point needs is put on the list (with the title's arguments) before work
// that uses it.
static void root_ensure(PcObj l, bool compute) {
    if (!g_own_rs_any) return;
    RootBind b;
    {
        std::lock_guard<std::mutex> g(g_bind_mu);
        auto& cur = g_root_bind[l][compute];
        PcObj want = cur.own ? cur.own : cur.title;
        if (!want || cur.bound == want) return;
        cur.bound = want;
        b = cur;
    }
    auto* L2 = as<ID3D12GraphicsCommandList>(l);
    if (compute) L2->SetComputeRootSignature(as<ID3D12RootSignature>(b.bound));
    else L2->SetGraphicsRootSignature(as<ID3D12RootSignature>(b.bound));
    gm_bind(l, b.bound, compute);
    for (auto& [i, v] : b.params) {
        D3D12_GPU_DESCRIPTOR_HANDLE h{v.second};
        if (!rs_param_ok(b.bound, i, v.first)) continue;   // an argument for another signature
        if (compute) switch (v.first) {
            case 0: L2->SetComputeRootDescriptorTable(i, h); break;
            case 1: L2->SetComputeRootConstantBufferView(i, v.second); break;
            case 2: L2->SetComputeRootShaderResourceView(i, v.second); break;
            default: L2->SetComputeRootUnorderedAccessView(i, v.second); break;
        } else switch (v.first) {
            case 0: L2->SetGraphicsRootDescriptorTable(i, h); break;
            case 1: L2->SetGraphicsRootConstantBufferView(i, v.second); break;
            case 2: L2->SetGraphicsRootShaderResourceView(i, v.second); break;
            default: L2->SetGraphicsRootUnorderedAccessView(i, v.second); break;
        }
    }
    for (auto& [i, c] : b.consts) {
        if (!rs_param_ok(b.bound, i, 4)) continue;
        if (compute) L2->SetComputeRoot32BitConstants(i, (UINT)c.size(), c.data(), 0);
        else L2->SetGraphicsRoot32BitConstants(i, (UINT)c.size(), c.data(), 0);
    }
}
// With lazily bound root signatures: whether rs is the one on the list for that bind point.
static bool root_is_bound(PcObj l, bool compute, PcObj rs) {
    if (!g_own_rs_any) return true;
    std::lock_guard<std::mutex> g(g_bind_mu);
    auto it = g_root_bind.find(l);
    return it != g_root_bind.end() && it->second[compute].bound == rs;
}
// Records a root argument; true when the list has the right root signature for it now.
static bool root_arg(PcObj l, bool compute, uint32_t i, int kind, uint64_t v) {
    if (!g_own_rs_any) return true;
    std::lock_guard<std::mutex> g(g_bind_mu);
    auto& b = g_root_bind[l][compute];
    b.params[i] = {kind, v};
    return b.bound && b.bound == (b.own ? b.own : b.title) && rs_param_ok(b.bound, i, kind);
}
static bool root_consts(PcObj l, bool compute, uint32_t i, uint32_t n, const void* d, uint32_t o) {
    if (!g_own_rs_any) return true;
    std::lock_guard<std::mutex> g(g_bind_mu);
    auto& b = g_root_bind[l][compute];
    auto& c = b.consts[i];
    if (c.size() < o + n) c.resize(o + n);
    memcpy(c.data() + o, d, n * 4ull);
    return b.bound && b.bound == (b.own ? b.own : b.title) && rs_param_ok(b.bound, i, 4);
}
// The title's root signature for a bind point; true when it is to be bound at once (no pipeline
// with a root signature of its own exists).
static bool root_title(PcObj l, bool compute, PcObj rs) {
    if (!g_own_rs_any) return true;
    std::lock_guard<std::mutex> g(g_bind_mu);
    auto& b = g_root_bind[l][compute];
    if (b.title != rs) { b.params.clear(); b.consts.clear(); }
    b.title = rs;
    return false;
}
// Layer passes that put their own state on a title list call this: the bind point is bound again
// before the title's next work.
static void root_lost(PcObj l, bool compute) {
    if (gm_enabled()) {   // the global-memory table is bound again with the root signature, not onto the layer's own
        std::lock_guard<std::mutex> g(g_gm_mu);
        auto it = g_list_gm_rs.find(l);
        if (it != g_list_gm_rs.end()) (compute ? it->second.second : it->second.first) = nullptr;
    }
    if (!g_own_rs_any) return;
    std::lock_guard<std::mutex> g(g_bind_mu);
    g_root_bind[l][compute].bound = nullptr;
}
void note_pso_rs(PcObj pso, PcObj rs, bool compute) {
    if (!g_info || !pso) return;
    std::lock_guard<std::mutex> g(g_rs_mu);
    g_pso_rs[pso] = {rs, compute};
}
void list_set_gfx_root_signature(PcObj l, PcObj rs) {
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].gfx = rs; }
    if (!root_title(l, false, rs)) return;   // bound before the next draw (root_ensure)
    L->SetGraphicsRootSignature(as<ID3D12RootSignature>(rs));
    gm_bind(l, rs, false);
}
static void shadow_param(PcObj l, uint32_t i, int kind, uint64_t v) {
    std::lock_guard<std::mutex> g(g_cs_mu);
    g_cs_shadow[l].params[i] = {kind, v};
}
void list_shadow_reset(PcObj l) {
    if (g_own_rs_any) { std::lock_guard<std::mutex> g(g_bind_mu); g_root_bind.erase(l); }
    std::lock_guard<std::mutex> g(g_cs_mu);
    g_cs_shadow.erase(l);
}
void list_set_compute_root_signature(PcObj l, PcObj rs) {
    {
        std::lock_guard<std::mutex> g(g_cs_mu);
        auto& sh = g_cs_shadow[l];
        if (sh.rs != rs) { sh.params.clear(); sh.consts.clear(); }
        sh.rs = rs;
    }
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].cs = rs; }
    if (!root_title(l, true, rs)) return;   // bound before the next dispatch (root_ensure)
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
// The name of the pipeline a list has selected ("xsl <shader keys>" for translated ones), for dumps.
// What a PC render-target handle points at, for the draw dump (known with XSL_DRAW_DUMP set).
std::string rtv_describe(uint64_t h) {
    std::lock_guard<std::mutex> g(g_wr_mu);
    auto it = g_rtv_res.find(h);
    if (it == g_rtv_res.end() || !it->second) return "?";
    const D3D12_RESOURCE_DESC d = as<ID3D12Resource>(it->second)->GetDesc();
    char b[96];
    snprintf(b, sizeof b, "%p %llux%u format %d", (void*)it->second, (unsigned long long)d.Width, d.Height, (int)d.Format);
    return b;
}
std::string list_pso_name(PcObj l) {
    PcObj pso = nullptr;
    { std::lock_guard<std::mutex> g(g_cs_mu); auto it = g_cs_shadow.find(l); if (it != g_cs_shadow.end()) pso = it->second.pso; }
    if (!pso) return "(none)";
    wchar_t name[160] = L"";
    UINT n = sizeof name;
    if (FAILED(as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &n, name))) return "(unnamed)";
    std::string out;
    for (const wchar_t* p = name; *p; p++) out += (char)*p;
    return out;
}
void list_set_pso(PcObj l, PcObj pso) {
    { std::lock_guard<std::mutex> g(g_cs_mu); g_cs_shadow[l].pso = pso; }
    if (g_own_rs_any) {   // the pipeline's own root signature, if it has one
        std::pair<PcObj, bool> own{nullptr, false};
        { std::lock_guard<std::mutex> g(g_own_mu); auto it = g_pso_own_rs.find(pso); if (it != g_pso_own_rs.end()) own = it->second; }
        std::lock_guard<std::mutex> g(g_bind_mu);
        auto& b = g_root_bind[l];
        b[0].own = own.first && !own.second ? own.first : nullptr;
        b[1].own = own.first && own.second ? own.first : nullptr;
    }
    if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_wr_pso[l] = pso; }
    if (g_info) { std::lock_guard<std::mutex> g(g_rs_mu); g_list_rs[l].pso = pso; }
    const ULONGLONG t0 = GetTickCount64();
    L->SetPipelineState(as<ID3D12PipelineState>(pso));
    if (const ULONGLONG dt = GetTickCount64() - t0; dt >= 20 && pso && pso_times()) XSL_INFO(CH, "pso %p: selected on list %p in %llu ms", pso, l, (unsigned long long)(GetTickCount64() - t0));
    if (const ULONGLONG dt = GetTickCount64() - t0; dt >= 500 && pso) {   // the driver made the list wait (a compile it had put off)
        wchar_t nm[128] = L"";
        UINT nn = sizeof nm;
        as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nn, nm);
        static volatile long shown = 0;
        if (InterlockedIncrement(&shown) <= 20) XSL_WARN(CH, "SetPipelineState took %llu ms in the driver (pipeline %ls)", (unsigned long long)dt, nm);
    }
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
static int32_t prof_begin(PcObj l, char kind = '?', uint32_t a = 0, uint32_t b = 0, uint32_t c = 0);
static void table_check(PcObj l);
static void prof_end(PcObj l, int32_t q);
// Work of an unusual size (a count read from memory the layer does not fill in yet hangs the GPU):
// the first few are logged with the pipeline's shaders.
static void note_big(PcObj l, const char* what, uint64_t n, uint32_t a, uint32_t b, uint32_t c) {
    if (n < (1ull << 24)) return;
    static volatile long k = 0;
    if (InterlockedIncrement(&k) > 24) return;
    PcObj pso = nullptr;
    { std::lock_guard<std::mutex> g(g_cs_mu); auto it = g_cs_shadow.find(l); if (it != g_cs_shadow.end()) pso = it->second.pso; }
    XSL_WARN(CH, "large %s(%u, %u, %u) on list %p, pipeline %p", what, a, b, c, (void*)l, (void*)pso);
}
void list_draw(PcObj l, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { note_big(l, "Draw", (uint64_t)a * b, a, b, c); root_ensure(l, false); table_check(l); trace_draw_target(l, a * b); if (getenv("XSL_TRACE_BUFFER")) note_dispatch(l, false); note_draw(l); note_reads(l, false); int32_t pq = prof_begin(l); L->DrawInstanced(a, b, c, d); prof_end(l, pq); }
void list_draw_indexed(PcObj l, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { note_big(l, "DrawIndexed", (uint64_t)a * b, a, b, c); root_ensure(l, false); table_check(l); trace_draw_target(l, a * b); if (getenv("XSL_TRACE_BUFFER")) note_dispatch(l, false); note_draw(l); note_reads(l, false); int32_t pq = prof_begin(l); L->DrawIndexedInstanced(a, b, c, d, e); prof_end(l, pq); }
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
// XSL_GPU_PROFILE=<present>: GPU time of every draw, dispatch and indirect execute recorded from
// the frame before that present on, while the queries last (32,768 operations): per frame the sum,
// the pipelines that took most and the slowest single operations, logged when the queries are used
// up, 40 presents after the start, or when the device is removed (then with the operation the GPU
// had begun and not finished: what hung it).
// XSL_GPU_PROFILE=slow: from the first present (past the 300th) that took over 250 ms;
// XSL_GPU_PROFILE=t<seconds>: from the first present that many seconds after start-up.
struct GpuProfOp { uint32_t q; PcObj pso; char kind; uint32_t a, b, c; uint64_t present; };   // first query, pipeline, what, frame
struct GpuProf {
    std::mutex mu;
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* rb = nullptr;
    ID3D12CommandQueue* queue = nullptr;   // (for its timestamp frequency)
    uint64_t freq = 0;
    uint64_t* map = nullptr;               // the read-back buffer, mapped for good (a removed device maps nothing)
    uint32_t next = 0;
    std::vector<GpuProfOp> ops;
    bool done = false;
} g_prof;
constexpr uint32_t kProfQueries = 1u << 16;
// XSL_GPU_PROFILE=slow: the frame after the first present (past the 300th) that took over 250 ms.
std::atomic<uint64_t> g_prof_at{0};
static bool prof_slow() { static bool v = [] { char b[16]; return GetEnvironmentVariableA("XSL_GPU_PROFILE", b, sizeof b) && !strcmp(b, "slow"); }(); return v; }
static double prof_after() {   // seconds after start-up (0: not asked for)
    static double v = [] { char b[16]; return GetEnvironmentVariableA("XSL_GPU_PROFILE", b, sizeof b) && b[0] == 't' ? atof(b + 1) : 0.0; }();
    return v;
}
static uint64_t prof_at() {
    static uint64_t fixed = [] { char b[16]; return GetEnvironmentVariableA("XSL_GPU_PROFILE", b, sizeof b) ? _strtoui64(b, nullptr, 10) : 0ull; }();
    return fixed ? fixed : g_prof_at.load();
}
// Also: presents per 10 s in the log (frame rate over a run), and the slowest present of each period.
static void prof_slow_check(uint64_t present) {
    static LARGE_INTEGER last{}, f{}, period{};
    static uint64_t periodStart = 0;
    static double worst = 0;
    static int slowRun = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    double ms = last.QuadPart ? double(now.QuadPart - last.QuadPart) * 1000.0 / double(f.QuadPart) : 0;
    last = now;
    worst = std::max(worst, ms);
    if (!period.QuadPart) { period = now; periodStart = present; }
    double ps = double(now.QuadPart - period.QuadPart) / double(f.QuadPart);
    if (ps >= 10.0) {
        long rr = InterlockedExchange(&g_rays_run, 0), rs = InterlockedExchange(&g_rays_skipped, 0);
        rt_debug_dump();
        long why[5];
        for (int k = 0; k < 5; k++) why[k] = InterlockedExchange(&g_rays_why[k], 0);
        char wb[160] = "";
        if (rs) snprintf(wb, sizeof wb, " (no pipeline %ld, start program building %ld, failed %ld, reached program building %ld, indirect %ld)", why[0], why[1], why[2], why[3], why[4]);
        XSL_INFO(CH, "frame rate: %.1f presents/s (presents %llu..%llu, slowest %.0f ms)%s%s", double(present - periodStart) / ps, (unsigned long long)periodStart,
                 (unsigned long long)present, worst, (rr || rs) ? fmt_rays(rr, rs).c_str() : "", wb);
        period = now;
        periodStart = present;
        worst = 0;
    }
    if (rt_dump_after() > 0 && !g_rtd_at.load()) {
        FILETIME c, e, k, u, n;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        GetSystemTimeAsFileTime(&n);
        const double up = double(((uint64_t)n.dwHighDateTime << 32 | n.dwLowDateTime) - ((uint64_t)c.dwHighDateTime << 32 | c.dwLowDateTime)) / 1e7;
        if (up >= rt_dump_after()) {
            g_rtd_at = present + 2;
            XSL_INFO(CH, "%.1f s after start-up: render targets of present %llu are written out", up, (unsigned long long)(present + 2));
        }
    }
    if (prof_after() > 0 && !g_prof_at.load()) {
        FILETIME c, e, k, u, n;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        GetSystemTimeAsFileTime(&n);
        const double up = double(((uint64_t)n.dwHighDateTime << 32 | n.dwLowDateTime) - ((uint64_t)c.dwHighDateTime << 32 | c.dwLowDateTime)) / 1e7;
        if (up >= prof_after()) {
            g_prof_at = present + 2;
            XSL_INFO(CH, "%.1f s after start-up: GPU times from present %llu on", up, (unsigned long long)(present + 2));
        }
    }
    if (!prof_slow() || g_prof_at.load()) return;
    slowRun = ms > 250 ? slowRun + 1 : 0;
    if (present > 300 && slowRun >= 3) {
        g_prof_at = present + 2;
        XSL_INFO(CH, "presents up to %llu took over 250 ms each: profiling present %llu", (unsigned long long)present, (unsigned long long)(present + 2));
    }
}
static int32_t prof_begin(PcObj l, char kind, uint32_t a, uint32_t b, uint32_t c) {
    uint64_t at = prof_at();
    if (!at || g_wr_present.load() + 1 < at || !g_main_device || g_prof.done) return -1;
    std::lock_guard<std::mutex> g(g_prof.mu);
    if (!g_prof.heap) {
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kProfQueries, 0};
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kProfQueries * 8ull;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_main_device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_prof.heap))) ||
            FAILED(g_main_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_prof.rb))))
            return -1;
        g_prof.rb->Map(0, nullptr, (void**)&g_prof.map);
        if (g_prof.queue) g_prof.queue->GetTimestampFrequency(&g_prof.freq);
    }
    if (g_prof.next + 2 > kProfQueries) return -1;
    uint32_t q = g_prof.next;
    g_prof.next += 2;
    PcObj pso = nullptr;
    { std::lock_guard<std::mutex> g2(g_cs_mu); auto it = g_cs_shadow.find(l); if (it != g_cs_shadow.end()) pso = it->second.pso; }
    g_prof.ops.push_back({q, pso, kind, a, b, c, g_wr_present.load() + 1});
    L->EndQuery(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP, q);
    L->ResolveQueryData(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP, q, 1, g_prof.rb, q * 8ull);   // (at once: an operation that hangs leaves its start)
    return (int32_t)q;
}
static void prof_end(PcObj l, int32_t q) {
    if (q < 0) return;
    L->EndQuery(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
    L->ResolveQueryData(g_prof.heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1, 1, g_prof.rb, (q + 1) * 8ull);
}
static void prof_report(ID3D12CommandQueue* queue, bool now = false) {
    uint64_t at = prof_at();
    if (queue) g_prof.queue = queue;
    if (!at || g_prof.done) return;
    std::lock_guard<std::mutex> g(g_prof.mu);
    if (queue && !g_prof.freq) queue->GetTimestampFrequency(&g_prof.freq);
    if (!g_prof.map || g_prof.ops.empty()) return;
    const uint64_t present = g_wr_present.load();
    const bool full = g_prof.next + 2 > kProfQueries;
    if (!now && !(full && present >= g_prof.ops.back().present + 4) && present < at + 40) return;
    g_prof.done = true;
    const uint64_t freq = g_prof.freq ? g_prof.freq : 1;
    const uint64_t* t = g_prof.map;
    auto name_of = [](PcObj pso, wchar_t* name, UINT bytes) {
        wcscpy(name, L"(none)");
        if (pso) as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &bytes, name);
    };
    for (size_t i0 = 0; i0 < g_prof.ops.size();) {   // frame by frame
        size_t i1 = i0;
        while (i1 < g_prof.ops.size() && g_prof.ops[i1].present == g_prof.ops[i0].present) i1++;
        std::map<PcObj, std::pair<double, uint32_t>> per;
        std::vector<std::pair<double, size_t>> single;
        double total = 0;
        uint32_t unreached = 0;
        uint64_t first = 0, last = 0;
        for (size_t i = i0; i < i1; i++) {
            const GpuProfOp& o = g_prof.ops[i];
            if (!t[o.q]) { unreached++; continue; }   // (the GPU did not get there)
            if (!first || t[o.q] < first) first = t[o.q];
            last = std::max(last, std::max(t[o.q], t[o.q + 1]));
            if (!t[o.q + 1]) {   // begun and not finished
                wchar_t name[128];
                name_of(o.pso, name, sizeof name);
                XSL_INFO(CH, "GPU profile: present %llu, operation %zu of the frame was begun and not finished: %c %u x %u x %u: %ls", (unsigned long long)o.present,
                         i - i0, o.kind, o.a, o.b, o.c, name);
                continue;
            }
            if (t[o.q + 1] < t[o.q]) continue;
            double us = double(t[o.q + 1] - t[o.q]) * 1e6 / double(freq);
            per[o.pso].first += us;
            per[o.pso].second++;
            single.push_back({us, i});
            total += us;
        }
        std::vector<std::pair<double, PcObj>> v;
        for (auto& [p, e] : per) v.push_back({e.first, p});
        std::sort(v.rbegin(), v.rend());
        std::sort(single.rbegin(), single.rend());
        XSL_INFO(CH, "GPU profile of present %llu: %zu operations (%u the GPU did not reach), %.1f ms in them, %.1f ms from the first to the last", (unsigned long long)g_prof.ops[i0].present,
                 i1 - i0, unreached, total / 1000.0, double(last - first) * 1e3 / double(freq));
        for (size_t i = 0; i < v.size() && i < 10; i++) {
            wchar_t name[128];
            name_of(v[i].second, name, sizeof name);
            XSL_INFO(CH, "  %9.1f us in %4u ops: %ls", v[i].first, per[v[i].second].second, name);
        }
        for (size_t i = 0; i < single.size() && i < 40; i++) {
            const GpuProfOp& o = g_prof.ops[single[i].second];
            wchar_t name[128];
            name_of(o.pso, name, sizeof name);
            XSL_INFO(CH, "  single: %9.1f us, %c %u x %u x %u: %ls", single[i].first, o.kind, o.a, o.b, o.c, name);
        }
        i0 = i1;
    }
}
static void prof_report_now() { prof_report(nullptr, true); }
// (see XSL_TABLE_CHECK above)
static void table_check(PcObj l) {
    if (!table_check_on() || !g_tc_read || !g_own_rs_any) return;
    PcObj pso = nullptr;
    { std::lock_guard<std::mutex> g(g_cs_mu); auto it = g_cs_shadow.find(l); if (it != g_cs_shadow.end()) pso = it->second.pso; }
    if (!pso) return;
    bool compute = true;
    { std::lock_guard<std::mutex> g(g_tc_mu); auto p = g_tc_pso.find(pso); if (p != g_tc_pso.end()) compute = p->second.compute; }
    RootBind b;
    { std::lock_guard<std::mutex> g(g_bind_mu); auto it = g_root_bind.find(l); if (it == g_root_bind.end()) return; b = it->second[compute ? 1 : 0]; }
    std::vector<TcRes> res;
    std::vector<TcRange> ranges;
    bool logIt = false;
    {
        std::lock_guard<std::mutex> g(g_tc_mu);
        logIt = table_log_on() && g_tc_done[pso] < 2;
        if (!logIt && !table_fix_on()) return;
        auto p = g_tc_pso.find(pso);
        auto r = g_tc_rs.find(b.own ? b.own : b.title);   // (what the dispatch runs with; `bound` is unset after the layer's own dispatches)
        if (logIt && (p == g_tc_pso.end() || r == g_tc_rs.end())) {
            static volatile long n = 0;
            wchar_t nm[128] = L"";
            UINT sz = sizeof nm;
            as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, nm);
            if (InterlockedIncrement(&n) <= 40)
                XSL_INFO(CH, "table check: %ls is not checked (%s; root signature %p, own %p, title %p)", nm,
                         p == g_tc_pso.end() ? "its resources are not known" : "its root signature is not one made from a shader's description", b.bound, b.own, b.title);
            g_tc_done[pso] = 2;
        }
        if (p == g_tc_pso.end() || r == g_tc_rs.end()) return;
        res = p->second.res;
        ranges = r->second;
    }
    // PSV0: type 1 sampler, 2 constant buffer, 3..5 shader resource (typed, raw, structured), 6..9
    // unordered access; kind 1 Texture1D, 2 2D, 3 2DMS, 4 3D, 5 cube, 6 1D array, 7 2D array, 8 2DMS
    // array, 9 cube array, 10..12 buffers, 16 acceleration structure. Views: see desc_set_kind.
    static const uint8_t dimOf[17] = {0, 2, 4, 6, 8, 9, 3, 5, 7, 10, 1, 1, 1, 0, 0, 0, 11};
    static const char* kindName[17] = {"?", "Texture1D", "Texture2D", "Texture2DMS", "Texture3D", "TextureCube", "Texture1DArray", "Texture2DArray", "Texture2DMSArray",
                                       "TextureCubeArray", "typed buffer", "raw buffer", "structured buffer", "cbuffer", "sampler", "tbuffer", "acceleration structure"};
    std::string report;
    int bad = 0, checked = 0;
    std::map<uint32_t, uint32_t> splitTables;   // parameter -> slots the shader reads of a table that runs into a page elsewhere
    // XSL_TABLE_CHECK=<part of a pipeline's name>: every resource of that pipeline, as found.
    static const std::wstring full = [] { char b[40] = ""; GetEnvironmentVariableA("XSL_TABLE_CHECK", b, sizeof b); return strlen(b) > 4 ? std::wstring(b, b + strlen(b)) : std::wstring(); }();
    bool all = false, named = false;
    if (!full.empty()) {
        wchar_t nm[128] = L"";
        UINT sz = sizeof nm;
        as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, nm);
        named = wcsstr(nm, full.c_str()) != nullptr;
        all = named && logIt;
    }
    for (auto& q : res) {
        if (q.type == 1) {   // a sampler: its slot in the sampler heap must have been written
            const TcRange* in = nullptr;
            for (auto& r : ranges)
                if (r.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && r.space == q.space && q.lo >= r.base && q.lo - r.base < r.num) { in = &r; break; }
            auto pa = in ? b.params.find(in->param) : b.params.end();
            int state = -1;   // -1 not in a table, 0 no table bound, 1 not a known heap, 2 written, 3 not written, 4 not written and given a sampler
            uint64_t index = 0;
            if (in) {
                state = 0;
                if (pa != b.params.end() && pa->second.first == 0) {
                    state = 1;
                    std::lock_guard<std::mutex> g(g_samp_mu);
                    for (auto& sh : g_samp_heaps) {
                        const uint64_t h = pa->second.second;
                        if (!sh.gpu || h < sh.gpu || h >= sh.gpu + (uint64_t)sh.n * sh.inc) continue;
                        index = (h - sh.gpu) / sh.inc + in->offset + (q.lo - in->base);
                        if (index >= sh.n) break;
                        state = sh.init[index] ? 2 : 3;
                        if (state == 3 && table_fix_on() && g_main_device) {
                            D3D12_SAMPLER_DESC sd{};
                            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                            sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                            sd.MaxAnisotropy = 1;
                            sd.MaxLOD = D3D12_FLOAT32_MAX;
                            g_main_device->CreateSampler(&sd, D3D12_CPU_DESCRIPTOR_HANDLE{(SIZE_T)(sh.cpu + index * sh.inc)});
                            sh.init[index] = 1;
                            g_samp_fixed++;
                            state = 4;
                        }
                        break;
                    }
                }
            }
            if (all || state == 0 || state == 3 || state == 4) {
                static const char* st[] = {"in no table range", "no table bound", "its table is in no sampler heap the layer knows", "written", "never written", "never written (given a sampler)"};
                char t[160];
                snprintf(t, sizeof t, " | s%u space%u: %s (heap slot %llu)", q.lo, q.space, st[state + 1], (unsigned long long)index);
                report += t;
                if (state == 0 || state == 3 || state == 4) bad++;
            }
            continue;
        }
        if (q.type < 2 || q.type > 9) continue;
        const uint32_t cls = q.type == 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV : q.type <= 5 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        const char reg = cls == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? 'b' : cls == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? 't' : 'u';
        const TcRange* in = nullptr;
        for (auto& r : ranges)
            if (r.type == cls && r.space == q.space && q.lo >= r.base && q.lo - r.base < r.num) { in = &r; break; }
        if (!in && all) { char t[96]; snprintf(t, sizeof t, " | %c%u space%u: in no table range", reg, q.lo, q.space); report += t; }
        if (!in) continue;   // (a root descriptor, the global-memory tables, or a register the signature does not name)
        auto pa = b.params.find(in->param);
        char line[256];
        if (pa == b.params.end() || pa->second.first != 0) {
            snprintf(line, sizeof line, " | %c%u space%u: no table bound at parameter %u", reg, q.lo, q.space, in->param);
            report += line;
            bad++;
            continue;
        }
        uint32_t kind = 0, slot = 0;
        bool split = false;
        const uint32_t at = in->offset + (q.lo - in->base);
        if (!g_tc_read(pa->second.second, at, &kind, &slot, &split)) continue;
        checked++;
        if (auto st = splitTables.find(in->param); split || st != splitTables.end()) {
            uint32_t& need = splitTables[in->param];
            need = std::max(need, at + 1);
        }
        const uint32_t want = cls == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? 1u : (cls == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? 0x10u : 0x20u) | dimOf[q.kind <= 16 ? q.kind : 0];
        if (kind >= 0x100) { if ((kind & 0xff) == want) continue; kind = 0xff; }   // (already put right by the layer)
        const bool buffer = (want & 15) == 1 && want != 1;
        const uint32_t form = q.type == 4 || q.type == 7 ? 1u : q.type == 5 || q.type >= 8 ? 2u : 0u;
        int fixed = 0;
        // XSL_TABLE_DUMP=<folder> (with XSL_TABLE_CHECK=<pipeline>): that pipeline's constants at each
        // dispatch, as b<register>_s<space>.bin (the last dispatch's remain).
        if (named && g_tc_dump && cls == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
            static char dir[200] = "";
            static const bool on = GetEnvironmentVariableA("XSL_TABLE_DUMP", dir, sizeof dir) != 0;
            char path[260];
            snprintf(path, sizeof path, "%s\\b%u_s%u.bin", dir, q.lo, q.space);
            if (on) g_tc_dump(pa->second.second, at, path);
        }
        if (table_fix_on() && g_tc_fix && cls == D3D12_DESCRIPTOR_RANGE_TYPE_CBV && (kind == 1 || kind == 2)) {   // (a copy of constants at console alignment, made anew)
            g_tc_fix(pa->second.second, at, 1);
            if (!all) continue;
        }
        if (table_fix_on() && g_tc_fix && q.kind && q.kind <= 16 && (kind == 0 || ((kind == 1 || kind == 2) && buffer && cls == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)))
            fixed = g_tc_fix(pa->second.second, at, want | form << 8);
        if (fixed > 0 && !logIt) continue;
        const char* what = nullptr;
        char held[48] = "";
        if (kind == 0) what = fixed > 0 ? "empty (given a view of no resource)" : "empty";
        else if ((kind == 1 || kind == 2) && buffer && cls == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) what = fixed > 0 ? "a constant buffer view (given a buffer view)" : "a constant buffer view";
        else if (kind == 0xfe) what = "a tag whose record is gone";
        else if (kind == 0xff || kind == 2) what = nullptr;   // (unknown kind, or console bytes: a constant or buffer view)
        else if ((kind & 0x7f) != want && q.kind && q.kind <= 16) { snprintf(held, sizeof held, "a view of kind %02x%s", kind & 0x7f, kind & 0x80 ? " (null)" : ""); what = held; }
        if (kind == 2 && cls != D3D12_DESCRIPTOR_RANGE_TYPE_CBV && dimOf[q.kind <= 16 ? q.kind : 0] != 1) what = "console bytes (a constant or buffer view)";
        if (!what && !split && all) {
            snprintf(line, sizeof line, " | %c%u space%u (%s, wants %02x) at %u + %u, slot %u: kind %02x", reg, q.lo, q.space, kindName[q.kind <= 16 ? q.kind : 0], want, in->param, at, slot, kind);
            report += line;
            bad++;
        }
        if (!what && !split) continue;
        snprintf(line, sizeof line, " | %c%u space%u (%s, wants %02x) at parameter %u + %u, title slot %u: %s%s", reg, q.lo, q.space, kindName[q.kind <= 16 ? q.kind : 0], want, in->param, at,
                 slot, what ? what : "", split ? (what ? ", and its page is not behind the table's" : "its page is not behind the table's") : "");
        if (report.size() < (all ? 12000u : 3000u)) report += line;
        bad++;
    }
    // A table that continues in a page which is not behind its first in the PC heap: bound from a
    // copy made for this draw (every slot the shader reads, see table_fresh in xs_descriptors.cpp).
    if (table_fix_on() && g_tc_fresh && !splitTables.empty()) {
        for (auto& q : res) {   // (the last slot read, whichever resource was seen split first)
            if (q.type < 2 || q.type > 9) continue;
            const uint32_t cls = q.type == 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV : q.type <= 5 ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            for (auto& r : ranges)
                if (r.type == cls && r.space == q.space && q.lo >= r.base && q.lo - r.base < r.num) {
                    auto st = splitTables.find(r.param);
                    if (st != splitTables.end()) st->second = std::max(st->second, r.offset + (q.lo - r.base) + 1);
                    break;
                }
        }
        auto* L2 = as<ID3D12GraphicsCommandList>(l);
        for (auto& [param, count] : splitTables) {
            auto pa = b.params.find(param);
            if (pa == b.params.end() || pa->second.first != 0) continue;
            const uint64_t fresh = g_tc_fresh(pa->second.second, count);
            if (!fresh) continue;
            if (compute) L2->SetComputeRootDescriptorTable(param, D3D12_GPU_DESCRIPTOR_HANDLE{fresh});
            else L2->SetGraphicsRootDescriptorTable(param, D3D12_GPU_DESCRIPTOR_HANDLE{fresh});
        }
    }
    if (!bad || !logIt) return;
    {
        std::lock_guard<std::mutex> g(g_tc_mu);
        if (g_tc_done[pso]++ >= 2) return;
    }
    wchar_t name[128] = L"?";
    UINT sz = sizeof name;
    as<ID3D12PipelineState>(pso)->GetPrivateData(WKPDID_D3DDebugObjectNameW, &sz, name);
    for (size_t at = 0; at < report.size() || !at; at += 1700)   // (a log line holds 2,048 characters)
        XSL_WARN(CH, "table check: %ls (present %llu): %d of %d declared resources are not what the shader reads:%s", name, (unsigned long long)g_wr_present.load(), bad, checked + 0,
                 report.substr(at, 1700).c_str());
}
void list_dispatch(PcObj l, uint32_t x, uint32_t y, uint32_t z) {
    root_ensure(l, true);
    table_check(l);
    note_big(l, "Dispatch", (uint64_t)x * y * z * 64, x, y, z);
    trace_dispatch_size(l, "Dispatch", x, y, z, nullptr);
    note_dispatch(l);
    note_reads(l, true);
    int32_t pq = prof_begin(l, 'D', x, y, z);
    L->Dispatch(x, y, z);
    prof_end(l, pq);
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
    root_ensure(l, false);
    if (!g_caps.meshShaders) return;
    {
        static volatile long n = 0;
        static volatile long biggest = 0;
        const uint64_t total = (uint64_t)x * y * z;
        long big = biggest;
        if (InterlockedIncrement(&n) <= 12 || (total > (uint64_t)big && InterlockedCompareExchange(&biggest, (long)std::min<uint64_t>(total, 0x7fffffff), big) == big))
            XSL_INFO(CH, "DispatchMesh %u x %u x %u", x, y, z);
        // More than 2^22 groups, or 2^16 in one dimension, is not a valid dispatch (PC limit); it
        // comes from counts a missing pass did not produce.
        if (x > 65535 || y > 65535 || z > 65535 || total > (1u << 22)) return;
    }
    ID3D12GraphicsCommandList6* l6 = nullptr;
    table_check(l);
    int32_t pq = prof_begin(l, 'M', x, y, z);
    if (SUCCEEDED(L->QueryInterface(IID_PPV_ARGS(&l6)))) { l6->DispatchMesh(x, y, z); l6->Release(); }
    prof_end(l, pq);
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
// Indirect-argument guard: every ExecuteIndirect that dispatches or draws runs from a copy of its
// arguments in which a dispatch with a group count above the D3D12 limit (65535), or a draw with a
// vertex/index/instance count (or their product) of 2^28 or more, becomes empty. Arguments like that are never valid;
// they come from work that did not run as on the console, and executing them hangs the GPU.
// XSL_INDIRECT_GUARD=0 turns it off.
static void restore_compute_state(PcObj l);
struct ArgGuard { ID3D12RootSignature* rs = nullptr; ID3D12PipelineState* pso = nullptr; bool failed = false; std::mutex mu; } g_guard;
static bool guard_init(ID3D12Device* dev) {
    std::lock_guard<std::mutex> g(g_guard.mu);
    if (g_guard.pso) return true;
    if (g_guard.failed) return false;
    g_guard.failed = true;
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    typedef HRESULT(WINAPI * Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = m ? (Compile)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) return false;
    // kind 0: dispatch (three group counts), 1: draw (vertex/index count, instance count)
    static const char src[] =
        "RWByteAddressBuffer a : register(u0);\n"
        "cbuffer C : register(b0) { uint off, stride, count, kind; };\n"
        "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) {\n"
        "  if (i >= count) return;\n"
        "  uint o = off + i * stride;\n"
        "  if (kind == 0u) { uint3 g = a.Load3(o); if (any(g > 65535u)) a.Store3(o, uint3(0, 0, 0)); }\n"
        "  else { uint2 c = a.Load2(o); if (any(c >= 0x10000000u) || (c.x != 0u && c.y >= 0x10000000u / c.x)) a.Store2(o, uint2(0, 0)); }\n"
        "}\n";
    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(compile(src, sizeof src - 1, "xsl_arg_guard", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        XSL_ERR(CH, "indirect guard: compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* rsb = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err)) ||
        FAILED(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&g_guard.rs))))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = g_guard.rs;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_guard.pso)))) return false;
    g_guard.failed = false;
    XSL_INFO(CH, "indirect arguments are checked before use (dispatches over 65535 groups, draws of 2^28+ items become empty)");
    xsl::log_arm(true);
    return true;
}
// Scratch ring for rewritten indirect arguments (default heap, COPY_DEST between uses).
struct ArgRing { ID3D12Resource* res = nullptr; uint64_t head = 0; std::mutex mu; } g_arg_ring;
constexpr uint64_t kArgRing = 16ull << 20;
static bool fixed_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t ao, PcObj cnt, uint64_t co, uint32_t stride, bool yz1,
                           const GuardSig* gs) {
    if (!max || !stride || max > 4096 || !g_main_device) return false;
    if (gs && !guard_init(g_main_device)) gs = nullptr;
    if (!yz1 && !gs) return false;
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
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;   // the argument guard rewrites in place
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
    uint64_t base = g_arg_ring.res->GetGPUVirtualAddress() + off;
    if (yz1) {
        std::vector<uint64_t> wa;
        std::vector<uint32_t> wv;
        for (uint32_t i = 0; i < max; i++) {
            wa.push_back(base + (uint64_t)i * stride + 4); wv.push_back(1);
            wa.push_back(base + (uint64_t)i * stride + 8); wv.push_back(1);
        }
        list_write_immediate(l, (uint32_t)wa.size(), wa.data(), wv.data());
    }
    if (gs) {
        const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier(g_arg_ring.res, D3D12_RESOURCE_STATE_COPY_DEST, uav);
        L->SetPipelineState(g_guard.pso);
        L->SetComputeRootSignature(g_guard.rs);
        L->SetComputeRootUnorderedAccessView(0, g_arg_ring.res->GetGPUVirtualAddress());
        uint32_t c[4] = {(uint32_t)off + gs->argOff, stride, max, gs->kind};
        L->SetComputeRoot32BitConstants(1, 4, c, 0);
        L->Dispatch((max + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = g_arg_ring.res;
        L->ResourceBarrier(1, &ub);
        barrier(g_arg_ring.res, uav, ind);
        restore_compute_state(l);
        // With root signatures of the pipelines' own, restoring only marks the title's as lost: it
        // has to go back on the list here, before the dispatch this function makes. (It did not:
        // every guarded indirect dispatch ran the title's shader with the guard's root signature
        // and arguments, and the first one with work to do hung the GPU.)
        root_ensure(l, true);
    } else barrier(g_arg_ring.res, D3D12_RESOURCE_STATE_COPY_DEST, ind);
    table_check(l);
    int32_t pq = prof_begin(l, 'I', max, 0, 0);
    L->ExecuteIndirect(as<ID3D12CommandSignature>(sig), max, g_arg_ring.res, off, as<ID3D12Resource>(cnt), co);
    prof_end(l, pq);
    barrier(g_arg_ring.res, ind, D3D12_RESOURCE_STATE_COPY_DEST);
    return true;
}
void list_execute_indirect(PcObj l, PcObj sig, uint32_t max, PcObj args, uint64_t ao, PcObj cnt, uint64_t co) {
    root_ensure(l, false);
    root_ensure(l, true);
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
            if (InterlockedIncrement(&n) <= 2) XSL_WARN(CH, "indirect ray dispatch skipped (no translated pipeline ready)");
            note_rays_skipped(4);
            return;
        }
    }
    uint32_t fix_stride = 0;
    {
        std::lock_guard<std::mutex> g(g_rays_mu);
        auto it = g_fix_sigs.find(sig);
        if (it != g_fix_sigs.end()) fix_stride = it->second;
    }
    GuardSig gs{};
    bool guarded = false;
    {
        std::lock_guard<std::mutex> g(g_rays_mu);
        auto it = g_guard_sigs.find(sig);
        if (it != g_guard_sigs.end()) { gs = it->second; guarded = true; }
    }
    uint32_t stride = fix_stride ? fix_stride : guarded ? gs.stride : 0;
    if (stride && fixed_indirect(l, sig, max, args, ao, cnt, co, stride, fix_stride != 0, guarded ? &gs : nullptr)) return;
    if (g_info) check_rs(l, "ExecuteIndirect");
    table_check(l);
    int32_t pq = prof_begin(l, 'I', max, 0, 0);
    L->ExecuteIndirect(as<ID3D12CommandSignature>(sig), max, as<ID3D12Resource>(args), ao, as<ID3D12Resource>(cnt), co);
    prof_end(l, pq);
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
    {   // table bindings refer to the previous heaps: not restored after this
        std::lock_guard<std::mutex> g(g_cs_mu);
        auto it = g_cs_shadow.find(l);
        if (it != g_cs_shadow.end())
            for (auto p = it->second.params.begin(); p != it->second.params.end();) p = p->second.first == 0 ? it->second.params.erase(p) : std::next(p);
    }
    if (gm_enabled()) {
        bool global = false;
        for (uint32_t i = 0; i < n; i++) global |= as<ID3D12DescriptorHeap>(h[i]) == g_gv;
        std::pair<PcObj, PcObj> cur{};
        {
            std::lock_guard<std::mutex> g(g_gm_mu);
            PcObj samp = nullptr;
            for (uint32_t i = 0; i < n; i++)
                if (h[i] && as<ID3D12DescriptorHeap>(h[i])->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) samp = h[i];
            g_list_sampler_heap[l] = samp;
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
    if (!root_arg(l, false, i, 0, gpu(h).ptr)) return;
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
    shadow_param(l, i, 0, h);
    if (!root_arg(l, true, i, 0, gpu(h).ptr)) return;
    L->SetComputeRootDescriptorTable(i, gpu(h));
}
void list_gfx_root_cbv(PcObj l, uint32_t i, uint64_t va) { if (root_arg(l, false, i, 1, va)) L->SetGraphicsRootConstantBufferView(i, va); }
void list_compute_root_cbv(PcObj l, uint32_t i, uint64_t va) { shadow_param(l, i, 1, va); if (root_arg(l, true, i, 1, va)) L->SetComputeRootConstantBufferView(i, va); }
void list_gfx_root_srv(PcObj l, uint32_t i, uint64_t va) { if (root_arg(l, false, i, 2, va)) L->SetGraphicsRootShaderResourceView(i, va); }
void list_compute_root_srv(PcObj l, uint32_t i, uint64_t va) { shadow_param(l, i, 2, va); if (root_arg(l, true, i, 2, va)) L->SetComputeRootShaderResourceView(i, va); }
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
void list_gfx_root_uav(PcObj l, uint32_t i, uint64_t va) { trace_root_uav(l, i, va, "graphics"); if (root_arg(l, false, i, 3, va)) L->SetGraphicsRootUnorderedAccessView(i, va); }
void list_compute_root_uav(PcObj l, uint32_t i, uint64_t va) { trace_root_uav(l, i, va, "compute"); shadow_param(l, i, 3, va); if (root_arg(l, true, i, 3, va)) L->SetComputeRootUnorderedAccessView(i, va); }
void list_gfx_root_constants(PcObj l, uint32_t i, uint32_t n, const void* d, uint32_t o) { if (root_consts(l, false, i, n, d, o)) L->SetGraphicsRoot32BitConstants(i, n, d, o); }
void list_compute_root_constants(PcObj l, uint32_t i, uint32_t n, const void* d, uint32_t o) {
    {
        std::lock_guard<std::mutex> g(g_cs_mu);
        auto& c = g_cs_shadow[l].consts[i];
        if (c.size() < o + n) c.resize(o + n);
        memcpy(c.data() + o, d, n * 4);
    }
    if (root_consts(l, true, i, n, d, o)) L->SetComputeRoot32BitConstants(i, n, d, o);
}
static void restore_compute_state(PcObj l) {
    CsShadow sh;
    {
        std::lock_guard<std::mutex> g(g_cs_mu);
        auto it = g_cs_shadow.find(l);
        if (it == g_cs_shadow.end()) return;
        sh = it->second;
    }
    if (sh.pso) L->SetPipelineState(as<ID3D12PipelineState>(sh.pso));
    root_lost(l, true);
    if (!sh.rs || g_own_rs_any) return;   // (with pipelines' own root signatures: bound again by root_ensure)
    L->SetComputeRootSignature(as<ID3D12RootSignature>(sh.rs));
    gm_bind(l, sh.rs, true);
    for (auto& [i, p] : sh.params) {
        switch (p.first) {
        case 0: L->SetComputeRootDescriptorTable(i, gpu(p.second)); break;
        case 1: L->SetComputeRootConstantBufferView(i, p.second); break;
        case 2: L->SetComputeRootShaderResourceView(i, p.second); break;
        default: L->SetComputeRootUnorderedAccessView(i, p.second); break;
        }
    }
    for (auto& [i, c] : sh.consts) L->SetComputeRoot32BitConstants(i, (UINT)c.size(), c.data(), 0);
}
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
UINT format_bytes_of(DXGI_FORMAT f) { return format_size(f).bytes; }
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
    if (FAILED(buf->Map(0, &none, &mapped))) { removed_after("a stage Map probe", (uint64_t)buf); return false; }  // not CPU-visible
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
    removed_after("the title's ExecuteCommandLists");
}
// Last GPU-side wait and signal per queue (hang diagnostics: dump_queue_state).
struct QueueLast { PcObj waitFence = nullptr, sigFence = nullptr; uint64_t waitValue = 0, sigValue = 0, waits = 0, sigs = 0; };
std::mutex g_ql_mu;
std::map<PcObj, QueueLast> g_ql;
void dump_queue_state() {
    std::lock_guard<std::mutex> g(g_ql_mu);
    for (auto& [q, s] : g_ql) {
        uint64_t wd = s.waitFence ? as<ID3D12Fence>(s.waitFence)->GetCompletedValue() : 0;
        uint64_t sd = s.sigFence ? as<ID3D12Fence>(s.sigFence)->GetCompletedValue() : 0;
        XSL_WARN(CH, "  queue %p: last wait fence %p >= %llu (completed %llu%s), last signal fence %p = %llu (completed %llu); %llu waits, %llu signals", q,
                 s.waitFence, (unsigned long long)s.waitValue, (unsigned long long)wd, wd < s.waitValue ? ", BLOCKED" : "", s.sigFence,
                 (unsigned long long)s.sigValue, (unsigned long long)sd, (unsigned long long)s.waits, (unsigned long long)s.sigs);
    }
}
HRESULT_T queue_signal(PcObj q, PcObj f, uint64_t v) {
    { std::lock_guard<std::mutex> g(g_ql_mu); auto& s = g_ql[q]; s.sigFence = f; s.sigValue = v; s.sigs++; }
    if (g_dred) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 400) XSL_INFO(CH, "queue %p signals fence %p = %llu", q, f, (unsigned long long)v);
    }
    return as<ID3D12CommandQueue>(q)->Signal(as<ID3D12Fence>(f), v);
}
HRESULT_T queue_wait(PcObj q, PcObj f, uint64_t v) {
    { std::lock_guard<std::mutex> g(g_ql_mu); auto& s = g_ql[q]; s.waitFence = f; s.waitValue = v; s.waits++; }
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
    HRESULT hr = as<ID3D12Resource>(res)->Map(0, &none, &p);
    removed_after("a buffer Map probe", (uint64_t)res, (uint64_t)hr);
    return SUCCEEDED(hr) ? p : nullptr;
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

// One texel of a render target as displayable BGRA (render target dumps): float colours tone-mapped
// x / (1 + x) and gamma 2.2, one- and two-channel formats as grey or red/green, integers by their
// low byte.
static void texel_bgra(const uint8_t* t, DXGI_FORMAT f, size_t bytes, uint8_t out[4]) {
    auto half = [](uint16_t h) -> float {
        const uint32_t e = (h >> 10) & 31, m = h & 1023;
        const float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? 0.0f : 65504.0f) : std::ldexp((float)(m + 1024), (int)e - 25);
        return (h & 0x8000) ? -v : v;
    };
    auto packedf = [](uint32_t bits, int mant) -> float {
        const uint32_t e = bits >> mant, m = bits & ((1u << mant) - 1);
        if (e == 31) return m ? 0.0f : 65504.0f;
        return e ? std::ldexp(1.0f + m / float(1u << mant), (int)e - 15) : std::ldexp(m / float(1u << mant), -14);
    };
    auto tone = [](float x) -> uint8_t {
        if (!(x > 0)) return 0;
        const float v = std::pow(x / (1.0f + x), 1.0f / 2.2f);
        return (uint8_t)(v >= 1.0f ? 255 : v * 255.0f + 0.5f);
    };
    auto unorm = [](float x) -> uint8_t { return (uint8_t)(x <= 0 ? 0 : x >= 1 ? 255 : x * 255.0f + 0.5f); };
    uint8_t r = 0, g = 0, b = 0;
    uint16_t h4[4] = {};
    uint32_t w0 = 0;
    float f4[4] = {};
    if (bytes >= 4) memcpy(&w0, t, 4);
    switch (f) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: memcpy(h4, t, 8); r = tone(half(h4[0])); g = tone(half(h4[1])); b = tone(half(h4[2])); break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: memcpy(f4, t, 16); r = tone(f4[0]); g = tone(f4[1]); b = tone(f4[2]); break;
        case DXGI_FORMAT_R11G11B10_FLOAT: r = tone(packedf(w0 & 0x7ff, 6)); g = tone(packedf((w0 >> 11) & 0x7ff, 6)); b = tone(packedf(w0 >> 22, 5)); break;
        case DXGI_FORMAT_R16G16B16A16_UNORM: memcpy(h4, t, 8); r = h4[0] >> 8; g = h4[1] >> 8; b = h4[2] >> 8; break;
        case DXGI_FORMAT_R16G16_FLOAT: memcpy(h4, t, 4); r = unorm(half(h4[0]) * 0.5f + 0.5f); g = unorm(half(h4[1]) * 0.5f + 0.5f); break;
        case DXGI_FORMAT_R16G16_UNORM: memcpy(h4, t, 4); r = h4[0] >> 8; g = h4[1] >> 8; break;
        case DXGI_FORMAT_R8G8_UNORM: r = t[0]; g = t[1]; break;
        case DXGI_FORMAT_R32G32_FLOAT: memcpy(f4, t, 8); r = unorm(f4[0] * 0.5f + 0.5f); g = unorm(f4[1] * 0.5f + 0.5f); break;
        case DXGI_FORMAT_R16_FLOAT: memcpy(h4, t, 2); r = g = b = tone(half(h4[0])); break;
        case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: memcpy(f4, t, 4); r = g = b = unorm(f4[0]); break;
        case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: memcpy(h4, t, 2); r = g = b = h4[0] >> 8; break;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: r = g = b = t[0]; break;
        default:   // integers and anything else: the low bytes
            r = t[0]; g = bytes > 1 ? t[1] : t[0]; b = bytes > 2 ? t[2] : t[0];
            break;
    }
    out[0] = b; out[1] = g; out[2] = r; out[3] = 0xFF;
}
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
    removed_after("an internal upload/copy list");
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
        // Other formats than the ones a plane has (render target dumps): converted texel by texel.
        const size_t bpt = w ? fp.Footprint.RowPitch / w : 4;
        const bool bgra = rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        const bool general = !rgba && !bgra && rd.Format != DXGI_FORMAT_R10G10B10A2_UNORM && rd.Format != DXGI_FORMAT_R10G10B10A2_TYPELESS;
        std::vector<uint8_t> line;
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
                if (general) {
                    line.resize((size_t)w * 4);
                    for (UINT x = 0; x < w; x++) texel_bgra(row + (size_t)x * bpt, rd.Format, bpt, &line[(size_t)x * 4]);
                    fwrite(line.data(), 4, w, out);
                    continue;
                }
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
// The render targets copied during the dump frame (XSL_RT_DUMP), written beside the shader cache.
static void rt_dump_write(ID3D12Device* dev, ID3D12CommandQueue* q) {
    std::vector<RtDump> all;
    {
        std::lock_guard<std::mutex> g(g_rtd_mu);
        if (g_rtd.empty() || rt_dump_now()) return;   // (not while the frame is still being recorded)
        all.swap(g_rtd);
    }
    const std::wstring wdir = rt_cache_dir() + L"rtdump";
    CreateDirectoryW(wdir.c_str(), nullptr);
    char dir[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, wdir.c_str(), -1, dir, sizeof dir, nullptr, nullptr);
    for (auto& d : all) {
        char path[MAX_PATH + 64];
        snprintf(path, sizeof path, "%s\\rt_%03u_%u_%llux%u_f%d.bmp", dir, d.pass, d.slot, (unsigned long long)d.desc.Width, d.desc.Height, (int)d.desc.Format);
        capture_plane(dev, q, d.copy, path, D3D12_RESOURCE_STATE_COPY_DEST);
        d.copy->Release();
    }
    XSL_INFO(CH, "render target dump: %zu targets of %u passes written to %s", all.size(), g_rtd_pass, dir);
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
    ID3DBlob *vs = nullptr, *psOver = nullptr;        // for the overlay pipelines
    std::map<int, ID3D12PipelineState*> over;         // swap-chain format -> pipeline that blends a plane over it
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
        "  return float4(saturate(c), 1); }\n"
        "float4 ps_over(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    compile(src, sizeof src - 1, "xsl_blit", nullptr, nullptr, "ps_over", "ps_5_0", 0, 0, &g_blit.psOver, &err);
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
    g_blit.vs = vs;
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
// A second present plane (the console composes it over the first by its alpha: titles draw their
// interface there) blended over the image already in the swap-chain buffer. The plane's colours
// are premultiplied by the blending that drew them into a transparent plane.
void blit_over(ID3D12Device* dev, ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* dst, UINT w, UINT h, DXGI_FORMAT fmt) {
    if (!g_blit.vs || !g_blit.psOver) return;
    ID3D12PipelineState*& pso = g_blit.over[(int)fmt];
    if (!pso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = g_blit.rs;
        pd.VS = {g_blit.vs->GetBufferPointer(), g_blit.vs->GetBufferSize()};
        pd.PS = {g_blit.psOver->GetBufferPointer(), g_blit.psOver->GetBufferSize()};
        auto& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)))) { pso = nullptr; return; }
        XSL_INFO(CH, "presentation: the second plane is blended over the first");
    }
    UINT i = g_blit.slot++ % 8;
    D3D12_CPU_DESCRIPTOR_HANDLE sc = g_blit.srv->GetCPUDescriptorHandleForHeapStart();
    sc.ptr += (SIZE_T)i * g_blit.srvInc;
    D3D12_GPU_DESCRIPTOR_HANDLE sg = g_blit.srv->GetGPUDescriptorHandleForHeapStart();
    sg.ptr += (UINT64)i * g_blit.srvInc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = src->GetDesc().Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(src, &sd, sc);
    D3D12_CPU_DESCRIPTOR_HANDLE rc = g_blit.rtv->GetCPUDescriptorHandleForHeapStart();
    rc.ptr += (SIZE_T)i * g_blit.rtvInc;
    D3D12_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = fmt;
    rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(dst, &rv, rc);
    l->SetGraphicsRootSignature(g_blit.rs);
    l->SetPipelineState(pso);
    ID3D12DescriptorHeap* heaps[] = {g_blit.srv};
    l->SetDescriptorHeaps(1, heaps);
    l->SetGraphicsRootDescriptorTable(0, sg);
    uint32_t consts[2] = {0, 0};
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
HRESULT_T present(PcObj devObj, PcObj queueObj, PcObj resObj, void* hwndPtr, uint32_t sync, uint32_t colorSpace, PcObj overlayObj) {
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
    // XSL_PRESENT_OVERLAY=0 shows the first plane alone.
    static bool overlayOn = [] { char eb[4]; return !(GetEnvironmentVariableA("XSL_PRESENT_OVERLAY", eb, 4) && eb[0] == '0'); }();
    if (auto* ov = as<ID3D12Resource>(overlayObj); ov && ov != res && overlayOn && blit_init(dev)) {
        D3D12_RESOURCE_BARRIER ob[2] = {};
        ob[0].Type = ob[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        ob[0].Transition = {ov, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        ob[1].Transition = {bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET};
        s.list->ResourceBarrier(copyable ? 2 : 1, ob);
        blit_over(dev, s.list, ov, bb, s.w, s.h, want);
        std::swap(ob[0].Transition.StateBefore, ob[0].Transition.StateAfter);
        std::swap(ob[1].Transition.StateBefore, ob[1].Transition.StateAfter);
        s.list->ResourceBarrier(copyable ? 2 : 1, ob);
    }
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    s.list->ResourceBarrier(2, b);
    s.list->Close();
    ID3D12CommandList* lists[] = {s.list};
    q->ExecuteCommandLists(1, lists);
    removed_after("an internal present list");
    {
        // XSL_CAPTURE=<path.bmp>[@start[+count[/step]]]: dump presented frames (count > 1 adds _<frame>;
        // with a step, every step-th frame from start, count frames in all).
        // The swap-chain buffer is captured, i.e. what is displayed after format/HDR conversion.
        static char cap[MAX_PATH] = "";
        static uint64_t capFrame = 0, capCount = 1, capStep = 1;
        static bool init = false;
        if (!init) {
            init = true;
            if (GetEnvironmentVariableA("XSL_CAPTURE", cap, sizeof cap)) {
                char* at = strrchr(cap, '@');
                capFrame = at ? _strtoui64(at + 1, nullptr, 10) : 60;
                if (char* plus = at ? strchr(at, '+') : nullptr) capCount = _strtoui64(plus + 1, nullptr, 10);
                if (char* sl = at ? strchr(at, '/') : nullptr) capStep = std::max<uint64_t>(1, _strtoui64(sl + 1, nullptr, 10));
                if (at) *at = 0;
            }
        }
        ++g_present_count;
        prof_slow_check(g_present_count);
        if (gm_enabled() && g_present_count % 600 == 0)
            XSL_INFO(CH, "global memory table: bound %llu, root signature without it %llu, other heap %llu, no heap yet %llu",
                     (unsigned long long)g_gm_bind_stats[0].load(), (unsigned long long)g_gm_bind_stats[1].load(), (unsigned long long)g_gm_bind_stats[2].load(),
                     (unsigned long long)g_gm_bind_stats[3].load());
        probe_present(g_present_count);
        prof_report(as<ID3D12CommandQueue>(q));
        ++g_gm_frame;
        if (getenv("XSL_AS_DUMP") || getenv("XSL_TLAS_DUMP")) print_dumps();
        {   // debug layer: what it reported since the last dump (every 30 presents, and once a second when presents are slow)
            static ULONGLONG last = 0;
            const ULONGLONG now = GetTickCount64();
            if (g_info && (g_present_count % 30 == 0 || now - last >= 1000)) { last = now; dump_debug_messages(); }
        }
        if (trace_writers()) { std::lock_guard<std::mutex> g(g_wr_mu); g_planes.insert((PcObj)res); }
        if (trace_writers() && plane_ops_now()) { XSL_INFO(CH, "plane op: present %llu of %p on queue %p", (unsigned long long)g_present_count, (void*)res, (void*)q); report_aliases((PcObj)res); }
        g_wr_present = g_present_count;
        g_present_tid = GetCurrentThreadId();
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
        if (cap[0] && g_present_count >= capFrame && g_present_count < capFrame + capCount * capStep && (g_present_count - capFrame) % capStep == 0) {
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
        rt_dump_write(dev, q);
    }
    bb->Release();
    hr = s.sc->Present(sync, 0);
    removed_after("Present");
    g_stage_frames++;
    update_fps_title(hwnd);
    q->Signal(s.fence, ++s.fenceValue);
    s.slotValue[s.slot] = s.fenceValue;
    s.slot = (s.slot + 1) % 3;
    if (FAILED(hr)) XSL_ERR(CH, "Present failed %08lx", hr);
    return hr;
}

}  // namespace pc
