// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Console drives. A console title addresses its storage by drive letter: Minecraft Preview keeps
// per-user data under "C:/Users/<xuid>/games/com.mojang", enumerates "C:/" as its own storage
// root and puts temporary data under "t:/". On the console those drives belong to the title.
// Under Wine, C: is the prefix's private drive and works as is; on a Windows PC it is the system
// drive (a standard user may not create folders under C:\Users, and the title would enumerate
// the whole disk) and T: does not exist.
//
// Paths the title's modules pass to the file APIs are therefore mapped: "C:" (Windows only) and
// "T:" go to per-title folders, <storage>\<title>\Drives\C and ...\T, where <storage> is the
// layer's storage root (C:\users\Public\OpenScarlett, writable by interactive users). Paths
// inside the title's own folder, the layer's folder, the system temporary folder or the storage
// root itself are left alone. XSL_CONSOLE_DRIVES=0 disables the mapping; XSL_FILE_TRACE=1 logs
// each mapped directory once.
#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>

#include "xsl/log.h"
#include "xsl_platform.h"

#define CH "platform"

namespace {
std::wstring g_storage;            // C:\users\Public\OpenScarlett
std::wstring g_drive_root[2];      // [0] C:, [1] T:  ("" = not mapped)
std::wstring g_exempt[3];          // title dir, layer dir, temp dir (lower case, trailing '\')

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
std::wstring dir_of(HMODULE m) {
    wchar_t p[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    std::wstring s(p, n);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}
void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); i++)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

// The mapped path, or empty if `p` is not on a mapped drive.
std::wstring map_path(const wchar_t* p) {
    if (!p || !p[0] || p[1] != L':') return {};
    int d = (p[0] == L'c' || p[0] == L'C') ? 0 : (p[0] == L't' || p[0] == L'T') ? 1 : -1;
    if (d < 0 || g_drive_root[d].empty()) return {};
    // Normalise: backslashes, no repeated separators.
    std::wstring rest;
    for (const wchar_t* q = p + 2; *q; q++) {
        wchar_t c = *q == L'/' ? L'\\' : *q;
        if (c == L'\\' && !rest.empty() && rest.back() == L'\\') continue;
        rest += c;
    }
    if (d == 0) {
        std::wstring full = lower(std::wstring(L"c:") + rest + L"\\");
        for (const auto& e : g_exempt)
            if (!e.empty() && full.compare(0, e.size(), e) == 0) return {};
        std::wstring st = lower(g_storage) + L"\\";
        if (full.compare(0, st.size(), st) == 0) return {};
    }
    if (!rest.empty() && rest[0] != L'\\') rest = L"\\" + rest;
    std::wstring out = g_drive_root[d] + rest;
    static bool trace = [] { char b[4]; return GetEnvironmentVariableA("XSL_FILE_TRACE", b, 4) && b[0] == '1'; }();
    if (trace) {
        static std::mutex mu;
        static std::set<std::wstring> seen;
        std::wstring dir = out.substr(0, out.find_last_of(L'\\'));
        std::lock_guard<std::mutex> l(mu);
        if (seen.size() < 2000 && seen.insert(dir).second) XSL_INFO(CH, "console drive: %ls -> %ls", p, out.c_str());
    }
    return out;
}
const wchar_t* mapped(const wchar_t* p, std::wstring& buf) {
    buf = map_path(p);
    return buf.empty() ? p : buf.c_str();
}
// Narrow (UTF-8 / ANSI code page) paths for the CRT entry points.
const char* mapped(const char* p, std::string& buf) {
    if (!p || !p[0] || p[1] != ':') return p;
    wchar_t w[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_ACP, 0, p, -1, w, MAX_PATH * 2)) return p;
    std::wstring m = map_path(w);
    if (m.empty()) return p;
    char out[MAX_PATH * 4];
    if (!WideCharToMultiByte(CP_ACP, 0, m.c_str(), -1, out, sizeof out, nullptr, nullptr)) return p;
    buf = out;
    return buf.c_str();
}

