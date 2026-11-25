// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: two queues ordered by waits for memory values.
// The console GPU stands at a point of a command list until a value in memory passes a comparison
// (Wait32BitValueX), and writes a value when the work before a point is done
// (Write32BitValueBottomOfPipeX). A title orders its queues with the two.
//  1. A list that begins with a wait is submitted before the list, on another queue, whose write
//     it waits for: its work must run after that list's.
//  2. A handshake: list A adds 5 to X, writes a value, waits for another, and adds 5 again with
//     nothing set in between; list B, on the other queue, waits for the first value, copies X to
//     Y and writes the second. Y must hold 5 (the copy ran between the two halves of A) and X 10
//     (the second half ran, with the state set before the wait).
#include <windows.h>

#include <d3d12_xs.h>
#include <xmem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

static std::vector<char> read_file(const char* path) {
    std::vector<char> d;
    if (FILE* f = std::fopen(path, "rb")) {
        std::fseek(f, 0, SEEK_END);
        d.resize(std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        std::fread(d.data(), 1, d.size(), f);
        std::fclose(f);
    }
    return d;
}

int main() {
    D3D12XBOX_CREATE_DEVICE_PARAMETERS params = {};
    params.Version = D3D12_SDK_VERSION;
    params.GraphicsCommandQueueRingSizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.GraphicsScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.ComputeScratchMemorySizeBytes = static_cast<UINT>(D3D12XBOX_DEFAULT_SIZE_BYTES);
    params.DisableGeometryShaderAllocations = TRUE;
    params.DisableTessellationShaderAllocations = TRUE;
    params.DisableDXR = TRUE;
    ID3D12Device* dev = nullptr;
    CHECK(SUCCEEDED(D3D12XboxCreateDevice(nullptr, &params, IID_GRAPHICS_PPV_ARGS(&dev))) && dev);
    if (!dev) return 1;

    // Queue 0 takes graphics lists, queue 1 compute lists; each with a list, an allocator and a fence.
    const D3D12_COMMAND_LIST_TYPE types[2] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE};
    ID3D12CommandQueue* q[2] = {};
    ID3D12CommandAllocator* alloc[2] = {};
    ID3D12GraphicsCommandList* list[2] = {};
    ID3D12Fence* fence[2] = {};
    UINT64 fenceValue[2] = {};
    for (int k = 0; k < 2; k++) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = types[k];
        CHECK(SUCCEEDED(dev->CreateCommandQueue(&qd, IID_GRAPHICS_PPV_ARGS(&q[k]))));
        dev->CreateCommandAllocator(types[k], IID_GRAPHICS_PPV_ARGS(&alloc[k]));
        dev->CreateCommandList(0, types[k], alloc[k], nullptr, IID_GRAPHICS_PPV_ARGS(&list[k]));
        list[k]->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_GRAPHICS_PPV_ARGS(&fence[k]));
    }
    auto submit = [&](int k) {
        list[k]->Close();
        ID3D12CommandList* l[] = {list[k]};
        q[k]->ExecuteCommandLists(1, l);
        q[k]->Signal(fence[k], ++fenceValue[k]);
    };
    // Waits for both queues' signals; false after `ms`.
    auto finish = [&](DWORD ms) {
        bool ok = true;
        for (int k = 0; k < 2; k++) {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence[k]->SetEventOnCompletion(fenceValue[k], e);
            ok &= WaitForSingleObject(e, ms) == WAIT_OBJECT_0;
            CloseHandle(e);
        }
        return ok;
    };
    auto begin = [&](int k) {
        alloc[k]->Reset();
        list[k]->Reset(alloc[k], nullptr);
    };

    auto add = read_file("list_wait_add.cso"), copy = read_file("list_wait_copy.cso");
    CHECK(!add.empty() && !copy.empty());
    auto pipeline = [&](std::vector<char>& code, ID3D12RootSignature** rs) {
        CHECK(SUCCEEDED(dev->CreateRootSignature(0, code.data(), code.size(), IID_GRAPHICS_PPV_ARGS(rs))));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = *rs;
        d.CS = {code.data(), code.size()};
        ID3D12PipelineState* p = nullptr;
        CHECK(SUCCEEDED(dev->CreateComputePipelineState(&d, IID_GRAPHICS_PPV_ARGS(&p))) && p);
        return p;
    };
    ID3D12RootSignature *addRs = nullptr, *copyRs = nullptr;
    ID3D12PipelineState *addPso = pipeline(add, &addRs), *copyPso = pipeline(copy, &copyRs);
    if (!addPso || !copyPso) return 1;

    // The values waited for live in title memory the GPU sees.
    auto* values = (volatile UINT*)XMemVirtualAlloc(nullptr, 1 << 16, MEM_COMMIT | MEM_RESERVE | MEM_64K_PAGES, XMEM_GRAPHICS, PAGE_READWRITE);
    CHECK(values != nullptr);
    if (!values) return 1;
    const D3D12_GPU_VIRTUAL_ADDRESS at = (D3D12_GPU_VIRTUAL_ADDRESS)(uintptr_t)values;

    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *x = nullptr, *y = nullptr, *readBuf = nullptr;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_GRAPHICS_PPV_ARGS(&x));
    dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_GRAPHICS_PPV_ARGS(&y));
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;
    bd.Width = 512;
    dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_GRAPHICS_PPV_ARGS(&readBuf));
    CHECK(x && y && readBuf);

    auto add_state = [&](ID3D12GraphicsCommandList* l, ID3D12Resource* to, UINT amount) {
        l->SetPipelineState(addPso);
        l->SetComputeRootSignature(addRs);
        l->SetComputeRootUnorderedAccessView(0, to->GetGPUVirtualAddress());
        l->SetComputeRoot32BitConstant(1, amount, 0);
    };
    auto copy_x_to_y = [&](ID3D12GraphicsCommandList* l) {
        l->SetPipelineState(copyPso);
        l->SetComputeRootSignature(copyRs);
        l->SetComputeRootUnorderedAccessView(0, x->GetGPUVirtualAddress());
        l->SetComputeRootUnorderedAccessView(1, y->GetGPUVirtualAddress());
        l->Dispatch(1, 1, 1);
    };
    UINT got[2] = {};
    // Reads word 0 of X and of Y (after everything submitted has run).
    auto read_back = [&] {
        begin(0);
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (int k = 0; k < 2; k++) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[k].Transition = {k ? y : x, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        }
        list[0]->ResourceBarrier(2, b);
        list[0]->CopyBufferRegion(readBuf, 0, x, 0, 256);
        list[0]->CopyBufferRegion(readBuf, 256, y, 0, 256);
        for (int k = 0; k < 2; k++) std::swap(b[k].Transition.StateBefore, b[k].Transition.StateAfter);
        list[0]->ResourceBarrier(2, b);
        submit(0);
        finish(10000);
        UINT* data = nullptr;
        D3D12_RANGE r = {0, 512};
        readBuf->Map(0, &r, (void**)&data);
        got[0] = data ? data[0] : ~0u;
        got[1] = data ? data[64] : ~0u;
        readBuf->Unmap(0, nullptr);
    };
    // X and Y start at zero (a buffer's first contents are not defined).
    auto zero = [&] {
        read_back();
        begin(0);
        add_state(list[0], x, 0u - got[0]);
        list[0]->Dispatch(1, 1, 1);
        add_state(list[0], y, 0u - got[1]);
        list[0]->Dispatch(1, 1, 1);
        submit(0);
        finish(10000);
    };

    // 1. The waiting list is submitted first; its copy must see what the other list adds.
    zero();
    values[0] = 0;
    begin(1);
    list[1]->Wait32BitValueX(at, D3D12_COMPARISON_FUNC_GREATER_EQUAL, 1, 0xffffffffu, D3D12XBOX_WAIT_FLAG_NONE);
    copy_x_to_y(list[1]);
    submit(1);
    Sleep(100);   // (without the wait the copy has long run by now)
    begin(0);
    add_state(list[0], x, 7);
    list[0]->Dispatch(1, 1, 1);
    list[0]->Write32BitValueBottomOfPipeX(at, 1, D3D12XBOX_FLUSH_NONE, D3D12XBOX_WRITE_VALUE_BOP_FLAG_NONE);
    submit(0);
    ULONGLONG t0 = GetTickCount64();
    CHECK(finish(10000));
    ULONGLONG took = GetTickCount64() - t0;
    read_back();
    std::printf("     X %u, Y %u, value %u, %llu ms\n", got[0], got[1], (UINT)values[0], took);
    CHECK(got[0] == 7 && got[1] == 7);
    CHECK(values[0] == 1);
    CHECK(took < 1500);   // (the wait was met, not given up)

    // 2. The handshake, with list A cut in the middle.
    zero();
    values[1] = values[2] = 0;
    begin(0);
    add_state(list[0], x, 5);
    list[0]->Dispatch(1, 1, 1);
    list[0]->Write32BitValueBottomOfPipeX(at + 4, 1, D3D12XBOX_FLUSH_NONE, D3D12XBOX_WRITE_VALUE_BOP_FLAG_NONE);
    list[0]->Wait32BitValueX(at + 8, D3D12_COMPARISON_FUNC_EQUAL, 1, 0xffffffffu, D3D12XBOX_WAIT_FLAG_NONE);
    list[0]->Dispatch(1, 1, 1);   // nothing set again: the state from before the wait
    submit(0);
    Sleep(100);
    begin(1);
    list[1]->Wait32BitValueX(at + 4, D3D12_COMPARISON_FUNC_GREATER_EQUAL, 1, 0xffffffffu, D3D12XBOX_WAIT_FLAG_NONE);
    copy_x_to_y(list[1]);
    list[1]->Write32BitValueBottomOfPipeX(at + 8, 1, D3D12XBOX_FLUSH_NONE, D3D12XBOX_WRITE_VALUE_BOP_FLAG_NONE);
    submit(1);
    t0 = GetTickCount64();
    CHECK(finish(10000));
    took = GetTickCount64() - t0;
    read_back();
    std::printf("     X %u, Y %u, values %u %u, %llu ms\n", got[0], got[1], (UINT)values[1], (UINT)values[2], took);
    CHECK(got[0] == 10);
    CHECK(got[1] == 5);
    CHECK(took < 1500);

    std::printf(g_failures ? "FAILED (%d)\n" : "PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
