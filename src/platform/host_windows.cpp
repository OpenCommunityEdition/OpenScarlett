// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What a title asks of the system that differs on a real Windows PC (it works the same under
// Wine, where the launcher prepares the prefix instead):
//
//   * The title-side xgameruntime.lib (and XCurl) load "xgameruntime.dll" from System32, where
//     Windows has Microsoft's PC Gaming Runtime. The layer never touches System32: loads of
//     xgameruntime.dll from the title's modules are redirected to the layer's copy next to
//     d3d12_xs.dll.
//   * XSystemGetDeviceType reads HKLM\...\OEM\ConsoleMode; the launcher passes the console to
//     present in XSL_CONSOLE_MODE (hex), and reads of that value from title modules return it,
//     leaving the machine's registry alone.
//
// Both are import-table interpositions into every module mapped from the title's directory
// (the executable and the DLLs it ships), done before the title's entry point runs.
#include <windows.h>
#include <tlhelp32.h>

#include <cstdlib>
#include <cwchar>
#include <string>

#include "xsl/log.h"
#include "xsl_platform.h"

namespace {
std::wstring g_layer_dir;  // directory of d3d12_xs.dll (the layer's DLLs)

std::wstring dir_of(HMODULE m) {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

template <class C> bool is_runtime(const C* name) {
    if (!name) return false;
    const C* base = name;
    for (const C* p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    const char want[] = "xgameruntime.dll";
    for (size_t i = 0;; i++) {
        C c = base[i];
        if (c >= 'A' && c <= 'Z') c = (C)(c - 'A' + 'a');
        if (c != (C)want[i]) return false;
        if (!c) return true;
    }
}

HMODULE load_layer_runtime() {
    static HMODULE m = [] {
        std::wstring path = g_layer_dir + L"xgameruntime.dll";
        HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        XSL_INFO("platform", "xgameruntime.dll load redirected to %ls: %s", path.c_str(), h ? "ok" : "FAILED");
        return h;
    }();
    return m;
}

HMODULE WINAPI H_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl) { return is_runtime(n) ? load_layer_runtime() : LoadLibraryExA(n, f, fl); }
HMODULE WINAPI H_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) { return is_runtime(n) ? load_layer_runtime() : LoadLibraryExW(n, f, fl); }
HMODULE WINAPI H_LoadLibraryA(LPCSTR n) { return is_runtime(n) ? load_layer_runtime() : LoadLibraryA(n); }
HMODULE WINAPI H_LoadLibraryW(LPCWSTR n) { return is_runtime(n) ? load_layer_runtime() : LoadLibraryW(n); }

bool console_mode(DWORD* v) {
    char b[16];
    if (!GetEnvironmentVariableA("XSL_CONSOLE_MODE", b, sizeof b)) return false;
    *v = (DWORD)strtoul(b, nullptr, 16);
    return true;
}

template <class C> bool is_console_mode_value(const C* name) {
    if (!name) return false;
    const char want[] = "consolemode";
    for (size_t i = 0;; i++) {
        C c = name[i];
        if (c >= 'A' && c <= 'Z') c = (C)(c - 'A' + 'a');
        if (c != (C)want[i]) return false;
        if (!c) return true;
    }
}

template <class C> LSTATUS answer(const C* value, LPDWORD type, LPBYTE data, LPDWORD size, bool* handled) {
    DWORD mode;
    *handled = is_console_mode_value(value) && console_mode(&mode);
    if (!*handled) return ERROR_SUCCESS;
    if (type) *type = REG_DWORD;
    if (!size) return data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS;
    if (!data) { *size = sizeof(DWORD); return ERROR_SUCCESS; }
    if (*size < sizeof(DWORD)) { *size = sizeof(DWORD); return ERROR_MORE_DATA; }
    *(DWORD*)data = mode;
    *size = sizeof(DWORD);
    return ERROR_SUCCESS;
}
LSTATUS WINAPI H_RegQueryValueExW(HKEY k, LPCWSTR v, LPDWORD r, LPDWORD t, LPBYTE d, LPDWORD s) {
    bool h;
    LSTATUS st = answer(v, t, d, s, &h);
    return h ? st : RegQueryValueExW(k, v, r, t, d, s);
}
LSTATUS WINAPI H_RegQueryValueExA(HKEY k, LPCSTR v, LPDWORD r, LPDWORD t, LPBYTE d, LPDWORD s) {
    bool h;
    LSTATUS st = answer(v, t, d, s, &h);
    return h ? st : RegQueryValueExA(k, v, r, t, d, s);
}
LSTATUS WINAPI H_RegGetValueW(HKEY k, LPCWSTR sub, LPCWSTR v, DWORD fl, LPDWORD t, PVOID d, LPDWORD s) {
    bool h;
    LSTATUS st = answer(v, t, (LPBYTE)d, s, &h);
    return h ? st : RegGetValueW(k, sub, v, fl, t, d, s);
}
// The key may not exist on a PC: opening it must succeed so that the value query is reached.
LSTATUS WINAPI H_RegOpenKeyW(HKEY k, LPCWSTR sub, PHKEY out) {
    LSTATUS st = RegOpenKeyW(k, sub, out);
    DWORD mode;
    if (st != ERROR_SUCCESS && sub && wcsstr(sub, L"CurrentVersion\\OEM") && console_mode(&mode))
        st = RegOpenKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", out);
    return st;
}
LSTATUS WINAPI H_RegOpenKeyExW(HKEY k, LPCWSTR sub, DWORD o, REGSAM sam, PHKEY out) {
    LSTATUS st = RegOpenKeyExW(k, sub, o, sam, out);
    DWORD mode;
    if (st != ERROR_SUCCESS && sub && wcsstr(sub, L"CurrentVersion\\OEM") && console_mode(&mode))
        st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", o, sam, out);
    return st;
}

void patch_module(HMODULE m) {
    using xsl::platform::patch_import;
    struct { const char* fn; void* p; } loads[] = {
        {"LoadLibraryExA", (void*)H_LoadLibraryExA}, {"LoadLibraryExW", (void*)H_LoadLibraryExW},
        {"LoadLibraryA", (void*)H_LoadLibraryA},     {"LoadLibraryW", (void*)H_LoadLibraryW},
    };
    for (auto& e : loads)
        for (const char* dll : {"kernel32", "api-ms-win-core-libraryloader", "kernelbase"}) patch_import(m, dll, e.fn, e.p);
    struct { const char* fn; void* p; } reg[] = {
        {"RegQueryValueExW", (void*)H_RegQueryValueExW}, {"RegQueryValueExA", (void*)H_RegQueryValueExA},
        {"RegGetValueW", (void*)H_RegGetValueW},         {"RegOpenKeyW", (void*)H_RegOpenKeyW},
        {"RegOpenKeyExW", (void*)H_RegOpenKeyExW},
    };
    for (auto& e : reg)
        for (const char* dll : {"advapi32", "api-ms-win-core-registry", "kernelbase"}) patch_import(m, dll, e.fn, e.p);
}
}  // namespace

namespace xsl::platform {
void install_host_redirects() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install_host_redirects, &self);
    g_layer_dir = dir_of(self);
    std::wstring titleDir = dir_of(nullptr);
    // Every module mapped from the title's directory, except the layer's own DLLs.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        patch_module(GetModuleHandleW(nullptr));
        return;
    }
    MODULEENTRY32W me{sizeof me};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring path = me.szExePath, dir = path.substr(0, path.find_last_of(L"\\/") + 1);
        if (_wcsicmp(dir.c_str(), titleDir.c_str()) != 0) continue;
        if (me.hModule == self) continue;
        bool layer = false;
        for (const wchar_t* n : {L"xgameruntime.dll", L"GameInput.dll", L"xmem.dll", L"XCRDAPI.dll", L"AcpHal.dll", L"xapu.dll",
                                 L"chad.dll", L"PIXEvt.dll", L"XFrontPanelDisplay.dll", L"d3d12_xs.dll"})
            layer |= _wcsicmp(me.szModule, n) == 0;
        if (!layer) patch_module(me.hModule);
    }
    CloseHandle(snap);
}
}  // namespace xsl::platform
