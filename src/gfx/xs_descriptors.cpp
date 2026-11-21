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
#include <mutex>
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
uint32_t g_viewCount = 0;
uint64_t g_pcCpu = 0, g_pcGpu = 0;
uint32_t g_pcInc = 0;
std::vector<ShadowHeap> g_rtvDsv;  // per RTV/DSV heap shadow arrays

bool enabled() {
    static bool on = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_SHADOW_DESCRIPTORS", b, 4) && b[0] == '0'); }();
    return on;
}

bool ensure(Device* dev) {
    if (g_ready) return true;
    PcObj pcdev = dev_extra(dev)->pcDevice;
    PcObj gh = pc::global_view_heap(pcdev);
    if (!gh) return false;
    g_dev = dev;
    D3D12_DESCRIPTOR_HEAP_DESC d;
    pc::heap_desc(gh, &d);
    g_viewCount = d.NumDescriptors;
    g_pcCpu = pc::heap_cpu_start(gh);
    g_pcGpu = pc::heap_gpu_start(gh);
    g_pcInc = pc::descriptor_increment(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    size_t bytes = (size_t)g_viewCount * kViewSlot;
    g_view = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!g_view) { XSL_ERR(CH, "descriptor shadow: VirtualAlloc(MEM_WRITE_WATCH) failed (%lu)", GetLastError()); return false; }
    g_snapshot.assign(bytes, 0);
    if (!g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1u << 18) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_RTV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1u << 14) ||
        !g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_DSV].init(pcdev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1u << 12))
        return false;
    XSL_INFO(CH, "descriptor memory in console layout (32/112/128-byte slots), synced to PC descriptors at submission");
    g_ready = true;
    return true;
}

uint64_t pc_view_cpu(uint32_t i) { return g_pcCpu + (uint64_t)i * g_pcInc; }
bool view_slot(uint64_t h, uint32_t* i) {
    if (!g_view || h < (uint64_t)g_view || h >= (uint64_t)g_view + (uint64_t)g_viewCount * kViewSlot) return false;
    *i = (uint32_t)((h - (uint64_t)g_view) / kViewSlot);
    return true;
}
const ShadowHeap* rtv_dsv_heap(uint64_t h) {
    for (auto& s : g_rtvDsv)
        if (h >= (uint64_t)s.base && h < (uint64_t)s.base + (uint64_t)s.count * s.slot) return &s;
    return nullptr;
}

// Decode a console buffer descriptor that an inlined CreateConstantBufferView wrote.
bool materialize_xbox_cbv(uint32_t i, const uint64_t* q) {
    if ((q[1] & kXboxCbvFormatMask) != (0x0002403C00000000ull & kXboxCbvFormatMask)) return false;
    uint64_t addr = q[0] & ((1ull << 48) - 1);
    uint64_t size = (q[1] & 0xFFFFFFFFull) << 4;
    PcObj pcdev = dev_extra(g_dev)->pcDevice;
    if (!addr) {
        pc::create_cbv(pcdev, nullptr, pc_view_cpu(i));
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
    pc::create_cbv(pcdev, &d, pc_view_cpu(i));
    return true;
}

// Make global slot i match its shadow bytes. Caller holds g_mu.
void materialize(uint32_t i) {
    const uint8_t* s = g_view + (size_t)i * kViewSlot;
    Tag t;
    memcpy(&t, s, sizeof t);
    uint64_t rec;
    if (t.magic == kMagic) {
        if (g_rec[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].resolve(t, &rec)) {
            pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, 1, pc_view_cpu(i), rec, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
uint64_t desc_begin_write(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type, Tag* tagOut) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_ready) return 0;
    uint32_t i;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? view_slot(h, &i) : rtv_dsv_heap(h) != nullptr) {
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
        uint8_t* s = g_view + (size_t)i * kViewSlot;
        memset(s, 0, kViewSlot);
        memcpy(s, &tag, sizeof tag);
        uint64_t rec;
        if (g_rec[type].resolve(tag, &rec))
            pc::copy_descriptors_simple(dev_extra(g_dev)->pcDevice, 1, pc_view_cpu(i), rec, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        memcpy(&g_snapshot[(size_t)i * kViewSlot], s, kViewSlot);
    } else if (const ShadowHeap* sh = rtv_dsv_heap(h)) {
        memset((void*)h, 0, sh->slot);
        memcpy((void*)h, &tag, sizeof tag);
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
        memmove((void*)dst, (const void*)src, (size_t)n * kViewSlot);
        for (uint32_t k = 0; k < n; k++) materialize(di + k);
        return true;
    }
    const ShadowHeap* d = rtv_dsv_heap(dst);
    if (!d || !rtv_dsv_heap(src)) return false;
    memmove((void*)dst, (const void*)src, (size_t)n * d->slot);
    return true;
}

// ---- Use: PC handles for console handles ----------------------------------------------------------
uint64_t desc_pc_cpu(uint64_t h, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (!g_ready || !h) return h;
    std::lock_guard<std::mutex> g(g_mu);
    uint32_t i;
    bool shadow = type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? view_slot(h, &i) : rtv_dsv_heap(h) != nullptr;
    if (!shadow) return h;
    Tag t;
    memcpy(&t, (const void*)h, sizeof t);
    uint64_t rec;
    if (g_rec[type].resolve(t, &rec)) return rec;
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) return pc_view_cpu(i);  // e.g. an inline CBV
    return 0;
}
uint64_t desc_pc_gpu(uint64_t h) {
    if (!g_ready || h < g_pcGpu || h >= g_pcGpu + (uint64_t)g_viewCount * kViewSlot) return h;
    return g_pcGpu + (h - g_pcGpu) / kViewSlot * g_pcInc;
}

// ---- Sync before submission ---------------------------------------------------------------------
void desc_sync() {
    if (!g_ready) return;
    std::lock_guard<std::mutex> g(g_mu);
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
    }
}

}  // namespace xs
