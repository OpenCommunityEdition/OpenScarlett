// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Console descriptor memory and its decoder (needed by titles built with D3DCOMPILE_NO_DEBUG).
//
// Titles built with D3DCOMPILE_NO_DEBUG (retail builds) do not call the runtime for two descriptor
// operations: d3d12_xs.h inlines CreateConstantBufferView, which writes a 16-byte Xbox buffer
// descriptor straight into descriptor memory, and CopyDescriptorsSimple (all heap types but
// samplers), which copies descriptor bytes with SSE stores using the console's descriptor sizes.
// So the CPU descriptor handles titles receive must point at memory the layer owns, laid out like
// the console's, and the layer must notice what the title wrote there.
//
// - Shadow memory: CBV/SRV/UAV handles point into one write-watched array of 32-byte console slots
//   that parallels the global shader-visible heap (xs_objects.cpp); RTV/DSV handles point into
//   per-heap arrays of 112/128-byte slots. Samplers are never inlined and keep PC handles.
// - Records: a descriptor the layer creates (Create*View) goes into an immutable PC "record" in a
//   ring of non-shader-visible descriptors; its slot holds a tag naming the record. Inline copies
//   move tags verbatim, so the copy still names the right descriptor.
// - Sync: before each ExecuteCommandLists the write watch lists the shadow pages written since the
//   last sync; every slot that differs from its snapshot is materialised into the global heap:
//   tags by copying their record, Xbox buffer descriptors by decoding them into a PC CBV.
// - RTV/DSV handles, and the CPU handle of UAV clears, are translated through their tags when used.
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "xs_cbv.h"
#include "xs_detile.h"
#include "xs_object.h"
#include "xs_unified.h"

namespace xs {
static bool table_slot_read(uint64_t pcGpu, uint32_t k, uint32_t* kind, uint32_t* titleSlot, bool* split);
static int table_slot_fix(uint64_t pcGpu, uint32_t k, uint32_t want);
static uint64_t table_fresh(uint64_t pcGpu, uint32_t count);
static bool table_slot_dump(uint64_t pcGpu, uint32_t k, const char* path);
static uint64_t table_memory(uint64_t pcGpu);
namespace {
constexpr uint32_t kViewSlot = 32, kRtvSlot = 112, kDsvSlot = 128;
constexpr uint32_t kMagic = 0x444C5358;  // "XSLD"
constexpr uint64_t kXboxCbvFormatMask = 0xFFFFFFFF00000000ull;

using Tag = DescTag;

// Ring of immutable PC descriptors for one heap type.
struct Records {
    PcObj heap = nullptr;
    uint64_t cpu = 0;
    uint32_t inc = 0, capacity = 0;
    uint64_t next = 0;
    std::vector<uint32_t> gen;
    // View records only: how many view slots name the record (their snapshots). A record in use is
    // passed over when the ring comes round: a title keeps the views of streamed resources for
    // minutes and copies them into tables all the while, by copying the tag.
    std::vector<uint32_t> refs;
    std::vector<uint8_t> kind;   // what the view is (desc_set_kind), for the table check
    std::vector<uint64_t> src;   // constant buffer views: the title's address of the range (desc_set_kind)
    uint64_t pinned = 0, passed = 0;
    bool init(PcObj dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t cap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{type, cap, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(pc::create_descriptor_heap(dev, &d, &heap))) return false;
        cpu = pc::heap_cpu_start(heap);
        inc = pc::descriptor_increment(dev, type);
        capacity = cap;
        gen.assign(cap, 0);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) { refs.assign(cap, 0); kind.assign(cap, 0); src.assign(cap, 0); }
        return true;
    }
    // A fresh record; its tag stays valid while a slot names it, and else until the ring wraps past it.
    Tag allocate(uint32_t type, uint64_t* handle) {
        if (!refs.empty())
            for (uint32_t tries = 0; tries < capacity && refs[next % capacity]; tries++) { next++; passed++; }
        uint32_t i = (uint32_t)(next % capacity);
        uint32_t g = (uint32_t)(next / capacity) + 1;
        next++;
        gen[i] = g;
        *handle = cpu + (uint64_t)i * inc;
        return Tag{kMagic, type, i, g};
    }
    bool resolve(const Tag& t, uint64_t* handle) const {
        if (t.magic != kMagic || t.record >= capacity || gen[t.record] != t.gen) return false;
        *handle = cpu + (uint64_t)t.record * inc;
        return true;
    }
    void ref(const Tag& t, int d) {
        if (refs.empty() || t.magic != kMagic || t.record >= capacity || gen[t.record] != t.gen) return;
        if (d > 0) { if (!refs[t.record]++) pinned++; }
        else if (refs[t.record] && !--refs[t.record]) pinned--;
    }
};

struct ShadowHeap {
    uint8_t* base;
    uint32_t count, slot;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
};

std::mutex g_mu;
bool g_ready = false;
Device* g_dev = nullptr;
Records g_rec[4];  // by D3D12_DESCRIPTOR_HEAP_TYPE (sampler unused)
// Global CBV/SRV/UAV shadow, parallel to the global shader-visible heap.
uint8_t* g_view = nullptr;
std::vector<uint8_t> g_snapshot;
uint32_t g_viewCount = 0;   // console-layout view slots (title heaps are numbered within these)
uint32_t g_pcCount = 0;     // slots of the PC shader-visible heap (title slots are placed in it by page: pc_slot)
constexpr uint32_t kPageSlots = 4096;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
std::vector<std::atomic<uint32_t>> g_pcPageOf;   // title page -> PC page + 1 (0: none now)
std::vector<uint32_t> g_pcPageOwner;             // PC page -> title page + 1
std::vector<std::atomic<uint64_t>> g_pgUsed;     // title page -> the present count at its last write or bind
std::vector<uint8_t> g_pgTable;                  // title page: tables were bound in it (it may lose its PC page while unused)
std::vector<uint8_t> g_pgWritten;                // title page: the mirror holds descriptors of it
// Title slot -> the kind of view the layer put in its PC slot in place of what the title's bytes
// say (table_slot_fix), until the title writes the slot again; 0: none.
std::vector<uint16_t> g_slotFix;
uint64_t g_fixNull = 0, g_fixBuffer = 0, g_fixNone = 0, g_fixCbv = 0, g_fixFresh = 0;
// The last pages of the PC heap (below the global-memory range) are kept for tables made afresh:
// a table that runs from one title page into the next, when that page is not behind the first in
// the PC heap, is copied there whole for the draw that reads it (a ring; a frame's lists have run
// long before it comes round).
constexpr uint32_t kFreshPages = 16;
uint32_t g_freshFirst = 0, g_freshNext = 0;   // first PC slot of the area, next free slot in it
// Title slot -> present count + 1 at which the copy its constant buffer view points at was made
// (views at console alignment point at copies in a ring that lasts a few frames); 0: none.
std::vector<uint32_t> g_slotRing;
uint64_t g_pgPlaced = 0, g_pgEvicted = 0, g_pgMoved = 0, g_pgNoRoom = 0, g_pgSplit = 0;
constexpr uint64_t kPageIdle = 4;                // presents without a use before a table page may lose its PC page
constexpr uint32_t kTableSpan = 1024;            // a table bound this close to its page's end may reach into the next page
uint64_t g_pcCpu = 0, g_pcGpu = 0;
uint32_t g_pcInc = 0;
// CPU-only mirror of the global view heap: slot i's current PC descriptor, the source for
// descriptor copies and clears (shader-visible heaps cannot be copied from), independent of the
// record ring (records of long-lived views are recycled while their slots are still copied).
PcObj g_mirror = nullptr;
uint64_t g_mirrorCpu = 0;
std::vector<ShadowHeap> g_rtvDsv;  // per RTV/DSV heap shadow arrays

bool enabled() {
    static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_SHADOW_DESCRIPTORS", b, 4) && b[0] == '0'); }();
    return on;
}

uint8_t* g_viewGpu = nullptr;   // GPU-readable copy of g_view (global memory at g_viewGpuAddr)
// Slot ranges whose bytes live in memory the title supplied (its XMem hooks answer the runtime's
// descriptor-heap allocation, and it checks that the heap's CPU handles point there). That memory
// is write-watched like g_view (xmem's xsl_xmem_write_watch); g_view is unused for those slots.
struct Seg { uint32_t first, count; uint8_t* base; };
std::vector<Seg> g_segs;
uint8_t* slot_ptr(uint32_t i) {
    for (auto& g : g_segs)
        if (i - g.first < g.count) return g.base + (size_t)(i - g.first) * kViewSlot;
    return g_view + (size_t)i * kViewSlot;
}
uint64_t g_viewGpuAddr = 0;
bool ensure(Device* dev) {
    if (g_ready) return true;
    PcObj pcdev = dev_extra(dev)->pcDevice;
    PcObj gh = pc::global_view_heap(pcdev);
    if (!gh) return false;
    g_dev = dev;
    D3D12_DESCRIPTOR_HEAP_DESC d;
    pc::heap_desc(gh, &d);
    g_pcCount = d.NumDescriptors;
    g_viewCount = std::max(pc::global_view_virtual_size(), g_pcCount);
    g_pcPageOf = std::vector<std::atomic<uint32_t>>(g_viewCount / kPageSlots + 1);
    g_pgUsed = std::vector<std::atomic<uint64_t>>(g_viewCount / kPageSlots + 1);
    g_pgTable.assign(g_viewCount / kPageSlots + 1, 0);
    g_pgWritten.assign(g_viewCount / kPageSlots + 1, 0);
    g_pcPageOwner.assign(std::min(g_pcCount, pc::gm_reserved_start()) / kPageSlots, 0);
    if (g_pcPageOwner.size() > 4 * kFreshPages) {
        g_pcPageOwner.resize(g_pcPageOwner.size() - kFreshPages);
        g_freshFirst = (uint32_t)g_pcPageOwner.size() * kPageSlots;
    }
    g_pcCpu = pc::heap_cpu_start(gh);
    g_pcGpu = pc::heap_gpu_start(gh);
    g_pcInc = pc::descriptor_increment(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    size_t bytes = (size_t)g_viewCount * kViewSlot;
    g_view = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!g_view) { XSL_ERR(CH, "descriptor shadow: VirtualAlloc(MEM_WRITE_WATCH) failed (%lu)", GetLastError()); return false; }
    g_snapshot.assign(bytes, 0);
    D3D12_DESCRIPTOR_HEAP_DESC md{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, g_viewCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};   // CPU-only: may exceed the PC limit
    if (FAILED(pc::create_descriptor_heap(pcdev, &md, &g_mirror))) { XSL_ERR(CH, "descriptor mirror heap (%u) failed", g_viewCount); return false; }
    g_mirrorCpu = pc::heap_cpu_start(g_mirror);
    if (!g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1u << 21) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1u << 14) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1u << 12))
        return false;
    // GPU-readable copy of the view slots (translated ray-tracing code reads descriptor memory):
    // at 0xF0000000 in global memory, refreshed from written pages at each submission.
    g_viewGpu = (uint8_t*)pc::gm_low_buffer(pcdev, 0, bytes, &g_viewGpuAddr);
    if (g_viewGpu) memcpy(g_viewGpu, g_view, bytes);
    pc::set_table_slot_reader(table_slot_read, table_slot_fix, table_fresh, table_slot_dump);
    pc::set_table_memory_reader(table_memory);
    g_slotFix.assign(g_viewCount, 0);
    g_slotRing.assign(g_viewCount, 0);
    XSL_INFO(CH, "descriptor memory in console layout (32/112/128-byte slots), synced to PC descriptors at submission");
    g_ready = true;
    return true;
}

