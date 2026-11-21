// SPDX-License-Identifier: AGPL-3.0-or-later
//
// xmem.dll replacement: console memory APIs on Win32.
//
// * XMemVirtualAlloc: console allocation types and protections (MEM_64K_PAGES, PAGE_GRAPHICS_*,
//   XMEM_* flags) reduced to their PC meaning. A reservation without MEM_COMMIT becomes a
//   placeholder region so physical pages can be mapped into it later.
// * Physical pages: 64 KB "pages" are slices of pagefile-backed sections; XMemMapPhysicalPages
//   splits the placeholder and maps the slice with MapViewOfFile3, so the same page can be
//   mapped at several addresses (the console's aliasing). This is the technique SlimEra uses
//   for the previous console generation written independently here.
// * XMemAlloc: honours a title's exported __XMemAllocationHooks__ (XMEM_SET_ALLOCATION_HOOKS),
//   otherwise XMemAllocDefault with the alignment and page size the attributes request.
#include <windows.h>
#include <psapi.h>

#include <xmem.h>

#include <cstring>

#include <map>
#include <mutex>
#include <vector>

#include "xsl/log.h"

#define CH "xmem"

namespace {
constexpr SIZE_T kPage = 64 * 1024;
constexpr SIZE_T kPagesPerChunk = 4096;  // 256 MB sections

DWORD pc_protect(DWORD p) {
    p &= ~(DWORD)(PAGE_GRAPHICS_NOACCESS | PAGE_GRAPHICS_READONLY | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_EXECUTE |
                  PAGE_GRAPHICS_EXECUTE_READ | PAGE_GRAPHICS_EXECUTE_READWRITE | PAGE_GRAPHICS_COHERENT | PAGE_GRAPHICS_NOCACHE);
    if (!(p & 0xFF)) p |= PAGE_READWRITE;
    return p;
}
DWORD pc_alloc_type(DWORD t) { return t & (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_RESET_UNDO); }

// ---- Physical pages -------------------------------------------------------------------------
std::mutex g_mu;
std::vector<HANDLE> g_chunks;
std::vector<ULONG_PTR> g_free;  // free page numbers

bool grow() {
    HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, (DWORD)(kPage * kPagesPerChunk), nullptr);
    if (!s) { XSL_ERR(CH, "cannot create a physical-page section (%lu)", GetLastError()); return false; }
    ULONG_PTR base = g_chunks.size() * kPagesPerChunk;
    g_chunks.push_back(s);
    for (ULONG_PTR i = kPagesPerChunk; i-- > 0;) g_free.push_back(base + i);
    return true;
}

// GPU-visible allocations (XMEM_GRAPHICS or PAGE_GRAPHICS_*). On the console a CPU address in such
// memory is also its GPU address; d3d12_xs.dll asks this registry to translate them (unified memory).
std::map<uintptr_t, SIZE_T> g_graphics;
bool is_graphics(ULONGLONG flags, DWORD protect) {
    return (flags & 0x2) != 0 ||
           (protect & (PAGE_GRAPHICS_READONLY | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_EXECUTE | PAGE_GRAPHICS_EXECUTE_READ |
                       PAGE_GRAPHICS_EXECUTE_READWRITE)) != 0;
}

// Placeholder reservations created by XMemVirtualAlloc (start -> size).
std::map<uintptr_t, SIZE_T> g_placeholders;

bool in_placeholder(uintptr_t va) {
    auto it = g_placeholders.upper_bound(va);
    if (it == g_placeholders.begin()) return false;
    --it;
    return va < it->first + it->second;
}

