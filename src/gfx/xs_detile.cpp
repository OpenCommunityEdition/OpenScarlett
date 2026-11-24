// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Console-tiled texture contents written by the CPU.
//
// CreatePlacedResourceX on committed memory gives the title a texture at its own address; the
// title then writes texels there in the console layout (e.g. 64KB_R_X swizzles). The PC resource is
// separate, so those writes are invisible to the GPU. When a view of such a texture is made, the
// layer converts its memory to linear rows with the title's own xg_xs.dll
// (XGCreateTextureComputer + XGTextureAddressComputer::CopyFromSubresource, interface facts from
// the GDK header, docs/provenance.md) and uploads it before the next submission.
//
// Contents written after the upload are found by a round-robin content check of uploaded textures
// (a byte budget per submission) and uploaded again.
#include <windows.h>

#include <iterator>
#include <atomic>
#include <map>
#include <mutex>
#include <algorithm>
#include <vector>

#include "xs_detile.h"

using xsl::Level;
using xsl::log_enabled;
namespace xs {
namespace {

// XGTextureAddressComputer: reference counted, virtual methods in this order (xg_xs.h).
struct XgComputer {
    virtual ULONG __cdecl AddRef() = 0;
    virtual ULONG __cdecl Release() = 0;
    virtual HRESULT __cdecl GetResourceLayout(void* layout) = 0;
    virtual UINT64 GetResourceSizeBytes() = 0;
    virtual UINT64 GetResourceBaseAlignmentBytes() = 0;
    virtual UINT64 __cdecl GetMipLevelOffsetBytes(UINT32 plane, UINT32 mip) = 0;
    virtual UINT64 __cdecl GetTexelElementOffsetBytes(UINT32, UINT32, UINT64, UINT32, UINT32, UINT32, UINT32*) = 0;
    virtual HRESULT __cdecl GetTexelCoordinate(UINT64, UINT32*, UINT32*, UINT64*, UINT32*, UINT32*, UINT32*) = 0;
    virtual HRESULT __cdecl CopyIntoSubresource(void* tiled, UINT32 plane, UINT32 sub, const void* linear, UINT32 rowPitch, UINT32 slicePitch) = 0;
    virtual HRESULT __cdecl CopyFromSubresource(void* linear, UINT32 plane, UINT32 sub, UINT32 sample, const void* tiled, UINT32 rowPitch,
                                                UINT32 slicePitch) = 0;
};
// XG_RESOURCE_DESC has the layout of D3D12_RESOURCE_DESC (xg_xs.h); the console desc is passed as is.
using CreateComputer = HRESULT(__cdecl*)(const void* desc, XgComputer** out);

struct Entry { uint64_t va; D3D12_RESOURCE_DESC desc; uint32_t state; bool queued = false, done = false; uint64_t size = 0, hash = 0; bool mapped = false;
               bool pending = false; };   // pending: its memory was not readable when it was first needed (pages mapped later)
std::map<PcObj, std::pair<D3D12_RESOURCE_DESC, uint32_t>> g_descs;   // console descriptions of textures the title may map
std::mutex g_mu;
std::map<PcObj, Entry> g_tex;
std::vector<PcObj> g_queue;
size_t g_scan = 0;   // round-robin position of the content check
std::atomic<uint64_t> g_stat[6];   // checked bytes, changed textures, uploads, uploaded bytes, first uploads, textures known

// The whole range is committed (it may span several adjacent regions).
bool committed(uint64_t va, uint64_t size) {
    for (uint64_t p = va, end = va + size; p < end;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)(uintptr_t)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        p = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// The hash of memory the title may have released since: read through the kernel in pieces, so a
// range that is gone fails instead of faulting (querying the range first costs milliseconds in a
// title's fragmented address space, on every check).
bool content_hash_guarded(uint64_t va, uint64_t size, uint64_t* out) {
    static thread_local std::vector<uint8_t> buf;
    const size_t piece = 1u << 20;
    if (buf.size() < piece) buf.resize(piece);
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (uint64_t at = 0; at < size; at += piece) {
        const size_t n = (size_t)std::min<uint64_t>(piece, size - at);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)(va + at), buf.data(), n, &got) || got != n) return false;
        const uint64_t* p = (const uint64_t*)buf.data();
        for (size_t i = 0, k = n / 8; i < k; i++) h = (h ^ p[i]) * 0x100000001b3ull + (h >> 29);
    }
    *out = h;
    return true;
}