uint64_t pc_view_cpu(uint32_t i) { return g_pcCpu + (uint64_t)i * g_pcInc; }
uint64_t mirror_cpu(uint32_t i) { return g_mirrorCpu + (uint64_t)i * g_pcInc; }
// Title view slots are numbered past what a PC heap holds (a driver limit of a million
// descriptors; one title's heap alone has 1.2 million: views it keeps, at the start, and rings it
// builds each frame's tables in, which come round every few seconds). The PC heap is therefore a
// cache of the title's heap in pages of 4096 slots, filled from the mirror:
// - A page takes its own position while that is free, so a title whose heap fits maps one to one.
//   Pages that are written but never bound as tables are taken for views shaders index directly:
//   they get their own position whoever holds it, and keep it.
// - A page tables are bound in may lose its PC page once it has not been used for kPageIdle
//   presents (the lists that named it have run by then). Such pages go after their predecessor's
//   place when they can, because a table that starts near the end of one continues in the next.
// The pages of the global-memory range at the end of the PC heap are never handed out.
void page_touch(uint32_t pg) {
    const uint64_t now = pc::present_count();
    if (g_pgUsed[pg].load(std::memory_order_relaxed) != now) g_pgUsed[pg].store(now, std::memory_order_relaxed);
}
// Title page pg now lies in PC page `at`: its descriptors from the mirror, and the payloads of its
// texture views name their new places (translated code indexes the PC heap with them).
void page_fill(uint32_t pg, uint32_t at) {
    const uint32_t first = pg * kPageSlots, n = std::min(kPageSlots, g_viewCount - first);
    if (!g_pgWritten[pg]) return;
    pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, n, pc_view_cpu(at * kPageSlots), mirror_cpu(first), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t* w = (uint32_t*)slot_ptr(first + k);
        if (!(w[1] & kDescTexture) || w[4] != kMagic || w[0] == at * kPageSlots + k) continue;
        w[0] = at * kPageSlots + k;
        memcpy(&g_snapshot[(size_t)(first + k) * kViewSlot], w, 4);
        if (g_viewGpu) memcpy(g_viewGpu + (size_t)(first + k) * kViewSlot, w, 4);
    }
}
// A PC page for title page pg (kNoSlot: none to be had). `want`: the PC page it should get, or
// none at all (the one after its predecessor's, for a table that spans both). The caller holds
// g_mu.
uint32_t page_place(uint32_t pg, uint32_t want = kNoSlot) {
    const uint32_t nPc = (uint32_t)g_pcPageOwner.size();
    const uint64_t now = pc::present_count();
    auto idle_for = [&](uint32_t q) { const uint64_t u = g_pgUsed[q].load(std::memory_order_relaxed); return now > u ? now - u : 0; };
    auto usable = [&](uint32_t at) {   // free, or held by a table page nothing in flight names
        if (at >= nPc) return false;
        const uint32_t o = g_pcPageOwner[at];
        return !o || (o - 1 != pg && g_pgTable[o - 1] && idle_for(o - 1) >= kPageIdle);
    };
    const bool table = g_pgTable[pg] || (pg && g_pgTable[pg - 1]);   // (a ring moves on from the page before)
    uint32_t take = kNoSlot;
    if (want != kNoSlot) {
        if (!usable(want)) return kNoSlot;
        take = want;
    }
    if (take == kNoSlot && table && pg) {   // after its predecessor
        const uint32_t prev = g_pcPageOf[pg - 1].load(std::memory_order_relaxed);
        if (prev && usable(prev)) take = prev;   // (prev is that page's PC page + 1)
    }
    if (take == kNoSlot && pg < nPc) {   // its own position (whoever holds it, for a page shaders may index directly)
        if (!g_pcPageOwner[pg] || usable(pg) || !table) take = pg;
    }
    if (take == kNoSlot) {
        // The start of the longest run of usable pages (the pages after this one will follow it);
        // with none usable, the table page unused for longest, if at least two presents old.
        uint32_t best = kNoSlot, bestLen = 0;
        for (uint32_t at = 0; at < nPc;) {
            if (!usable(at)) { at++; continue; }
            uint32_t e = at;
            while (e < nPc && usable(e)) e++;
            if (e - at > bestLen) { best = at; bestLen = e - at; }
            at = e;
        }
        if (best == kNoSlot) {
            uint64_t oldest = 1;
            for (uint32_t at = 0; at < nPc; at++) {
                const uint32_t o = g_pcPageOwner[at];
                if (o && o - 1 != pg && g_pgTable[o - 1] && idle_for(o - 1) > oldest) { oldest = idle_for(o - 1); best = at; }
            }
        }
        take = best;
    }
    if (take == kNoSlot) { g_pgNoRoom++; return kNoSlot; }
    if (const uint32_t o = g_pcPageOwner[take]) {   // its holder loses the page (and finds another at its next use)
        g_pcPageOf[o - 1].store(0, std::memory_order_release);
        if (g_pgEvicted++ == 0)
            XSL_INFO(CH, "the PC view heap (%u pages of %u slots) is smaller than what the title uses: table pages unused for %llu presents give up their place", nPc,
                     kPageSlots, (unsigned long long)kPageIdle);
    }
    if (const uint32_t had = g_pcPageOf[pg].load(std::memory_order_relaxed)) {   // moved: its old place is free
        g_pcPageOwner[had - 1] = 0;
        g_pgMoved++;
    }
    if (take != pg) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) == 1)
            XSL_INFO(CH, "title view slots past the PC heap (%u slots): pages of %u slots are placed where the PC heap has room", g_pcCount, kPageSlots);
    }
    g_pcPageOwner[take] = pg + 1;
    page_fill(pg, take);
    g_pgPlaced++;
    g_pcPageOf[pg].store(take + 1, std::memory_order_release);   // (after the fill: a writer waiting for g_mu publishes its slot again)
    return take;
}
// The PC slot of title slot i. A caller that may place a page (one not seen before) holds g_mu.
uint32_t pc_slot(uint32_t i) {
    const uint32_t pg = i / kPageSlots;
    if (pg >= g_pcPageOf.size()) return kNoSlot;
    page_touch(pg);
    uint32_t have = g_pcPageOf[pg].load(std::memory_order_acquire);
    if (!have) {
        const uint32_t at = page_place(pg);
        if (at == kNoSlot) return kNoSlot;
        have = at + 1;
    }
    return (have - 1) * kPageSlots + i % kPageSlots;
}
// Slot i's PC descriptor is in the mirror: publish it to the shader-visible heap.
uint32_t g_hi[17] = {};   // highest slot published per 125k-slot band (usage diagnostics)
uint64_t g_unbacked = 0;  // writes to slots past the PC heap
// Usage per 4096-slot page since the last report (XSL_VIEW_STATS=1): slots published, tables bound.
uint32_t g_pg_written[512] = {}, g_pg_bound[512] = {};
bool view_stats() { static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_VIEW_STATS", b, 4) && b[0] == '1'; }(); return on; }
void publish(uint32_t i) {
    if (i / 125000 < 17 && i > g_hi[i / 125000]) g_hi[i / 125000] = i;
    if (i / 4096 < 512) g_pg_written[i / 4096]++;
    g_pgWritten[i / kPageSlots] = 1;
    const uint32_t p = pc_slot(i);
    if (p == kNoSlot) {   // no page of the PC heap left: kept in descriptor memory only
        if (g_unbacked++ == 0)
            XSL_WARN(CH, "title view slot %u: the PC heap (%u slots) is full, the descriptor is not visible to PC descriptor tables", i, g_pcCount);
        return;
    }
    if (p >= pc::gm_reserved_start()) {   // (cannot happen: those pages are not handed out)
        static volatile long n = 0;
        if (InterlockedIncrement(&n) == 1)
            XSL_ERR(CH, "title view slot %u lies in the global-memory range (from %u): run with XSL_GLOBAL_MEMORY=0", i, pc::gm_reserved_start());
        return;
    }
    pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, 1, pc_view_cpu(p), mirror_cpu(i), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}
void set_slot(uint32_t i, uint64_t src) {   // src: a CPU-only PC descriptor
    pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, 1, mirror_cpu(i), src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    publish(i);
}
bool view_slot(uint64_t h, uint32_t* i) {
    if (g_view && h >= (uint64_t)g_view && h < (uint64_t)g_view + (uint64_t)g_viewCount * kViewSlot) {
        *i = (uint32_t)((h - (uint64_t)g_view) / kViewSlot);
        return true;
    }
    for (auto& g : g_segs)
        if (h >= (uint64_t)g.base && h < (uint64_t)g.base + (uint64_t)g.count * kViewSlot) {
            *i = g.first + (uint32_t)((h - (uint64_t)g.base) / kViewSlot);
            return true;
        }
    return false;
}
const ShadowHeap* rtv_dsv_heap(uint64_t h) {
    for (auto& s : g_rtvDsv)
        if (h >= (uint64_t)s.base && h < (uint64_t)s.base + (uint64_t)s.count * s.slot) return &s;
    return nullptr;
}

// Nothing in the slot: zeros, or a single index (a title keeps its free slots as a linked list in
// the slots themselves).
bool slot_blank(const uint8_t* s) {
    const uint32_t* w = (const uint32_t*)s;
    for (int k = 1; k < 8; k++)
        if (w[k]) return false;
    return true;
}
// The console GPU's own buffer descriptor, which titles also write into a slot themselves (16
// bytes; what the slot's other half held stays): the address (48 bits), the element size in bytes
// (14 bits; 0: the count is in bytes), the element count, and a dword with the format (bits 12..18)
// whose top two bits are 0 for a buffer.
bool native_buffer(const uint8_t* s, uint64_t* a, uint32_t* stride, uint64_t* bytes) {
    const uint32_t* w = (const uint32_t*)s;
    *a = w[0] | (uint64_t)(w[1] & 0xffff) << 32;
    *stride = (w[1] >> 16) & 0x3fff;
    *bytes = *stride ? (uint64_t)*stride * w[2] : w[2];
    return *a && w[2] && !(w[1] >> 30) && !(w[3] >> 30) && ((w[3] >> 12) & 0x7f) && *bytes <= (1u << 28);
}
uint64_t g_nativeViews = 0, g_nativeNone = 0, g_undecoded = 0;
// The PC buffer and offset of an address a descriptor names: a PC GPU address of zero-copy memory,
// a title address of unified memory, of a buffer registered in global memory, or of page-mapped
// pool memory (in that order, as gpu_va resolves them).
bool resolve_buffer(uint64_t a, PcObj* res, uint64_t* off) {
    UnifiedRef u{};
    if (unified_from_gpu(a, &u) || unified_lookup(g_dev, a, &u)) { *res = u.res; *off = u.offset; return u.res != nullptr; }
    uint64_t avail = 0;
    if (pc::gm_resolve(a, res, off, &avail) && *res) return true;
    uint64_t run = 0;
    if (section_lookup(g_dev, a, &u, &run) && u.res) { *res = u.res; *off = u.offset; return true; }
    return false;
}
// Decode a console buffer descriptor that an inlined CreateConstantBufferView wrote.
bool materialize_xbox_cbv(uint32_t i, const uint64_t* q) {
    uint64_t addr, size;
    const uint32_t* w = (const uint32_t*)q;
    if ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask)) {
        addr = q[0] & ((1ull << 48) - 1);
        size = (q[1] & 0xFFFFFFFFull) << 4;
    } else if (q[0] && q[0] == q[2] && w[2] && !(w[2] & 31) && w[6] == (uint32_t)(q[0] >> 6) && (w[7] >> 20) == w[2] / 32 && !(w[7] & 0xFFFFF)) {
        // The form newer runtimes inline: the view description as the title gave it (address, size
        // in bytes; the fourth dword is padding the title left as it was), then the address again,
        // the address in 64-byte units and the size in 32-byte units (shifted by 20).
        addr = q[0];
        size = w[2];
    } else if (q[0] && q[3] == q[0] && w[5] &&
               ((w[4] == 0x80000000u && !(w[5] & 3) && w[5] <= (1u << 24) && w[2] == w[5] / 4 - 1) ||
                (w[4] && w[4] <= 65536 && !(w[4] & 3) && w[2] == w[5] - 1 && (uint64_t)w[4] * w[5] <= (1u << 28)) ||
                (w[4] != 0x80000000u && !(w[5] & 3) && w[5] <= (1u << 24) && w[2] == w[5] / 4 - 1))) {
        // An inlined shader view of a small buffer (a title's movie and UI quads keep their
        // transforms in one): the address, the size in dwords less one, a dword the title leaves
        // as it was, flags, the size in bytes, and the address again. The element size is the
        // shader's business on the console; here the view is made of 64-byte elements when the
        // size allows (matrices), else of dwords.
        // Two forms: dword 4 is 0x80000000 for a raw view (dword 5 its size in bytes, dword 2 its
        // dwords less one), or the element size (dword 5 the element count, dword 2 that less one).
        const uint64_t a = q[0];
        const bool counted = w[4] != 0x80000000u && w[4] && w[4] <= 65536 && !(w[4] & 3) && w[2] == w[5] - 1;
        // (The counted form names the element size the shader's code was built with, and the PC view
        // has to have that one: with another, what a shader reads past the view's end is not
        // defined, and GPU-based validation names every such draw.)
        const uint32_t bytes = counted ? w[4] * w[5] : w[5], stride = counted ? w[4] : bytes % 64 == 0 ? 64 : 4;
        {
            static volatile long seen[5] = {};
            const int cls = !counted ? 0 : w[4] == 4 ? 1 : w[4] == 16 ? 2 : w[4] == 64 ? 3 : 4;
            if (InterlockedIncrement(&seen[cls]) == 1)
                XSL_INFO(CH, "inline buffer view, first of its kind: %s, element size %u, %u elements, %u bytes", counted ? "counted" : "raw", counted ? w[4] : stride, counted ? w[5] : bytes / stride, bytes);
        }
        UnifiedRef u{};
        PcObj pcd = dev_extra(g_dev)->pcDevice;
        bool ok = unified_from_gpu(a, &u) || unified_lookup(g_dev, a, &u);
        uint64_t off = u.offset;
        if (!ok) { PcObj r = nullptr; uint64_t avail = 0; ok = pc::gm_resolve(a, &r, &off, &avail) && r; u.res = r; }
        static volatile long n = 0;
        const long k = InterlockedIncrement(&n);
        if (k == 1 || (!ok && k <= 8))
            XSL_INFO(CH, "inline buffer view of %u bytes at %llx: %s", bytes, (unsigned long long)a, ok ? "a shader view of its PC buffer" : "no PC memory known there, null view");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.StructureByteStride = stride;
        sd.Buffer.NumElements = bytes / stride;
        if (ok && off % stride == 0) {
            sd.Buffer.FirstElement = off / stride;
            pc::create_srv(pcd, u.res, &sd, mirror_cpu(i));
        } else {
            sd.Buffer.NumElements = 1;
            pc::create_srv(pcd, nullptr, &sd, mirror_cpu(i));
        }
        publish(i);
        return true;
    } else {
        // The console's own descriptor, of any element size: a view of the same bytes in dwords
        // (the table check gives it the form a shader declares), or of nothing when no PC memory
        // is known there. Left as it was, the PC slot held the view of whatever was there before.
        uint64_t a = 0, bytes = 0;
        uint32_t stride = 0;
        if (!native_buffer((const uint8_t*)q, &a, &stride, &bytes)) return false;
        UnifiedRef u{};
        PcObj pcd = dev_extra(g_dev)->pcDevice;
        uint64_t off = 0;
        bool ok = resolve_buffer(a, &u.res, &off);
        ok = ok && !(off & 3) && bytes >= 4;
        if (!ok) {
            static volatile long said = 0;
            const uint32_t* w = (const uint32_t*)q;
            if (InterlockedIncrement(&said) <= 40)
                XSL_INFO(CH, "view slot %u: no PC memory for the console buffer descriptor %08x %08x %08x %08x | %08x %08x %08x %08x (%llu bytes at %llx, offset %llx)", i, w[0], w[1], w[2], w[3],
                         w[4], w[5], w[6], w[7], (unsigned long long)bytes, (unsigned long long)a, (unsigned long long)off);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.StructureByteStride = 4;
        sd.Buffer.NumElements = ok ? (UINT)(bytes / 4) : 1;
        sd.Buffer.FirstElement = ok ? off / 4 : 0;
        pc::create_srv(pcd, ok ? u.res : nullptr, &sd, mirror_cpu(i));
        const uint64_t k = ok ? ++g_nativeViews : ++g_nativeNone;
        if (k <= 3)
            XSL_INFO(CH, "view slot %u holds the console's own buffer descriptor (%llu bytes in elements of %u at %llx): %s", i, (unsigned long long)bytes, stride, (unsigned long long)a,
                     ok ? "a shader view of its PC buffer" : "no PC memory known there, a view of nothing");
        publish(i);
        return true;
    }
    PcObj pcdev = dev_extra(g_dev)->pcDevice;
    if (!addr) {
        pc::create_cbv(pcdev, nullptr, mirror_cpu(i));
        publish(i);
        return true;
    }
    uint64_t gpu = gpu_va(g_dev, addr);
    if (gpu & 255) {   // console CBVs need only 16-byte alignment: a copy (made anew in each frame the view is read, see table_slot_fix)
        gpu = cbv_fix_view(g_dev, addr, gpu, size);
        if (i < g_slotRing.size()) g_slotRing[i] = (uint32_t)pc::present_count() + 1;
    }
    if (gpu & 255) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "inline CBV at %llx is not 256-aligned and cannot be re-aligned", (unsigned long long)addr);
        return true;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC d{gpu, (UINT)((size + 255) & ~255ull)};
    if (d.SizeInBytes > 65536) d.SizeInBytes = 65536;
    pc::create_cbv(pcdev, &d, mirror_cpu(i));
    publish(i);
    return true;
}