// Make [va, va+kPage) its own placeholder (splitting the enclosing one) and map `page` there.
bool map_one(uintptr_t va, ULONG_PTR page) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)va, &mbi, sizeof mbi);
    if (mbi.Type == MEM_MAPPED) UnmapViewOfFile2(GetCurrentProcess(), (void*)va, MEM_PRESERVE_PLACEHOLDER);
    VirtualQuery((void*)va, &mbi, sizeof mbi);
    if (mbi.RegionSize > kPage || (uintptr_t)mbi.BaseAddress != va) {
        // Split the placeholder so this 64 KB unit stands alone.
        if ((uintptr_t)mbi.BaseAddress < va)
            VirtualFree(mbi.BaseAddress, va - (uintptr_t)mbi.BaseAddress, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        VirtualFree((void*)va, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    }
    if (page == (ULONG_PTR)-1) return true;  // unmap only
    HANDLE s = g_chunks[page / kPagesPerChunk];
    ULONG64 off = (ULONG64)(page % kPagesPerChunk) * kPage;
    void* p = MapViewOfFile3(s, GetCurrentProcess(), (void*)va, off, kPage, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
    if (p != (void*)va) {
        XSL_ERR(CH, "MapViewOfFile3 at %p failed (%lu)", (void*)va, GetLastError());
        return false;
    }
    return true;
}

// ---- Allocation hooks ------------------------------------------------------------------------
XMEM_ALLOCATION_HOOKS* title_hooks() {
    static XMEM_ALLOCATION_HOOKS* h = [] {
        auto* p = (XMEM_ALLOCATION_HOOKS*)GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__");
        if (p) XSL_INFO(CH, "title supplies XMem allocation hooks");
        return p;
    }();
    return h;
}
XMEM_ALLOCATION_HOOKS g_setHooks{};
}  // namespace

extern "C" {

PVOID WINAPI XMemVirtualAlloc(PVOID base, SIZE_T size, DWORD type, ULONGLONG flags, DWORD protect) {
    bool large = (type & (MEM_2MB_PAGES | MEM_PHYSICAL)) != 0;  // MEM_64K_PAGES = MEM_2MB_PAGES | MEM_PHYSICAL
    if ((type & MEM_RESERVE) && !(type & MEM_COMMIT) && large) {
        // Reservation meant for XMemMapPhysicalPages: a placeholder.
        SIZE_T sz = (size + kPage - 1) & ~(kPage - 1);
        void* p = VirtualAlloc2(GetCurrentProcess(), base, sz, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (p) {
            std::lock_guard<std::mutex> g(g_mu);
            g_placeholders[(uintptr_t)p] = sz;
        } else {
            XSL_ERR(CH, "placeholder reservation of %zu bytes failed (%lu)", sz, GetLastError());
        }
        return p;
    }
    // Commit inside a 64 KB-page reservation (the console's reserve-then-commit pattern): the
    // reservation is a placeholder, which VirtualAlloc cannot commit, so fresh zeroed pages are
    // mapped into each 64 KB unit not mapped yet.
    if ((type & MEM_COMMIT) && !(type & MEM_RESERVE) && base) {
        std::lock_guard<std::mutex> g(g_mu);
        if (in_placeholder((uintptr_t)base)) {
            uintptr_t start = (uintptr_t)base & ~(uintptr_t)(kPage - 1);
            uintptr_t end = ((uintptr_t)base + size + kPage - 1) & ~(uintptr_t)(kPage - 1);
            for (uintptr_t va = start; va < end; va += kPage) {
                MEMORY_BASIC_INFORMATION mbi{};
                VirtualQuery((void*)va, &mbi, sizeof mbi);
                if (mbi.Type == MEM_MAPPED) continue;  // already committed
                if (g_free.empty() && !grow()) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
                ULONG_PTR page = g_free.back();
                g_free.pop_back();
                if (!map_one(va, page)) return nullptr;
                memset((void*)va, 0, kPage);  // committed memory reads as zero
            }
            if (is_graphics(flags, protect)) g_graphics[start] = end - start;
            return base;
        }
    }
    // GPU-visible memory is imported into the GPU with 64 KB granularity; keep it 64 KB aligned/sized.
    if (is_graphics(flags, protect) && (type & MEM_COMMIT)) size = (size + kPage - 1) & ~(kPage - 1);
    void* p = VirtualAlloc(base, size, pc_alloc_type(type), pc_protect(protect));
    if (p && is_graphics(flags, protect) && (type & MEM_COMMIT)) {
        std::lock_guard<std::mutex> g(g_mu);
        g_graphics[(uintptr_t)p] = size;
    }
    if (!p) XSL_WARN(CH, "XMemVirtualAlloc(%p, %zu, %lx, %llx, %lx) failed (%lu)", base, size, type, flags, protect, GetLastError());
    return p;
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONGLONG, PULONG_PTR count, PULONG_PTR pages) {
    std::lock_guard<std::mutex> g(g_mu);
    while (g_free.size() < *count)
        if (!grow()) { *count = 0; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (ULONG_PTR i = 0; i < *count; i++) { pages[i] = g_free.back(); g_free.pop_back(); }
    return TRUE;
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR pages) {
    std::lock_guard<std::mutex> g(g_mu);
    for (ULONG_PTR i = 0; i < count; i++) g_free.push_back(pages[i]);
    return TRUE;
}

PVOID WINAPI XMemMapPhysicalPages(PVOID va, ULONG_PTR count, PULONG_PTR pages) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!in_placeholder((uintptr_t)va)) {
        XSL_ERR(CH, "XMemMapPhysicalPages at %p: not inside an XMem placeholder reservation", va);
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    for (ULONG_PTR i = 0; i < count; i++)
        if (!map_one((uintptr_t)va + i * kPage, pages ? pages[i] : (ULONG_PTR)-1)) return nullptr;
    return va;
}

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* vas, ULONG_PTR count, PULONG_PTR pages) {
    std::lock_guard<std::mutex> g(g_mu);
    for (ULONG_PTR i = 0; i < count; i++)
        if (!map_one((uintptr_t)vas[i], pages ? pages[i] : (ULONG_PTR)-1)) return FALSE;
    return TRUE;
}

BOOL WINAPI XMemGetPhysicalMemoryLayout(PXMEM_PHYSICAL_MEMORY_LAYOUT layout) {
    layout->LowestPageNumber = 0;
    layout->LowestPageNumberGpuOptimalBandwidth = 0;
    return TRUE;
}

BOOL WINAPI XMemVirtualQuery(HANDLE process, PVOID address, PXMEM_BASIC_INFORMATION out) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQueryEx(process, address, &m, sizeof m)) return FALSE;
    *out = {};
    out->BaseAddress = m.BaseAddress;
    out->AllocationBase = m.AllocationBase;
    out->AllocationProtect = m.AllocationProtect;
    out->RegionSize = m.RegionSize;
    out->State = m.State;
    out->Protect = m.Protect;
    out->Type = m.Type;
    out->XMemFlags = XMEM_CPU;
    return TRUE;
}

