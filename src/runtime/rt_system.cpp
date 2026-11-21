// SPDX-License-Identifier: AGPL-3.0-or-later
// XSystem, XGame, XPackage, XThread, XNetworking and XDisplay basics. Values that identify a
// console or user are synthetic and stable per machine; nothing reaches Xbox services.
#include <windows.h>

#include <XDisplay.h>
#include <XError.h>
#include <XGame.h>
#include <XGameUI.h>
#include <XGameRuntimeFeature.h>
#include <XNetworking.h>
#include <XPackage.h>
#include <XSystem.h>
#include <XThread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <set>
#include <vector>

#include "rt_async.h"
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

std::string read_text(const std::string& path) {
    std::string text;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

// The title package's root: the directory holding MicrosoftGame.config, which is the
// executable's directory or, for titles such as Unreal Engine ones, a few levels above it.
const std::string& package_root() {
    static std::string root = [] {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir(exe);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        std::string d = dir;
        for (int i = 0; i < 6 && d.size() > 3; i++) {
            if (GetFileAttributesA((d + "MicrosoftGame.config").c_str()) != INVALID_FILE_ATTRIBUTES) return d;
            d = d.substr(0, d.find_last_of("\\/", d.size() - 2) + 1);
        }
        return dir;
    }();
    return root;
}

// Title ID and identity come from the package's MicrosoftGame.config, if present.
struct Identity {
    uint32_t titleId = 0;
    std::string packageId;
    Identity() {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string base(exe);
        base = base.substr(base.find_last_of("\\/") + 1);
        packageId = base.substr(0, base.find_last_of('.')) + "_1.0.0.0_x64__xslayer";
        std::string text = read_text(package_root() + "MicrosoftGame.config");
        if (text.empty()) return;
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
// Install chunks. A dumped package is installed completely, so every chunk, language, tag and
// feature its layout (layout_*.xml in the package root) declares is Ready.
struct Chunks {
    std::vector<uint32_t> ids;
    std::set<std::string> languages, tags, features;
    Chunks() {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((package_root() + "layout*.xml").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::string text = read_text(package_root() + fd.cFileName);
        FindClose(h);
        auto attr = [](const std::string& tag, const char* name) {
            std::string key = std::string(" ") + name + "=\"";
            auto p = tag.find(key);
            if (p == std::string::npos) return std::string();
            p += key.size();
            return tag.substr(p, tag.find('"', p) - p);
        };
        auto split = [](const std::string& v, std::set<std::string>& out) {
            size_t a = 0;
            while (a < v.size()) {
                size_t b = v.find_first_of(";, ", a);
                if (b == std::string::npos) b = v.size();
                if (b > a) out.insert(v.substr(a, b - a));
                a = b + 1;
            }
        };
        for (size_t p = text.find("<Chunk "); p != std::string::npos; p = text.find("<Chunk ", p + 1)) {
            std::string tag = text.substr(p, text.find('>', p) - p);
            std::string id = attr(tag, "Id");
            if (!id.empty()) ids.push_back((uint32_t)strtoul(id.c_str(), nullptr, 10));
            split(attr(tag, "Languages"), languages);
            split(attr(tag, "Tags"), tags);
            split(attr(tag, "Features"), features);
        }
        XSL_INFO(CH, "package layout %s: %zu chunk(s), all installed", fd.cFileName, ids.size());
    }
};
const Chunks& chunks() { static Chunks c; return c; }

HRESULT M_XPackageEnumerateChunkAvailability(void*, const char*, XPackageChunkSelectorType type, void* context,
                                             XPackageChunkAvailabilityCallback* callback) {
    if (!callback) return E_INVALIDARG;
    const Chunks& c = chunks();
    XPackageChunkSelector sel{};
    sel.type = type;
    switch (type) {
        case XPackageChunkSelectorType::Chunk:
            for (uint32_t id : c.ids) {
                sel.chunkId = id;
                if (!callback(context, &sel, XPackageChunkAvailability::Ready)) break;
            }
            break;
        case XPackageChunkSelectorType::Language:
        case XPackageChunkSelectorType::Tag:
        case XPackageChunkSelectorType::Feature: {
            const std::set<std::string>& v = type == XPackageChunkSelectorType::Language ? c.languages
                                             : type == XPackageChunkSelectorType::Tag    ? c.tags
                                                                                         : c.features;
            for (const std::string& name : v) {
                sel.language = name.c_str();  // the union's string member for all three kinds
                if (!callback(context, &sel, XPackageChunkAvailability::Ready)) break;
            }
            break;
        }
    }
    return S_OK;
}
HRESULT M_XPackageFindChunkAvailability(void*, const char*, uint32_t, XPackageChunkSelector*, XPackageChunkAvailability* out) {
    if (!out) return E_INVALIDARG;
    *out = XPackageChunkAvailability::Ready;
    return S_OK;
}

// Installation monitors: the package is fully installed, so progress is complete from the start
// and never changes (registered callbacks are kept but have nothing to report).
struct Monitor {
    uint64_t bytes = 0;
};
uint64_t package_bytes() {
    static uint64_t total = [] {
        uint64_t sum = 0;
        std::vector<std::string> dirs{package_root()};
        while (!dirs.empty()) {
            std::string d = dirs.back();
            dirs.pop_back();
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((d + "*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(d + fd.cFileName + "\\");
                else sum += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return sum ? sum : 1;
    }();
    return total;
}
HRESULT M_XPackageCreateInstallationMonitor(void*, const char*, uint32_t, XPackageChunkSelector*, uint32_t, XTaskQueueHandle,
                                            XPackageInstallationMonitorHandle* out) {
    if (!out) return E_INVALIDARG;
    auto* m = new Monitor;
    m->bytes = package_bytes();
    *out = (XPackageInstallationMonitorHandle)m;
    return S_OK;
}
void M_XPackageCloseInstallationMonitorHandle(void*, XPackageInstallationMonitorHandle h) { delete (Monitor*)h; }
void M_XPackageGetInstallationProgress(void*, XPackageInstallationMonitorHandle h, XPackageInstallationProgress* p) {
    if (!p) return;
    uint64_t b = h ? ((Monitor*)h)->bytes : package_bytes();
    *p = {b, b, b, true, true};
}
bool M_XPackageUpdateInstallationMonitor(void*, XPackageInstallationMonitorHandle) { return false; }  // nothing changed
HRESULT M_XPackageRegisterInstallationProgressChanged(void*, XPackageInstallationMonitorHandle, void*, XPackageInstallationProgressCallback*,
                                                      XTaskQueueRegistrationToken* token) {
    if (token) token->token = 1;
    return S_OK;
}
bool M_XPackageUnregisterInstallationProgressChanged(void*, XPackageInstallationMonitorHandle, XTaskQueueRegistrationToken, bool) { return true; }

// System message dialog: there is no system UI to show it in, so its text goes to the log (a
// title that reports a fatal condition this way then usually exits) and the default button is
// the answer.
const char kDialogIdentity[] = "XGameUiShowMessageDialogAsync";
HRESULT M_XGameUiShowMessageDialogAsync(void*, XAsyncBlock* async, const char* title, const char* content, const char* b1,
                                        const char* b2, const char* b3, XGameUiMessageDialogButton def, XGameUiMessageDialogButton) {
    XSL_WARN(CH, "message dialog: \"%s\": \"%s\" [%s%s%s%s%s] -> button %u (default)", title ? title : "", content ? content : "",
             b1 ? b1 : "", b2 ? " | " : "", b2 ? b2 : "", b3 ? " | " : "", b3 ? b3 : "", (unsigned)def);
    return xsl_async_immediate(async, kDialogIdentity, "XGameUiShowMessageDialogAsync", S_OK, &def, sizeof def);
}
HRESULT M_XGameUiShowMessageDialogResult(void*, XAsyncBlock* async, XGameUiMessageDialogButton* out) {
    return xsl_async_result(async, kDialogIdentity, sizeof *out, out, nullptr);
}

// Error reporting callback: kept, so that XErrorReport (where implemented) can reach it.
XErrorCallback* g_error_callback = nullptr;
void* g_error_context = nullptr;
void M_XErrorSetCallback(void*, XErrorCallback* cb, void* ctx) {
    g_error_callback = cb;
    g_error_context = ctx;
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
XSL_RT_REGISTER(XPackageEnumerateChunkAvailability, &M_XPackageEnumerateChunkAvailability);
XSL_RT_REGISTER(XPackageFindChunkAvailability, &M_XPackageFindChunkAvailability);
XSL_RT_REGISTER(XGameUiShowMessageDialogAsync, &M_XGameUiShowMessageDialogAsync);
XSL_RT_REGISTER(XGameUiShowMessageDialogResult, &M_XGameUiShowMessageDialogResult);
XSL_RT_REGISTER(XErrorSetCallback, &M_XErrorSetCallback);
XSL_RT_REGISTER(XPackageCreateInstallationMonitor, &M_XPackageCreateInstallationMonitor);
XSL_RT_REGISTER(XPackageCloseInstallationMonitorHandle, &M_XPackageCloseInstallationMonitorHandle);
XSL_RT_REGISTER(XPackageGetInstallationProgress, &M_XPackageGetInstallationProgress);
XSL_RT_REGISTER(XPackageUpdateInstallationMonitor, &M_XPackageUpdateInstallationMonitor);
XSL_RT_REGISTER(XPackageRegisterInstallationProgressChanged, &M_XPackageRegisterInstallationProgressChanged);
XSL_RT_REGISTER(XPackageUnregisterInstallationProgressChanged, &M_XPackageUnregisterInstallationProgressChanged);
