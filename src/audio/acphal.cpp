// SPDX-License-Identifier: AGPL-3.0-or-later
// AcpHal.dll replacement: the console audio processor (APU memory, ACP/SHAPE/XMA contexts).
// The host has no such hardware. Memory works for real (titles and FMOD use APU memory as
// ordinary buffers, and the SHAPE context arrays are plain APU memory the title fills in);
// the ACP below runs the protocol without processing audio (silent). A software SHAPE/XMA
// implementation behind it is what real sound needs.
#include <windows.h>

#include <acphal.h>  // SDK (build time): signatures, context sizes, error codes

#include <cstdint>
#include <map>
#include <mutex>
#include <new>
#include <vector>

#include "xsl/log.h"

namespace {

struct Block {
    SIZE_T size;
    APU_ADDRESS apu;
};
std::mutex g_lock;
std::map<uintptr_t, Block> g_blocks;  // virtual address -> block
std::map<APU_ADDRESS, uintptr_t> g_by_apu;
uint64_t g_next_apu = 0x10000;        // fake 32-bit APU address space, never 0
bool g_heap = false;
UINT64 g_used[3] = {};

bool lookup(uintptr_t va, uintptr_t* base, Block* b) {
    auto it = g_blocks.upper_bound(va);
    if (it == g_blocks.begin()) return false;
    --it;
    if (va >= it->first + it->second.size) return false;
    *base = it->first;
    *b = it->second;
    return true;
}

SHAPE_CONTEXTS g_ctx;
bool g_ctx_allocated = false;

template <class T> HRESULT alloc_array(UINT32 n, T** va, APU_ADDRESS* pa) {
    *va = nullptr;
    *pa = 0;
    if (!n) return S_OK;
    void* p = nullptr;
    HRESULT hr = ApuAlloc(&p, pa, n * (UINT32)sizeof(T), 256, APU_ALLOC_NONCACHED);
    *va = (T*)p;
    return hr;
}
}  // namespace

STDAPI ApuCreateHeap(UINT32 cached, UINT32 noncached) {
    std::lock_guard<std::mutex> l(g_lock);
    XSL_INFO("audio", "ApuCreateHeap(cached=%u, noncached=%u)", cached, noncached);
    if (g_heap) return APU_E_HEAP_ALREADY_ALLOCATED;
    g_heap = true;
    return S_OK;
}

STDAPI ApuAlloc(void** va, APU_ADDRESS* pa, UINT32 size, UINT32 alignment, UINT32 flags) {
    if (va) *va = nullptr;
    if (pa) *pa = 0;
    if (!va || !size || alignment < 4 || (alignment & (alignment - 1)) ||
        (flags != APU_ALLOC_CACHED && flags != APU_ALLOC_NONCACHED))
        return E_INVALIDARG;
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // 64 KiB aligned
    if (!p) return E_OUTOFMEMORY;
    std::lock_guard<std::mutex> l(g_lock);
    uint64_t align = alignment < 4096 ? 4096 : alignment;
    uint64_t apu = (g_next_apu + align - 1) & ~(align - 1);
    if (apu + size >= 0xFFFFFFFFull) {
        VirtualFree(p, 0, MEM_RELEASE);
        return E_OUTOFMEMORY;
    }
    g_next_apu = apu + size;
    g_blocks[(uintptr_t)p] = {size, (APU_ADDRESS)apu};
    g_by_apu[(APU_ADDRESS)apu] = (uintptr_t)p;
    g_used[flags] += size;
    *va = p;
    if (pa) *pa = (APU_ADDRESS)apu;
    return S_OK;
}

STDAPI_(void) ApuFree(void* va) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_blocks.find((uintptr_t)va);
    if (it == g_blocks.end()) return;
    g_by_apu.erase(it->second.apu);
    g_blocks.erase(it);
    VirtualFree(va, 0, MEM_RELEASE);
}

STDAPI_(APU_ADDRESS) ApuMapVirtualAddress(const void* va) {
    std::lock_guard<std::mutex> l(g_lock);
    uintptr_t base;
    Block b;
    if (!lookup((uintptr_t)va, &base, &b)) return 0;
    return b.apu + (APU_ADDRESS)((uintptr_t)va - base);
}

STDAPI_(void*) ApuMapApuAddress(APU_ADDRESS pa) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_by_apu.upper_bound(pa);
    if (it == g_by_apu.begin()) return nullptr;
    --it;
    const Block& b = g_blocks[it->second];
    if (pa >= it->first + b.size) return nullptr;
    return (void*)(it->second + (pa - it->first));
}

STDAPI_(bool) ApuIsVirtualAddressValid(const void* va, UINT32 physicalAlignment) {
    APU_ADDRESS pa = ApuMapVirtualAddress(va);
    return pa && (!physicalAlignment || (pa % physicalAlignment) == 0);
}