HANDLE WINAPI D_CreateFileW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) {
    std::wstring b;
    return CreateFileW(mapped(n, b), a, s, sa, c, f, t);
}
HANDLE WINAPI D_CreateFile2(LPCWSTR n, DWORD a, DWORD s, DWORD c, LPCREATEFILE2_EXTENDED_PARAMETERS x) {
    std::wstring b;
    return CreateFile2(mapped(n, b), a, s, c, x);
}
BOOL WINAPI D_CreateDirectoryW(LPCWSTR n, LPSECURITY_ATTRIBUTES sa) {
    std::wstring b;
    return CreateDirectoryW(mapped(n, b), sa);
}
BOOL WINAPI D_RemoveDirectoryW(LPCWSTR n) {
    std::wstring b;
    return RemoveDirectoryW(mapped(n, b));
}
BOOL WINAPI D_DeleteFileW(LPCWSTR n) {
    std::wstring b;
    return DeleteFileW(mapped(n, b));
}
BOOL WINAPI D_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID i) {
    std::wstring b;
    return GetFileAttributesExW(mapped(n, b), l, i);
}
DWORD WINAPI D_GetFileAttributesW(LPCWSTR n) {
    std::wstring b;
    return GetFileAttributesW(mapped(n, b));
}
BOOL WINAPI D_SetFileAttributesW(LPCWSTR n, DWORD a) {
    std::wstring b;
    return SetFileAttributesW(mapped(n, b), a);
}
HANDLE WINAPI D_FindFirstFileExW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl) {
    std::wstring b;
    return FindFirstFileExW(mapped(n, b), l, d, o, f, fl);
}
HANDLE WINAPI D_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW d) {
    std::wstring b;
    return FindFirstFileW(mapped(n, b), d);
}
BOOL WINAPI D_MoveFileExW(LPCWSTR a, LPCWSTR b2, DWORD f) {
    std::wstring x, y;
    return MoveFileExW(mapped(a, x), mapped(b2, y), f);
}
BOOL WINAPI D_CopyFileW(LPCWSTR a, LPCWSTR b2, BOOL f) {
    std::wstring x, y;
    return CopyFileW(mapped(a, x), mapped(b2, y), f);
}
BOOL WINAPI D_GetDiskFreeSpaceExW(LPCWSTR n, PULARGE_INTEGER a, PULARGE_INTEGER t, PULARGE_INTEGER f) {
    std::wstring b;
    return GetDiskFreeSpaceExW(mapped(n, b), a, t, f);
}
BOOL WINAPI D_GetDiskFreeSpaceW(LPCWSTR n, LPDWORD spc, LPDWORD bps, LPDWORD fc, LPDWORD tc) {
    // Takes a root directory: the mapped folder's drive.
    std::wstring b = map_path(n);
    if (!b.empty()) b = b.substr(0, 3);
    return GetDiskFreeSpaceW(b.empty() ? n : b.c_str(), spc, bps, fc, tc);
}
// Renames through a handle carry the new name in FILE_RENAME_INFO.
BOOL WINAPI D_SetFileInformationByHandle(HANDLE h, FILE_INFO_BY_HANDLE_CLASS c, LPVOID info, DWORD size) {
    if ((c == FileRenameInfo || c == (FILE_INFO_BY_HANDLE_CLASS)22 /* FileRenameInfoEx */) && info &&
        size >= sizeof(FILE_RENAME_INFO)) {
        auto* ri = (FILE_RENAME_INFO*)info;
        std::wstring name(ri->FileName, ri->FileNameLength / sizeof(wchar_t));
        std::wstring m = map_path(name.c_str());
        if (!m.empty()) {
            DWORD n = (DWORD)(offsetof(FILE_RENAME_INFO, FileName) + (m.size() + 1) * sizeof(wchar_t));
            std::string buf(n, '\0');
            auto* r2 = (FILE_RENAME_INFO*)buf.data();
            memcpy(r2, ri, offsetof(FILE_RENAME_INFO, FileName));
            r2->FileNameLength = (DWORD)(m.size() * sizeof(wchar_t));
            memcpy(r2->FileName, m.c_str(), (m.size() + 1) * sizeof(wchar_t));
            return SetFileInformationByHandle(h, c, r2, n);
        }
    }
    return SetFileInformationByHandle(h, c, info, size);
}
// CRT: the title's own CRT (ucrtbase) is called, so FILE objects and errno stay in the CRT
// instance the title uses.
template <class F> F crt(const char* name) {
    static HMODULE u = LoadLibraryW(L"ucrtbase.dll");
    return u ? (F)GetProcAddress(u, name) : nullptr;
}
#define CRT(name, type) static auto real = crt<type>(name)
FILE* __cdecl D__wfopen(const wchar_t* n, const wchar_t* m) {
    CRT("_wfopen", FILE*(__cdecl*)(const wchar_t*, const wchar_t*));
    std::wstring b;
    return real(mapped(n, b), m);
}
errno_t __cdecl D__wfopen_s(FILE** f, const wchar_t* n, const wchar_t* m) {
    CRT("_wfopen_s", errno_t(__cdecl*)(FILE**, const wchar_t*, const wchar_t*));
    std::wstring b;
    return real(f, mapped(n, b), m);
}
FILE* __cdecl D__wfsopen(const wchar_t* n, const wchar_t* m, int sh) {
    CRT("_wfsopen", FILE*(__cdecl*)(const wchar_t*, const wchar_t*, int));
    std::wstring b;
    return real(mapped(n, b), m, sh);
}
FILE* __cdecl D_fopen(const char* n, const char* m) {
    CRT("fopen", FILE*(__cdecl*)(const char*, const char*));
    std::string b;
    return real(mapped(n, b), m);
}
errno_t __cdecl D_fopen_s(FILE** f, const char* n, const char* m) {
    CRT("fopen_s", errno_t(__cdecl*)(FILE**, const char*, const char*));
    std::string b;
    return real(f, mapped(n, b), m);
}
int __cdecl D__wstat64(const wchar_t* n, struct _stat64* st) {
    CRT("_wstat64", int(__cdecl*)(const wchar_t*, struct _stat64*));
    std::wstring b;
    return real(mapped(n, b), st);
}
int __cdecl D__stat64i32(const char* n, struct _stat64i32* st) {
    CRT("_stat64i32", int(__cdecl*)(const char*, struct _stat64i32*));
    std::string b;
    return real(mapped(n, b), st);
}
int __cdecl D_remove(const char* n) {
    CRT("remove", int(__cdecl*)(const char*));
    std::string b;
    return real(mapped(n, b));
}
int __cdecl D__wremove(const wchar_t* n) {
    CRT("_wremove", int(__cdecl*)(const wchar_t*));
    std::wstring b;
    return real(mapped(n, b));
}
int __cdecl D__wchmod(const wchar_t* n, int m) {
    CRT("_wchmod", int(__cdecl*)(const wchar_t*, int));
    std::wstring b;
    return real(mapped(n, b), m);
}
int __cdecl D__wmkdir(const wchar_t* n) {
    CRT("_wmkdir", int(__cdecl*)(const wchar_t*));
    std::wstring b;
    return real(mapped(n, b));
}
int __cdecl D__wrmdir(const wchar_t* n) {
    CRT("_wrmdir", int(__cdecl*)(const wchar_t*));
    std::wstring b;
    return real(mapped(n, b));
}
int __cdecl D__wrename(const wchar_t* a, const wchar_t* b2) {
    CRT("_wrename", int(__cdecl*)(const wchar_t*, const wchar_t*));
    std::wstring x, y;
    return real(mapped(a, x), mapped(b2, y));
}

