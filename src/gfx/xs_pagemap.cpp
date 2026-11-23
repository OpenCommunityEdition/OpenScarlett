// SPDX-License-Identifier: AGPL-3.0-or-later
//
// GPU page mapping: RegisterPagePoolX, CreatePlacedResourceX and the queue's Copy/ClearPageMappingsX.
//
// Console titles reserve GPU virtual address space, place resources on it, and map 64 KB pages
// from registered page pools (DRAM blocks, ESRAM) into it on the GPU timeline. On PC the same
// effect comes from reserved (tiled) resources:
//   - each page pool becomes a PC heap of PageCount x 64 KB;
//   - a resource placed on reserved address space becomes a PC reserved resource whose tiles, in a
//     fixed linear order, stand for the 64 KB pages from its address onwards;
//   - a page mapping becomes UpdateTileMappings on the same queue, for every placed resource the
//     page belongs to (aliasing resources all see it).
// Contents follow PC tiling, not the console's, which is fine for GPU-only memory: the title reads
// and writes these pages only through the resources. Mappings made before a resource is placed are
// remembered per page and applied when it appears.
#include <map>
#include <mutex>
#include <vector>

#include "xs_object.h"
#include "xs_detile.h"

namespace xs {
namespace {
constexpr uint64_t kPage = 64 * 1024;

struct Pool {
    uint64_t va;
    uint32_t pages;
    PcObj heap;
};
struct Placed {
    uint64_t va, extent;
    PcObj res;  // owned by the resource object
    std::vector<pc::TileCoord> tiles;
};
struct PageMap {
    const Pool* pool;  // null: unmapped
    uint32_t index;
};
std::mutex g_mu;
std::map<uint64_t, Pool*> g_pools;       // pool VA -> pool
std::vector<PcObj> g_retired_heaps;      // unregistered pools stay alive while tiles may map them
std::map<uint64_t, Placed*> g_placed;    // placed VA -> resource
std::map<uint64_t, PageMap> g_pages;     // page VA -> current mapping (for resources placed later)
uint64_t g_max_extent = 0;               // largest placed extent: bounds the backwards search

uint64_t reservation_extent(uint64_t va) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((const void*)va, &mbi, sizeof mbi)) return 0;
    return (uint64_t)mbi.BaseAddress + mbi.RegionSize - va;
}

// Tiles of every placed resource touched by one mapping call, grouped for UpdateTileMappings.
struct Pending {
    std::vector<pc::TileCoord> mapped, unmapped;
    std::vector<uint32_t> heapPages;
    const Pool* pool = nullptr;
};

void apply_page(std::map<Placed*, Pending>& out, uint64_t pageVa, const PageMap& m, uint32_t tilesPerPage) {
    // Every placed resource whose extent covers this page (resources may alias).
    for (auto it = g_placed.upper_bound(pageVa); it != g_placed.begin();) {
        --it;
        Placed* p = it->second;
        if (pageVa - p->va >= g_max_extent) break;  // nothing at or below this address reaches the page
        if (pageVa >= p->va + p->extent) continue;
        uint64_t first = (pageVa - p->va) / kPage;
        auto& pend = out[p];
        for (uint32_t k = 0; k < tilesPerPage; k++) {
            uint64_t t = first + k;
            if (t >= p->tiles.size()) break;
            if (m.pool) {
                pend.pool = m.pool;
                pend.mapped.push_back(p->tiles[t]);
                pend.heapPages.push_back(m.index * tilesPerPage + k);
            } else {
                pend.unmapped.push_back(p->tiles[t]);
            }
        }
    }
}

void flush(PcObj queue, std::map<Placed*, Pending>& pending) {
    for (auto& [p, pend] : pending) {
        if (!pend.mapped.empty())
            pc::queue_map_tiles(queue, p->res, (uint32_t)pend.mapped.size(), pend.mapped.data(), pend.pool->heap, pend.heapPages.data());
        if (!pend.unmapped.empty()) pc::queue_map_tiles(queue, p->res, (uint32_t)pend.unmapped.size(), pend.unmapped.data(), nullptr, nullptr);
    }
}