// A private copy of title memory (false: not all of it is readable), and its hash as
// content_hash_guarded computes it. Texels are converted from the copy: the title may unmap or
// remap page-mapped memory at any moment, and a fault while reading it would reach the title's
// own crash handler.
bool copy_guarded(uint64_t va, uint64_t size, std::vector<uint8_t>& out, uint64_t* hash) {
    out.resize((size_t)size + 65536);   // slack: readers of block formats may look a little past the end
    const size_t piece = 1u << 20;
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (uint64_t at = 0; at < size; at += piece) {
        const size_t n = (size_t)std::min<uint64_t>(piece, size - at);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)(va + at), out.data() + at, n, &got) || got != n) return false;
        const uint64_t* p = (const uint64_t*)(out.data() + at);
        for (size_t i = 0, k = n / 8; i < k; i++) h = (h ^ p[i]) * 0x100000001b3ull + (h >> 29);
    }
    memset(out.data() + size, 0, 65536);
    *hash = h;
    return true;
}

CreateComputer xg() {
    static CreateComputer f = [] {
        HMODULE m = GetModuleHandleA("xg_xs.dll");
        if (!m) m = LoadLibraryA("xg_xs.dll");
        auto p = m ? (CreateComputer)GetProcAddress(m, "XGCreateTextureComputer") : nullptr;
        if (!p) XSL_WARN(CH, "xg_xs.dll (from the GDK) not found: CPU-written console textures stay empty");
        return p;
    }();
    return f;
}

