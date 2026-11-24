// SPDX-License-Identifier: MIT
// allocator_cost_check [count]: what a D3D12 command allocator costs the process on this driver.
// Creates <count> allocators (default 100), reports the process's private bytes after creating
// them, after recording and running one small list on each, and after Reset of all of them.
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

static double private_mb() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&c, sizeof c);
    return c.PrivateUsage / 1048576.0;
}

int main(int argc, char** argv) {
    const int count = argc > 1 ? atoi(argv[1]) : 100;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no device\n"); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd{};
    ID3D12CommandQueue* q = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12Fence* f = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const double base = private_mb();
    std::vector<ID3D12CommandAllocator*> allocs(count);
    std::vector<ID3D12GraphicsCommandList*> lists(count);
    for (int i = 0; i < count; i++) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocs[i]));
    const double created = private_mb();
    for (int i = 0; i < count; i++) {
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[i], nullptr, IID_PPV_ARGS(&lists[i]));
        D3D12_VIEWPORT vp{0, 0, 64, 64, 0, 1};
        lists[i]->RSSetViewports(1, &vp);
        lists[i]->Close();
    }
    const double listed = private_mb();
    for (int i = 0; i < count; i++) { ID3D12CommandList* l[] = {lists[i]}; q->ExecuteCommandLists(1, l); }
    q->Signal(f, 1);
    f->SetEventOnCompletion(1, e);
    WaitForSingleObject(e, 10000);
    const double ran = private_mb();
    for (int i = 0; i < count; i++) allocs[i]->Reset();
    const double reset = private_mb();
    // One large list on the first allocator: how its memory grows with what is recorded.
    const int big = argc > 2 ? atoi(argv[2]) : 0;
    double bigMb = 0;
    if (big) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 1 << 20;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *a = nullptr, *b = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&a));
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&b));
        const double before = private_mb();
        lists[0]->Reset(allocs[0], nullptr);
        for (int i = 0; i < big; i++) lists[0]->CopyBufferRegion(b, (i % 1024) * 64, a, (i % 1024) * 64, 64);
        lists[0]->Close();
        bigMb = private_mb() - before;
        ID3D12CommandList* l[] = {lists[0]};
        q->ExecuteCommandLists(1, l);
        q->Signal(f, 2);
        f->SetEventOnCompletion(2, e);
        WaitForSingleObject(e, 20000);
        allocs[0]->Reset();
        printf("one list of %d copies: +%.1f MB recorded, %.1f MB still held after its allocator's Reset\n", big, bigMb, private_mb() - before);
    }
    for (int i = 0; i < count; i++) { lists[i]->Release(); allocs[i]->Release(); }
    Sleep(500);
    const double released = private_mb();
    printf("%d allocators: +%.1f MB created (%.2f MB each), +%.1f MB with one small list each (%.2f MB each), %.1f MB after running, %.1f MB after Reset, %.1f MB after Release\n",
           count, created - base, (created - base) / count, listed - base, (listed - base) / count, ran - base, reset - base, released - base);
    return 0;
}
