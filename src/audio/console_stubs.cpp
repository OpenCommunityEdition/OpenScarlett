// SPDX-License-Identifier: AGPL-3.0-or-later
// Console-only DLLs with no host hardware behind them: PIXEvt.dll (PIX capture control, CPU
// counters), XFrontPanelDisplay.dll (dev-kit front panel). Each DLL's .def maps its exports onto these behaviours; arguments are ignored,
// which the x64 calling convention allows.
#include <windows.h>

#include "xsl/log.h"

extern "C" {
// HRESULT: the device does not exist (logged once per DLL).
HRESULT WINAPI xsl_no_device(void) {
    static LONG once;
    if (!InterlockedExchange(&once, 1)) XSL_INFO("platform", "console-only device requested: not present on this host");
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
// 0 / false / nullptr / "not capturing".
UINT64 WINAPI xsl_zero(void) { return 0; }
// S_OK for calls whose only effect is on hardware that is absent (disconnect, deactivate, ...).
HRESULT WINAPI xsl_ok(void) { return S_OK; }
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
