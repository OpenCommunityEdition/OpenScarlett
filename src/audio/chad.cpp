// SPDX-License-Identifier: AGPL-3.0-or-later
// chad.dll: the console's audio DSP hardware (convolution reverb, FFT) behind XDsp*. A PC has no
// such device; this software stand-in accepts every stream and command and completes each command
// at once with a silent output block, so titles that require the connection (some stop when
// XDspConnect fails) run. The processing itself (convolution, FFT) is not implemented yet: the
// effect is missing from the mix, the dry signal is unaffected.
#include <windows.h>

#include <mf_x/xdspaudio.h>

#include "xsl/log.h"

#define CH "audio"

struct XDSPClient { long streams = 0; };
struct XDSPStream {
    XDSPClient* client;
    XDspActivationParameters params;
    XDspStatus status{};
    volatile long sequence = 0;
};

extern "C" {
STDAPI XDspConnectWithMaximumStreamLimit(float* base, uint32_t baseLen, uint32_t irSeconds, uint32_t maxStreams, uint32_t maxChannels, XDspClientHandle* handle) {
    if (!handle) return E_INVALIDARG;
    *handle = new XDSPClient;
    XSL_INFO(CH, "XDspConnect(%u bytes, %u s of impulse responses, %u streams, %u channels): software stand-in, output is silence", baseLen, irSeconds,
             maxStreams, maxChannels);
    return S_OK;
}
STDAPI XDspConnect(float* base, uint32_t baseLen, uint32_t irSeconds, XDspClientHandle* handle) {
    return XDspConnectWithMaximumStreamLimit(base, baseLen, irSeconds, 0, 0, handle);
}
STDAPI XDspDisconnect(XDspClientHandle h) {
    if (!h) return E_INVALIDARG;
    if (h->streams) return XDSP_E_NOT_ALL_HANDLES_DEACTIVATED;
    delete h;
    return S_OK;
}
STDAPI XDspActivate(XDspClientHandle c, XDspActivationParameters* p, XDspStatus** status, XDspStreamHandle* out) {
    if (!c || !p || !out) return E_INVALIDARG;
    auto* s = new XDSPStream{c, *p};
    InterlockedIncrement(&c->streams);
    if (status) *status = &s->status;
    *out = s;
    return S_OK;
}
STDAPI XDspDeactivate(XDspStreamHandle s) {
    if (!s) return E_INVALIDARG;
    InterlockedDecrement(&s->client->streams);
    delete s;
    return S_OK;
}
STDAPI XDspSubmitCommandWithEnvelope(XDspStreamHandle s, XDspCommand* c, float*, uint32_t* sequence) {
    if (!s || !c) return E_INVALIDARG;
    uint32_t ch = s->params.channelCount ? s->params.channelCount : 1;
    if (c->outputBlockBuffer) memset(c->outputBlockBuffer, 0, (size_t)s->params.blockFrameCount * ch * sizeof(float));
    uint32_t n = (uint32_t)InterlockedIncrement(&s->sequence);
    s->status.lastSubmittedCommandSequence = n;
    s->status.lastProcessedCommandSequence = n;
    if (sequence) *sequence = n;
    return S_OK;
}
STDAPI XDspSubmitCommand(XDspStreamHandle s, XDspCommand* c, uint32_t* sequence) { return XDspSubmitCommandWithEnvelope(s, c, nullptr, sequence); }
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
