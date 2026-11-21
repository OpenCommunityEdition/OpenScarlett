// SPDX-License-Identifier: AGPL-3.0-or-later
// XSL_FIXED_FPS=<n> (testing): the title's QueryPerformanceCounter advances exactly 1/n second per
// presented frame, so time-animated titles render the same frame N on every run (frame-by-frame
// A/B comparisons). Time continues from the real counter value when the device is created; reads
// within a frame advance by one tick so the counter stays strictly increasing.
#include <windows.h>

#include <atomic>
#include <cstdlib>

#include "xsl/log.h"
#include "xsl_platform.h"

namespace {
std::atomic<long long> g_frames{0}, g_calls{0};
long long g_base = 0, g_step = 0;

BOOL WINAPI Fixed_QueryPerformanceCounter(LARGE_INTEGER* c) {
    if (!c) return FALSE;
    c->QuadPart = g_base + g_frames.load() * g_step + (g_calls.fetch_add(1) % (g_step > 2 ? g_step / 2 : 1));
    return TRUE;
}
}  // namespace

void xsl_fixed_time_install() {
    const char* e = getenv("XSL_FIXED_FPS");
    int fps = e ? atoi(e) : 0;
    if (fps <= 0 || g_step) return;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    g_step = f.QuadPart / fps;
    g_base = now.QuadPart;
    int n = xsl::platform::patch_import(GetModuleHandleW(nullptr), "", "QueryPerformanceCounter", (void*)Fixed_QueryPerformanceCounter);
    XSL_INFO("gfx", "fixed time step: %d frames per second (%d import(s) patched)", fps, n);
}

void xsl_fixed_time_frame() {
    if (g_step) g_frames.fetch_add(1);
}