constexpr size_t kTagOffset = 16;   // view slots: payload in bytes 0..15, tag in 16..31
// Slot i's snapshot becomes the bytes at s; the records the old and the new tag name follow.
void snapshot_set(uint32_t i, const void* s) {
    uint8_t* d = &g_snapshot[(size_t)i * kViewSlot];
    if (i < g_slotFix.size()) { g_slotFix[i] = 0; g_slotRing[i] = 0; }
    Tag o, n;
    memcpy(&o, d + kTagOffset, sizeof o);
    memcpy(&n, (const uint8_t*)s + kTagOffset, sizeof n);
    if (memcmp(&o, &n, sizeof o) != 0) {
        g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].ref(o, -1);
        g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].ref(n, +1);
    }
    memcpy(d, s, kViewSlot);
}
bool g_payload_dirty = false;       // payloads written by copies since the last sync
thread_local uint32_t t_payload[4];
thread_local bool t_has_payload = false;
// A slot whose bytes the layer cannot make a view of: a buffer view of no resource in the PC heap
// (a shader that reads it gets zeros; the view the slot had before could be of a released resource,
// and reading that took the device down). The table check changes its kind to what a shader declares.
void slot_nothing(uint32_t i) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.StructureByteStride = 4;
    sd.Buffer.NumElements = 1;
    pc::create_srv(dev_extra(g_dev)->pcDevice, nullptr, &sd, mirror_cpu(i));
    g_undecoded++;
    if (i / kPageSlots < g_pcPageOf.size() && g_pcPageOf[i / kPageSlots].load(std::memory_order_acquire)) publish(i);
    else g_pgWritten[i / kPageSlots] = 1;   // (no PC page yet, and none is taken for this: it comes with the page's fill)
}
// Make global slot i match its shadow bytes. Caller holds g_mu.
void materialize(uint32_t i) {
    const uint8_t* s = slot_ptr(i);
    Tag t;
    memcpy(&t, s + kTagOffset, sizeof t);
    uint64_t rec;
    if (t.magic == kMagic) {
        if (g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].resolve(t, &rec)) {
            set_slot(i, rec);
        } else {
            // (The title wrote its own descriptor over a slot whose other half still held an old
            // tag, or the view was released: the PC slot must not keep what it had.)
            if (!materialize_xbox_cbv(i, (const uint64_t*)s)) slot_nothing(i);
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 6) {
                const Records& r = g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
                const uint32_t* w = (const uint32_t*)s;
                XSL_WARN(CH, "descriptor slot %u names a record that is not there (type %u, record %u of %u, generation %u, the record's %u; payload %08x %08x %08x %08x)", i, t.type,
                         t.record, r.capacity, t.gen, t.record < r.capacity ? r.gen[t.record] : 0, w[0], w[1], w[2], w[3]);
            }
        }
    } else if (!materialize_xbox_cbv(i, (const uint64_t*)s)) {
        if (slot_blank(s)) {
            if (!slot_blank(&g_snapshot[(size_t)i * kViewSlot])) slot_nothing(i);   // (cleared by the title: the view it had goes)
        } else {
            slot_nothing(i);
            static volatile long n = 0;
            const uint32_t* w = (const uint32_t*)s;
            if (InterlockedIncrement(&n) <= 12)
                XSL_WARN(CH, "descriptor slot %u holds console bytes the layer cannot decode yet: %08x %08x %08x %08x %08x %08x %08x %08x", i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
    snapshot_set(i, s);
}
}  // namespace

bool desc_shadow_enabled() { return enabled(); }

// ---- Heaps -------------------------------------------------------------------------------------
uint64_t desc_view_cpu_start(Device* dev, uint32_t offset) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!enabled() || !ensure(dev)) return 0;
    return (uint64_t)g_view + (uint64_t)offset * kViewSlot;
}
uint64_t desc_view_gpu_start(uint32_t offset) { return g_pcGpu + (uint64_t)offset * kViewSlot; }
// Global-memory address of the descriptor memory at console GPU handle h (0 if none).
// Diagnostics: the first 16 bytes (payload) of the view slot `slot` entries after a GPU handle.
bool desc_peek(uint64_t h, uint32_t slot, uint32_t out[8]) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return false;
    size_t off = (size_t)(h - g_pcGpu) + (size_t)slot * kViewSlot;
    if (off + kViewSlot > (size_t)g_viewCount * kViewSlot) return false;
    memcpy(out, slot_ptr((uint32_t)(off / kViewSlot)), kViewSlot);
    static volatile long once = 0;
    if (InterlockedIncrement(&once) == 200) {   // diagnostics: how much of the shadow memory holds anything
        uint64_t nz = 0, tagged = 0, first = 0, last = 0;
        for (uint32_t i = 0; i < g_viewCount; i++) {
            const uint32_t* w = (const uint32_t*)slot_ptr(i);
            bool any = false;
            for (int k = 0; k < 8; k++) any |= w[k] != 0;
            if (!any) continue;
            if (!nz) first = i;
            last = i;
            nz++;
            Tag t;
            memcpy(&t, (const uint8_t*)w + kTagOffset, sizeof t);
            tagged += t.magic == 0x444C5358u;
        }
        XSL_INFO(CH, "descriptor memory: %llu of %u slots hold data (%llu tagged), first %llu, last %llu; g_view %p, GPU start %llx", (unsigned long long)nz, g_viewCount,
                 (unsigned long long)tagged, (unsigned long long)first, (unsigned long long)last, (void*)g_view, (unsigned long long)g_pcGpu);
    }
    return true;
}
static uint32_t table_slot(uint64_t i);
uint64_t desc_memory_address(uint64_t h) {
    if (!g_viewGpu || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return 0;
    table_slot((h - g_pcGpu) / kViewSlot);   // (the code reads PC slot numbers from the payloads there: the page stays placed)
    return g_viewGpuAddr + (h - g_pcGpu);
}
uint64_t desc_rtv_dsv_heap(Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!enabled() || !ensure(dev)) return 0;
    uint32_t slot = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? kRtvSlot : kDsvSlot;
    auto* p = (uint8_t*)VirtualAlloc(nullptr, (size_t)count * slot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return 0;
    g_rtvDsv.push_back({p, count, slot, type});
    return (uint64_t)p;
}
void desc_rtv_dsv_heap_free(uint64_t base) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto it = g_rtvDsv.begin(); it != g_rtvDsv.end(); ++it)
        if ((uint64_t)it->base == base) {
            VirtualFree(it->base, 0, MEM_RELEASE);
            g_rtvDsv.erase(it);
            return;
        }
}
uint32_t desc_increment(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t pcInc) {
    if (!enabled()) return pcInc;
    switch (t) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return kViewSlot;
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return kRtvSlot;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return kDsvSlot;
        default: return pcInc;
    }
}

