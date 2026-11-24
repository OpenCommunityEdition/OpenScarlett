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
#include <set>
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
               bool pending = false;
               int written = 0;       // (mapped) checks in a row that found new writes
               uint64_t sample = 0;   // (movie planes) hash of a few pieces, taken at every submission
               int64_t rowBytes = -1;  // console bytes per row when the layout is plain rows (0: it is not; -1: not looked at yet)
               uint64_t rowBase = 0; };   // ... and where row 0 begins   // pending: its memory was not readable when it was first needed (pages mapped later)
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

struct FillCtx { XgComputer* c; uint64_t va; UINT64 size; int64_t rowBytes = 0; uint64_t rowBase = 0; uint32_t texelRow = 0; };
// Bytes per texel of the formats whose console layout may be plain rows (0: not one of them).
uint32_t plain_texel_bytes(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM: case DXGI_FORMAT_R8_UINT: return 1;
        case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT: return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 4;
        default: return 0;
    }
}
// The layout library converts any console layout, a texel at a time (a 4K movie plane took over
// 100 ms, every frame). A one-level 2D texture laid out as plain rows is copied row by row
// instead: found by asking where a few texels lie. Returns the bytes from one row to the next
// (0: not plain rows) and where row 0 begins.
int64_t plain_rows(XgComputer* c, const D3D12_RESOURCE_DESC& d, uint64_t* base) {
    const uint32_t bpp = plain_texel_bytes(d.Format);
    if (!bpp || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 || d.DepthOrArraySize != 1 || d.SampleDesc.Count > 1 || d.Height < 2) return 0;
    UINT32 bit = 0;
    const uint64_t w = d.Width, h = d.Height;
    const uint64_t b = c->GetTexelElementOffsetBytes(0, 0, 0, 0, 0, 0, &bit);
    const uint64_t pitch = c->GetTexelElementOffsetBytes(0, 0, 0, 1, 0, 0, &bit) - b;
    if (pitch < w * bpp || pitch > w * bpp + 4096) return 0;
    const uint64_t probes[][2] = {{1, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}, {w / 2 + 3 < w ? w / 2 + 3 : 0, h / 2}, {w / 3, h / 3 + 1 < h ? h / 3 + 1 : 0}, {7 < w ? 7 : 0, 9 < h ? 9 : 0}};
    for (auto& p : probes)
        if (c->GetTexelElementOffsetBytes(0, 0, p[0], (UINT32)p[1], 0, 0, &bit) != b + p[1] * pitch + p[0] * bpp) return 0;
    *base = b;
    return (int64_t)pitch;
}
std::atomic<uint64_t> g_time[4];   // microseconds: layout object, reading the memory, conversion, the rest of the upload
uint64_t now_us() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)(t.QuadPart * 1000000 / f.QuadPart); }
bool fill(void* ctx, uint32_t sub, void* dst, uint32_t rowPitch, uint32_t rows, uint32_t depth) {
    auto* f = static_cast<FillCtx*>(ctx);
    (void)depth;
    // The title may unmap the pages while they are read (page-mapped memory): the texture is then
    // tried again later.
    const uint64_t t0 = now_us();
    bool ok = false;
    if (f->rowBytes > 0 && sub == 0) {   // plain rows, from the private copy
        const size_t n = std::min<size_t>({(size_t)rowPitch, (size_t)f->rowBytes, (size_t)f->texelRow});
        const uint8_t* src = (const uint8_t*)(uintptr_t)f->va + f->rowBase;
        for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t*)dst + (size_t)y * rowPitch, src + (size_t)y * f->rowBytes, n);
        g_time[2] += now_us() - t0;
        return true;
    }
    __try {
        ok = SUCCEEDED(f->c->CopyFromSubresource(dst, 0, sub, 0, (const void*)(uintptr_t)f->va, rowPitch, rowPitch * rows));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_time[2] += now_us() - t0;
    return ok;
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

namespace {
// Movie planes placed on title memory. A decoder fills them on the CPU every frame, far more
// often than the round-robin content check comes by, so a few pieces of each are hashed at every
// submission (a new frame changes nearly every texel) and a plane that differs is uploaded.
std::set<PcObj> g_planes;
// Where a texture's bytes are read: its address, or for an address in a GPU view of the title's
// CPU memory with no page mapped at it, the same offset of the CPU view (xmem.dll keeps the pairs).
uint64_t source_of(uint64_t va) {
    typedef uintptr_t(WINAPI * Fn)(uintptr_t);
    static Fn f = [] { HMODULE m = GetModuleHandleW(L"xmem.dll"); return m ? (Fn)GetProcAddress(m, "xsl_xmem_source") : nullptr; }();
    return f ? (uint64_t)f((uintptr_t)va) : va;
}
bool sample_hash(uint64_t va, uint64_t size, uint64_t* out) {
    constexpr size_t kPiece = 4096, kPieces = 12;
    uint64_t buf[kPiece / 8];
    uint64_t h = 0x9e3779b97f4a7c15ull;
    if (size < kPiece * kPieces) return false;
    for (size_t k = 0; k < kPieces; k++) {
        const uint64_t at = ((size - kPiece) / (kPieces - 1) * k) & ~7ull;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void*)(uintptr_t)(va + at), buf, kPiece, &got) || got != kPiece) return false;
        for (size_t i = 0; i < kPiece / 8; i++) h = (h ^ buf[i]) * 0x100000001b3ull + (h >> 29);
    }
    *out = h;
    return true;
}
}  // namespace
void detile_register(PcObj pcRes, uint64_t va, const void* consoleDesc, uint32_t initialState) {
    auto* d = static_cast<const D3D12_RESOURCE_DESC*>(consoleDesc);
    if (!d || d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || d->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) return;
    if (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return;   // GPU-written
    if (d->SampleDesc.Count > 1) return;
    std::lock_guard<std::mutex> g(g_mu);
    g_tex[pcRes] = {va, *d, initialState};
    // A plane of a movie: one level of 8-bit luma or chroma that a decoder rewrites every frame.
    if (d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->MipLevels == 1 && d->DepthOrArraySize == 1 &&
        (d->Format == DXGI_FORMAT_R8_UNORM || d->Format == DXGI_FORMAT_R8G8_UNORM) && d->Width >= 256 && d->Height >= 128)
        g_planes.insert(pcRes);
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
namespace {
// Textures the title holds mapped. Their CPU copy is uploaded at Unmap, and also while it stays
// mapped: when writes to it have stopped for one check (a decoded movie frame is complete), or
// after three checks that all found writes.
std::set<PcObj> g_mapped_tex;
}  // namespace
void detile_mapped(PcObj pcRes, void* copy) {
    if (!copy) return;
    std::lock_guard<std::mutex> g(g_mu);
    auto d = g_descs.find(pcRes);
    if (d == g_descs.end() || g_tex.count(pcRes)) return;
    Entry e{(uint64_t)(uintptr_t)copy, d->second.first, d->second.second};
    e.mapped = true;
    g_tex[pcRes] = e;
    g_mapped_tex.insert(pcRes);
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
    g_mapped_tex.erase(pcRes);
    g_planes.erase(pcRes);
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
        static uint64_t rounds = 0;
        size_t nNotUp = 0, nUnread = 0, nSame = 0, nBig = 0;
        const bool report = ++rounds % 600 == 0 && log_enabled(CH, Level::Info) && getenv("XSL_PLANE_STATS");
        for (PcObj r : g_planes) {
            auto it = g_tex.find(r);
            if (it == g_tex.end()) continue;
            Entry& e = it->second;
            if (e.desc.Width >= 1920) nBig++;
            if (e.mapped || e.queued || !e.done || !e.size) { nNotUp++; continue; }   // (uploaded once first, when a view names it)
            uint64_t now = 0;
            if (!sample_hash(source_of(e.va), e.size, &now)) { nUnread++; continue; }
            if (now == e.sample) { nSame++; continue; }
            const bool first = e.sample == 0;
            e.sample = now;
            if (first) continue;   // what the first upload already carried
            e.queued = true;
            e.done = false;
            g_queue.push_back(r);
            static volatile long n = 0;
            if (InterlockedIncrement(&n) == 1)
                XSL_INFO(CH, "movie plane at %llx (%llux%u format %d) changes on the CPU: uploaded at every change", (unsigned long long)e.va,
                         (unsigned long long)e.desc.Width, e.desc.Height, (int)e.desc.Format);
        }
        if (report)
            XSL_INFO(CH, "movie planes: %zu known (%zu at least 1920 wide), %zu not uploaded yet, %zu unreadable, %zu unchanged this time", g_planes.size(), nBig, nNotUp,
                     nUnread, nSame);
        for (PcObj r : g_mapped_tex) {
            auto it = g_tex.find(r);
            if (it == g_tex.end()) continue;
            Entry& e = it->second;
            const bool w = pc::map_shadow_written(r);
            if (w) e.written++;
            if (e.written && (!w || e.written >= 3) && !e.queued) {
                e.written = 0;
                e.queued = true;
                e.done = false;
                g_queue.push_back(r);
                static volatile long n = 0;
                if (InterlockedIncrement(&n) == 1)
                    XSL_INFO(CH, "a texture the title keeps mapped (%llux%u format %d) is uploaded whenever it has been written", (unsigned long long)e.desc.Width,
                             e.desc.Height, (int)e.desc.Format);
            }
        }
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
                    if (!content_hash_guarded(source_of(e.va), e.size, &now)) { if (!e.pending) e.size = 0; }   // the title released the memory: stop checking
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
        uint64_t tk = now_us();
        HRESULT hr = create(&e.desc, &c);
        g_time[0] += now_us() - tk;
        if (FAILED(hr) || !c) {
            if (InterlockedIncrement(&failed) <= 5)
                XSL_WARN(CH, "XGCreateTextureComputer failed %08lx (%llux%u format %d layout %#x)", hr, (unsigned long long)e.desc.Width, e.desc.Height,
                         (int)e.desc.Format, (unsigned)e.desc.Layout);
            continue;
        }
        FillCtx ctx{c, e.va, c->GetResourceSizeBytes()};
        if (e.rowBytes < 0) {
            e.rowBytes = plain_rows(c, e.desc, &e.rowBase);
            if (e.rowBytes > 0 && e.rowBase + (uint64_t)e.rowBytes * (e.desc.Height - 1) + e.desc.Width * plain_texel_bytes(e.desc.Format) > ctx.size) e.rowBytes = 0;
            static volatile long nr = 0;
            if (e.rowBytes > 0 && InterlockedIncrement(&nr) <= 3)
                XSL_INFO(CH, "texture at %llx (%llux%u format %d): plain rows of %lld bytes on the console, copied by rows", (unsigned long long)e.va,
                         (unsigned long long)e.desc.Width, e.desc.Height, (int)e.desc.Format, (long long)e.rowBytes);
        }
        ctx.rowBytes = e.rowBytes;
        ctx.rowBase = e.rowBase;
        ctx.texelRow = (uint32_t)(e.desc.Width * plain_texel_bytes(e.desc.Format));
        bool readable = true;
        // Hash before reading: texels the title writes during or after the upload (e.g. glyphs a
        // font cache adds) then differ from the hash and are uploaded at a later check. Hashing
        // afterwards recorded such writes as uploaded and left them out for good.
        uint64_t hash = 0;
        static thread_local std::vector<uint8_t> copy;
        const uint64_t src = source_of(e.va);
        if (src != e.va) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 3)
                XSL_INFO(CH, "texture at %llx (%llux%u format %d) lies in a GPU view of the title's CPU memory: read at %llx", (unsigned long long)e.va,
                         (unsigned long long)e.desc.Width, e.desc.Height, (int)e.desc.Format, (unsigned long long)src);
        }
        tk = now_us();
        if (!copy_guarded(src, ctx.size, copy, &hash)) readable = false;
        else ctx.va = (uint64_t)(uintptr_t)copy.data();
        g_time[1] += now_us() - tk;
        if (readable && e.desc.Width >= 1920 && e.desc.Format == DXGI_FORMAT_R8_UNORM && getenv("XSL_PLANE_STATS")) {   // what a movie plane holds
            static volatile long np = 0;
            const long k = InterlockedIncrement(&np);
            if (k <= 6 || k % 45 == 0) {
                uint64_t sum = 0;
                uint8_t lo = 255, hi = 0;
                for (uint64_t i = 0; i < ctx.size; i += 97) { const uint8_t v = copy[(size_t)i]; sum += v; lo = std::min(lo, v); hi = std::max(hi, v); }
                XSL_INFO(CH, "movie plane upload #%ld: %llx (read at %llx) %llux%u, bytes %u..%u, mean %llu, rows of %lld from %llu, PC texture %p", k, (unsigned long long)e.va,
                         (unsigned long long)src, (unsigned long long)e.desc.Width, e.desc.Height, lo, hi, (unsigned long long)(sum / (ctx.size / 97 + 1)), (long long)e.rowBytes,
                         (unsigned long long)e.rowBase, (void*)r);
            }
        }
        tk = now_us();
        const uint64_t conv0 = g_time[2].load();
        if (readable) hr = pc::upload_texture(pcDevice, pcQueue, r, e.state, fill, &ctx);
        g_time[3] += now_us() - tk - (g_time[2].load() - conv0);
        {
            static volatile long ups = 0;
            if (InterlockedIncrement(&ups) % 100 == 0 && getenv("XSL_SUBMIT_TIMES"))
                XSL_INFO(CH, "last 100 texture uploads: layout object %llu ms, reading %llu ms, conversion %llu ms, upload %llu ms", (unsigned long long)g_time[0].exchange(0) / 1000,
                         (unsigned long long)g_time[1].exchange(0) / 1000, (unsigned long long)g_time[2].exchange(0) / 1000, (unsigned long long)g_time[3].exchange(0) / 1000);
        }
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
            if (it != g_tex.end()) { it->second.size = ctx.size; it->second.hash = hash; it->second.pending = false; it->second.rowBytes = e.rowBytes; it->second.rowBase = e.rowBase; }
        }
        if (InterlockedIncrement(&logged) <= 5 || log_enabled(CH, Level::Trace))
            XSL_INFO(CH, "texture at %llx (%llux%ux%u, %u mips, format %d, layout %#x, %llu bytes): detiled and uploaded", (unsigned long long)e.va,
                     (unsigned long long)e.desc.Width, e.desc.Height, e.desc.DepthOrArraySize, e.desc.MipLevels, (int)e.desc.Format, (unsigned)e.desc.Layout,
                     (unsigned long long)ctx.size);
    }
}
}  // namespace xs
