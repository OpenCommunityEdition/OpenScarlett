// SPDX-License-Identifier: MIT
// section_heap_check <mode> [MB] [count] [seconds]: does memory the layer shows to the GPU survive
// the process?
//
// The layer imports a title's physical-page sections as D3D12 heaps
// (OpenExistingHeapFromFileMapping) with a buffer placed over each. This program does the same
// with <count> page-file-backed sections of <MB> each and ends. <mode> is a list of words:
//   address  import a view of each section by address (OpenExistingHeapFromAddress) instead
//   written  the GPU copies the first megabyte of each buffer into the next, and the copy is
//            checked through a second view of the section (the GPU and the CPU see one memory)
//   release  release the buffers (and so the heaps) and wait a second before ending
//   hang     sleep <seconds> with everything referenced: for ending it from outside (taskkill /F)
// Run it with the committed memory of the system read before and after (\Memory\Committed Bytes).
//
// Measured on Windows 10 19044.7663 with an RTX 5090 (driver 591.59): a section imported as a file
// mapping stays committed after the process, however it ends and even when everything was
// released first (512 MB of 512 each time); the same sections imported by address do not.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: section_heap_check <words of: address written release hang, or plain> [MB] [count] [seconds]\n"); return 2; }
    const char* mode = argv[1];
    auto has = [&](const char* w) { return strstr(mode, w) != nullptr; };
    const UINT64 bytes = (UINT64)(argc > 2 ? atoi(argv[2]) : 256) << 20;
    const int count = argc > 3 ? atoi(argv[3]) : 4, seconds = argc > 4 ? atoi(argv[4]) : 30;
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { printf("device %08lx\n", hr); return 1; }
    ID3D12Device3* d3 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d3)))) { printf("no ID3D12Device3\n"); return 1; }
    std::vector<ID3D12Resource*> buffers;
    std::vector<HANDLE> sections;
    for (int i = 0; i < count; i++) {
        HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, (DWORD)(bytes >> 32), (DWORD)bytes, nullptr);
        if (!s) { printf("section %d: error %lu\n", i, GetLastError()); return 1; }
        ID3D12Heap* heap = nullptr;
        sections.push_back(s);
        if (has("address")) {
            void* view = MapViewOfFile(s, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
            hr = view ? d3->OpenExistingHeapFromAddress(view, IID_PPV_ARGS(&heap)) : E_FAIL;
        } else hr = d3->OpenExistingHeapFromFileMapping(s, IID_PPV_ARGS(&heap));
        if (FAILED(hr)) { printf("heap %d: %08lx\n", i, hr); return 1; }
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        ID3D12Resource* r = nullptr;
        hr = dev->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        heap->Release();
        if (FAILED(hr)) { printf("buffer %d: %08lx\n", i, hr); return 1; }
        buffers.push_back(r);
    }
    printf("%d sections of %llu MB imported (%s)\n", count, (unsigned long long)(bytes >> 20), mode);
    if (has("written")) {
        std::vector<uint8_t*> check;   // views of our own, apart from what the heaps were made from
        for (HANDLE s : sections) check.push_back((uint8_t*)MapViewOfFile(s, FILE_MAP_ALL_ACCESS, 0, 0, 1 << 20));
        for (size_t i = 0; i < check.size(); i++) memset(check[i], i == 0 ? 0x5a : 0, 1 << 20);
        D3D12_COMMAND_QUEUE_DESC qd{};
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        ID3D12Fence* f = nullptr;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
        for (size_t i = 0; i + 1 < buffers.size(); i++) l->CopyBufferRegion(buffers[i + 1], 0, buffers[i], 0, 1 << 20);
        l->Close();
        ID3D12CommandList* ls[] = {l};
        q->ExecuteCommandLists(1, ls);
        q->Signal(f, 1);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(1, e);
        WaitForSingleObject(e, 10000);
        size_t good = 0;
        for (size_t i = 1; i < check.size(); i++) good += check[i][0] == 0x5a && check[i][(1 << 20) - 1] == 0x5a;
        printf("copies done (fence %llu): %zu of %zu sections hold what the GPU wrote\n", (unsigned long long)f->GetCompletedValue(), good, check.size() - 1);
    }
    fflush(stdout);
    if (has("release")) {
        for (auto* r : buffers) r->Release();
        Sleep(1000);
    }
    if (has("hang")) Sleep(seconds * 1000);
    return 0;
}
