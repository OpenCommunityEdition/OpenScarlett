// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Unified memory. On the console the CPU and GPU share one address space, so a title passes the CPU
// address of GPU-visible memory (XMemVirtualAlloc with XMEM_GRAPHICS / PAGE_GRAPHICS_*) wherever a
// GPU virtual address is expected. xmem.dll records those allocations; here an address inside one is
// translated by wrapping the allocation once as a zero-copy GPU buffer and offsetting into it.
#include <map>
#include <mutex>

#include "xs_unified.h"

namespace xs {
namespace {
typedef BOOL(WINAPI* PFN_find)(const void*, void**, SIZE_T*);
PFN_find find_fn() {
    static PFN_find f = [] {
        HMODULE m = GetModuleHandleW(L"xmem.dll");
        return m ? (PFN_find)GetProcAddress(m, "xsl_xmem_find_graphics") : nullptr;
    }();
    return f;
}
struct Imported { uint64_t size; PcObj res; uint64_t gpu; };
std::mutex g_mu;
std::map<uintptr_t, Imported> g_imported;
}  // namespace

bool unified_lookup(Device* dev, uint64_t va, UnifiedRef* out) {
    PFN_find f = find_fn();
    void* base;
    SIZE_T size;
    if (!f || !f((const void*)va, &base, &size)) return false;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_imported.find((uintptr_t)base);
    if (it == g_imported.end() || it->second.size != size) {
        Imported im{size, nullptr, 0};
        if (FAILED(pc::import_host_memory(dev_extra(dev)->pcDevice, base, size, &im.res, &im.gpu))) return false;
        XSL_INFO(CH, "unified memory: %p (+%zu) visible to the GPU at %llx", base, (size_t)size, (unsigned long long)im.gpu);
        it = g_imported.insert_or_assign((uintptr_t)base, im).first;
    }
    out->res = it->second.res;
    out->offset = va - (uintptr_t)base;
    out->gpu = it->second.gpu + out->offset;
    return true;
}

uint64_t gpu_va(Device* dev, uint64_t va) {
    UnifiedRef r;
    return unified_lookup(dev, va, &r) ? r.gpu : va;
}

}  // namespace xs
