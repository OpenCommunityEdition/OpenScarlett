// SPDX-License-Identifier: AGPL-3.0-or-later
// xapu.dll: the console's audio decode/sample-rate-convert hardware (Opus decode, PCM convert),
// driven through a command queue. There is no such device on a PC; this is a software stand-in
// that accepts every command and answers each with silence of the length the hardware would have
// produced, so titles that require the connection (some stop when XApuConnect fails) keep their
// audio pipeline running. Decoding real audio is not implemented yet.
#include <windows.h>

#include <deque>
#include <mutex>
#include <vector>

#include <mf_x/xapu.h>

#include "xsl/log.h"

#define CH "audio"

struct XAPU {
    XApuConnectInputParameters in{};
    void* base = nullptr;
    HANDLE event = nullptr;
    std::mutex mu;
    std::deque<XApuResult> results;
    struct Stream { bool active = false; uint32_t channels = 2; float pitch = 1.0f; uint32_t stored = 0; };
    std::vector<Stream> streams;
    uint32_t commands = 0, taken = 0;
};

extern "C" {
STDAPI XApuConnect(XApuConnectInputParameters* params, XApuConnectOutputParameters* data, XApuHandle* handle) {
    if (!params || !data || !handle || !params->maxStreamCount || params->maxStreamCount > 500) return E_INVALIDARG;
    auto* x = new XAPU;
    x->in = *params;
    x->streams.resize(params->maxStreamCount);
    x->base = VirtualAlloc(nullptr, params->baseDataLength ? params->baseDataLength : 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!x->base) { delete x; return E_OUTOFMEMORY; }
    if ((uint32_t)params->options & (uint32_t)XApuConnectOptions::EnableResultReadyEvent) x->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    data->baseData = x->base;
    data->resultReadyEvent = x->event;
    *handle = x;
    XSL_INFO(CH, "XApuConnect(process type %u, %u streams x %u commands, %u bytes, %u frames per output, options %#x): software stand-in, output is silence",
             (unsigned)params->processType, params->maxStreamCount, params->maxQueuedCommandsPerStream, params->baseDataLength,
             params->maxFrameCountPerOutput, (unsigned)params->options);
    return S_OK;
}

STDAPI XApuDisconnect(XApuHandle x) {
    if (!x) return E_INVALIDARG;
    if (x->event) CloseHandle(x->event);
    VirtualFree(x->base, 0, MEM_RELEASE);
    delete x;
    return S_OK;
}

STDAPI XApuEnqueueCommand(XApuHandle x, XApuCommandId* c, uint32_t* total) {
    if (!x || !c) return E_INVALIDARG;
    std::lock_guard<std::mutex> g(x->mu);
    if (c->streamIndex >= x->streams.size()) return XAPU_E_INVALID_STREAM_INDEX;
    if (x->results.size() >= (size_t)x->in.maxStreamCount * (x->in.maxQueuedCommandsPerStream ? x->in.maxQueuedCommandsPerStream : 1)) return XAPU_E_QUEUE_FULL;
    auto& s = x->streams[c->streamIndex];
    XApuResult r{};
    r.id = *c;
    r.errorCode = S_OK;
    r.currentPitch = s.pitch;
    switch (c->type) {
    case XApuCommandType::Activate: {
        auto* a = (XApuDecodeConvertActivateCommand*)c;
        s = {};
        s.active = true;
        s.channels = a->channelCount == 1 ? 1 : 2;
        s.pitch = a->startPitch > 0 ? a->startPitch : 1.0f;
        r.currentPitch = s.pitch;
        break;
    }
    case XApuCommandType::Deactivate: s.active = false; break;
    case XApuCommandType::Reset: s.stored = 0; break;
    case XApuCommandType::Notify: break;
    case XApuCommandType::Process: {
        auto* p = (XApuDecodeConvertCommand*)c;
        const bool pcm = x->in.processType == XApuProcessType::ConvertPCM;
        const bool convert = pcm || x->in.processType == XApuProcessType::DecodeConvertOpus;
        uint32_t frames = p->frameCount == 0xFFFFFFFFu ? (pcm ? p->inputDataLength / (4 * s.channels) : 960) : p->frameCount;
        if (frames > 960) frames = 960;
        frames = frames > p->firstFrameIndex ? frames - p->firstFrameIndex : 0;
        uint32_t out = frames;
        if (convert) {
            // Input frames at the target pitch give frames / pitch output frames; output leaves in
            // blocks of maxFrameCountPerOutput (any remainder with Flush).
            if (p->targetPitch > 0) s.pitch = p->targetPitch;
            s.stored += (uint32_t)(frames / s.pitch);
            const uint32_t block = x->in.maxFrameCountPerOutput ? x->in.maxFrameCountPerOutput : 512;
            const bool flush = ((uint32_t)c->options & (uint32_t)XApuCommandOptions::Flush) != 0;
            out = s.stored >= block ? block : (flush ? s.stored : 0);
            s.stored -= out;
            uint32_t st = out ? (uint32_t)XApuStateOptions::OutputReady : (uint32_t)XApuStateOptions::NeedMoreInput;
            if (s.stored >= block) st |= (uint32_t)XApuStateOptions::MoreOutputAvailable;
            r.state = (XApuStateOptions)st;
            ((uint16_t*)&r.stateData)[0] = (uint16_t)s.stored;
            ((uint16_t*)&r.stateData)[1] = (uint16_t)frames;
            r.currentPitch = s.pitch;
        }
        uint32_t bytes = out * s.channels * 4;
        if (bytes > p->maxOutputDataLength) { bytes = p->maxOutputDataLength & ~15u; out = bytes / (s.channels * 4); }
        if (p->outputData && p->maxOutputDataLength) memset(p->outputData, 0, p->maxOutputDataLength);
        r.outputFrameCount = out;
        r.outputDataLength = bytes;
        r.outputData = p->outputData;
        r.inputDataLength = p->inputDataLength;
        r.inputData = p->inputData;
        break;
    }
    default: return XAPU_E_INVALID_COMMAND;
    }
    x->results.push_back(r);
    x->commands++;
    if (total) *total = x->commands;
    if (x->event && ((uint32_t)c->options & (uint32_t)XApuCommandOptions::SignalOnCompletion)) SetEvent(x->event);
    return S_OK;
}

STDAPI XApuDequeueResult(XApuHandle x, XApuResult* out) {
    if (!x || !out) return E_INVALIDARG;
    std::lock_guard<std::mutex> g(x->mu);
    if (x->results.empty()) return XAPU_E_PENDING_RESULTS;
    *out = x->results.front();
    x->results.pop_front();
    x->taken++;
    return S_OK;
}

STDAPI_(uint32_t) XApuGetResultCount(XApuHandle x, uint32_t* total) {
    if (!x) return 0;
    std::lock_guard<std::mutex> g(x->mu);
    if (total) *total = x->taken + (uint32_t)x->results.size();
    return (uint32_t)x->results.size();
}

STDAPI XApuGetUtilization(XApuProcessType, uint32_t, XApuUtilization* u) {
    if (!u) return E_INVALIDARG;
    u->availableStreamCount = 500;
    u->maximumStreamCount = 500;
    return S_OK;
}
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
