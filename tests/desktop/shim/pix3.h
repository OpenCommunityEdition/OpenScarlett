// SPDX-License-Identifier: MIT
// Reference builds only: the samples' desktop projects take pix3.h from the WinPixEventRuntime NuGet
// package, which is not part of the GDK. PIX markers do not affect rendering, so they are no-ops here.
#pragma once
#include <cstdint>
#define PIX_COLOR(r, g, b) ((UINT32)(0xff000000u | ((r) << 16) | ((g) << 8) | (b)))
#define PIX_COLOR_INDEX(i) ((UINT64)(i))
#define PIX_COLOR_DEFAULT PIX_COLOR_INDEX(0)
template <class... A> inline void PIXBeginEvent(A&&...) {}
template <class... A> inline void PIXEndEvent(A&&...) {}
template <class... A> inline void PIXSetMarker(A&&...) {}
template <class... A> inline void PIXScopedEvent(A&&...) {}
template <class... A> inline void PIXReportCounter(A&&...) {}
template <class... A> inline void PIXNotifyWakeFromFenceSignal(A&&...) {}
class PIXScopedEventObject {
public:
    template <class... A> PIXScopedEventObject(A&&...) {}
};