STDAPI ApuHeapGetState(ApuHeapState* s, UINT32 flags) {
    if (!s || (flags != APU_ALLOC_CACHED && flags != APU_ALLOC_NONCACHED)) return E_INVALIDARG;
    std::lock_guard<std::mutex> l(g_lock);
    s->bytesAllocated = (UINT32)g_used[flags];
    s->bytesFree = 64u << 20;
    s->bytesLost = 0;
    s->maximumBlockSizeAvailable = 64u << 20;
    s->allocationCount = (UINT32)g_blocks.size();
    return S_OK;
}

// The ACP (audio control processor) without the audio: it speaks the protocol (commands complete
// at once, audio frames tick every 128 samples at 48 kHz, a loaded flowgraph "completes" each frame
// it runs) but SHAPE/XMA/DMA do no processing, so the title's audio engine runs and stays silent.
namespace {
constexpr DWORD kFrameUs = 2667;  // SHAPE frame: 128 samples at 48 kHz

class SilentAcp final : public IAcpHal {
  public:
    STDMETHOD(Connect)(UINT32 numCommands, UINT32 numMessages) override {
        if (!numCommands) return E_INVALIDARG;
        std::lock_guard<std::mutex> l(m_);
        if (connected_) return ACP_E_ALREADY_CONNECTED;
        connected_ = true;
        maxMessages_ = numMessages;
        stop_ = false;
        thread_ = CreateThread(nullptr, 0, &SilentAcp::tick_main, this, 0, nullptr);
        XSL_INFO("audio", "ACP connected (%u commands, %u messages): silent software ACP", numCommands, numMessages);
        return S_OK;
    }
    STDMETHOD(Disconnect)() override {
        {
            std::lock_guard<std::mutex> l(m_);
            if (!connected_) return ACP_E_NOT_CONNECTED;
            connected_ = false;
            stop_ = true;
        }
        if (thread_) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        std::lock_guard<std::mutex> l(m_);
        ACP_MESSAGE msg{};
        msg.type = ACP_MESSAGE_TYPE_DISCONNECTED;
        push(msg, ACP_MESSAGE_TYPE_DISCONNECTED);
        return S_OK;
    }
    STDMETHOD(SubmitCommand)(ACP_COMMAND_TYPE command, UINT64 commandId, UINT32, const void* data, APU_ADDRESS notification) override {
        std::lock_guard<std::mutex> l(m_);
        if (!connected_) return ACP_E_NOT_CONNECTED;
        switch (command) {
            case ACP_COMMAND_TYPE_REGISTER_MESSAGE:
                if (data) registered_ |= static_cast<const ACP_COMMAND_MESSAGE*>(data)->message;
                break;
            case ACP_COMMAND_TYPE_UNREGISTER_MESSAGE:
                if (data) registered_ &= ~static_cast<const ACP_COMMAND_MESSAGE*>(data)->message;
                break;
            case ACP_COMMAND_TYPE_LOAD_SHAPE_FLOWGRAPH:
                if (data) {
                    auto* f = static_cast<const ACP_COMMAND_LOAD_SHAPE_FLOWGRAPH*>(data);
                    flowgraph_ = f->flowgraph;
                    persist_ = f->persist != 0;
                    running_ = !f->waitForStart;
                }
                break;
            case ACP_COMMAND_TYPE_START_FLOWGRAPH: running_ = flowgraph_ != 0; break;
            default: break;  // context updates: nothing processes the contexts
        }
        if (notification)
            if (auto* flag = (volatile UINT32*)ApuMapApuAddress(notification)) *flag = 1;
        if (commandId) {
            ACP_MESSAGE msg{};
            msg.type = ACP_MESSAGE_TYPE_COMMAND_COMPLETED;
            msg.commandCompleted.commandType = command;
            msg.commandCompleted.commandId = commandId;
            msg.commandCompleted.audioFrame = frame_;
            push(msg, ACP_MESSAGE_TYPE_COMMAND_COMPLETED);
        }
        return S_OK;
    }
    STDMETHOD_(bool, PopMessage)(ACP_MESSAGE* message) override {
        std::lock_guard<std::mutex> l(m_);
        if (queue_.empty() || !message) return false;
        *message = queue_.front();
        message->droppedMessageCount = dropped_;
        dropped_ = 0;
        queue_.erase(queue_.begin());
        return true;
    }
    STDMETHOD_(UINT32, GetNumMessages)() override {
        std::lock_guard<std::mutex> l(m_);
        return (UINT32)queue_.size();
    }
    STDMETHOD_(void, Release)() override {
        if (connected_) Disconnect();
        delete this;
    }

