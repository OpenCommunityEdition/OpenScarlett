// SPDX-License-Identifier: AGPL-3.0-or-later
// Persistent local storage: a per-title folder on the host. The console drive letters G: (title,
// read-only), T: (temporary) and D: (scratch) are mapped by the launcher (tools/xsl-run).
#include <windows.h>

#include <XPersistentLocalStorage.h>

#include <cstring>
#include <string>

#include "runtime_classes.h"
#include "xsl/log.h"

#define CH "rt"

namespace {
const std::string& pls_path() {
    static std::string p = [] {
        char buf[MAX_PATH];
        if (GetEnvironmentVariableA("XSL_PLS_DIR", buf, sizeof buf)) return std::string(buf);
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string name(exe);
        name = name.substr(name.find_last_of("\\/") + 1);
        name = name.substr(0, name.find_last_of('.'));
        std::string base = "C:\\users\\Public\\OpenScarlett";
        CreateDirectoryA(base.c_str(), nullptr);
        base += "\\" + name;
        CreateDirectoryA(base.c_str(), nullptr);
        base += "\\PersistentLocalStorage";
        CreateDirectoryA(base.c_str(), nullptr);
        XSL_INFO(CH, "persistent local storage: %s", base.c_str());
        return base;
    }();
    return p;
}

HRESULT M_GetPathSize(void*, size_t* size) { *size = pls_path().size() + 1; return S_OK; }
HRESULT M_GetPath(void*, size_t size, char* out, size_t* used) {
    size_t n = pls_path().size() + 1;
    if (used) *used = n;
    if (size < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(out, pls_path().c_str(), n);
    return S_OK;
}
HRESULT M_GetSpaceInfo(void*, XPersistentLocalStorageSpaceInfo* info) {
    ULARGE_INTEGER freeBytes{}, total{};
    GetDiskFreeSpaceExA(pls_path().c_str(), &freeBytes, &total, nullptr);
    *info = {};
    info->availableFreeBytes = freeBytes.QuadPart;
    info->totalFreeBytes = freeBytes.QuadPart;
    info->usedBytes = 0;
    info->totalBytes = total.QuadPart;
    return S_OK;
}
}  // namespace

XSL_RT_REGISTER(XPersistentLocalStorageGetPathSize, &M_GetPathSize);
XSL_RT_REGISTER(XPersistentLocalStorageGetPath, &M_GetPath);
XSL_RT_REGISTER(XPersistentLocalStorageGetSpaceInfo, &M_GetSpaceInfo);