PVOID WINAPI XMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    XALLOC_ATTRIBUTES a{attributes};
    SIZE_T align = a.s.Alignment ? (SIZE_T)1 << a.s.Alignment : 16;
    if (a.s.PageSize != XALLOC_PAGESIZE_4KB || align >= kPage) {
        SIZE_T gran = a.s.PageSize == XALLOC_PAGESIZE_2MB ? 2 * 1024 * 1024 : kPage;
        if (align > gran) gran = align;
        // VirtualAlloc returns 64 KB-aligned memory; larger alignment needs an over-reservation.
        if (gran <= kPage) return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        MEM_ADDRESS_REQUIREMENTS req{};
        req.Alignment = gran;
        MEM_EXTENDED_PARAMETER ep{};
        ep.Type = MemExtendedParameterAddressRequirements;
        ep.Pointer = &req;
        return VirtualAlloc2(GetCurrentProcess(), nullptr, (size + gran - 1) & ~(gran - 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, &ep, 1);
    }
    return _aligned_malloc(size ? size : 1, align < 16 ? 16 : align);
}

VOID WINAPI XMemFreeDefault(PVOID p, ULONGLONG attributes) {
    if (!p) return;
    XALLOC_ATTRIBUTES a{attributes};
    SIZE_T align = a.s.Alignment ? (SIZE_T)1 << a.s.Alignment : 16;
    if (a.s.PageSize != XALLOC_PAGESIZE_4KB || align >= kPage) VirtualFree(p, 0, MEM_RELEASE);
    else _aligned_free(p);
}

