// SPDX-License-Identifier: MIT
// interior_heap_check: can a second buffer be placed inside memory imported by address?
//
// The layer shows a title's memory to the GPU as one buffer over a heap opened from the
// allocation's address. A title also places buffers of its own inside such an allocation (an arena
// the GPU writes and reads by address). This program checks what a PC allows there:
//   1. a second heap opened from the same base address;
//   2. a buffer with unordered access placed in that heap at a 64 KiB multiple (not 0), beside
//      the buffer over the whole heap: what the CPU writes in the allocation is what the GPU
//      copies out of the inner buffer, and what the GPU copies into it is what the CPU reads;
//   3. a heap opened from an address inside the allocation (reported, not required).
// Exit code 0: 1 and 2 hold.
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static ID3D12Resource* buffer(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_TYPE type) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    HRESULT hr;
    if (heap) hr = dev->CreatePlacedResource(heap, off, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    else {
        D3D12_HEAP_PROPERTIES hp{type};
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&r));
    }
    if (FAILED(hr)) printf("     buffer (%llu bytes at %llu, flags %x): %08lx\n", (unsigned long long)bytes, (unsigned long long)off, (unsigned)flags, hr);
    return r;
}

int main() {
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no device\n"); return 1; }
    ID3D12Device3* d3 = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&d3)))) { printf("no ID3D12Device3\n"); return 1; }
    const UINT64 bytes = 32ull << 20, inner = 11ull << 20, innerBytes = 4ull << 20;
    auto* mem = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    ID3D12Heap *h1 = nullptr, *h2 = nullptr, *h3 = nullptr;
    HRESULT hr = d3->OpenExistingHeapFromAddress(mem, IID_PPV_ARGS(&h1));
    printf("heap from the base address: %08lx\n", hr);
    if (FAILED(hr)) return 1;
    const D3D12_RESOURCE_FLAGS f = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* whole = buffer(dev, h1, 0, bytes, f, D3D12_HEAP_TYPE_DEFAULT);
    hr = d3->OpenExistingHeapFromAddress(mem, IID_PPV_ARGS(&h2));
    printf("a second heap from the same address: %08lx\n", hr);
    hr = d3->OpenExistingHeapFromAddress(mem + inner, IID_PPV_ARGS(&h3));
    printf("a heap from an address inside the allocation: %08lx", hr);
    if (SUCCEEDED(hr)) printf(" (%llu bytes)", (unsigned long long)h3->GetDesc().SizeInBytes);
    printf("\n");
    bool ok = whole != nullptr;
    for (int which = 0; which < 2 && ok; which++) {
        ID3D12Heap* heap = which ? h2 : h1;
        if (!heap) continue;
        ID3D12Resource* in = buffer(dev, heap, inner, innerBytes, f, D3D12_HEAP_TYPE_DEFAULT);
        printf("a buffer placed at %llu MiB in the %s heap: %s\n", (unsigned long long)(inner >> 20), which ? "second" : "first", in ? "made" : "refused");
        if (!in) { if (!which) ok = false; continue; }
        ID3D12Resource* rb = buffer(dev, nullptr, 0, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_READBACK);
        ID3D12Resource* up = buffer(dev, nullptr, 0, 4096, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        ID3D12CommandQueue* q = nullptr;
        ID3D12CommandAllocator* a = nullptr;
        ID3D12GraphicsCommandList* l = nullptr;
        ID3D12Fence* fence = nullptr;
        dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        for (int i = 0; i < 1024; i++) ((uint32_t*)(mem + inner + 8192))[i] = 0xC0DE0000u + i + which;   // the CPU writes; the GPU reads it through the inner buffer
        uint32_t* u = nullptr;
        up->Map(0, nullptr, (void**)&u);
        for (int i = 0; i < 1024; i++) u[i] = 0xFEED0000u + i + which;
        l->CopyBufferRegion(rb, 0, in, 8192, 2048);          // inner buffer -> read-back
        l->CopyBufferRegion(in, 65536, up, 0, 4096);         // upload -> inner buffer (the CPU reads it in the allocation)
        l->CopyBufferRegion(rb, 2048, whole, inner + 8192 + 2048, 2048);   // the same memory through the buffer over the whole heap
        l->Close();
        ID3D12CommandList* ls[] = {l};
        q->ExecuteCommandLists(1, ls);
        q->Signal(fence, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 10000);
        uint32_t* r = nullptr;
        rb->Map(0, nullptr, (void**)&r);
        int bad = 0;
        for (int i = 0; i < 512; i++) bad += r[i] != 0xC0DE0000u + i + which;
        for (int i = 0; i < 512; i++) bad += r[512 + i] != 0xC0DE0000u + 512 + i + which;
        int bad2 = 0;
        for (int i = 0; i < 1024; i++) bad2 += ((uint32_t*)(mem + inner + 65536))[i] != 0xFEED0000u + i + which;
        printf("     the GPU read %d of 1024 words wrong; the CPU reads %d of 1024 words the GPU wrote wrong (device %08lx)\n", bad, bad2, dev->GetDeviceRemovedReason());
        if (bad || bad2) ok = false;
    }
    printf(ok ? "PASSED\n" : "FAILED\n");
    return ok ? 0 : 1;
}
