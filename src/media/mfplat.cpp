// SPDX-License-Identifier: AGPL-3.0-or-later
//
// MFPlat.dll: the console's Media Foundation platform DLL is the PC one plus console-only
// exports, and a title that imports one of those fails to load on a PC ("The procedure entry
// point MFResetDXGIDeviceManagerX could not be located"). This DLL, placed next to the title,
// exports everything the system mfplat.dll does, forwarding each call to the system copy, and
// implements the console additions. It forwards the full export set because system Media
// Foundation DLLs (mfreadwrite, mfcore, mf) delay-load "MFPlat.DLL" by name and bind to whichever
// module of that name the process has loaded.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

#include <cstring>
#include <string>

#include "xsl/log.h"

extern "C" void* xsl_fwd_table[];
extern "C" const char* const xsl_fwd_names[];
extern "C" const unsigned xsl_fwd_count;

namespace {
HMODULE g_system = nullptr;

// Unresolved exports land here instead of jumping to address 0.
HRESULT WINAPI missing_export() { return E_NOTIMPL; }

void resolve() {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring path = std::wstring(dir, n) + L"\\mfplat.dll";
    // By full path: loading "mfplat.dll" by name would return this module.
    g_system = LoadLibraryW(path.c_str());
    unsigned missing = 0;
    for (unsigned i = 0; i < xsl_fwd_count; i++) {
        void* p = g_system ? (void*)GetProcAddress(g_system, xsl_fwd_names[i]) : nullptr;
        xsl_fwd_table[i] = p ? p : (void*)&missing_export;
        missing += !p;
    }
    if (!g_system)
        XSL_ERR("media", "MFPlat.dll: the system's %ls could not be loaded; Media Foundation calls fail", path.c_str());
    else if (missing)
        XSL_INFO("media", "MFPlat.dll: %u export(s) absent from the system copy return E_NOTIMPL", missing);
}
}  // namespace

// Console-only: re-binds the DXGI device manager to a device (the console's variant of
// IMFDXGIDeviceManager::ResetDevice). The title passes its console D3D12 device; PC Media
// Foundation accepts it only if it is a PC device, so a refusal leaves the manager without a
// device and Media Foundation decodes in software, which the title is told succeeded.
// Console objects made by d3d12_xs.dll carry a 64-byte header in front of the interface:
// magic 'XSLO' (dword 0) and the PC object they front (pointer at offset 8; src/gfx/xs_object.h).
static IUnknown* pc_device_of(IUnknown* device) {
    if (!device) return nullptr;
    const char* h = reinterpret_cast<const char*>(device) - 64;
    uint32_t magic;
    memcpy(&magic, h, 4);
    if (magic != 0x4F4C5358) return device;   // not a layer object: pass through
    IUnknown* pc;
    memcpy(&pc, h + 8, sizeof pc);
    return pc;
}

extern "C" HRESULT WINAPI MFResetDXGIDeviceManagerX(IMFDXGIDeviceManager* manager, IUnknown* device, UINT token) {
    if (!manager) return E_POINTER;
    // PC Media Foundation needs the PC device, not the console object fronting it.
    HRESULT hr = manager->ResetDevice(pc_device_of(device), token);
    if (FAILED(hr)) {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            XSL_WARN("media", "MFResetDXGIDeviceManagerX: PC Media Foundation refused the console device (%08lx); video decodes in software", hr);
    }
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) resolve();
    return TRUE;
}