struct FillCtx { XgComputer* c; uint64_t va; UINT64 size; };
bool fill(void* ctx, uint32_t sub, void* dst, uint32_t rowPitch, uint32_t rows, uint32_t depth) {
    auto* f = static_cast<FillCtx*>(ctx);
    (void)depth;
    // The title may unmap the pages while they are read (page-mapped memory): the texture is then
    // tried again later.
    __try {
        return SUCCEEDED(f->c->CopyFromSubresource(dst, 0, sub, 0, (const void*)(uintptr_t)f->va, rowPitch, rowPitch * rows));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

bool console_resource_size(const D3D12_RESOURCE_DESC* d, uint64_t* size, uint64_t* align) {
    if (!d) return false;
    if (d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        *size = (d->Width + 0xFFFF) & ~0xFFFFull;
        *align = 0x10000;
        return true;
    }
    CreateComputer f = xg();
    XgComputer* c = nullptr;
    if (!f || FAILED(f(d, &c)) || !c) return false;
    *size = c->GetResourceSizeBytes();
    *align = c->GetResourceBaseAlignmentBytes();
    c->Release();
    return *size != 0;
}

void detile_register(PcObj pcRes, uint64_t va, const void* consoleDesc, uint32_t initialState) {
    auto* d = static_cast<const D3D12_RESOURCE_DESC*>(consoleDesc);
    if (!d || d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || d->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) return;
    if (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return;   // GPU-written
    if (d->SampleDesc.Count > 1) return;
    std::lock_guard<std::mutex> g(g_mu);
    g_tex[pcRes] = {va, *d, initialState};
}

void detile_stats(uint64_t out[6]) {
    for (int i = 0; i < 5; i++) out[i] = g_stat[i].exchange(0);
    std::lock_guard<std::mutex> g(g_mu);
    out[5] = g_tex.size();
}
void detile_note_desc(PcObj pcRes, const void* consoleDesc, uint32_t initialState) {
    auto* d = static_cast<const D3D12_RESOURCE_DESC*>(consoleDesc);
    if (!d || d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || d->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN || d->SampleDesc.Count > 1) return;
    std::lock_guard<std::mutex> g(g_mu);
    g_descs[pcRes] = {*d, initialState};
}
void detile_mapped(PcObj pcRes, void* copy) {
    if (!copy) return;
    std::lock_guard<std::mutex> g(g_mu);
    auto d = g_descs.find(pcRes);
    if (d == g_descs.end() || g_tex.count(pcRes)) return;
    Entry e{(uint64_t)(uintptr_t)copy, d->second.first, d->second.second};
    e.mapped = true;
    g_tex[pcRes] = e;
}
void detile_unmapped(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_tex.find(pcRes);
    if (it == g_tex.end() || !it->second.mapped || it->second.queued) return;
    it->second.queued = true;
    it->second.done = false;
    g_queue.push_back(pcRes);
}
namespace {
struct MappedBuffer { void* mem; uint64_t size; uint64_t hash; bool fresh; };
std::map<PcObj, MappedBuffer> g_mapped_buffers;
}  // namespace
void mapped_buffer_add(PcObj pcRes, void* mem, uint64_t size) {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_mapped_buffers.count(pcRes)) return;
    g_mapped_buffers[pcRes] = {mem, size, 0, true};
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 4)
        XSL_INFO(CH, "Map of a %llu-byte buffer in video memory: the title writes into the resource's console memory at %p, copied to the buffer when it changes",
                 (unsigned long long)size, mem);
}
bool mapped_buffer_known(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
    return g_mapped_buffers.count(pcRes) != 0;
}
void mapped_buffer_forget(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
    g_mapped_buffers.erase(pcRes);
}
// Copies the mapped buffers whose bytes changed (called under g_mu's absence, before submissions).
static void mapped_buffers_sync(PcObj pcDevice, PcObj pcQueue) {
    std::vector<std::pair<PcObj, MappedBuffer>> all;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_mapped_buffers.empty()) return;
        all.assign(g_mapped_buffers.begin(), g_mapped_buffers.end());
    }
    static thread_local std::vector<uint8_t> copy;
    for (auto& [r, b] : all) {
        uint64_t h = 0;
        if (!copy_guarded((uint64_t)(uintptr_t)b.mem, b.size, copy, &h)) continue;   // not readable now
        if (!b.fresh && h == b.hash) continue;
        {
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_mapped_buffers.find(r);
            if (it == g_mapped_buffers.end()) continue;   // released meanwhile
            it->second.hash = h;
            it->second.fresh = false;
        }
        pc::upload_buffer(pcDevice, pcQueue, r, 0, copy.data(), b.size);
    }
}
void detile_forget(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
    g_mapped_buffers.erase(pcRes);
    g_descs.erase(pcRes);
    g_tex.erase(pcRes);
    for (auto& q : g_queue) if (q == pcRes) q = nullptr;
}
void detile_view_created(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_tex.find(pcRes);
    if (it == g_tex.end() || it->second.queued) return;
    it->second.queued = true;
    g_queue.push_back(pcRes);
}

