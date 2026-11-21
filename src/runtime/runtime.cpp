// SPDX-License-Identifier: AGPL-3.0-or-later
//
// xgameruntime.dll replacement.
//
// Protocol, from the title-side static library (xgameruntime.lib, docs/provenance.md):
//   * XGameRuntimeInitialize* loads "xgameruntime.dll" with LoadLibraryExW and resolves
//     QueryApiImpl, InitializeApiImpl, InitializeApiImplEx, InitializeApiImplEx2 and
//     UninitializeApiImpl by name, preferring the newest initializer present.
//   * Every API thunk calls QueryApiImpl(&classGuid, &interfaceGuid, &obj) — in this SDK
//     the two GUIDs are the same — calls one vtable slot of obj with the caller's
//     arguments, then Release (slot 2).
// A class this DLL does not provide answers REGDB_E_CLASSNOTREG, which the library turns
// into E_GAMERUNTIME_NOT_INITIALIZED-family errors ("XGameRuntime is missing ...").
#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "runtime_classes.h"
#include "xsl/log.h"

#define CH "rt"

static void guid_str(REFGUID g, char* b, size_t n) {
    snprintf(b, n, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
             g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

extern "C" HRESULT QueryApiImpl(const GUID* cls, const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (void* obj = xsl_rt::find_class(*cls, *iid)) {
        *out = obj;
        return S_OK;
    }
    char a[48], b[48];
    guid_str(*cls, a, sizeof a);
    guid_str(*iid, b, sizeof b);
    XSL_WARN(CH, "QueryApiImpl: class %s (iid %s) not provided", a, b);
    return REGDB_E_CLASSNOTREG;
}

extern "C" HRESULT InitializeApiImpl(uint64_t a, uint64_t b) {
    XSL_INFO(CH, "InitializeApiImpl(%llx, %llx)", (unsigned long long)a, (unsigned long long)b);
    return S_OK;
}
extern "C" HRESULT InitializeApiImplEx(uint64_t a, uint64_t b, uint64_t c) {
    XSL_INFO(CH, "InitializeApiImplEx(%llx, %llx, %llx)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return S_OK;
}
extern "C" HRESULT InitializeApiImplEx2(uint64_t a, uint64_t b, uint64_t c, void* options) {
    XSL_INFO(CH, "InitializeApiImplEx2(%llx, %llx, %llx, %p)", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, options);
    return S_OK;
}
extern "C" void UninitializeApiImpl() { XSL_INFO(CH, "UninitializeApiImpl"); }

// Error sink the static library requires before it will initialize (g_errorReport).
extern "C" void XErrorReport(HRESULT hr, const char* msg) { XSL_ERR(CH, "XErrorReport %08lx: %s", hr, msg ? msg : ""); }