  private:
    void push(ACP_MESSAGE msg, UINT32 type) {
        if (!(registered_ & type) && type != ACP_MESSAGE_TYPE_DISCONNECTED) return;
        if (queue_.size() >= (maxMessages_ ? maxMessages_ : 1)) {
            dropped_++;
            return;
        }
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        msg.usec = (UINT32)(t.QuadPart * 1000000 / f.QuadPart);
        queue_.push_back(msg);
    }
    static DWORD WINAPI tick_main(void* p) {
        auto* self = static_cast<SilentAcp*>(p);
        LARGE_INTEGER f, start, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&start);
        for (uint64_t n = 1;; n++) {
            for (;;) {  // sleep to the next frame boundary without drift
                QueryPerformanceCounter(&now);
                int64_t due = start.QuadPart + (int64_t)(n * kFrameUs * f.QuadPart / 1000000);
                if (now.QuadPart >= due) break;
                Sleep((DWORD)((due - now.QuadPart) * 1000 / f.QuadPart) > 1 ? 1 : 0);
            }
            std::lock_guard<std::mutex> l(self->m_);
            if (self->stop_) return 0;
            self->frame_++;
            ACP_MESSAGE msg{};
            msg.type = ACP_MESSAGE_TYPE_AUDIO_FRAME_START;
            msg.audioFrameStart.audioFrame = self->frame_;
            self->push(msg, ACP_MESSAGE_TYPE_AUDIO_FRAME_START);
            if (self->running_ && self->flowgraph_) {
                ACP_MESSAGE done{};
                done.type = ACP_MESSAGE_TYPE_FLOWGRAPH_COMPLETED;
                done.flowgraphCompleted.flowgraph = self->flowgraph_;
                self->push(done, ACP_MESSAGE_TYPE_FLOWGRAPH_COMPLETED);
                if (!self->persist_) self->running_ = false, self->flowgraph_ = 0;
            }
        }
    }

    std::mutex m_;
    bool connected_ = false, stop_ = false, running_ = false, persist_ = false;
    UINT32 maxMessages_ = 0, registered_ = 0, dropped_ = 0, frame_ = 0;
    APU_ADDRESS flowgraph_ = 0;
    std::vector<ACP_MESSAGE> queue_;
    HANDLE thread_ = nullptr;
};
}  // namespace

STDAPI AcpHalCreate(IAcpHal** acp) {
    if (!acp) return E_INVALIDARG;
    *acp = new (std::nothrow) SilentAcp();
    return *acp ? S_OK : E_OUTOFMEMORY;
}

STDAPI AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* c) {
    if (!c) return E_INVALIDARG;
    if (g_ctx_allocated) return ACP_E_ALREADY_ALLOCATED;
    HRESULT hr = S_OK;
    if (SUCCEEDED(hr)) hr = alloc_array(c->numSrcContexts, &c->srcContextArray, &c->apuSrcContextArray);
    if (SUCCEEDED(hr)) hr = alloc_array(c->numEqCompContexts, &c->eqCompContextArray, &c->apuEqCompContextArray);
    if (SUCCEEDED(hr)) hr = alloc_array(c->numFiltVolContexts, &c->filtVolContextArray, &c->apuFiltVolContextArray);
    if (SUCCEEDED(hr)) hr = alloc_array(c->numDmaContexts, &c->dmaContextArray, &c->apuDmaContextArray);
    if (SUCCEEDED(hr)) hr = alloc_array(c->numXmaContexts, &c->xmaContextArray, &c->apuXmaContextArray);
    if (SUCCEEDED(hr)) hr = alloc_array(c->numPcmContexts, &c->pcmContextArray, &c->apuPcmContextArray);
    XSL_INFO("audio", "AcpHalAllocateShapeContexts(src %u, eq %u, filtvol %u, dma %u, xma %u, pcm %u): %08lx",
             c->numSrcContexts, c->numEqCompContexts, c->numFiltVolContexts, c->numDmaContexts, c->numXmaContexts,
             c->numPcmContexts, (unsigned long)hr);
    if (FAILED(hr)) return hr;
    g_ctx = *c;
    g_ctx_allocated = true;
    return S_OK;
}

STDAPI AcpHalReleaseShapeContexts(void) {
    if (!g_ctx_allocated) return ACP_E_NOT_ALLOCATED;
    for (void* p : {(void*)g_ctx.srcContextArray, (void*)g_ctx.eqCompContextArray, (void*)g_ctx.filtVolContextArray,
                    (void*)g_ctx.dmaContextArray, (void*)g_ctx.xmaContextArray, (void*)g_ctx.pcmContextArray})
        if (p) ApuFree(p);
    g_ctx_allocated = false;
    return S_OK;
}