// ---- Descriptor creation: returns the PC handle to write, or 0 to use `h` itself ----------------
// After the PC descriptor is written there, desc_commit() publishes it to the title's slot.
thread_local uint8_t t_kind = 0;
thread_local uint64_t t_src = 0;
void desc_set_kind(uint32_t kind, uint64_t titleAddress) { t_kind = (uint8_t)kind; t_src = titleAddress; }
// Diagnostics (XSL_TABLE_CHECK): what the title's slot holds that lies k entries after the table
// bound at PC handle h. kind: 0 nothing, 2 console bytes written in line (a constant or buffer
// view), 0xfe a tag whose record is gone, 0xff a view of unknown kind, else as desc_set_kind.
static bool table_slot_read(uint64_t pcGpu, uint32_t k, uint32_t* kind, uint32_t* titleSlot, bool* split) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || pcGpu < g_pcGpu) return false;
    const uint64_t p = (pcGpu - g_pcGpu) / g_pcInc;
    if (p / kPageSlots >= g_pcPageOwner.size() || !g_pcPageOwner[p / kPageSlots]) return false;
    const uint64_t t = (uint64_t)(g_pcPageOwner[p / kPageSlots] - 1) * kPageSlots + p % kPageSlots + k;
    if (t >= g_viewCount) return false;
    *titleSlot = (uint32_t)t;
    const uint32_t have = g_pcPageOf[t / kPageSlots].load(std::memory_order_relaxed);
    *split = !have || (uint64_t)(have - 1) * kPageSlots + t % kPageSlots != p + k;
    const uint8_t* s = slot_ptr((uint32_t)t);
    static const uint8_t zero[kViewSlot] = {};
    Tag tg;
    memcpy(&tg, s + 16, sizeof tg);
    const Records& r = g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
    uint64_t rec;
    if (g_slotFix[t] && !memcmp(s, &g_snapshot[(size_t)t * kViewSlot], kViewSlot)) *kind = 0x100 | (g_slotFix[t] & 0xff);   // (given that kind by the layer)
    else if (slot_blank(s)) *kind = 0;
    else if (tg.magic != 0x444C5358u) *kind = 2;
    else if (!r.resolve(tg, &rec)) *kind = 0xfe;
    else *kind = r.kind[tg.record] ? r.kind[tg.record] : 0xff;
    return true;
}
// A shader reads the title's slot k entries after the table at PC handle h as `want` (bits 0..7 as
// desc_set_kind, bits 8..9 for buffers: 0 typed, 1 raw, 2 structured) and the slot holds something
// else. Two cases are put right in the PC heap, because a PC GPU takes a descriptor for what the
// shader declares and what follows is not defined (devices were hung by it):
// - the title left the slot empty (code that never reads it on its path): a view of no resource;
// - the title made a constant buffer view where the shader reads a buffer (on the console both are
//   an address and a size): a buffer view of the same bytes, in 4-byte elements, which takes any
//   offset a console constant view has; the shader's own element size indexes it.
// Returns 1 or 2 for these, 0 when nothing was to do, -1 when the slot holds a view the layer does
// not replace.
static int table_slot_fix(uint64_t pcGpu, uint32_t k, uint32_t want) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || pcGpu < g_pcGpu) return 0;
    const uint64_t p = (pcGpu - g_pcGpu) / g_pcInc;
    if (p / kPageSlots >= g_pcPageOwner.size() || !g_pcPageOwner[p / kPageSlots]) return 0;
    const uint64_t t64 = (uint64_t)(g_pcPageOwner[p / kPageSlots] - 1) * kPageSlots + p % kPageSlots + k;
    if (t64 >= g_viewCount) return 0;
    const uint32_t t = (uint32_t)t64;
    const uint8_t* s = slot_ptr(t);
    if (memcmp(s, &g_snapshot[(size_t)t * kViewSlot], kViewSlot) != 0) materialize(t);   // (written since the last submission)
    if (g_slotFix[t] == (uint16_t)(want | 0x8000)) return 0;
    static const uint8_t zero[kViewSlot] = {};
    Tag tg;
    memcpy(&tg, s + 16, sizeof tg);
    const Records& r = g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
    uint64_t rec;
    const bool empty = slot_blank(s);
    uint32_t cur = empty ? 0 : tg.magic != 0x444C5358u ? 2 : !r.resolve(tg, &rec) ? 0xfe : r.kind[tg.record];
    if (cur == 0xfe) cur = 2;   // (an old tag behind bytes the title wrote: read as console bytes)
    const uint32_t cls = want & 0x30, dim = want & 15, form = (want >> 8) & 3;
    PcObj dev = dev_extra(g_dev)->pcDevice;
    // The range a constant buffer view names: the title's address and the size.
    auto range_of = [&](uint64_t* a, uint64_t* bytes) {
        const uint64_t* q = (const uint64_t*)s;
        const uint32_t* w = (const uint32_t*)s;
        *a = *bytes = 0;
        if (cur == 1) { *a = r.src[tg.record]; *bytes = w[2]; }
        else if ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask)) { *a = q[0] & ((1ull << 48) - 1); *bytes = (q[1] & 0xFFFFFFFFull) << 4; }
        else if (q[0] && q[0] == q[2] && w[2] && !(w[2] & 31) && w[6] == (uint32_t)(q[0] >> 6)) { *a = q[0]; *bytes = w[2]; }
    };
    // A constant buffer view read as one: when the title's address is at console alignment the
    // PC view points at a copy in a ring of a few frames. A title makes such views once and
    // rewrites the constants every frame, so the copy is made anew in each frame the view is read
    // in (it was the first frame's constants for four frames, then whatever the ring held next).
    if (want == 1 && (cur == 1 || cur == 2)) {
        uint64_t a = 0, bytes = 0;
        range_of(&a, &bytes);
        if (!a) return 0;
        const uint64_t gpu = gpu_va(g_dev, a);
        if (!(gpu & 255)) return 0;
        const uint32_t now = (uint32_t)pc::present_count() + 1;
        if (g_slotRing[t] == now) return 0;
        if (!bytes) bytes = 4096;
        const uint64_t copy = cbv_fix_view(g_dev, a, gpu, bytes);
        if (copy & 255) return -1;
        D3D12_CONSTANT_BUFFER_VIEW_DESC d{copy, (UINT)std::min<uint64_t>(65536, (bytes + 255) & ~255ull)};
        pc::create_cbv(dev, &d, mirror_cpu(t));
        publish(t);
        g_slotRing[t] = now;
        g_fixCbv++;
        return 3;
    }
    auto srv_buffer = [&](PcObj res, uint64_t first, uint32_t count) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = form == 0 ? DXGI_FORMAT_R32_UINT : form == 1 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        sd.Buffer.FirstElement = first;
        sd.Buffer.NumElements = count ? count : 1;
        sd.Buffer.StructureByteStride = form == 2 ? 4 : 0;
        sd.Buffer.Flags = form == 1 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        pc::create_srv(dev, res, &sd, mirror_cpu(t));
    };
    // (Console bytes that name no memory are a view of nothing too.)
    bool nothing = empty;
    if (cur == 2 && !nothing) {
        const uint64_t* q = (const uint64_t*)s;
        const uint32_t* w = (const uint32_t*)s;
        nothing = ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask) && !(q[0] & ((1ull << 48) - 1))) || (!q[0] && !w[2] && !q[3]);
    }
    if (nothing && cur == 2 && want == 1) return 0;   // (a constant buffer view of nothing is what the sync made of it)
    // A view of no resource of another kind than declared (a title binds its table of such views,
    // one of each kind, where a program's table is not in use): one of the declared kind.
    if (!nothing && cur > 2 && cur < 0xfe && (cur & 0x80) && (cur & 0x7f) != (want & 0xff) && want != 1) nothing = true;
    // Console bytes that name memory, read as a buffer: a view of those bytes in the form the
    // shader declares. Any other pairing of console bytes and a declaration, and bytes that name
    // no PC memory, become a view of nothing of the declared kind below.
    uint64_t nat_a = 0, nat_bytes = 0;
    UnifiedRef nat_u{};
    uint64_t nat_off = 0;
    bool nat_ok = false;
    if (!nothing && (cur == 1 || cur == 2) && dim == 1 && want != 1 && (cls == 0x10 || cls == 0x20) && !(cls == 0x10 && form == 0 && false)) {
        const uint64_t* q = (const uint64_t*)s;
        const uint32_t* w = (const uint32_t*)s;
        uint32_t st = 0;
        if (cur == 1) { nat_a = r.src[tg.record] ? r.src[tg.record] : w[0] | (uint64_t)(w[1] & 0xffff) << 32; nat_bytes = w[2]; }
        else if ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask)) { nat_a = gpu_va(g_dev, q[0] & ((1ull << 48) - 1)); nat_bytes = (q[1] & 0xFFFFFFFFull) << 4; }
        else if (q[0] && q[0] == q[2] && w[2] && !(w[2] & 31) && w[6] == (uint32_t)(q[0] >> 6)) { nat_a = gpu_va(g_dev, q[0]); nat_bytes = w[2]; }
        else if (q[0] && q[3] == q[0] && w[5] && !(w[5] & 3)) { nat_a = q[0]; nat_bytes = w[5]; }
        else if (!native_buffer(s, &nat_a, &st, &nat_bytes)) nat_a = nat_bytes = 0;
        nat_ok = nat_a && nat_bytes >= 4 && resolve_buffer(nat_a, &nat_u.res, &nat_off);
        if (nat_off & 3) nat_ok = false;
        if (nat_ok && cls == 0x10) {
            srv_buffer(nat_u.res, nat_off / 4, (uint32_t)(nat_bytes / 4));
            publish(t);
            g_slotFix[t] = (uint16_t)(want | 0x8000);
            if (g_fixBuffer++ < 4)
                XSL_INFO(CH, "view slot %u holds a constant buffer view (%llu bytes at %llx) where a shader reads a buffer: a buffer view of the same bytes takes its place", t,
                         (unsigned long long)nat_bytes, (unsigned long long)nat_a);
            return 2;
        }
        if (nat_ok && cls == 0x20) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Format = form == 0 ? DXGI_FORMAT_R32_UINT : form == 1 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
            ud.Buffer.FirstElement = nat_off / 4;
            ud.Buffer.NumElements = (UINT)(nat_bytes / 4);
            ud.Buffer.StructureByteStride = form == 2 ? 4 : 0;
            ud.Buffer.Flags = form == 1 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
            pc::create_uav(dev, nat_u.res, nullptr, &ud, mirror_cpu(t));
            publish(t);
            g_slotFix[t] = (uint16_t)(want | 0x8000);
            g_fixBuffer++;
            return 2;
        }
        const uint32_t* w2 = (const uint32_t*)s;
        if (g_fixNone++ < 6)
            XSL_WARN(CH, "view slot %u holds console bytes (%llu bytes at %llx, kind %u) where a shader reads a buffer: %s, a view of nothing takes its place (slot bytes %08x %08x %08x %08x %08x %08x %08x %08x)",
                     t, (unsigned long long)nat_bytes, (unsigned long long)nat_a, cur, nat_a ? "no PC buffer is known at that address" : "they name no memory the layer can decode", w2[0], w2[1],
                     w2[2], w2[3], w2[4], w2[5], w2[6], w2[7]);
        nothing = true;
    } else if (!nothing && cur == 2 && want != 1) {
        g_fixNone++;
        nothing = true;   // (console bytes where a texture or an acceleration structure is declared)
    }
    if (nothing) {
        if (want == 1) pc::create_cbv(dev, nullptr, mirror_cpu(t));
        else if (cls == 0x10) {
            if (dim == 1) srv_buffer(nullptr, 0, 1);
            else if (dim == 11) return -1;
            else {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = (D3D12_SRV_DIMENSION)dim;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                switch (dim) {
                    case 2: sd.Texture1D.MipLevels = 1; break;
                    case 3: sd.Texture1DArray.MipLevels = 1; sd.Texture1DArray.ArraySize = 1; break;
                    case 4: sd.Texture2D.MipLevels = 1; break;
                    case 5: sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = 1; break;
                    case 7: sd.Texture2DMSArray.ArraySize = 1; break;
                    case 8: sd.Texture3D.MipLevels = 1; break;
                    case 9: sd.TextureCube.MipLevels = 1; break;
                    case 10: sd.TextureCubeArray.MipLevels = 1; sd.TextureCubeArray.NumCubes = 1; break;
                    default: break;
                }
                pc::create_srv(dev, nullptr, &sd, mirror_cpu(t));
            }
        } else if (cls == 0x20) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = (D3D12_UAV_DIMENSION)dim;
            ud.Format = dim == 1 ? (form == 0 ? DXGI_FORMAT_R32_UINT : form == 1 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN) : DXGI_FORMAT_R8G8B8A8_UNORM;
            if (dim == 1) { ud.Buffer.NumElements = 1; ud.Buffer.StructureByteStride = form == 2 ? 4 : 0; ud.Buffer.Flags = form == 1 ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE; }
            else if (dim == 3) ud.Texture1DArray.ArraySize = 1;
            else if (dim == 5) ud.Texture2DArray.ArraySize = 1;
            else if (dim == 8) ud.Texture3D.WSize = 1;
            pc::create_uav(dev, nullptr, nullptr, &ud, mirror_cpu(t));
        } else return -1;
        publish(t);
        g_slotFix[t] = (uint16_t)(want | 0x8000);
        g_fixNull++;
        return 1;
    }
    if ((cur == 1 || cur == 2) && cls == 0x10 && dim == 1 && form != 0) {
        const uint64_t* q = (const uint64_t*)s;
        const uint32_t* w = (const uint32_t*)s;
        uint64_t a = 0, bytes = 0;
        if (cur == 1) { a = r.src[tg.record] ? r.src[tg.record] : w[0] | (uint64_t)(w[1] & 0xffff) << 32; bytes = w[2]; }
        else if ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask)) { a = gpu_va(g_dev, q[0] & ((1ull << 48) - 1)); bytes = (q[1] & 0xFFFFFFFFull) << 4; }
        else if (q[0] && q[0] == q[2] && w[2] && !(w[2] & 31) && w[6] == (uint32_t)(q[0] >> 6)) { a = gpu_va(g_dev, q[0]); bytes = w[2]; }
        else if (q[0] && q[3] == q[0] && w[5] && !(w[5] & 3)) { a = q[0]; bytes = w[5]; }
        UnifiedRef u{};
        bool ok = a && (unified_lookup(g_dev, a, &u) || unified_from_gpu(a, &u));
        uint64_t off = u.offset;
        if (a && !ok) { PcObj res = nullptr; uint64_t avail = 0; ok = pc::gm_resolve(a, &res, &off, &avail) && res; u.res = res; }
        if (!ok || (off & 3)) {
            if (g_fixNone++ < 6)
                XSL_WARN(CH, "view slot %u holds a constant buffer view (%llu bytes at %llx, kind %u) where a shader reads a buffer: %s (slot bytes %08x %08x %08x %08x %08x %08x %08x %08x)", t,
                         (unsigned long long)bytes, (unsigned long long)a, cur, ok ? "its offset is not a multiple of 4" : "no PC buffer is known at that address", w[0], w[1], w[2], w[3],
                         w[4], w[5], w[6], w[7]);
            return -1;
        }
        srv_buffer(u.res, off / 4, (uint32_t)(bytes / 4));
        publish(t);
        g_slotFix[t] = (uint16_t)(want | 0x8000);
        if (g_fixBuffer++ < 4)
            XSL_INFO(CH, "view slot %u holds a constant buffer view (%llu bytes at %llx) where a shader reads a buffer: a buffer view of the same bytes takes its place", t,
                     (unsigned long long)bytes, (unsigned long long)a);
        return 2;
    }
    return -1;
}
// Diagnostics (XSL_TABLE_DUMP): the bytes a constant buffer view names, written to a file (to run
// a dispatch again outside the title with the constants it had).
static bool table_slot_dump(uint64_t pcGpu, uint32_t k, const char* path) {
    std::vector<uint8_t> data;
    uint64_t dumpAt = 0;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_ready || pcGpu < g_pcGpu) return false;
        const uint64_t p = (pcGpu - g_pcGpu) / g_pcInc;
        if (p / kPageSlots >= g_pcPageOwner.size() || !g_pcPageOwner[p / kPageSlots]) return false;
        const uint64_t t = (uint64_t)(g_pcPageOwner[p / kPageSlots] - 1) * kPageSlots + p % kPageSlots + k;
        if (t >= g_viewCount) return false;
        const uint8_t* s = slot_ptr((uint32_t)t);
        const uint64_t* q = (const uint64_t*)s;
        const uint32_t* w = (const uint32_t*)s;
        Tag tg;
        memcpy(&tg, s + 16, sizeof tg);
        const Records& r = g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
        uint64_t rec, a = 0, bytes = 0;
        if (tg.magic == 0x444C5358u) { if (r.resolve(tg, &rec) && r.kind[tg.record] == 1) { a = r.src[tg.record]; bytes = w[2]; } }
        else if ((q[1] & kXboxCbvFormatMask) == (0x0002403C00000000ull & kXboxCbvFormatMask)) { a = q[0] & ((1ull << 48) - 1); bytes = (q[1] & 0xFFFFFFFFull) << 4; }
        else if (q[0] && q[0] == q[2] && w[2] && !(w[2] & 31) && w[6] == (uint32_t)(q[0] >> 6)) { a = q[0]; bytes = w[2]; }
        static volatile long shown = 0;
        dumpAt = a;
        if (!a || !bytes || bytes > 65536) {
            if (InterlockedIncrement(&shown) <= 6)
                XSL_INFO(CH, "table dump: slot %llu names no constants (address %llx, %llu bytes; slot bytes %08x %08x %08x %08x %08x %08x %08x %08x)", (unsigned long long)t,
                         (unsigned long long)a, (unsigned long long)bytes, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            return false;
        }
        data.resize((size_t)bytes);
        SIZE_T got = 0;
        if ((!ReadProcessMemory(GetCurrentProcess(), (const void*)a, data.data(), data.size(), &got) || got != data.size()) &&
            !unified_read_gpu(a, data.data(), data.size()) && !mapped_buffer_read_gpu(a, data.data(), data.size()) &&
            !cbv_read_gpu(a, data.data(), data.size()) &&
            !(pc::gm_title_of_gpu(a) && ReadProcessMemory(GetCurrentProcess(), (const void*)pc::gm_title_of_gpu(a), data.data(), data.size(), &got) && got == data.size())) {   // (a view made with the memory's GPU address)
            // A PC buffer of its own that the title maps (a custom heap): through a mapping of ours.
            PcObj res = nullptr;
            uint64_t off = 0, avail = 0;
            void* m = nullptr;
            D3D12_HEAP_PROPERTIES hp{};
            const bool known = pc::gm_resolve(a, &res, &off, &avail) && res && avail >= bytes;
            int heap = -1;
            if (known) { pc::res_heap_props(res, &hp); heap = (int)hp.Type; }
            if (known && heap != D3D12_HEAP_TYPE_DEFAULT && SUCCEEDED(pc::res_map(res, 0, nullptr, &m)) && m) memcpy(data.data(), (const uint8_t*)m + off, data.size());
            else {
                if (InterlockedIncrement(&shown) <= 6)
                    XSL_INFO(CH, "table dump: slot %llu, %llu bytes at %llx cannot be read (%s, heap type %d)", (unsigned long long)t, (unsigned long long)bytes, (unsigned long long)a,
                             known ? "a PC buffer of its own" : "no buffer known there", heap);
                return false;
            }
        }
    }
    {
        static volatile long said = 0;
        if (InterlockedIncrement(&said) <= 10)
            XSL_INFO(CH, "table dump: %s: %zu bytes, the title's address %llx, PC GPU address %llx", path, data.size(), (unsigned long long)dumpAt, (unsigned long long)gpu_va(g_dev, dumpAt));
    }
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}
// Where translated code reads the words of the table bound at PC handle h (the title's own slot
// bytes, in the GPU-readable copy of its descriptor memory); for 0, of the heap's first slot.
static uint64_t table_memory(uint64_t pcGpu) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || !g_viewGpuAddr) return 0;
    if (!pcGpu) return g_viewGpuAddr;
    if (pcGpu < g_pcGpu) return 0;
    const uint64_t p = (pcGpu - g_pcGpu) / g_pcInc;
    if (p / kPageSlots >= g_pcPageOwner.size() || !g_pcPageOwner[p / kPageSlots]) return 0;
    const uint64_t t = (uint64_t)(g_pcPageOwner[p / kPageSlots] - 1) * kPageSlots + p % kPageSlots;
    return t < g_viewCount ? g_viewGpuAddr + t * kViewSlot : 0;
}
// The first `count` slots of the table bound at PC handle h, copied to the area for fresh tables
// (see g_freshFirst): the PC handle to bind for the draw, or 0.
static uint64_t table_fresh(uint64_t pcGpu, uint32_t count) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || !g_freshFirst || pcGpu < g_pcGpu || !count || count > kPageSlots) return 0;
    const uint64_t p = (pcGpu - g_pcGpu) / g_pcInc;
    if (p / kPageSlots >= g_pcPageOwner.size() || !g_pcPageOwner[p / kPageSlots]) return 0;
    const uint64_t t0 = (uint64_t)(g_pcPageOwner[p / kPageSlots] - 1) * kPageSlots + p % kPageSlots;
    if (t0 + count > g_viewCount) return 0;
    for (uint32_t k = 0; k < count; k++) {   // (what the title wrote since the last submission)
        const uint32_t t = (uint32_t)t0 + k;
        if (memcmp(slot_ptr(t), &g_snapshot[(size_t)t * kViewSlot], kViewSlot) != 0) materialize(t);
    }
    if (g_freshNext + count > kFreshPages * kPageSlots) g_freshNext = 0;
    const uint32_t at = g_freshFirst + g_freshNext;
    g_freshNext += count;
    pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, count, pc_view_cpu(at), mirror_cpu((uint32_t)t0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_fixFresh++;
    return g_pcGpu + (uint64_t)at * g_pcInc;
}
void desc_set_payload(const uint32_t words[4]) {
    memcpy(t_payload, words, sizeof t_payload);
    t_has_payload = true;
}
// ---- Payloads of descriptors in CPU-only (staging) heaps ----------------------------------------
// Staging heaps are PC heaps (their handles are PC handles), so a view created there has no slot
// to carry its console-style payload. Titles that create every view in staging heaps and copy them
// into shader-visible tables through CopyDescriptors would leave the descriptor memory translated
// code reads empty. Each staging heap therefore gets a payload array (16 bytes per descriptor),
// and copies carry payloads between staging heaps and view slots.
struct Stage { uint64_t start, end; uint32_t inc; uint32_t* pay; };
std::shared_mutex g_stage_mu;
std::vector<Stage> g_stages;   // sorted by start
static uint32_t* stage_at(uint64_t h) {   // caller holds g_stage_mu (shared)
    auto it = std::upper_bound(g_stages.begin(), g_stages.end(), h, [](uint64_t v, const Stage& s) { return v < s.start; });
    if (it == g_stages.begin()) return nullptr;
    --it;
    return h < it->end ? it->pay + (h - it->start) / it->inc * 4 : nullptr;
}
void desc_staging_heap(uint64_t cpuStart, uint32_t count, uint32_t inc, bool add) {
    if (!cpuStart || !count || !inc) return;
    std::unique_lock<std::shared_mutex> g(g_stage_mu);
    for (auto it = g_stages.begin(); it != g_stages.end(); ++it)
        if (it->start == cpuStart) { free(it->pay); g_stages.erase(it); break; }
    if (!add) return;
    Stage s{cpuStart, cpuStart + (uint64_t)count * inc, inc, (uint32_t*)calloc((size_t)count * 4, 4)};
    if (!s.pay) return;
    g_stages.insert(std::upper_bound(g_stages.begin(), g_stages.end(), s.start, [](uint64_t v, const Stage& x) { return v < x.start; }), s);
}
// A view was created at PC handle h (not a view slot): its payload, or none.
void desc_staging_store(uint64_t h) {
    uint32_t w[4] = {};
    if (t_has_payload) { memcpy(w, t_payload, sizeof w); t_has_payload = false; }
    std::shared_lock<std::shared_mutex> g(g_stage_mu);
    if (uint32_t* p = stage_at(h)) memcpy(p, w, sizeof w);
}
// The payload follows a descriptor copied from src to dst (each a view slot or a staging handle).
void desc_payload_copy(uint64_t dst, uint64_t src) {
    uint32_t w[4] = {}, i;
    if (view_slot(src, &i)) memcpy(w, slot_ptr(i), sizeof w);
    else {
        std::shared_lock<std::shared_mutex> g(g_stage_mu);
        if (const uint32_t* p = stage_at(src)) memcpy(w, p, sizeof w);
    }
    if (view_slot(dst, &i)) {
        std::lock_guard<std::mutex> g(g_mu);
        if (w[1] & kDescTexture) w[0] = pc_slot(i);   // texture payloads name their own slot (in the PC heap)
        uint8_t* s = slot_ptr(i);
        if (!memcmp(s, w, sizeof w)) return;
        memcpy(s, w, sizeof w);
        memcpy(&g_snapshot[(size_t)i * kViewSlot], w, sizeof w);   // not a title write: nothing to decode
        g_payload_dirty = true;
    } else {
        std::shared_lock<std::shared_mutex> g(g_stage_mu);
        if (uint32_t* p = stage_at(dst)) memcpy(p, w, sizeof w);
    }
}
uint64_t desc_begin_write(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, Tag* tagOut) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready) return 0;
    uint32_t i;
    // Render-target and depth views may be created anywhere: on the console a descriptor is plain
    // data, and titles keep such views in memory of their own (and copy them around).
    const bool rtvDsv = type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? view_slot(h, &i) : (rtvDsv && h != 0)) {
        uint64_t rec;
        *tagOut = g_rec[type].allocate(type, &rec);
        if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) { g_rec[type].kind[tagOut->record] = t_kind; g_rec[type].src[tagOut->record] = t_src; t_kind = 0; t_src = 0; }
        return rec;
    }
    return 0;
}
void desc_commit(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, const Tag& tag) {
    std::lock_guard<std::mutex> g(g_mu);
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        uint32_t i;
        if (!view_slot(h, &i)) return;
        uint8_t* s = slot_ptr(i);
        memset(s, 0, kViewSlot);
        if (t_has_payload) {
            uint32_t w[4] = {t_payload[0], t_payload[1], t_payload[2], t_payload[3]};
            if (w[1] & kDescTexture) w[0] = pc_slot(i);   // textures: the slot's own index in the PC heap
            memcpy(s, w, sizeof w);
            t_has_payload = false;
        }
        memcpy(s + kTagOffset, &tag, sizeof tag);
        uint64_t rec;
        if (g_rec[type].resolve(tag, &rec)) set_slot(i, rec);
        snapshot_set(i, s);
    } else if (const ShadowHeap* sh = rtv_dsv_heap(h)) {
        memset((void*)h, 0, sh->slot);
        memcpy((void*)h, &tag, sizeof tag);
    } else if (h) {
        memcpy((void*)h, &tag, sizeof tag);   // a view in the title's own memory: the tag alone
    }
}

