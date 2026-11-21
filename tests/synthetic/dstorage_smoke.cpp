// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: console DirectStorage memory requests (plain, zlib, BCPack,
// zlib+BCPack), error records, cancellation and fence signals through the layer's XCRDAPI.dll.
#include <windows.h>

#include <d3d12_xs.h>
#include <dstorage_xs.h>
#include <dstorageerr.h>
#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

typedef HRESULT(__cdecl* PFN_BCPackCompress)(size_t, size_t*, void*, size_t, const void*, size_t);

int main() {
    // BC1 test blocks: smooth endpoints and repeating indices, so every codec has something to do.
    std::vector<uint8_t> blocks(4096 * 8);
    for (size_t b = 0; b < 4096; b++) {
        uint16_t c0 = (uint16_t)(0xF800 - b * 3), c1 = (uint16_t)(0x001F + b * 2);
        memcpy(&blocks[b * 8], &c0, 2);
        memcpy(&blocks[b * 8 + 2], &c1, 2);
        uint32_t idx = 0x1B1B1B1Bu ^ (uint32_t)(b & 0xF);
        memcpy(&blocks[b * 8 + 4], &idx, 4);
    }
    std::vector<uint8_t> zlibbed(compressBound((uLong)blocks.size()));
    uLongf zn = (uLongf)zlibbed.size();
    CHECK(compress2(zlibbed.data(), &zn, blocks.data(), (uLong)blocks.size(), 9) == Z_OK);
    zlibbed.resize(zn);

    auto bcCompress = (PFN_BCPackCompress)GetProcAddress(LoadLibraryW(L"BCPack.dll"), "BCPackCompress");
    CHECK(bcCompress != nullptr);
    std::vector<uint8_t> packed(blocks.size() * 2), packedZ;
    size_t pn = 0;
    if (bcCompress) {
        CHECK(SUCCEEDED(bcCompress(1, &pn, packed.data(), packed.size(), blocks.data(), blocks.size())));
        packed.resize(pn);
        packedZ.resize(compressBound((uLong)pn));
        uLongf pzn = (uLongf)packedZ.size();
        compress2(packedZ.data(), &pzn, packed.data(), (uLong)pn, 9);
        packedZ.resize(pzn);
        std::printf("BC1 %zu bytes: zlib %lu, BCPack %zu, BCPack+zlib %zu\n", blocks.size(), (unsigned long)zn, pn, packedZ.size());
    }

    IDStorageFactoryX* factory = nullptr;
    CHECK(SUCCEEDED(DStorageGetFactory(__uuidof(IDStorageFactoryX), (void**)&factory)) && factory);
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "dstorage_smoke";
    IDStorageQueueX* queue = nullptr;
    CHECK(SUCCEEDED(factory->CreateQueue(&qd, __uuidof(IDStorageQueueX), (void**)&queue)));
    IDStorageStatusArrayX* status = nullptr;
    CHECK(SUCCEEDED(factory->CreateStatusArray(8, "smoke", __uuidof(IDStorageStatusArrayX), (void**)&status)));

    // A console fence (the layer's d3d12_xs.dll) for EnqueueSignal.
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params = {};
    params.Version = D3D12_SDK_VERSION;
    params.GraphicsCommandQueueRingSizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.GraphicsScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.ComputeScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.DisableGeometryShaderAllocations = TRUE;
    params.DisableTessellationShaderAllocations = TRUE;
    params.DisableDXR = TRUE;
    ID3D12Device* dev = nullptr;
    CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_GRAPHICS_PPV_ARGS(&dev))));
    ID3D12Fence* fence = nullptr;
    if (dev) dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(&fence));

    auto request = [&](const std::vector<uint8_t>& src, void* dst, uint32_t dstSize, bool zlib, int bc, uint32_t intermediate, uint64_t tag) {
        DSTORAGE_REQUEST r{};
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r.Options.ZlibDecompress = zlib ? 1 : 0;
        r.Options.BcpackMode = (DSTORAGE_BCPACK_MODE)bc;
        r.Source = (void*)src.data();
        r.SourceSize = (UINT32)src.size();
        r.Destination = dst;
        r.DestinationSize = dstSize;
        r.IntermediateSize = intermediate;
        r.CancellationTag = tag;
        queue->EnqueueRequest(&r);
    };
    auto wait = [&](UINT32 i) {
        for (int k = 0; k < 2000 && !status->IsComplete(i); k++) Sleep(1);
        return status->IsComplete(i);
    };

    std::vector<uint8_t> out(blocks.size());
    const uint32_t size = (uint32_t)blocks.size();
    struct Case { const char* name; const std::vector<uint8_t>* src; bool zlib; int bc; uint32_t inter; };
    Case cases[] = {
        {"plain", &blocks, false, 0, 0},
        {"zlib", &zlibbed, true, 0, 0},
        {"BCPack", &packed, false, 1, 0},
        {"zlib+BCPack", &packedZ, true, 1, (uint32_t)pn},
    };
    UINT32 slot = 0;
    for (auto& c : cases) {
        if (c.bc && !bcCompress) continue;
        memset(out.data(), 0xCD, out.size());
        request(*c.src, out.data(), size, c.zlib, c.bc, c.inter, 1);
        queue->EnqueueStatus(status, slot);
        queue->Submit();
        bool done = wait(slot);
        HRESULT hr = status->GetHResult(slot);
        bool same = memcmp(out.data(), blocks.data(), size) == 0;
        std::printf("%-12s complete=%d hr=%08lx match=%d\n", c.name, done, hr, same);
        CHECK(done && SUCCEEDED(hr) && same);
        slot++;
    }

    // A corrupt zlib stream fails its status entry and fills the error record.
    std::vector<uint8_t> junk(64, 0x5A);
    request(junk, out.data(), size, true, 0, 0, 2);
    queue->EnqueueStatus(status, slot);
    queue->Submit();
    CHECK(wait(slot) && FAILED(status->GetHResult(slot)));
    DSTORAGE_ERROR_RECORD rec{};
    queue->RetrieveErrorRecord(&rec);
    CHECK(rec.FailureCount == 1 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST);
    slot++;

    // Cancellation: the request is dropped, the destination stays untouched, the status is S_OK.
    memset(out.data(), 0xCD, out.size());
    request(blocks, out.data(), size, false, 0, 0, 0x77);
    queue->CancelRequestsWithTag(0xFF, 0x77);
    queue->EnqueueStatus(status, slot);
    queue->Submit();
    CHECK(wait(slot) && SUCCEEDED(status->GetHResult(slot)) && out[0] == 0xCD);
    slot++;

    // Fence signal after a request.
    if (fence) {
        request(blocks, out.data(), size, false, 0, 0, 3);
        queue->EnqueueSignal(fence, 42);
        queue->Submit();
        for (int k = 0; k < 2000 && fence->GetCompletedValue() < 42; k++) Sleep(1);
        CHECK(fence->GetCompletedValue() == 42 && memcmp(out.data(), blocks.data(), size) == 0);
    }

    queue->Release();
    status->Release();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
