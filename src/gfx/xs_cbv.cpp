// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Constant-buffer alignment.
//
// The console's D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT is 16 bytes (d3d12_xs.h); the
// PC's is 256, and Vulkan drivers round unaligned uniform-buffer offsets down. Console titles
// (DirectXTK's GraphicsMemory among them) bind constants at 16-byte offsets, which on a PC
// silently reads the wrong data. Root CBVs whose address is not 256-aligned are therefore
// re-pointed at a copy in a layer-owned, 256-aligned upload ring.
//
// The copy is taken when the bind is recorded. That matches titles that write constants
// before binding them (DirectXTK and the GDK samples do); a title that writes an upload
// buffer after recording the bind but before ExecuteCommandLists would see stale data, which
// the log reports once per call site class. Upload-heap buffers are tracked by GPU address
// range with a persistent CPU mapping so the source bytes can be found.
#include <map>
#include <mutex>
#include <vector>

#include "xs_cbv.h"
#include "xs_unified.h"

using xsl::Level;
using xsl::log_enabled;
namespace xs {
namespace {
struct UploadRange { uint64_t size; uint8_t* cpu; };
std::mutex g_mu;
std::map<uint64_t, UploadRange> g_upload;  // GPU VA start -> range

constexpr uint64_t kChunk = 16ull << 20;     // ring memory is added in chunks of this size
constexpr uint64_t kMaxChunks = 32;          // per segment (512 MB)
constexpr uint32_t kSegments = 4;            // one per in-flight frame; > maximum frame latency + 1
constexpr uint64_t kCopyBytes = 4096;        // bytes copied per rebind (shader CB size is not known here)

struct Chunk {
    PcObj res = nullptr;
    uint8_t* cpu = nullptr;
    uint64_t gpu = 0;
};
// Each presented frame uses one segment; a segment grows by chunks when a frame binds more
// constants than it holds, and keeps them for later frames.
struct Segment {
    std::vector<Chunk> chunks;
    size_t cur = 0;     // chunk being filled
    uint64_t head = 0;  // offset in it
};
struct Ring {
    Segment seg[kSegments];
    uint64_t frame = UINT64_MAX;
    std::map<uint64_t, uint64_t> seen;  // source VA -> ring GPU VA, this frame
} g_ring;

bool add_chunk(Device* dev, Segment& sg) {
    DeviceExtra* de = dev_extra(dev);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = kChunk;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Chunk c;
    if (FAILED(pc::create_committed_resource(de->pcDevice, &hp, 0, &rd, 0x8c3, nullptr, &c.res))) return false;
    D3D12_RANGE none{0, 0};
    void* p = nullptr;
    pc::res_map(c.res, 0, &none, &p);
    c.cpu = (uint8_t*)p;
    c.gpu = pc::res_gpu_va(c.res);
    sg.chunks.push_back(c);
    static bool first = true;
    if (first) {
        first = false;
        XSL_INFO(CH, "constant buffers bound at console (16-byte) alignment are re-aligned through a ring (%llu MB chunks)",
                 (unsigned long long)(kChunk >> 20));
    }
    return true;
}
}  // namespace

void cbv_track_upload(PcObj pcRes, uint64_t va, uint64_t size) {
    void* p = pc::buffer_try_map(pcRes);
    if (!p) return;
    std::lock_guard<std::mutex> g(g_mu);
    g_upload[va] = {size, (uint8_t*)p};
}

void cbv_untrack(uint64_t va) {
    std::lock_guard<std::mutex> g(g_mu);
    g_upload.erase(va);
}

uint64_t cbv_fix_root(Device* dev, uint64_t va) {
    static bool enabled = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_CBV_REALIGN", b, 4) && b[0] == '0'); }();
    if (!enabled || (va & 255) == 0 || va == 0) return va;
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_upload.upper_bound(va);
    if (it == g_upload.begin()) goto untracked;
    --it;
    if (va >= it->first + it->second.size) goto untracked;
    {
        DeviceExtra* de = dev_extra(dev);
        // One segment per presented frame; PresentX bounds frames in flight below kSegments.
        uint64_t frame = de->presentCount;
        Segment& sg = g_ring.seg[frame % kSegments];
        if (frame != g_ring.frame) { g_ring.frame = frame; sg.cur = 0; sg.head = 0; g_ring.seen.clear(); }
        // The same constants bound twice in a frame (one per draw is common) share one copy, which is
        // safe because titles write constants before recording the bind.
        if (auto s = g_ring.seen.find(va); s != g_ring.seen.end()) return s->second;
        if (sg.cur < sg.chunks.size() && sg.head + kCopyBytes > kChunk) { sg.cur++; sg.head = 0; }
        if (sg.cur >= sg.chunks.size() && (sg.chunks.size() >= kMaxChunks || !add_chunk(dev, sg))) {
            static volatile long warned = 0;
            if (InterlockedIncrement(&warned) <= 1)
                XSL_WARN(CH, "constant-buffer ring full (%llu MB this frame); binding unaligned addresses",
                         (unsigned long long)(sg.chunks.size() * kChunk >> 20));
            return va;
        }
        Chunk& c = sg.chunks[sg.cur];
        uint64_t avail = it->first + it->second.size - va;
        uint64_t n = avail < kCopyBytes ? avail : kCopyBytes;
        uint64_t off = sg.head;
        memcpy(c.cpu + off, it->second.cpu + (va - it->first), n);
        g_ring.seen[va] = c.gpu + off;
        const float* f = (const float*)(it->second.cpu + (va - it->first));
        if (log_enabled("gfx.cbv", Level::Trace)) {
            XSL_TRACE("gfx.cbv", "rebind %llx (+%llu bytes avail) -> ring chunk %zu+%llx: %g %g %g %g | %g %g %g %g | %g %g %g %g",
                      (unsigned long long)va, (unsigned long long)avail, sg.cur, (unsigned long long)off, f[0], f[1], f[2], f[3], f[4],
                      f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        }
        sg.head += (n + 255) & ~255ull;
        return c.gpu + off;
    }
untracked:
    static volatile long warned = 0;
    if (InterlockedIncrement(&warned) <= 3) XSL_WARN(CH, "root CBV %llx is not 256-aligned and not in a tracked upload buffer", (unsigned long long)va);
    return va;
}

// Copies `n` bytes from `src` into this frame's ring segment; 0 if the ring is full.
static uint64_t ring_copy(Device* dev, const uint8_t* src, uint64_t n) {
    DeviceExtra* de = dev_extra(dev);
    uint64_t frame = de->presentCount;
    Segment& sg = g_ring.seg[frame % kSegments];
    if (frame != g_ring.frame) { g_ring.frame = frame; sg.cur = 0; sg.head = 0; g_ring.seen.clear(); }
    if (sg.cur < sg.chunks.size() && sg.head + n > kChunk) { sg.cur++; sg.head = 0; }
    if (sg.cur >= sg.chunks.size() && (sg.chunks.size() >= kMaxChunks || !add_chunk(dev, sg))) return 0;
    Chunk& c = sg.chunks[sg.cur];
    uint64_t off = sg.head;
    memcpy(c.cpu + off, src, n);
    sg.head += (n + 255) & ~255ull;
    return c.gpu + off;
}

uint64_t cbv_fix_view(Device* dev, uint64_t title_va, uint64_t gpu, uint64_t size) {
    static bool enabled = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_CBV_REALIGN", b, 4) && b[0] == '0'); }();
    if (!enabled || (gpu & 255) == 0 || gpu == 0) return gpu;
    size = size ? (size + 15) & ~15ull : 256;
    if (size > 65536) size = 65536;
    // Source bytes: unified memory (the title's address is the CPU address) or a mapped upload buffer.
    const uint8_t* src = nullptr;
    uint64_t avail = 0;
    UnifiedRef u;
    if (unified_lookup(dev, title_va, &u)) {
        src = (const uint8_t*)(uintptr_t)title_va;
        avail = size;   // the view lies inside the allocation (the title made it)
    }
    std::lock_guard<std::mutex> g(g_mu);
    if (!src) {
        auto it = g_upload.upper_bound(gpu);
        if (it != g_upload.begin()) {
            --it;
            if (gpu < it->first + it->second.size) {
                src = it->second.cpu + (gpu - it->first);
                avail = it->first + it->second.size - gpu;
            }
        }
    }
    if (!src) {
        static volatile long warned = 0;
        if (InterlockedIncrement(&warned) <= 3) XSL_WARN(CH, "constant buffer view %llx is not 256-aligned and its memory is not CPU-visible", (unsigned long long)gpu);
        return gpu;
    }
    uint64_t n = size < avail ? size : avail;
    uint64_t r = ring_copy(dev, src, n);
    if (!r) {
        static volatile long warned = 0;
        if (InterlockedIncrement(&warned) <= 1) XSL_WARN(CH, "constant-buffer ring full; constant buffer views stay unaligned");
        return gpu;
    }
    static volatile long once = 0;
    if (!InterlockedExchange(&once, 1))
        XSL_INFO(CH, "constant buffer views at console alignment point at copies taken when the view is created");
    return r;
}

}  // namespace xs
