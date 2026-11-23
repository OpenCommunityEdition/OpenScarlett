// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Window classes. Console titles built with CharacterSet=Unicode register their window class
// with RegisterClassExA yet forward messages to DefWindowProcW. The console shows no title bar,
// so the mismatch is invisible there; on a PC, DefWindowProcW reads the ANSI caption as UTF-16
// and the title bar shows garbage. When the title imports DefWindowProcW, its ANSI class
// registrations are made Unicode, which is what that window procedure expects.
//
// Closing. Console titles have no quit path (the system terminates them), so they ignore
// WM_CLOSE and the window's close button did nothing. The layer wraps the window procedures of
// the title's classes and ends the process on WM_CLOSE (XSL_CLOSE_EXITS=0 passes it through).
//
// Client area. The title's GetClientRect on its own windows reports the console display size
// (xsl/display.h); the swap chain stretches the title's planes onto the real window.
#include <windows.h>

#include <map>
#include <mutex>
#include <string>

#include "xsl/display.h"
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

std::mutex g_mu;
std::map<ATOM, WNDPROC> g_procs;  // title class -> its window procedure

LRESULT CALLBACK close_wrapper(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC orig = nullptr;
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_procs.find((ATOM)GetClassLongPtrW(h, GCW_ATOM));
        if (it != g_procs.end()) orig = it->second;
    }
    if (m == WM_CLOSE) {
        static bool exits = [] { char b[4]; return !(GetEnvironmentVariableA("XSL_CLOSE_EXITS", b, 4) && b[0] == '0'); }();
        if (exits) {
            XSL_INFO("platform", "window closed: ending the title");
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
    return orig ? CallWindowProcW(orig, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

bool g_ansi_as_unicode = false;

ATOM remember(ATOM atom, WNDPROC proc) {
    if (atom) {
        std::lock_guard<std::mutex> g(g_mu);
        g_procs[atom] = proc;
    }
    return atom;
}

// Console titles have no input method editor. On a PC the text-input framework attaches to the
// title's window when it gets the focus and can keep the thread in DispatchMessage for a long time
// (one title's watchdog ended the process after 30 s there): the IME is turned off for the process
// before its first window class exists. XSL_IME=1 keeps it.
void disable_ime() {
    static bool done = false;
    if (done) return;
    done = true;
    char b[4];
    if (GetEnvironmentVariableA("XSL_IME", b, 4) && b[0] == '1') return;
    HMODULE imm = LoadLibraryW(L"imm32.dll");
    auto fn = imm ? (BOOL(WINAPI*)(DWORD))GetProcAddress(imm, "ImmDisableIME") : nullptr;
    if (fn && fn((DWORD)-1)) XSL_INFO("platform", "input method editor disabled for the title's windows");
}

ATOM WINAPI Xsl_RegisterClassExW(const WNDCLASSEXW* a) {
    disable_ime();
    if (!a) return RegisterClassExW(a);
    WNDCLASSEXW w = *a;
    w.lpfnWndProc = close_wrapper;
    return remember(RegisterClassExW(&w), a->lpfnWndProc);
}

ATOM WINAPI Xsl_RegisterClassExA(const WNDCLASSEXA* a) {
    disable_ime();
    if (!a) return RegisterClassExA(a);
    if (!g_ansi_as_unicode) {
        WNDCLASSEXA c = *a;
        c.lpfnWndProc = close_wrapper;
        return remember(RegisterClassExA(&c), a->lpfnWndProc);
    }
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
    w.lpfnWndProc = close_wrapper;
    ATOM atom = RegisterClassExW(&w);
    XSL_INFO("platform", "window class '%s' registered as Unicode (title uses DefWindowProcW)",
             IS_INTRESOURCE(a->lpszClassName) ? "#atom" : a->lpszClassName);
    return remember(atom, a->lpfnWndProc);
}

BOOL WINAPI Xsl_GetClientRect(HWND h, LPRECT r) {
    BOOL ok = GetClientRect(h, r);
    LONG w, ht;
    if (!ok || !r || !xsl::display_size(&w, &ht)) return ok;
    bool title;
    {
        std::lock_guard<std::mutex> g(g_mu);
        title = g_procs.count((ATOM)GetClassLongPtrW(h, GCW_ATOM)) != 0;
    }
    if (title && !(GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)) {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            XSL_INFO("platform", "window client area %ldx%ld reported to the title as the console's %ldx%ld (XSL_DISPLAY_SIZE)",
                     r->right - r->left, r->bottom - r->top, w, ht);
        *r = {0, 0, w, ht};
    }
    return ok;
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
    // Consoles have no hung-window detection, and titles do long blocking work on their window
    // thread (world generation, network lookups). On Windows a window that pumps no messages for
    // 5 s is replaced by a "Not Responding" ghost that offers to kill the title.
    DisableProcessWindowsGhosting();
    g_ansi_as_unicode = imports(exe, "DefWindowProcW") && !imports(exe, "DefWindowProcA");
    patch_import(exe, "", "RegisterClassExA", (void*)Xsl_RegisterClassExA);
    patch_import(exe, "", "RegisterClassExW", (void*)Xsl_RegisterClassExW);
    patch_import(exe, "", "GetClientRect", (void*)Xsl_GetClientRect);
}
}  // namespace xsl::platform
