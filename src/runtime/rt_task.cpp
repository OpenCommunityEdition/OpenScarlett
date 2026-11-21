// SPDX-License-Identifier: AGPL-3.0-or-later
// XTaskQueue and XAsync: Microsoft's MIT-licensed implementation from libHttpClient
// (third_party/libHttpClient-task), which shares the GDK's API and semantics.
#include <windows.h>

#include <XAsync.h>
#include <XAsyncProvider.h>
#include <XTaskQueue.h>

#include "runtime_classes.h"

XSL_RT_FORWARD(XTaskQueueCreate);
XSL_RT_FORWARD(XTaskQueueCreateComposite);
XSL_RT_FORWARD(XTaskQueueGetPort);
XSL_RT_FORWARD(XTaskQueueDuplicateHandle);
XSL_RT_FORWARD(XTaskQueueDispatch);
static void M_XTaskQueueCloseHandle(void*, XTaskQueueHandle q) { if (q) XTaskQueueCloseHandle(q); }
XSL_RT_REGISTER(XTaskQueueCloseHandle, &M_XTaskQueueCloseHandle);
// libHttpClient dereferences a null queue handle; the console runtime rejects it.
static HRESULT M_XTaskQueueTerminate(void*, XTaskQueueHandle q, bool wait, void* ctx, XTaskQueueTerminatedCallback* cb) {
    return q ? XTaskQueueTerminate(q, wait, ctx, cb) : E_INVALIDARG;
}
XSL_RT_REGISTER(XTaskQueueTerminate, &M_XTaskQueueTerminate);
XSL_RT_FORWARD(XTaskQueueSubmitCallback);
XSL_RT_FORWARD(XTaskQueueSubmitDelayedCallback);
XSL_RT_FORWARD(XTaskQueueRegisterWaiter);
XSL_RT_FORWARD(XTaskQueueUnregisterWaiter);
XSL_RT_FORWARD(XTaskQueueGetCurrentProcessTaskQueue);
XSL_RT_FORWARD(XTaskQueueSetCurrentProcessTaskQueue);
XSL_RT_FORWARD(XTaskQueueRegisterMonitor);
XSL_RT_FORWARD(XTaskQueueUnregisterMonitor);
XSL_RT_FORWARD(XAsyncGetStatus);
XSL_RT_FORWARD(XAsyncGetResultSize);
XSL_RT_FORWARD(XAsyncCancel);
XSL_RT_FORWARD(XAsyncRun);
XSL_RT_FORWARD(XAsyncBegin);
XSL_RT_FORWARD(XAsyncSchedule);
XSL_RT_FORWARD(XAsyncComplete);
XSL_RT_FORWARD(XAsyncGetResult);

// ---- Immediate async completion for runtime classes -------------------------------------------
#include <cstring>
#include <vector>

#include "rt_async.h"

namespace {
struct Immediate {
    HRESULT status;
    std::vector<unsigned char> bytes;
};
HRESULT CALLBACK immediate_provider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* c = static_cast<Immediate*>(data->context);
    switch (op) {
        case XAsyncOp::Begin: return XAsyncSchedule(data->async, 0);
        case XAsyncOp::DoWork: XAsyncComplete(data->async, c->status, c->bytes.size()); return S_OK;
        case XAsyncOp::GetResult: if (!c->bytes.empty()) memcpy(data->buffer, c->bytes.data(), c->bytes.size()); return S_OK;
        case XAsyncOp::Cleanup: delete c; return S_OK;
        default: return S_OK;
    }
}
}  // namespace

HRESULT xsl_async_immediate(XAsyncBlock* async, const void* identity, const char* name, HRESULT status, const void* result, size_t size) {
    auto* c = new Immediate{status, {}};
    if (FAILED(status)) size = 0;
    if (size) c->bytes.assign((const unsigned char*)result, (const unsigned char*)result + size);
    HRESULT hr = XAsyncBegin(async, c, identity, name, immediate_provider);
    if (FAILED(hr)) delete c;
    return hr;
}
HRESULT xsl_async_result(XAsyncBlock* async, const void* identity, size_t size, void* buffer, size_t* used) {
    return XAsyncGetResult(async, identity, size, buffer, used);
}
HRESULT xsl_async_result_size(XAsyncBlock* async, size_t* size) { return XAsyncGetResultSize(async, size); }
