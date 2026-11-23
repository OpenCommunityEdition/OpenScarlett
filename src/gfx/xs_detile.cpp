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

struct Entry { uint64_t va; D3D12_RESOURCE_DESC desc; uint32_t state; bool queued = false, done = false; uint64_t size = 0, hash = 0; bool mapped = false; };
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
    return SUCCEEDED(f->c->CopyFromSubresource(dst, 0, sub, 0, (const void*)(uintptr_t)f->va, rowPitch, rowPitch * rows));
    (void)depth;
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
void detile_forget(PcObj pcRes) {
    std::lock_guard<std::mutex> g(g_mu);
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
    std::vector<std::pair<PcObj, Entry>> work;
    {
        std::lock_guard<std::mutex> g(g_mu);
        // Content check: up to 8 MB of uploaded textures per submission.
        static bool check = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_DETILE_RECHECK", b, 4) && b[0] == '0'); }();
        if (check && !g_tex.empty()) {
            uint64_t budget = 8ull << 20;
            auto it = g_tex.begin();
            std::advance(it, g_scan % g_tex.size());
            for (size_t k = 0; k < g_tex.size() && budget; k++, g_scan++) {
                if (it == g_tex.end()) it = g_tex.begin();
                Entry& e = it->second;
                if (e.done && e.size && e.size <= budget && !e.mapped) {   // (mapped copies upload at their unmap)
                    budget -= e.size;
                    uint64_t now = 0;
                    if (!content_hash_guarded(e.va, e.size, &now)) e.size = 0;   // the title released the memory: stop checking
                    else {
                        g_stat[0] += e.size;
                        if (now != e.hash) { g_stat[1]++; e.done = false; e.queued = true; g_queue.push_back(it->first); }
                    }
                }
                ++it;
            }
        }
        if (g_queue.empty()) return;
        for (PcObj r : g_queue) {
            if (!r) continue;
            auto it = g_tex.find(r);
            if (it != g_tex.end() && !it->second.done) { it->second.done = true; it->second.queued = false; work.push_back(*it); }
        }
        g_queue.clear();
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
        bool readable = committed(e.va, ctx.size);
        // Hash before reading: texels the title writes during or after the upload (e.g. glyphs a
        // font cache adds) then differ from the hash and are uploaded at a later check. Hashing
        // afterwards recorded such writes as uploaded and left them out for good.
        uint64_t hash = 0;
        if (readable && !content_hash_guarded(e.va, ctx.size, &hash)) readable = false;
        if (readable) hr = pc::upload_texture(pcDevice, pcQueue, r, e.state, fill, &ctx);
        g_stat[2]++;
        g_stat[3] += ctx.size;
        if (!e.size) g_stat[4]++;
        c->Release();
        if (!readable || FAILED(hr)) {
            if (InterlockedIncrement(&failed) <= 5)
                XSL_WARN(CH, "texture at %llx (%llux%u format %d): %s %08lx", (unsigned long long)e.va, (unsigned long long)e.desc.Width, e.desc.Height,
                         (int)e.desc.Format, readable ? "upload failed" : "memory not committed for its whole size", readable ? hr : 0);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(g_mu);
            auto it = g_tex.find(r);
            if (it != g_tex.end()) { it->second.size = ctx.size; it->second.hash = hash; }
        }
        if (InterlockedIncrement(&logged) <= 5 || log_enabled(CH, Level::Trace))
            XSL_INFO(CH, "texture at %llx (%llux%ux%u, %u mips, format %d, layout %#x, %llu bytes): detiled and uploaded", (unsigned long long)e.va,
                     (unsigned long long)e.desc.Width, e.desc.Height, e.desc.DepthOrArraySize, e.desc.MipLevels, (int)e.desc.Format, (unsigned)e.desc.Layout,
                     (unsigned long long)ctx.size);
    }
}
}  // namespace xs
