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
// Physical pages (sections of xmem.dll) mapped in title address space: pool pages, and the GPU
// addresses they are page-mapped to. Each section is one zero-copy buffer.
typedef BOOL(WINAPI* PFN_section)(const void*, HANDLE*, ULONG64*, ULONG64*, ULONG64*);
PFN_section section_fn() {
    static PFN_section f = [] {
        HMODULE m = GetModuleHandleW(L"xmem.dll");
        return m ? (PFN_section)GetProcAddress(m, "xsl_xmem_section_of") : nullptr;
    }();
    return f;
}
std::map<HANDLE, Imported> g_sections;
}  // namespace

bool section_lookup(Device* dev, uint64_t va, UnifiedRef* out, uint64_t* run) {
    PFN_section f = section_fn();
    HANDLE s = nullptr;
    ULONG64 off = 0, bytes = 0, n = 0;
    if (!f || !f((const void*)va, &s, &off, &bytes, &n)) return false;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_sections.find(s);
    if (it == g_sections.end()) {
        Imported im{bytes, nullptr, 0};
        const ULONGLONG t0 = GetTickCount64();
        HRESULT hr = pc::import_file_mapping(dev_extra(dev)->pcDevice, s, bytes, &im.res, &im.gpu);
        static volatile long shown = 0;
        if (InterlockedIncrement(&shown) <= 40)
            XSL_INFO(CH, "physical-page section %p (%llu MB): %s %llx (%llu ms)", (void*)s, (unsigned long long)(bytes >> 20), SUCCEEDED(hr) ? "visible to the GPU at" : "import failed",
                     (unsigned long long)(SUCCEEDED(hr) ? im.gpu : (uint64_t)(uint32_t)hr), (unsigned long long)(GetTickCount64() - t0));
        it = g_sections.insert({s, im}).first;   // a failed import is remembered too
    }
    if (!it->second.res) return false;
    out->res = it->second.res;
    out->offset = off;
    out->gpu = it->second.gpu + off;
    if (run) *run = n;
    return true;
}

bool unified_block(uint64_t va, uint64_t* base, uint64_t* size) {
    PFN_find f = find_fn();
    void* b = nullptr;
    SIZE_T n = 0;
    if (!f || !f((const void*)va, &b, &n)) return false;
    *base = (uint64_t)(uintptr_t)b;
    *size = n;
    return true;
}
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
        pc::gm_register(im.res, (uint64_t)(uintptr_t)base, size, 0);   // title addresses of unified memory are CPU addresses
        it = g_imported.insert_or_assign((uintptr_t)base, im).first;
    }
    out->res = it->second.res;
    out->offset = va - (uintptr_t)base;
    out->gpu = it->second.gpu + out->offset;
    return true;
}

// The reverse: the zero-copy buffer a PC GPU address lies in (a title that asked a resource of
// its unified memory for its address works with PC addresses from then on).
bool unified_from_gpu(uint64_t gpu, UnifiedRef* out) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto& [base, im] : g_imported)
        if (im.res && gpu >= im.gpu && gpu < im.gpu + im.size) { *out = {im.res, gpu - im.gpu, gpu}; return true; }
    for (auto& [h, im] : g_sections)
        if (im.res && gpu >= im.gpu && gpu < im.gpu + im.size) { *out = {im.res, gpu - im.gpu, gpu}; return true; }
    return false;
}
// Diagnostics: n bytes of zero-copy memory at a PC GPU address (unified memory, or a physical-page
// section, read through a view of its own).
bool unified_read_gpu(uint64_t gpu, void* dst, size_t n) {
    HANDLE section = nullptr;
    uint64_t off = 0;
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& [base, im] : g_imported)
            if (im.res && gpu >= im.gpu && gpu + n <= im.gpu + im.size) {
                SIZE_T got = 0;
                return ReadProcessMemory(GetCurrentProcess(), (const void*)(base + (gpu - im.gpu)), dst, n, &got) && got == n;
            }
        for (auto& [h, im] : g_sections)
            if (im.res && gpu >= im.gpu && gpu + n <= im.gpu + im.size) { section = h; off = gpu - im.gpu; break; }
    }
    if (!section) return false;
    const uint64_t start = off & ~0xffffull;
    void* v = MapViewOfFile(section, FILE_MAP_READ, (DWORD)(start >> 32), (DWORD)start, (SIZE_T)(off - start + n));
    if (!v) return false;
    memcpy(dst, (const uint8_t*)v + (off - start), n);
    UnmapViewOfFile(v);
    return true;
}
// The CPU address of unified memory at a PC GPU address (0: none; diagnostics read through it).
uint64_t unified_cpu_of_gpu(uint64_t gpu) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto& [base, im] : g_imported)
        if (im.res && gpu >= im.gpu && gpu < im.gpu + im.size) return (uint64_t)base + (gpu - im.gpu);
    return 0;
}
uint64_t gpu_va(Device* dev, uint64_t va) {
    {   // (a buffer the GPU writes, placed there: its own PC buffer)
        PcObj w = nullptr;
        uint64_t off = 0;
        if (written_buffer_at(va, &w, &off))
            if (const uint64_t base = pc::res_gpu_va(w)) return base + off;
    }
    UnifiedRef r;
    if (unified_lookup(dev, va, &r)) return r.gpu;
    if (va < (1ull << 32)) return va;
    // A buffer the title placed at that address (GPU-written ones have PC memory of their own).
    PcObj res = nullptr;
    uint64_t off = 0, avail = 0;
    if (pc::gm_resolve(va, &res, &off, &avail) && res) {
        const uint64_t base = pc::res_gpu_va(res);
        if (base && base + off != va) return base + off;
        if (base) return va;
    }
    // Page-mapped pool memory: the bytes the title wrote, in the section's buffer.
    uint64_t run = 0;
    if (section_lookup(dev, va, &r, &run)) return r.gpu;
    return va;
}

}  // namespace xs
