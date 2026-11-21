// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Window classes. Console titles built with CharacterSet=Unicode register their window class
// with RegisterClassExA yet forward messages to DefWindowProcW. The console shows no title bar,
// so the mismatch is invisible there; on a PC, DefWindowProcW reads the ANSI caption as UTF-16
// and the title bar shows garbage. When the title imports DefWindowProcW, its ANSI class
// registrations are made Unicode, which is what that window procedure expects.
#include <windows.h>

#include <string>

#include "xsl/log.h"
#include "xsl_platform.h"

namespace {
std::wstring widen(const char* s) {
    if (!s) return {};
    if (IS_INTRESOURCE(s)) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

ATOM WINAPI Xsl_RegisterClassExA(const WNDCLASSEXA* a) {
    WNDCLASSEXW w{};
    w.cbSize = sizeof w;
    w.style = a->style;
    w.lpfnWndProc = a->lpfnWndProc;
    w.cbClsExtra = a->cbClsExtra;
    w.cbWndExtra = a->cbWndExtra;
    w.hInstance = a->hInstance;
    w.hIcon = a->hIcon;
    w.hCursor = a->hCursor;
    w.hbrBackground = a->hbrBackground;
    w.hIconSm = a->hIconSm;
    std::wstring cls = widen(a->lpszClassName), menu = widen(a->lpszMenuName);
    w.lpszClassName = IS_INTRESOURCE(a->lpszClassName) ? (LPCWSTR)a->lpszClassName : cls.c_str();
    w.lpszMenuName = IS_INTRESOURCE(a->lpszMenuName) ? (LPCWSTR)a->lpszMenuName : (menu.empty() ? nullptr : menu.c_str());
    ATOM atom = RegisterClassExW(&w);
    XSL_INFO("platform", "window class '%s' registered as Unicode (title uses DefWindowProcW)",
             IS_INTRESOURCE(a->lpszClassName) ? "#atom" : a->lpszClassName);
    return atom;
}

bool imports(HMODULE m, const char* func) {
    auto* base = (BYTE*)m;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        auto* t = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; t->u1.AddressOfData; t++)
            if (!IMAGE_SNAP_BY_ORDINAL64(t->u1.Ordinal) && !strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + t->u1.AddressOfData))->Name, func))
                return true;
    }
    return false;
}
}  // namespace

namespace xsl::platform {
void install_windowing(HMODULE exe) {
    if (!imports(exe, "DefWindowProcW") || imports(exe, "DefWindowProcA")) return;
    patch_import(exe, "", "RegisterClassExA", (void*)Xsl_RegisterClassExA);
}
}  // namespace xsl::platform
