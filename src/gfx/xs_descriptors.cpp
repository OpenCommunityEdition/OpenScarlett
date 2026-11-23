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
#include "xs_object.h"
#include "xs_unified.h"

namespace xs {
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
    bool init(PcObj dev, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t cap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{type, cap, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(pc::create_descriptor_heap(dev, &d, &heap))) return false;
        cpu = pc::heap_cpu_start(heap);
        inc = pc::descriptor_increment(dev, type);
        capacity = cap;
        gen.assign(cap, 0);
        return true;
    }
    // A fresh record; its tag stays valid until the ring wraps past it.
    Tag allocate(uint32_t type, uint64_t* handle) {
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
std::mutex g_page_mu;
std::vector<std::atomic<uint32_t>> g_pcPageOf;   // title page -> PC page + 1 (0: none yet)
std::vector<uint32_t> g_pcPageOwner;             // PC page -> title page + 1
uint32_t g_pcPagesFree = 0;                      // lowest PC page that may be free
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
    g_pcPageOwner.assign(std::min(g_pcCount, pc::gm_reserved_start()) / kPageSlots, 0);
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
    if (!g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1u << 18) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1u << 14) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1u << 12))
        return false;
    // GPU-readable copy of the view slots (translated ray-tracing code reads descriptor memory):
    // at 0xF0000000 in global memory, refreshed from written pages at each submission.
    g_viewGpu = (uint8_t*)pc::gm_low_buffer(pcdev, 0, bytes, &g_viewGpuAddr);
    if (g_viewGpu) memcpy(g_viewGpu, g_view, bytes);
    XSL_INFO(CH, "descriptor memory in console layout (32/112/128-byte slots), synced to PC descriptors at submission");
    g_ready = true;
    return true;
}

uint64_t pc_view_cpu(uint32_t i) { return g_pcCpu + (uint64_t)i * g_pcInc; }
// Title view slots are numbered past what a PC heap holds (a driver limit of about a million
// descriptors; one title's heap alone has 1.2 million, of which it uses a part of each 4096-slot
// chunk). The PC heap is therefore handed out in pages of 4096 slots as title pages are first
// written or bound: a title page keeps its own position when that PC page is free (so titles within
// the limit map one to one), else it takes the lowest free one. The pages of the global-memory
// range at the end of the PC heap are never handed out.
uint32_t pc_slot(uint32_t i) {
    const uint32_t pg = i / kPageSlots;
    if (pg >= g_pcPageOf.size()) return kNoSlot;
    uint32_t have = g_pcPageOf[pg].load(std::memory_order_acquire);
    if (!have) {
        std::lock_guard<std::mutex> g(g_page_mu);
        have = g_pcPageOf[pg].load(std::memory_order_relaxed);
        if (!have) {
            uint32_t take = kNoSlot;
            if (pg < g_pcPageOwner.size() && !g_pcPageOwner[pg]) take = pg;
            else {
                while (g_pcPagesFree < g_pcPageOwner.size() && g_pcPageOwner[g_pcPagesFree]) g_pcPagesFree++;
                if (g_pcPagesFree < g_pcPageOwner.size()) take = g_pcPagesFree;
            }
            if (take == kNoSlot) return kNoSlot;
            if (take != pg) {
                static volatile long n = 0;
                if (InterlockedIncrement(&n) == 1)
                    XSL_INFO(CH, "title view slots past the PC heap (%u slots): pages of %u slots are placed where the PC heap has room", g_pcCount, kPageSlots);
            }
            g_pcPageOwner[take] = pg + 1;
            have = take + 1;
            g_pcPageOf[pg].store(have, std::memory_order_release);
        }
    }
    return (have - 1) * kPageSlots + i % kPageSlots;
}
uint64_t mirror_cpu(uint32_t i) { return g_mirrorCpu + (uint64_t)i * g_pcInc; }
// Slot i's PC descriptor is in the mirror: publish it to the shader-visible heap.
uint32_t g_hi[17] = {};   // highest slot published per 125k-slot band (usage diagnostics)
uint64_t g_unbacked = 0;  // writes to slots past the PC heap
// Usage per 4096-slot page since the last report (XSL_VIEW_STATS=1): slots published, tables bound.
uint32_t g_pg_written[512] = {}, g_pg_bound[512] = {};
bool view_stats() { static bool on = [] { char b[4]; return GetEnvironmentVariableA("XSL_VIEW_STATS", b, 4) && b[0] == '1'; }(); return on; }
void publish(uint32_t i) {
    if (i / 125000 < 17 && i > g_hi[i / 125000]) g_hi[i / 125000] = i;
    if (i / 4096 < 512) g_pg_written[i / 4096]++;
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
    } else {
        return false;
    }
    PcObj pcdev = dev_extra(g_dev)->pcDevice;
    if (!addr) {
        pc::create_cbv(pcdev, nullptr, mirror_cpu(i));
        publish(i);
        return true;
    }
    uint64_t gpu = gpu_va(g_dev, addr);
    if (gpu & 255) gpu = cbv_fix_root(g_dev, gpu);  // console CBVs need only 16-byte alignment
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
bool g_payload_dirty = false;       // payloads written by copies since the last sync
thread_local uint32_t t_payload[4];
thread_local bool t_has_payload = false;
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
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "descriptor slot %u names a recycled record (raise the record ring)", i);
        }
    } else if (!materialize_xbox_cbv(i, (const uint64_t*)s)) {
        static const uint8_t zero[kViewSlot] = {};
        if (memcmp(s, zero, kViewSlot) != 0) {
            static volatile long n = 0;
            if (InterlockedIncrement(&n) <= 3) XSL_WARN(CH, "descriptor slot %u holds console bytes the layer cannot decode yet", i);
        }
    }
    memcpy(&g_snapshot[(size_t)i * kViewSlot], s, kViewSlot);
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
uint64_t desc_memory_address(uint64_t h) {
    if (!g_viewGpu || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return 0;
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
        if (w[1] & kDescTexture) w[0] = pc_slot(i);   // texture payloads name their own slot (in the PC heap)
        std::lock_guard<std::mutex> g(g_mu);
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
        memcpy(&g_snapshot[(size_t)i * kViewSlot], s, kViewSlot);
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
            memcpy(&g_snapshot[(size_t)(di + k) * kViewSlot], w, kViewSlot);
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
uint64_t desc_pc_gpu(uint64_t h) {
    if (!g_ready || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return h;
    uint64_t i = (h - g_pcGpu) / kViewSlot;
    if (i / 4096 < 512) g_pg_bound[i / 4096]++;
    uint32_t p = pc_slot((uint32_t)i);
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
    if (++syncs % 2000 == 0)
        XSL_INFO(CH, "view slots written, highest per 125k band: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u; %llu writes past the PC heap", g_hi[0], g_hi[1], g_hi[2], g_hi[3], g_hi[4],
                 g_hi[5], g_hi[6], g_hi[7], g_hi[8], g_hi[9], g_hi[10], g_hi[11], g_hi[12], g_hi[13], g_hi[14], g_hi[15], g_hi[16], (unsigned long long)g_unbacked);
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
