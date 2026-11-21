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

#include "xs_cbv.h"

using xsl::Level;
using xsl::log_enabled;
namespace xs {
namespace {
struct UploadRange { uint64_t size; uint8_t* cpu; };
std::mutex g_mu;
std::map<uint64_t, UploadRange> g_upload;  // GPU VA start -> range

constexpr uint64_t kSegment = 16ull << 20;  // per in-flight frame
constexpr uint32_t kSegments = 4;          // > maximum supported frame latency + 1
constexpr uint64_t kCopyBytes = 4096;      // bytes copied per rebind (shader CB size is not known here)

struct Ring {
    PcObj res = nullptr;
    uint8_t* cpu = nullptr;
    uint64_t gpu = 0;
    uint64_t frame = UINT64_MAX;
    uint64_t head = 0;
    std::map<uint64_t, uint64_t> seen;  // source VA -> ring offset, this frame
} g_ring;
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
        if (!g_ring.res) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = kSegment * kSegments;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(pc::create_committed_resource(de->pcDevice, &hp, 0, &rd, 0x8c3, nullptr, &g_ring.res))) return va;
            D3D12_RANGE none{0, 0};
            void* p = nullptr;
            pc::res_map(g_ring.res, 0, &none, &p);
            g_ring.cpu = (uint8_t*)p;
            g_ring.gpu = pc::res_gpu_va(g_ring.res);
            XSL_INFO(CH, "constant buffers bound at console (16-byte) alignment are re-aligned through a %llu MB ring",
                     (unsigned long long)(kSegment * kSegments >> 20));
        }
        // One segment per presented frame; PresentX bounds frames in flight below kSegments.
        uint64_t frame = de->presentCount;
        if (frame != g_ring.frame) { g_ring.frame = frame; g_ring.head = 0; g_ring.seen.clear(); }
        // The same constants bound twice in a frame (one per draw is common) share one copy, which is
        // safe because titles write constants before recording the bind.
        if (auto s = g_ring.seen.find(va); s != g_ring.seen.end()) return g_ring.gpu + s->second;
        uint64_t avail = it->first + it->second.size - va;
        uint64_t n = avail < kCopyBytes ? avail : kCopyBytes;
        uint64_t off = (frame % kSegments) * kSegment + g_ring.head;
        if (g_ring.head + kCopyBytes > kSegment) {
            XSL_WARN(CH, "constant-buffer ring segment full; binding unaligned address");
            return va;
        }
        memcpy(g_ring.cpu + off, it->second.cpu + (va - it->first), n);
        g_ring.seen[va] = off;
        const float* f = (const float*)(it->second.cpu + (va - it->first));
        if (log_enabled("gfx.cbv", Level::Trace)) {
            XSL_TRACE("gfx.cbv", "rebind %llx (+%llu bytes avail) -> ring+%llx: %g %g %g %g | %g %g %g %g | %g %g %g %g", (unsigned long long)va,
                      (unsigned long long)avail, (unsigned long long)off, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        }
        g_ring.head += (n + 255) & ~255ull;
        return g_ring.gpu + off;
    }
untracked:
    static volatile long warned = 0;
    if (InterlockedIncrement(&warned) <= 3) XSL_WARN(CH, "root CBV %llx is not 256-aligned and not in a tracked upload buffer", (unsigned long long)va);
    return va;
}

}  // namespace xs