void detile_flush(PcObj pcDevice, PcObj pcQueue) {
    mapped_buffers_sync(pcDevice, pcQueue);
    std::vector<std::pair<PcObj, Entry>> work;
    {
        std::lock_guard<std::mutex> g(g_mu);
        // Content check: up to 8 MB of uploaded textures per submission.
        static bool check = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_DETILE_RECHECK", b, 4) && b[0] == '0'); }();
        if (check && !g_tex.empty()) {
            const uint64_t kCheckBudget = 8ull << 20;
            uint64_t budget = kCheckBudget;
            auto it = g_tex.begin();
            std::advance(it, g_scan % g_tex.size());
            for (size_t k = 0; k < g_tex.size() && budget; k++, g_scan++) {
                if (it == g_tex.end()) it = g_tex.begin();
                Entry& e = it->second;
                // (Mapped copies upload at their unmap. A texture larger than the whole budget is
                // checked when nothing else was this time, else it would never be: font atlases.)
                if (e.done && e.size && (e.size <= budget || budget == kCheckBudget) && !e.mapped) {
                    budget -= std::min(budget, e.size);
                    uint64_t now = 0;
                    if (!content_hash_guarded(e.va, e.size, &now)) { if (!e.pending) e.size = 0; }   // the title released the memory: stop checking
                    else {
                        g_stat[0] += e.size;
                        if (now != e.hash || e.pending) { g_stat[1]++; e.done = false; e.queued = true; g_queue.push_back(it->first); }
                    }
                }
                ++it;
            }
        }
        if (g_queue.empty()) return;
        // Up to 16 MB of console texels a submission (a title that streams in thousands of
        // textures at once otherwise spends tens of seconds in one submission, long enough for
        // its own watchdog to end it); the rest wait for the next one, drawn empty until then.
        const uint64_t kUploadBudget = 16ull << 20;
        uint64_t taken = 0;
        size_t used = 0;
        for (; used < g_queue.size() && taken < kUploadBudget; used++) {
            PcObj r = g_queue[used];
            if (!r) continue;
            auto it = g_tex.find(r);
            if (it == g_tex.end() || it->second.done) continue;
            it->second.done = true;
            it->second.queued = false;
            work.push_back(*it);
            uint64_t sz = it->second.size, al = 0;
            if (!sz) console_resource_size(&it->second.desc, &sz, &al);
            taken += sz ? sz : 65536;
        }
        g_queue.erase(g_queue.begin(), g_queue.begin() + used);
    }
    CreateComputer create = xg();
    if (!create) return;
    static volatile long logged = 0, failed = 0;
    for (auto& [r, e] : work) {
        XgComputer* c = nullptr;
        HRESULT hr = create(&e.desc, &c);
        if (FAILED(hr) || !c) {
            if (InterlockedIncrement(&failed) <= 5)
                XSL_WARN(CH, "XGCreateTextureComputer failed %08lx (%llux%u format %d layout %#x)", hr, (unsigned long long)e.desc.Width, e.desc.Height,
                         (int)e.desc.Format, (unsigned)e.desc.Layout);
            continue;
        }
        FillCtx ctx{c, e.va, c->GetResourceSizeBytes()};
        bool readable = true;
        // Hash before reading: texels the title writes during or after the upload (e.g. glyphs a
        // font cache adds) then differ from the hash and are uploaded at a later check. Hashing
        // afterwards recorded such writes as uploaded and left them out for good.
        uint64_t hash = 0;
        static thread_local std::vector<uint8_t> copy;
        if (!copy_guarded(e.va, ctx.size, copy, &hash)) readable = false;
        else ctx.va = (uint64_t)(uintptr_t)copy.data();
        if (readable) hr = pc::upload_texture(pcDevice, pcQueue, r, e.state, fill, &ctx);
        g_stat[2]++;
        g_stat[3] += ctx.size;
        if (!e.size) g_stat[4]++;
        c->Release();
        if (!readable || hr == E_FAIL) {   // pages not mapped there (yet, or no longer): looked at again by the content check
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_tex.find(r);
            if (it != g_tex.end()) { it->second.size = ctx.size; it->second.pending = true; }
            static volatile long np = 0;
            if (InterlockedIncrement(&np) <= 3)
                XSL_INFO(CH, "texture at %llx (%llux%u format %d): its memory is not there yet, uploaded once it is", (unsigned long long)e.va,
                         (unsigned long long)e.desc.Width, e.desc.Height, (int)e.desc.Format);
            continue;
        }
        if (!readable || FAILED(hr)) {
            if (InterlockedIncrement(&failed) <= 5)
                XSL_WARN(CH, "texture at %llx (%llux%u format %d): %s %08lx", (unsigned long long)e.va, (unsigned long long)e.desc.Width, e.desc.Height,
                         (int)e.desc.Format, readable ? "upload failed" : "memory not committed for its whole size", readable ? hr : 0);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_tex.find(r);
            if (it != g_tex.end()) { it->second.size = ctx.size; it->second.hash = hash; it->second.pending = false; }
        }
        if (InterlockedIncrement(&logged) <= 5 || log_enabled(CH, Level::Trace))
            XSL_INFO(CH, "texture at %llx (%llux%ux%u, %u mips, format %d, layout %#x, %llu bytes): detiled and uploaded", (unsigned long long)e.va,
                     (unsigned long long)e.desc.Width, e.desc.Height, e.desc.DepthOrArraySize, e.desc.MipLevels, (int)e.desc.Format, (unsigned)e.desc.Layout,
                     (unsigned long long)ctx.size);
    }
}
}  // namespace xs