void patch_module(HMODULE m) {
    using xsl::platform::patch_import;
    struct { const char* fn; void* p; } t[] = {
        {"CreateFileW", (void*)D_CreateFileW},
        {"CreateFile2", (void*)D_CreateFile2},
        {"CreateDirectoryW", (void*)D_CreateDirectoryW},
        {"RemoveDirectoryW", (void*)D_RemoveDirectoryW},
        {"DeleteFileW", (void*)D_DeleteFileW},
        {"GetFileAttributesExW", (void*)D_GetFileAttributesExW},
        {"GetFileAttributesW", (void*)D_GetFileAttributesW},
        {"SetFileAttributesW", (void*)D_SetFileAttributesW},
        {"FindFirstFileExW", (void*)D_FindFirstFileExW},
        {"FindFirstFileW", (void*)D_FindFirstFileW},
        {"MoveFileExW", (void*)D_MoveFileExW},
        {"CopyFileW", (void*)D_CopyFileW},
        {"GetDiskFreeSpaceExW", (void*)D_GetDiskFreeSpaceExW},
        {"GetDiskFreeSpaceW", (void*)D_GetDiskFreeSpaceW},
        {"SetFileInformationByHandle", (void*)D_SetFileInformationByHandle},
        {"_wfopen", (void*)D__wfopen},
        {"_wfopen_s", (void*)D__wfopen_s},
        {"_wfsopen", (void*)D__wfsopen},
        {"fopen", (void*)D_fopen},
        {"fopen_s", (void*)D_fopen_s},
        {"_wstat64", (void*)D__wstat64},
        {"_stat64i32", (void*)D__stat64i32},
        {"remove", (void*)D_remove},
        {"_wremove", (void*)D__wremove},
        {"_wchmod", (void*)D__wchmod},
        {"_wmkdir", (void*)D__wmkdir},
        {"_wrmdir", (void*)D__wrmdir},
        {"_wrename", (void*)D__wrename},
    };
    for (auto& e : t) patch_import(m, "", e.fn, e.p);
}
}  // namespace