// Copies between console-layout slots (the out-of-line CopyDescriptorsSimple). Returns false when
// the handles are not shadow handles (samplers, or shadowing disabled).
bool desc_copy(uint32_t n, uint64_t dst, uint64_t src, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready) return false;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        uint32_t di, si;
        if (!view_slot(dst, &di) || !view_slot(src, &si)) return false;
        {
            static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_RT_DEBUG", b, 4) && b[0] == '1'; }();
            static volatile long cn = 0;
            if (on && InterlockedIncrement(&cn) % 5000 < 12) {
                const uint32_t* w = (const uint32_t*)src;
                XSL_INFO(CH, "desc copy: %u slots %u -> %u; source slot: %x %x %x %x tag %x %x", n, si, di, w[0], w[1], w[2], w[3], w[4], w[5]);
            }
        }
        memmove((void*)dst, (const void*)src, (size_t)n * kViewSlot);
        // PC descriptors follow from the source slots' mirror entries (in an order that is safe
        // for overlapping ranges); the copied bytes become the destinations' snapshots.
        for (uint32_t q = 0; q < n; q++) {
            uint32_t k = di > si ? n - 1 - q : q;
            if (di + k == si + k) continue;
            set_slot(di + k, mirror_cpu(si + k));
            uint32_t* w = (uint32_t*)slot_ptr(di + k);
            if (w[1] & kDescTexture) w[0] = pc_slot(di + k);   // texture payloads name their own slot (in the PC heap)
            snapshot_set(di + k, w);
        }
        return true;
    }
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return false;
    // Render-target and depth views: the slots' bytes (their tags), wherever the two ranges lie.
    const ShadowHeap* d = rtv_dsv_heap(dst);
    const uint32_t slot = d ? d->slot : type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? kRtvSlot : kDsvSlot;
    if (d && rtv_dsv_heap(src)) { memmove((void*)dst, (const void*)src, (size_t)n * slot); return true; }
    for (uint32_t k = 0; k < n; k++) memmove((void*)(dst + (uint64_t)k * slot), (const void*)(src + (uint64_t)k * slot), sizeof(Tag));
    return true;
}