PVOID WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes) {
    if (auto* h = title_hooks(); h && h->AllocRoutine) return h->AllocRoutine(size, attributes);
    if (g_setHooks.AllocRoutine) return g_setHooks.AllocRoutine(size, attributes);
    return XMemAllocDefault(size, attributes);
}

VOID WINAPI XMemFree(PVOID p, ULONGLONG attributes) {
    if (auto* h = title_hooks(); h && h->FreeRoutine) return h->FreeRoutine(p, attributes);
    if (g_setHooks.FreeRoutine) return g_setHooks.FreeRoutine(p, attributes);
    XMemFreeDefault(p, attributes);
}

VOID WINAPI XMemSetAllocationHooks(PXMEMALLOC_ROUTINE a, PXMEMFREE_ROUTINE f) { g_setHooks = {a, f}; }

HANDLE WINAPI XMemGetTitleHeap() { return GetProcessHeap(); }
HANDLE WINAPI XMemCreateWin32Heap(ULONGLONG, PVOID) { return HeapCreate(0, 0, 0); }

UINT WINAPI xsl_GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID* addrs, ULONG_PTR* count, LPDWORD gran) {
    return GetWriteWatch(flags, base, size, addrs, count, gran);
}
UINT WINAPI xsl_ResetWriteWatch(LPVOID base, SIZE_T size) { return ResetWriteWatch(base, size); }

// Statistics and tuning: report empty statistics and accept every option.
BOOL WINAPI xsl_xmem_true() { return TRUE; }
UINT32 WINAPI xsl_xmem_zero() { return 0; }
}  // extern "C"

extern "C" {
ULONGLONG WINAPI xsl_XMemMakeTag(UINT32 tag) { return tag; }
VOID xsl_XMemSetOption(XMEM_OPTION) {}
// Title memory as a console reports it: Series X titles get 13.5 GB (here at most 85% of the
// host's RAM), GPU-optimal memory is 10 GB; usage is the process's private commit.
BOOL WINAPI XMemGetWorkingSetStatistics(DWORD, PXMEM_WORKING_SET_STATISTICS st) {
    if (!st) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(st, 0, sizeof *st);
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    SIZE_T limit = (SIZE_T)13824 << 20;
    if (ms.ullTotalPhys && ms.ullTotalPhys / 100 * 85 < limit) limit = (SIZE_T)(ms.ullTotalPhys / 100 * 85);
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&pmc, sizeof pmc);
    st->GameLimit = limit;
    st->GameUsed = pmc.PrivateUsage < limit ? pmc.PrivateUsage : limit;
    st->GpuOptimalBandwidthLimit = (SIZE_T)10 << 30;
    st->GameAllocatedPhysicalPageCount = (UINT32)(st->GameUsed / kPage);
    return TRUE;
}

BOOL WINAPI xsl_xmem_unsupported() { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
SIZE_T WINAPI xsl_xmem_size_zero() { return 0; }
}

extern "C" {
// Layer-private exports: a temporary view of one physical page (DirectStorage page-array requests).
void* WINAPI xsl_xmem_page_view(ULONG_PTR page) {
    std::lock_guard<std::mutex> g(g_mu);
    if (page / kPagesPerChunk >= g_chunks.size()) return nullptr;
    ULONG64 off = (ULONG64)(page % kPagesPerChunk) * kPage;
    return MapViewOfFile(g_chunks[page / kPagesPerChunk], FILE_MAP_READ | FILE_MAP_WRITE, (DWORD)(off >> 32), (DWORD)off, kPage);
}
void WINAPI xsl_xmem_page_unview(void* view) { if (view) UnmapViewOfFile(view); }

// Layer-private export: the GPU-visible allocation containing `address`, if any.
BOOL WINAPI xsl_xmem_find_graphics(const void* address, void** base, SIZE_T* size) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_graphics.upper_bound((uintptr_t)address);
    if (it == g_graphics.begin()) return FALSE;
    --it;
    if ((uintptr_t)address >= it->first + it->second) return FALSE;
    *base = (void*)it->first;
    *size = it->second;
    return TRUE;
}
}