namespace xsl::platform {
void install_console_drives() {
    char b[8];
    if (GetEnvironmentVariableA("XSL_CONSOLE_DRIVES", b, sizeof b) && b[0] == '0') return;
    bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring stem(exe);
    stem = stem.substr(stem.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.find_last_of(L'.'));
    g_storage = L"C:\\users\\Public\\OpenScarlett";
    std::wstring drives = g_storage + L"\\" + stem + L"\\Drives";
    if (!wine) g_drive_root[0] = drives + L"\\C";
    g_drive_root[1] = drives + L"\\T";
    for (const auto& r : g_drive_root)
        if (!r.empty()) make_dirs(r);

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&install_console_drives, &self);
    std::wstring titleDir = dir_of(nullptr);
    g_exempt[0] = lower(titleDir);
    g_exempt[1] = lower(dir_of(self));
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n) {
        g_exempt[2] = lower(std::wstring(tmp, n));
        if (g_exempt[2].back() != L'\\') g_exempt[2] += L'\\';
    }

    // Every module mapped from the title's directory (the executable and the DLLs it ships),
    // except the layer's own.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        patch_module(GetModuleHandleW(nullptr));
    } else {
        MODULEENTRY32W me{sizeof me};
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
            std::wstring path = me.szExePath, dir = path.substr(0, path.find_last_of(L"\\/") + 1);
            if (_wcsicmp(dir.c_str(), titleDir.c_str()) != 0 || me.hModule == self) continue;
            bool layer = false;
            for (const wchar_t* l : {L"xgameruntime.dll", L"GameInput.dll", L"xmem.dll", L"XCRDAPI.dll", L"AcpHal.dll", L"xapu.dll",
                                     L"chad.dll", L"PIXEvt.dll", L"XFrontPanelDisplay.dll", L"d3d12_xs.dll"})
                layer |= _wcsicmp(me.szModule, l) == 0;
            if (!layer) patch_module(me.hModule);
        }
        CloseHandle(snap);
    }
    if (!g_drive_root[0].empty())
        XSL_INFO(CH, "console drives: the title's C: is %ls, T: is %ls (XSL_CONSOLE_DRIVES=0 disables)", g_drive_root[0].c_str(),
                 g_drive_root[1].c_str());
    else
        XSL_INFO(CH, "console drives: the title's T: is %ls; C: is the Wine prefix's own drive", g_drive_root[1].c_str());
}
}  // namespace xsl::platform
