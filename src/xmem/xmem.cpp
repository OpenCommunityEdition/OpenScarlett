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
#include <set>
#include <mutex>
#include <vector>

#include "xsl/log.h"

#define CH "xmem"

namespace {
constexpr SIZE_T kPage = 64 * 1024;
constexpr SIZE_T kPagesPerChunk = 4096;  // 256 MB sections
// Page numbers given to the title are 4 KB frame numbers, as on the console: a 64 KB page's number
// is a multiple of 16. Titles rely on it (one keeps its page table with the low four bits dropped;
// with consecutive numbers, sixteen addresses shared one page).
constexpr ULONG_PTR kFramesPerPage = 16;

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
// GPU-visible ranges of placeholders the title turned into physical-page reservations
// (MEM_REPLACE_PLACEHOLDER): backed at once by one private, write-watched allocation each, because
// the GPU can import such memory (OpenExistingHeapFromAddress) but not a run of mapped section
// views. Pages the title maps there later are taken as already in place; a page mapped at a second
// address does not alias (logged).
std::map<uintptr_t, SIZE_T> g_private_graphics;
std::map<ULONG_PTR, uintptr_t> g_page_home;   // physical page -> where it was first mapped (aliasing check)
bool in_private_graphics(uintptr_t va) {
    auto it = g_private_graphics.upper_bound(va);
    if (it == g_private_graphics.begin()) return false;
    --it;
    return va < it->first + it->second;
}
// Note that `page` is mapped at va inside private graphics memory; warns when it is mapped twice.
void note_private_page(uintptr_t va, ULONG_PTR page) {
    auto [it, fresh] = g_page_home.emplace(page, va);
    if (!fresh && it->second != va) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 5)
            XSL_WARN(CH, "physical page %llu mapped at %p and %p: GPU-visible memory here does not alias", (unsigned long long)page, (void*)it->second, (void*)va);
    }
}

bool in_placeholder(uintptr_t va) {
    auto it = g_placeholders.upper_bound(va);
    if (it == g_placeholders.begin()) return false;
    --it;
    return va < it->first + it->second;
}

// Which physical page is mapped where. A page must never be handed out while a view of it exists:
// the new owner's writes would show through the old address (a title's shader blobs were
// overwritten by file reads into another buffer). Pages the layer took itself for a commit
// (g_owned) return to the free list when their address is decommitted; pages a title frees while
// still mapped wait (g_zombie) until the view goes.
// XSL_XMEM_TRACE=<n>: logs the first n XMem calls of the title, and every page mapped twice.
long g_trace_left = [] { const char* e = getenv("XSL_XMEM_TRACE"); return e ? atol(e) : 0; }();
bool trace_on() { return g_trace_left != 0; }
bool trace_take() { return g_trace_left > 1 && InterlockedDecrement(&g_trace_left) > 0; }
std::map<uintptr_t, ULONG_PTR> g_va_page;
std::map<ULONG_PTR, int> g_page_views;
std::set<ULONG_PTR> g_owned, g_zombie;
void release_va(uintptr_t va) {
    auto it = g_va_page.find(va);
    if (it == g_va_page.end()) return;
    ULONG_PTR page = it->second;
    g_va_page.erase(it);
    if (--g_page_views[page] > 0) return;
    g_page_views.erase(page);
    if (g_owned.erase(page) || g_zombie.erase(page)) g_free.push_back(page);
}
// Make [va, va+kPage) its own placeholder (splitting the enclosing one) and map `page` there.
bool map_one(uintptr_t va, ULONG_PTR page) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((void*)va, &mbi, sizeof mbi);
    if (mbi.Type == MEM_MAPPED) { UnmapViewOfFile2(GetCurrentProcess(), (void*)va, MEM_PRESERVE_PLACEHOLDER); release_va(va); }
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
    g_va_page[va] = page;
    if (g_page_views[page]++ > 0 && trace_on()) {
        uintptr_t other = 0;
        for (auto& [a, pg] : g_va_page) if (pg == page && a != va) { other = a; break; }
        XSL_INFO(CH, "trace: page %llu now mapped at %p and %p (owned %d, zombie %d)", (unsigned long long)page, (void*)other, (void*)va,
                 (int)g_owned.count(page), (int)g_zombie.count(page));
    }
    return true;
}