HRESULT map_ranges(PcObj queue, uint64_t dest, UINT rangeCount, const D3D12XBOX_PAGE_MAPPING_RANGE* ranges, const Pool* pool,
                   D3D12XBOX_PAGE_MAPPING_FLAGS flags, std::map<Placed*, Pending>& pending) {
    const uint32_t tilesPerPage = (flags & D3D12XBOX_PAGE_MAPPING_FLAG_2MB_PAGES) ? 32 : 1;
    const uint64_t pageBytes = kPage * tilesPerPage;
    uint64_t va = dest;
    for (UINT r = 0; r < rangeCount; r++) {
        const auto& rg = ranges[r];
        for (UINT i = 0; i < rg.PageCount; i++, va += pageBytes) {
            PageMap m{nullptr, 0};
            switch (rg.RangeType) {
                case D3D12XBOX_PAGE_MAPPING_RANGE_TYPE_INCREMENTING_PAGE_INDICES: m = {pool, rg.StartPageIndexInPool + i}; break;
                case D3D12XBOX_PAGE_MAPPING_RANGE_TYPE_CONSTANT_PAGE_INDEX: m = {pool, rg.StartPageIndexInPool}; break;
                case D3D12XBOX_PAGE_MAPPING_RANGE_TYPE_NULL_PAGES: break;
                case D3D12XBOX_PAGE_MAPPING_RANGE_TYPE_SKIP_RANGE: continue;
                default: return E_INVALIDARG;
            }
            if (m.pool && (!pool || m.index >= pool->pages)) return E_INVALIDARG;
            for (uint32_t k = 0; k < tilesPerPage; k++) {
                PageMap sub = m.pool ? PageMap{m.pool, m.index * tilesPerPage + k} : m;
                if (sub.pool) g_pages[va + k * kPage] = sub;
                else g_pages.erase(va + k * kPage);
            }
            apply_page(pending, va, m, tilesPerPage);
        }
    }
    return S_OK;
}

const Pool* find_pool(uint64_t va, UINT pageCount) {
    auto it = g_pools.find(va);
    if (it == g_pools.end() || it->second->pages < pageCount) return nullptr;
    return it->second;
}
}  // namespace

HRESULT D3DAPI D_RegisterPagePoolX(ID3D12Device* d, D3D12_GPU_VIRTUAL_ADDRESS va, UINT pageCount, HANDLE* out) {
    if (!out || !pageCount) return E_INVALIDARG;
    auto* pool = new Pool{va, pageCount, nullptr};
    HRESULT hr = pc::create_page_pool_heap(dev_extra(d)->pcDevice, (uint64_t)pageCount * kPage, &pool->heap);
    if (FAILED(hr)) { delete pool; return hr; }
    std::lock_guard<std::mutex> g(g_mu);
    g_pools[va] = pool;
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 3) XSL_INFO(CH, "page pool %llx: %u pages backed by a PC heap", (unsigned long long)va, pageCount);
    *out = (HANDLE)pool;
    return S_OK;
}
HRESULT D3DAPI D_UnregisterPagePoolX(ID3D12Device*, HANDLE h) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto it = g_pools.begin(); it != g_pools.end(); ++it) {
        if ((HANDLE)it->second != h) continue;
        g_retired_heaps.push_back(it->second->heap);
        for (auto p = g_pages.begin(); p != g_pages.end();) p = p->second.pool == it->second ? g_pages.erase(p) : std::next(p);
        delete it->second;
        g_pools.erase(it);
        return S_OK;
    }
    return E_INVALIDARG;
}

HRESULT D3DAPI D_CreatePlacedResourceX(ID3D12Device* d, D3D12_GPU_VIRTUAL_ADDRESS va, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
                                       const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    DeviceExtra* de = dev_extra(d);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery((const void*)va, &mbi, sizeof mbi);
    if (getenv("XSL_TRACE_VIDEO_PLANES") && desc && (desc->Format == DXGI_FORMAT_R8_UNORM || desc->Format == DXGI_FORMAT_R8G8_UNORM) && desc->Width >= 1024)
        XSL_INFO(CH, "placed X fmt %d %llux%u at %llx (%s)", (int)desc->Format, (unsigned long long)desc->Width, desc->Height, (unsigned long long)va,
                 mbi.State == MEM_COMMIT ? "committed" : "reserved");
    // XSL_PLACED_COMMITTED=1 (diagnostics): ignore page mappings, give every placed resource its own memory.
    static bool forceCommitted = [] { char b[4]; return GetEnvironmentVariableA("XSL_PLACED_COMMITTED", b, 4) && b[0] == '1'; }();
    if (va && va < (1ull << 32) && mbi.State == MEM_FREE && desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        PcObj r = nullptr;
        HRESULT hr = pc::create_committed_resource(de->pcDevice, &hp, 0, desc, st, cv, &r);
        if (FAILED(hr)) return hr;
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 2)
            XSL_INFO(CH, "CreatePlacedResourceX at %llx (no memory there): plane %d of a video frame (format %d %llux%u)", (unsigned long long)va, va ? 1 : 0,
                     (int)desc->Format, (unsigned long long)desc->Width, desc->Height);
        note_plane_alias(r, va);
        return make_resource((Device*)d, r, riid, out);
    }
    if (mbi.State == MEM_COMMIT || forceCommitted) {
        // Placed on committed title memory: contents the CPU writes there are not shared with the
        // PC resource yet (that needs console-tiling conversion); GPU-produced contents work.
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3)
            XSL_WARN(CH, "CreatePlacedResourceX on committed memory %llx: using a separate PC resource (CPU-written contents not shared)",
                     (unsigned long long)va);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        PcObj r = nullptr;
        HRESULT hr = pc::create_committed_resource(de->pcDevice, &hp, 0, desc, st, cv, &r);
        if (FAILED(hr)) return hr;
        detile_register(r, va, desc, pc::pc_state_of(st));
        return make_resource((Device*)d, r, riid, out);
    }
    PcObj r = nullptr;
    HRESULT hr = pc::create_reserved_resource(de->pcDevice, desc, st, cv, &r);
    if (FAILED(hr)) return hr;
    detile_note_desc(r, desc, pc::pc_state_of(st));   // should the title map it
    auto* p = new Placed{va, reservation_extent(va), r, pc::resource_tiles(de->pcDevice, r)};
    if (!p->extent) p->extent = p->tiles.size() * kPage;
    if (p->tiles.size() * kPage > p->extent) {
        static volatile long n = 0;
        if (InterlockedIncrement(&n) <= 3)
            XSL_WARN(CH, "placed resource at %llx needs %zu PC tiles but the console reservation holds %llu pages; the rest stay unmapped",
                     (unsigned long long)va, p->tiles.size(), (unsigned long long)(p->extent / kPage));
    }
    hr = make_resource((Device*)d, r, riid, out);
    if (FAILED(hr)) { delete p; return hr; }
    resource_set_placed(*out, va);
    // Register, and apply pages the title mapped at this address before placing the resource
    // (grouped by pool: UpdateTileMappings takes one heap per call).
    std::map<const Pool*, Pending> byPool;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_placed.insert({va, p});
        if (p->extent > g_max_extent) g_max_extent = p->extent;
        for (auto it = g_pages.lower_bound(va); it != g_pages.end() && it->first < va + p->extent; ++it) {
            uint64_t t = (it->first - va) / kPage;
            if (t >= p->tiles.size()) break;
            auto& bp = byPool[it->second.pool];
            bp.mapped.push_back(p->tiles[t]);
            bp.heapPages.push_back(it->second.index);
        }
    }
    PcObj queue = de->pcQueue;
    for (auto& [pool, bp] : byPool)
        if (queue) pc::queue_map_tiles(queue, p->res, (uint32_t)bp.mapped.size(), bp.mapped.data(), pool->heap, bp.heapPages.data());
    return S_OK;
}