// ---- Use: PC handles for console handles ----------------------------------------------------------
uint64_t desc_pc_cpu(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!g_ready || !h) return h;
    std::lock_guard<std::mutex> g(g_mu);
    uint32_t i;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return view_slot(h, &i) ? mirror_cpu(i) : h;   // the slot's current PC descriptor
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV && type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) return h;
    // A render-target or depth view: its tag, wherever the descriptor lies (the layer's heaps, or a
    // copy the title keeps in its own memory). Memory that is not readable resolves to nothing.
    Tag t;
    const bool known = rtv_dsv_heap(h) != nullptr;
    SIZE_T got = 0;
    if (known) memcpy(&t, (const void*)h, sizeof t);
    else if (!ReadProcessMemory(GetCurrentProcess(), (const void*)h, &t, sizeof t, &got) || got != sizeof t) return 0;
    uint64_t rec;
    if (g_rec[type].resolve(t, &rec)) return rec;
    if (!known && t.magic != 0x444C5358u) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 4) XSL_WARN(CH, "%s view handle %llx names no view of the layer (first bytes %08x %08x)", type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ? "render-target" : "depth",
                                                    (unsigned long long)h, t.magic, t.type);
    }
    return 0;
}
// After a PC descriptor was written to desc_pc_cpu(h) of a console-layout view slot.
void desc_publish(uint64_t h) {
    if (!g_ready) return;
    std::lock_guard<std::mutex> g(g_mu);
    uint32_t i;
    if (view_slot(h, &i)) publish(i);
}
// A table is bound at title slot i: its page (and the next, when the table may reach into it) is
// in the PC heap, and both count as used now. Returns the table's PC slot.
static uint32_t table_slot(uint64_t i) {
    const uint32_t pg = (uint32_t)(i / kPageSlots);
    if (pg >= g_pcPageOf.size()) return kNoSlot;
    const bool spans = i % kPageSlots + kTableSpan > kPageSlots && (uint64_t)(pg + 1) * kPageSlots < g_viewCount;
    uint32_t have = g_pcPageOf[pg].load(std::memory_order_acquire);
    uint32_t next = spans ? g_pcPageOf[pg + 1].load(std::memory_order_acquire) : 0;
    if (!have || !g_pgTable[pg] || (spans && (next != have + 1 || !g_pgTable[pg + 1]))) {
        std::lock_guard<std::mutex> g(g_mu);
        g_pgTable[pg] = 1;
        have = g_pcPageOf[pg].load(std::memory_order_acquire);
        if (!have) {
            const uint32_t at = page_place(pg);
            if (at == kNoSlot) return kNoSlot;
            have = at + 1;
        }
        if (spans) {
            g_pgTable[pg + 1] = 1;
            next = g_pcPageOf[pg + 1].load(std::memory_order_acquire);
            // The next page goes behind this one, unless lists in flight still name its present place.
            const uint64_t now = pc::present_count(), u = g_pgUsed[pg + 1].load(std::memory_order_relaxed);
            if (next != have + 1 && (!next || now >= u + kPageIdle)) page_place(pg + 1, have);
            next = g_pcPageOf[pg + 1].load(std::memory_order_acquire);
            if (next != have + 1) {
                static volatile long n = 0;
                g_pgSplit++;
                if (InterlockedIncrement(&n) <= 3)
                    XSL_WARN(CH, "a table at view slot %llu may reach into the next page, which is not behind it in the PC heap (its place is in use)", (unsigned long long)i);
            }
        }
    }
    page_touch(pg);
    if (spans) page_touch(pg + 1);
    return (have - 1) * kPageSlots + (uint32_t)(i % kPageSlots);
}
uint64_t desc_pc_gpu(uint64_t h) {
    if (!g_ready || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return h;
    uint64_t i = (h - g_pcGpu) / kViewSlot;
    if (i / 4096 < 512) g_pg_bound[i / 4096]++;
    uint32_t p = table_slot(i);
    if (p == kNoSlot) {   // no page left for the table: the heap's start instead (wrong contents, but no GPU fault)
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "descriptor table at view slot %llu: the PC heap (%u slots) is full", (unsigned long long)i, g_pcCount);
        p = 0;
    }
    return g_pcGpu + (uint64_t)p * g_pcInc;
}