// XMEM_GRAPHICS_32BIT_ADDRESS asks for GPU memory whose address fits in 32 bits. Such memory is
// placed between 2 GB (the lowest graphics address D3D12ConfigureVirtualMemoryX reports) and
// 0xF0000000, where d3d12_xs.dll keeps a range of its own.
constexpr ULONGLONG kXmem32 = 0x7;
void* reserve_va(void* base, SIZE_T size, ULONG type, ULONG protect, ULONGLONG flags) {
    MEM_ADDRESS_REQUIREMENTS req{};
    MEM_EXTENDED_PARAMETER ep{};
    ULONG n = 0;
    if (!base && (flags & kXmem32) == kXmem32) {
        req.LowestStartingAddress = (void*)0x80000000ull;
        req.HighestEndingAddress = (void*)0xEFFFFFFFull;
        ep.Type = MemExtendedParameterAddressRequirements;
        ep.Pointer = &req;
        n = 1;
    }
    void* p = VirtualAlloc2(GetCurrentProcess(), base, size, type, protect, n ? &ep : nullptr, n);
    if (!p && n) {
        XSL_WARN(CH, "no room below 4 GB for %zu bytes of 32-bit graphics memory (%lu); placing it anywhere", size, GetLastError());
        p = VirtualAlloc2(GetCurrentProcess(), base, size, type, protect, nullptr, 0);
    }
    return p;
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

static PVOID WINAPI XMemVirtualAlloc_(PVOID base, SIZE_T size, DWORD type, ULONGLONG flags, DWORD protect);
PVOID WINAPI XMemVirtualAlloc(PVOID base, SIZE_T size, DWORD type, ULONGLONG flags, DWORD protect) {
    PVOID r = XMemVirtualAlloc_(base, size, type, flags, protect);
    if (trace_take()) XSL_INFO(CH, "trace: XMemVirtualAlloc(%p, %zx, type %lx, flags %llx, protect %lx) = %p", base, size, type, flags, protect, r);
    return r;
}
static PVOID WINAPI XMemVirtualAlloc_(PVOID base, SIZE_T size, DWORD type, ULONGLONG flags, DWORD protect) {
    bool large = (type & (MEM_2MB_PAGES | MEM_PHYSICAL)) != 0;  // MEM_64K_PAGES = MEM_2MB_PAGES | MEM_PHYSICAL
    // MEM_REPLACE_PLACEHOLDER (with MEM_RESERVE, without a commit): the console turns (part of) a
    // placeholder into a reservation for physical pages. Here the placeholder already is one; XMemMapPhysicalPages maps
    // into it. The range is recorded as GPU-visible when the title says so.
    if ((type & MEM_REPLACE_PLACEHOLDER) && !(type & MEM_COMMIT) && base) {
        std::lock_guard<std::mutex> g(g_mu);
        if (in_placeholder((uintptr_t)base)) {
            SIZE_T sz = (size + kPage - 1) & ~(kPage - 1);
            if (in_private_graphics((uintptr_t)base)) return base;   // backed with the rest of its reservation
            if (is_graphics(flags, protect)) {
                // The whole reservation becomes one private allocation (titles carve one reservation
                // into several ranges and place one buffer across them; a PC buffer cannot span
                // allocations). Its placeholders are coalesced first; nothing is mapped there yet.
                auto it = std::prev(g_placeholders.upper_bound((uintptr_t)base));
                uintptr_t ps = it->first;
                SIZE_T psz = it->second;
                bool whole = VirtualFree((void*)ps, psz, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
                if (!whole) {   // already one placeholder, or parts in use: try it as it is, else just this range
                    MEMORY_BASIC_INFORMATION mbi{};
                    VirtualQuery((void*)ps, &mbi, sizeof mbi);
                    whole = mbi.State == MEM_RESERVE && mbi.RegionSize >= psz;
                }
                uintptr_t start = whole ? ps : (uintptr_t)base;
                SIZE_T len = whole ? psz : sz;
                if (!whole) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    VirtualQuery(base, &mbi, sizeof mbi);
                    if ((uintptr_t)mbi.BaseAddress < (uintptr_t)base)
                        VirtualFree(mbi.BaseAddress, (uintptr_t)base - (uintptr_t)mbi.BaseAddress, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
                    VirtualQuery(base, &mbi, sizeof mbi);
                    if (mbi.RegionSize > sz) VirtualFree(base, sz, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
                }
                void* p = VirtualAlloc2(GetCurrentProcess(), (void*)start, len, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH, PAGE_READWRITE, nullptr, 0);
                if (p == (void*)start) {
                    g_private_graphics[start] = len;
                    g_graphics[start] = len;
                    XSL_INFO(CH, "GPU-visible physical-page memory %p+%zu: one private allocation", (void*)start, (size_t)len);
                } else {
                    XSL_WARN(CH, "GPU-visible physical-page range %p+%zu kept as mapped pages (%lu): the GPU cannot import it", base, sz, GetLastError());
                    g_graphics[(uintptr_t)base] = sz;
                }
            }
            return base;
        }
    }
    if ((type & MEM_RESERVE) && !(type & MEM_COMMIT) && (large || (type & MEM_RESERVE_PLACEHOLDER))) {
        // Reservation meant for XMemMapPhysicalPages, or a placeholder the title splits itself
        // (VirtualFree with MEM_PRESERVE_PLACEHOLDER): a placeholder either way.
        SIZE_T sz = (size + kPage - 1) & ~(kPage - 1);
        void* p = reserve_va(base, sz, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, flags);
        if (p) {
            std::lock_guard<std::mutex> g(g_mu);
            g_placeholders[(uintptr_t)p] = sz;
        } else {
            XSL_ERR(CH, "placeholder reservation of %zu bytes at %p failed (%lu; type %lx, flags %llx, protect %lx)", sz, base, GetLastError(), type, flags, protect);
        }
        return p;
    }
    // Commit inside a 64 KB-page reservation (the console's reserve-then-commit pattern): the
    // reservation is a placeholder, which VirtualAlloc cannot commit, so fresh zeroed pages are
    // mapped into each 64 KB unit not mapped yet.
    if ((type & MEM_COMMIT) && !(type & MEM_RESERVE) && base) {   // (MEM_REPLACE_PLACEHOLDER | MEM_COMMIT too)
        std::lock_guard<std::mutex> g(g_mu);
        if (in_private_graphics((uintptr_t)base)) return base;   // committed already
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
                g_owned.insert(page);
                if (!map_one(va, page)) return nullptr;
                memset((void*)va, 0, kPage);  // committed memory reads as zero
            }
            if (is_graphics(flags, protect)) g_graphics[start] = end - start;
            return base;
        }
    }
    // GPU-visible memory is imported into the GPU with 64 KB granularity; keep it 64 KB aligned/sized.
    if (is_graphics(flags, protect) && (type & MEM_COMMIT)) size = (size + kPage - 1) & ~(kPage - 1);
    void* p = (type & MEM_RESERVE) && !base ? reserve_va(nullptr, size, pc_alloc_type(type), pc_protect(protect), flags)
                                           : VirtualAlloc(base, size, pc_alloc_type(type), pc_protect(protect));
    if (p && is_graphics(flags, protect) && (type & MEM_COMMIT)) {
        std::lock_guard<std::mutex> g(g_mu);
        g_graphics[(uintptr_t)p] = size;
    }
    if (!p) XSL_WARN(CH, "XMemVirtualAlloc(%p, %zu, %lx, %llx, %lx) failed (%lu)", base, size, type, flags, protect, GetLastError());
    return p;
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONGLONG fl, PULONG_PTR count, PULONG_PTR pages) {
    if (trace_take()) XSL_INFO(CH, "trace: XMemAllocatePhysicalPages(flags %llx, %llu pages)", fl, (unsigned long long)*count);
    std::lock_guard<std::mutex> g(g_mu);
    while (g_free.size() < *count)
        if (!grow()) { *count = 0; SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (ULONG_PTR i = 0; i < *count; i++) { pages[i] = g_free.back() * kFramesPerPage; g_free.pop_back(); }
    return TRUE;
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, PULONG_PTR pages) {
    if (trace_take()) XSL_INFO(CH, "trace: XMemFreePhysicalPages(%llu pages, first %llu)", (unsigned long long)count, (unsigned long long)(count ? pages[0] : 0));
    std::lock_guard<std::mutex> g(g_mu);
    for (ULONG_PTR i = 0; i < count; i++) {
        ULONG_PTR pg = pages[i] / kFramesPerPage;
        if (g_page_views.count(pg)) g_zombie.insert(pg);   // still mapped: free once the view goes
        else g_free.push_back(pg);
    }
    return TRUE;
}

PVOID WINAPI XMemMapPhysicalPages(PVOID va, ULONG_PTR count, PULONG_PTR pages) {
    if (trace_take()) XSL_INFO(CH, "trace: XMemMapPhysicalPages(%p, %llu pages, first %lld)", va, (unsigned long long)count, pages && count ? (long long)pages[0] : -1ll);
    std::lock_guard<std::mutex> g(g_mu);
    if (in_private_graphics((uintptr_t)va)) {   // memory already there (see g_private_graphics)
        for (ULONG_PTR i = 0; pages && i < count; i++) note_private_page((uintptr_t)va + i * kPage, pages[i] / kFramesPerPage);
        return va;
    }
    if (!in_placeholder((uintptr_t)va)) {
        XSL_ERR(CH, "XMemMapPhysicalPages at %p: not inside an XMem placeholder reservation", va);
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    for (ULONG_PTR i = 0; i < count; i++)
        if (!map_one((uintptr_t)va + i * kPage, pages ? pages[i] / kFramesPerPage : (ULONG_PTR)-1)) return nullptr;
    return va;
}

BOOL WINAPI XMemMapPhysicalPagesScatter(PVOID* vas, ULONG_PTR count, PULONG_PTR pages) {
    if (trace_take()) XSL_INFO(CH, "trace: XMemMapPhysicalPagesScatter(%llu pages, first %p <- %lld)", (unsigned long long)count, count ? vas[0] : nullptr, pages && count ? (long long)pages[0] : -1ll);
    std::lock_guard<std::mutex> g(g_mu);
    for (ULONG_PTR i = 0; i < count; i++) {
        if (in_private_graphics((uintptr_t)vas[i])) {
            if (pages) note_private_page((uintptr_t)vas[i], pages[i] / kFramesPerPage);
            continue;
        }
        if (!map_one((uintptr_t)vas[i], pages ? pages[i] / kFramesPerPage : (ULONG_PTR)-1)) return FALSE;
    }
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
// Layer-private export: whether the title answers XMemAlloc itself (exported hooks or XMemSetAllocationHooks).
BOOL WINAPI xsl_xmem_title_hooks() {
    auto* h = title_hooks();
    return (h && h->AllocRoutine) || g_setHooks.AllocRoutine;
}

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

// Layer-private export: turn [base, base + size) of a placeholder reservation (pages mapped with
// XMemMapPhysicalPages or committed later) into one write-watched private allocation with the same
// contents. d3d12_xs.dll uses it for descriptor memory a title supplies through its XMem hooks: the
// layer must see which descriptors the title writes. The physical pages that were mapped there stay
// allocated to the title (it never learns they are gone; titles do not remap descriptor memory).
BOOL WINAPI xsl_xmem_write_watch(void* base, SIZE_T size) {
    std::lock_guard<std::mutex> g(g_mu);
    uintptr_t b = (uintptr_t)base;
    if (in_private_graphics(b) && in_private_graphics(b + size - 1)) {   // write-watched from the start
        ResetWriteWatch(base, size);
        return TRUE;
    }
    if ((b & (kPage - 1)) || !size || !in_placeholder(b)) return FALSE;
    size = (size + kPage - 1) & ~(kPage - 1);
    std::vector<uint8_t> keep(size);
    for (uintptr_t va = b; va < b + size; va += kPage) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery((void*)va, &mbi, sizeof mbi);
        if (mbi.Type == MEM_MAPPED) {
            memcpy(&keep[va - b], (void*)va, kPage);
            UnmapViewOfFile2(GetCurrentProcess(), (void*)va, MEM_PRESERVE_PLACEHOLDER);
            release_va(va);
        } else if (mbi.State != MEM_RESERVE) {
            XSL_WARN(CH, "write watch for %p+%zu: %p is neither mapped nor a placeholder", base, size, (void*)va);
            return FALSE;
        }
    }
    // One placeholder over exactly the range (the title split it; map_one split it per page).
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(base, &mbi, sizeof mbi);
    if (mbi.RegionSize < size && !VirtualFree(base, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        XSL_WARN(CH, "write watch for %p+%zu: placeholders not coalesced (%lu)", base, size, GetLastError());
        return FALSE;
    }
    VirtualQuery(base, &mbi, sizeof mbi);
    if (mbi.RegionSize > size) VirtualFree(base, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);   // split off the rest
    void* p = VirtualAlloc2(GetCurrentProcess(), base, size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER | MEM_WRITE_WATCH, PAGE_READWRITE, nullptr, 0);
    if (p != base) {
        XSL_WARN(CH, "write watch for %p+%zu: write-watched memory not placed (%lu)", base, size, GetLastError());
        return FALSE;
    }
    memcpy(base, keep.data(), size);
    ResetWriteWatch(base, size);
    XSL_INFO(CH, "%p+%zu: now write-watched memory (descriptor heap memory from the title)", base, size);
    return TRUE;
}

// Layer-private export: VirtualFree on memory inside an XMem placeholder reservation (the platform
// DLL routes the title's VirtualFree here). Decommit unmaps the 64 KB views (VirtualFree cannot) and
// frees the pages the layer took for them; a release of a whole reservation unmaps everything first.
// Returns FALSE when the address is not XMem's (the caller then calls the real VirtualFree).
BOOL WINAPI xsl_xmem_virtual_free(void* base, SIZE_T size, DWORD type, BOOL* result) {
    if (trace_take()) XSL_INFO(CH, "trace: VirtualFree(%p, %zx, type %lx)", base, size, type);
    std::lock_guard<std::mutex> g(g_mu);
    uintptr_t b = (uintptr_t)base;
    if (!in_placeholder(b) || (type & (MEM_PRESERVE_PLACEHOLDER | MEM_COALESCE_PLACEHOLDERS))) return FALSE;
    if (in_private_graphics(b)) {   // private memory: stays committed (the GPU imported it)
        if (type & MEM_DECOMMIT) { *result = TRUE; return TRUE; }
        return FALSE;
    }
    auto it = std::prev(g_placeholders.upper_bound(b));
    uintptr_t start = b & ~(uintptr_t)(kPage - 1), end;
    if (type & MEM_DECOMMIT) end = size ? (b + size + kPage - 1) & ~(uintptr_t)(kPage - 1) : it->first + it->second;
    else { start = it->first; end = it->first + it->second; if (b != it->first) return FALSE; }
    for (auto v = g_va_page.lower_bound(start); v != g_va_page.end() && v->first < end;) {
        uintptr_t va = v->first;
        ++v;
        UnmapViewOfFile2(GetCurrentProcess(), (void*)va, MEM_PRESERVE_PLACEHOLDER);
        release_va(va);
    }
    if (type & MEM_DECOMMIT) { *result = TRUE; return TRUE; }
    VirtualFree((void*)start, end - start, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    *result = VirtualFree((void*)start, 0, MEM_RELEASE);
    g_placeholders.erase(it);
    return TRUE;
}

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