void pagemap_forget(uint64_t va, PcObj res) {
    std::lock_guard<std::mutex> g(g_mu);
    auto range = g_placed.equal_range(va);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second->res != res) continue;
        delete it->second;
        g_placed.erase(it);
        return;
    }
}

HRESULT D3DAPI Q_CopyPageMappingsBatchX(ID3D12CommandQueue* q, UINT batchCount, const D3D12XBOX_PAGE_MAPPING_BATCH* batches,
                                        D3D12_GPU_VIRTUAL_ADDRESS poolVa, UINT poolPages, D3D12XBOX_PAGE_MAPPING_FLAGS flags) {
    std::map<Placed*, Pending> pending;
    {
        std::lock_guard<std::mutex> g(g_mu);
        const Pool* pool = find_pool(poolVa, poolPages);
        if (!pool) {
            XSL_ERR(CH, "CopyPageMappingsX: %llx is not a registered page pool", (unsigned long long)poolVa);
            return E_INVALIDARG;
        }
        for (UINT b = 0; b < batchCount; b++) {
            HRESULT hr = map_ranges(pc_of(q), batches[b].DestinationAddress, batches[b].RangeCount, batches[b].pRanges, pool, flags, pending);
            if (FAILED(hr)) return hr;
        }
    }
    flush(pc_of(q), pending);
    return S_OK;
}
HRESULT D3DAPI Q_CopyPageMappingsX(ID3D12CommandQueue* q, D3D12_GPU_VIRTUAL_ADDRESS dest, UINT rangeCount, const D3D12XBOX_PAGE_MAPPING_RANGE* ranges,
                                   D3D12_GPU_VIRTUAL_ADDRESS poolVa, UINT poolPages, D3D12XBOX_PAGE_MAPPING_FLAGS flags) {
    D3D12XBOX_PAGE_MAPPING_BATCH b{dest, rangeCount, ranges};
    return Q_CopyPageMappingsBatchX(q, 1, &b, poolVa, poolPages, flags);
}
HRESULT D3DAPI Q_ClearPageMappingsX(ID3D12CommandQueue* q, D3D12_GPU_VIRTUAL_ADDRESS dest, UINT pageCount, D3D12XBOX_PAGE_MAPPING_FLAGS flags) {
    D3D12XBOX_PAGE_MAPPING_RANGE r{D3D12XBOX_PAGE_MAPPING_RANGE_TYPE_NULL_PAGES, 0, pageCount};
    std::map<Placed*, Pending> pending;
    {
        std::lock_guard<std::mutex> g(g_mu);
        HRESULT hr = map_ranges(pc_of(q), dest, 1, &r, nullptr, flags, pending);
        if (FAILED(hr)) return hr;
    }
    flush(pc_of(q), pending);
    return S_OK;
}

}  // namespace xs
