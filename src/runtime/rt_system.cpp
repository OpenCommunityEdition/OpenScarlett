// SPDX-License-Identifier: AGPL-3.0-or-later
// XSystem, XGame, XPackage, XThread, XNetworking and XDisplay basics. Values that identify a
// console or user are synthetic and stable per machine; nothing reaches Xbox services.
#include <windows.h>

#include <XDisplay.h>
#include <XGame.h>
#include <XGameRuntimeFeature.h>
#include <XNetworking.h>
#include <XPackage.h>
#include <XSystem.h>
#include <XThread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime_classes.h"
#include "xsl/log.h"

#define CH "rt"

namespace {
HRESULT copy_str(const char* s, size_t size, char* out, size_t* used) {
    size_t n = strlen(s) + 1;
    if (used) *used = n;
    if (!out || size < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(out, s, n);
    return S_OK;
}

// Title ID and identity come from the MicrosoftGame.config next to the executable, if present.
struct Identity {
    uint32_t titleId = 0;
    std::string packageId;
    Identity() {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir(exe);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        std::string base = std::string(exe).substr(dir.size());
        packageId = base.substr(0, base.find_last_of('.')) + "_1.0.0.0_x64__xslayer";
        FILE* f = fopen((dir + "MicrosoftGame.config").c_str(), "rb");
        if (!f) return;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
        auto p = text.find("<TitleId>");
        if (p != std::string::npos) titleId = strtoul(text.c_str() + p + 9, nullptr, 16);
        auto q = text.find("Name=\"", text.find("<Identity"));
        if (q != std::string::npos) {
            auto e = text.find('"', q + 6);
            packageId = text.substr(q + 6, e - q - 6) + "_1.0.0.0_x64__xslayer";
        }
    }
};
const Identity& identity() { static Identity i; return i; }

thread_local bool t_timeSensitive = false;

HRESULT M_XGameGetXboxTitleId(void*, uint32_t* id) { *id = identity().titleId; return S_OK; }
HRESULT M_XSystemGetConsoleId(void*, size_t size, char* out, size_t* used) {
    // 38 characters + NUL = XSystemConsoleIdBytes.
    return copy_str("0000000000000000.XSLAYER00000000000000", size, out, used);
}
HRESULT M_XSystemGetXboxLiveSandboxId(void*, size_t size, char* out, size_t* used) { return copy_str("RETAIL", size, out, used); }
HRESULT M_XSystemGetAppSpecificDeviceId(void*, size_t size, char* out, size_t* used) {
    return copy_str("XSLAYER-LOCAL-DEVICE-0000000000000000000000", size, out, used);
}
// Returns a struct by value: the title calls runtime methods as C++ member functions, so the
// hidden return buffer comes after `this` (rdx), not first as for a free function (rcx).
XSystemAnalyticsInfo* M_XSystemGetAnalyticsInfo(void*, XSystemAnalyticsInfo* ret) {
    XSystemAnalyticsInfo& i = *ret;
    i = {};
    i.osVersion = {10, 0, 26100, 0};
    i.hostingOsVersion = {10, 0, 26100, 0};
    strcpy_s(i.family, "Gaming");
    strcpy_s(i.form, "Xbox Series X");
    return ret;
}
HRESULT M_XPackageGetCurrentProcessPackageIdentifier(void*, size_t size, char* buf) {
    return copy_str(identity().packageId.c_str(), size, buf, nullptr);
}
bool M_XPackageIsPackagedProcess(void*) { return true; }
HRESULT M_XPackageGetUserLocale(void*, size_t size, char* buf) { return copy_str("en-US", size, buf, nullptr); }
// Every API family exists in this runtime (individual APIs may still fail); a console reports all as available.
bool M_XGameRuntimeIsFeatureAvailable(void*, XGameRuntimeFeature) { return true; }
HRESULT M_XThreadSetTimeSensitive(void*, bool v) { t_timeSensitive = v; return S_OK; }
bool M_XThreadIsTimeSensitive(void*) { return t_timeSensitive; }
void M_XThreadAssertNotTimeSensitive(void*) {}
HRESULT M_XThreadVerifyNotTimeSensitive(void*) { return t_timeSensitive ? E_FAIL : S_OK; }
HRESULT M_XNetworkingGetConnectivityHint(void*, XNetworkingConnectivityHint* h) {
    // Offline by construction: online services are never contacted.
    *h = {};
    h->connectivityLevel = XNetworkingConnectivityLevelHint::LocalAccess;
    h->connectivityCost = XNetworkingConnectivityCostHint::Unrestricted;
    h->networkInitialized = true;
    return S_OK;
}
XDisplayHdrModeResult M_XDisplayTryEnableHdrMode(void*, XDisplayHdrModePreference, XDisplayHdrModeInfo* info) {
    if (info) *info = {};
    return XDisplayHdrModeResult::Disabled;
}
}  // namespace

XSL_RT_REGISTER(XGameGetXboxTitleId, &M_XGameGetXboxTitleId);
XSL_RT_REGISTER(XSystemGetConsoleId, &M_XSystemGetConsoleId);
XSL_RT_REGISTER(XSystemGetXboxLiveSandboxId, &M_XSystemGetXboxLiveSandboxId);
XSL_RT_REGISTER(XSystemGetAppSpecificDeviceId, &M_XSystemGetAppSpecificDeviceId);
XSL_RT_REGISTER(XSystemGetAnalyticsInfo, &M_XSystemGetAnalyticsInfo);
XSL_RT_REGISTER(XPackageGetCurrentProcessPackageIdentifier, &M_XPackageGetCurrentProcessPackageIdentifier);
XSL_RT_REGISTER(XPackageIsPackagedProcess, &M_XPackageIsPackagedProcess);
XSL_RT_REGISTER(XPackageGetUserLocale, &M_XPackageGetUserLocale);
XSL_RT_REGISTER(XGameRuntimeIsFeatureAvailable, &M_XGameRuntimeIsFeatureAvailable);
XSL_RT_REGISTER(XThreadSetTimeSensitive, &M_XThreadSetTimeSensitive);
XSL_RT_REGISTER(XThreadIsTimeSensitive, &M_XThreadIsTimeSensitive);
XSL_RT_REGISTER(XThreadAssertNotTimeSensitive, &M_XThreadAssertNotTimeSensitive);
XSL_RT_REGISTER(XThreadVerifyNotTimeSensitive, &M_XThreadVerifyNotTimeSensitive);
XSL_RT_REGISTER(XNetworkingGetConnectivityHint, &M_XNetworkingGetConnectivityHint);
XSL_RT_REGISTER(XDisplayTryEnableHdrMode, &M_XDisplayTryEnableHdrMode);