// ---- Sync before submission ---------------------------------------------------------------------
void desc_sync() {
    if (!g_ready) return;
    std::lock_guard<std::mutex> g(g_mu);
    static uint64_t syncs = 0;
    if (view_stats() && syncs % 600 == 599) {
        std::string w, b;
        unsigned pw = 0, pb = 0;
        for (unsigned k = 0; k < 512; k++) {
            char t[32];
            if (g_pg_written[k]) { pw++; if (w.size() < 900) { snprintf(t, sizeof t, " %u:%u", k, g_pg_written[k]); w += t; } }
            if (g_pg_bound[k]) { pb++; if (b.size() < 900) { snprintf(t, sizeof t, " %u:%u", k, g_pg_bound[k]); b += t; } }
            g_pg_written[k] = g_pg_bound[k] = 0;
        }
        XSL_INFO(CH, "view pages (4096 slots) over 600 submissions: %u written (page:slots%s)", pw, w.c_str());
        XSL_INFO(CH, "view pages with tables bound: %u (page:binds%s)", pb, b.c_str());
    }
    if (++syncs % 1000 == 0) {
        XSL_INFO(CH, "view slots written, highest per 125k band: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u; %llu writes past the PC heap", g_hi[0], g_hi[1], g_hi[2], g_hi[3], g_hi[4],
                 g_hi[5], g_hi[6], g_hi[7], g_hi[8], g_hi[9], g_hi[10], g_hi[11], g_hi[12], g_hi[13], g_hi[14], g_hi[15], g_hi[16], (unsigned long long)g_unbacked);
        uint32_t held = 0, tables = 0;
        for (uint32_t o : g_pcPageOwner) { held += o != 0; tables += o && g_pgTable[o - 1]; }
        const Records& r = g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
        XSL_INFO(CH, "view pages: %u of %zu PC pages held (%u by table pages), %llu placed, %llu taken from idle pages, %llu moved, %llu times no room, %llu tables split; "
                     "view records: %llu made, %llu in use by slots, %llu passed over",
                 held, g_pcPageOwner.size(), tables, (unsigned long long)g_pgPlaced, (unsigned long long)g_pgEvicted, (unsigned long long)g_pgMoved,
                 (unsigned long long)g_pgNoRoom, (unsigned long long)g_pgSplit, (unsigned long long)r.next, (unsigned long long)r.pinned, (unsigned long long)r.passed);
        XSL_INFO(CH, "tables put right for what shaders declare: %llu empty slots given views of no resource, %llu constant buffer views given buffer views, %llu console bytes given views of nothing, "
                     "%llu copies of constants at console alignment made anew, %llu tables that run into a page elsewhere made afresh; "
                     "slots with the console's own buffer descriptor: %llu given views, %llu name no PC memory; %llu slots the layer cannot decode or the title cleared given views of nothing",
                 (unsigned long long)g_fixNull, (unsigned long long)g_fixBuffer, (unsigned long long)g_fixNone, (unsigned long long)g_fixCbv, (unsigned long long)g_fixFresh,
                 (unsigned long long)g_nativeViews, (unsigned long long)g_nativeNone, (unsigned long long)g_undecoded);
    }
    size_t bytes = (size_t)g_viewCount * kViewSlot;
    static std::vector<void*> pages;
    pages.resize(bytes / 4096 + 1);
    ULONG_PTR count = pages.size();
    DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_view, bytes, pages.data(), &count, &gran) != 0) return;
    for (ULONG_PTR p = 0; p < count; p++) {
        size_t off = (uint8_t*)pages[p] - g_view;
        uint32_t first = (uint32_t)(off / kViewSlot), last = (uint32_t)((off + gran) / kViewSlot);
        if (last > g_viewCount) last = g_viewCount;
        for (uint32_t i = first; i < last; i++)
            if (memcmp(g_view + (size_t)i * kViewSlot, &g_snapshot[(size_t)i * kViewSlot], kViewSlot) != 0) materialize(i);
        if (g_viewGpu) memcpy(g_viewGpu + off, g_view + off, std::min<size_t>(gran, bytes - off));
    }
    for (auto& sg : g_segs) {   // title-supplied slot memory
        size_t sb = (size_t)sg.count * kViewSlot;
        pages.resize(sb / 4096 + 1);
        count = pages.size();
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, sg.base, sb, pages.data(), &count, &gran) != 0) continue;
        for (ULONG_PTR p = 0; p < count; p++) {
            size_t off = (uint8_t*)pages[p] - sg.base;
            uint32_t first = sg.first + (uint32_t)(off / kViewSlot), last = sg.first + (uint32_t)((off + gran) / kViewSlot);
            if (last > sg.first + sg.count) last = sg.first + sg.count;
            for (uint32_t i = first; i < last; i++)
                if (memcmp(slot_ptr(i), &g_snapshot[(size_t)i * kViewSlot], kViewSlot) != 0) materialize(i);
            if (g_viewGpu) memcpy(g_viewGpu + (size_t)sg.first * kViewSlot + off, sg.base + off, std::min<size_t>(gran, sb - off));
        }
    }
}

// Slots [first, first + count) of a heap now live in title memory `mem` (write-watched already).
// Its current bytes become the slots' snapshot, so nothing is decoded until the title writes.
bool desc_view_attach(uint32_t first, uint32_t count, void* mem) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready || (uint64_t)first + count > g_viewCount) return false;
    g_segs.push_back({first, count, (uint8_t*)mem});
    memcpy(&g_snapshot[(size_t)first * kViewSlot], mem, (size_t)count * kViewSlot);
    if (g_viewGpu) memcpy(g_viewGpu + (size_t)first * kViewSlot, mem, (size_t)count * kViewSlot);
    return true;
}
void desc_view_detach(uint32_t first) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto it = g_segs.begin(); it != g_segs.end(); ++it)
        if (it->first == first) { g_segs.erase(it); return; }
}

}  // namespace xs
