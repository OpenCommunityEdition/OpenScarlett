// SPDX-License-Identifier: AGPL-3.0-or-later
//
// User patches: byte patches the user keeps next to the title executable, applied to the loaded
// image before the title's entry point runs (d3d12_xs.dll's DllMain). The layer ships no patches;
// the file format is documented in docs/patches.md.
//
//   <exe stem>.patches.txt, else patches.txt, in the executable's directory.
//
//   # comment
//   [Group name]                       a group applies as a whole, or not at all
//   [Group name] off                   a disabled group
//   0x140F12F55: 0F 44 D0 -> 90 90 90   virtual address (as shown by disassemblers)
//   rva 0xF12F55: 0F 44 D0 -> 90 90 90  or an RVA
//
// Every line's original bytes must match the image, so a patch made for another build of the
// title is reported and skipped rather than applied to the wrong code.
#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "xsl/log.h"

#define CH "patches"

namespace {
struct Edit {
    int line;
    uint64_t rva;
    std::vector<uint8_t> from, to;
};
struct Group {
    std::string name;
    bool enabled = true;
    std::vector<Edit> edits;
};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool parse_bytes(const std::string& s, std::vector<uint8_t>* out) {
    out->clear();
    std::string hex;
    for (char c : s)
        if (!isspace((unsigned char)c)) hex += c;
    if (hex.empty() || hex.size() % 2) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        std::string byte = hex.substr(i, 2);
        unsigned long v = strtoul(byte.c_str(), &end, 16);
        if (*end) return false;
        out->push_back((uint8_t)v);
    }
    return true;
}

bool read_file(const std::wstring& path, std::string* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    text->resize((size_t)size.QuadPart);
    DWORD got = 0;
    BOOL ok = size.QuadPart == 0 || ReadFile(f, text->data(), (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(f);
    return ok;
}

// The executable's preferred image base, from the file on disk (the in-memory header may hold
// the relocated base), so virtual addresses from a disassembler map onto the loaded image.
uint64_t preferred_base(const std::wstring& exePath, uint64_t fallback) {
    std::string head;
    HANDLE f = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fallback;
    head.resize(4096);
    DWORD got = 0;
    ReadFile(f, head.data(), 4096, &got, nullptr);
    CloseHandle(f);
    if (got < 0x200) return fallback;
    auto* dos = (const IMAGE_DOS_HEADER*)head.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || (DWORD)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) return fallback;
    auto* nt = (const IMAGE_NT_HEADERS64*)(head.data() + dos->e_lfanew);
    return nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? nt->OptionalHeader.ImageBase : fallback;
}

bool parse(const std::string& text, uint64_t base, std::vector<Group>* groups, std::string* error) {
    groups->clear();
    groups->push_back({"(ungrouped)"});
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        lineNo++;
        if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) {
                *error = "line " + std::to_string(lineNo) + ": unterminated group name";
                return false;
            }
            Group g;
            g.name = trim(line.substr(1, close - 1));
            std::string rest = trim(line.substr(close + 1));
            for (auto& c : rest) c = (char)tolower((unsigned char)c);
            g.enabled = !(rest == "off" || rest == "disabled");
            groups->push_back(g);
            continue;
        }
        size_t colon = line.find(':'), arrow = line.find("->");
        if (colon == std::string::npos || arrow == std::string::npos || arrow < colon) {
            *error = "line " + std::to_string(lineNo) + ": expected '<address>: <original bytes> -> <new bytes>'";
            return false;
        }
        std::string addr = trim(line.substr(0, colon));
        bool rva = false;
        if (addr.size() > 3 && (addr.compare(0, 3, "rva") == 0 || addr.compare(0, 3, "RVA") == 0)) {
            rva = true;
            addr = trim(addr.substr(3));
        }
        char* end = nullptr;
        uint64_t a = _strtoui64(addr.c_str(), &end, 16);
        if (addr.empty() || *end) {
            *error = "line " + std::to_string(lineNo) + ": bad address '" + addr + "'";
            return false;
        }
        Edit e{lineNo, rva ? a : a - base, {}, {}};
        if (!rva && a < base) {
            *error = "line " + std::to_string(lineNo) + ": address below the image base (use 'rva' for RVAs)";
            return false;
        }
        if (!parse_bytes(line.substr(colon + 1, arrow - colon - 1), &e.from) || !parse_bytes(line.substr(arrow + 2), &e.to) ||
            e.from.size() != e.to.size()) {
            *error = "line " + std::to_string(lineNo) + ": original and new bytes must be hex and of equal length";
            return false;
        }
        groups->back().edits.push_back(std::move(e));
    }
    return true;
}

bool in_image(const uint8_t* image, uint64_t rva, size_t n) {
    auto* nt = (const IMAGE_NT_HEADERS64*)(image + ((const IMAGE_DOS_HEADER*)image)->e_lfanew);
    return rva + n <= nt->OptionalHeader.SizeOfImage;
}

bool write(uint8_t* p, const std::vector<uint8_t>& bytes) {
    DWORD old = 0;
    if (!VirtualProtect(p, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes.data(), bytes.size());
    VirtualProtect(p, bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, bytes.size());
    return true;
}
}  // namespace

namespace xsl::platform {
void apply_user_patches() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    std::wstring exePath(exe), dir = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
    std::wstring stem = exePath.substr(dir.size());
    if (size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
    std::string text;
    std::wstring used;
    for (const std::wstring& name : {stem + L".patches.txt", std::wstring(L"patches.txt")}) {
        if (read_file(dir + name, &text)) {
            used = name;
            break;
        }
    }
    if (used.empty()) return;
    char usedA[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, used.c_str(), -1, usedA, sizeof usedA, nullptr, nullptr);

    auto* image = (uint8_t*)GetModuleHandleW(nullptr);
    uint64_t base = preferred_base(exePath, 0x140000000ull);
    std::vector<Group> groups;
    std::string error;
    if (!parse(text, base, &groups, &error)) {
        XSL_ERR(CH, "%s: %s; no patches applied", usedA, error.c_str());
        return;
    }
    unsigned applied = 0, skipped = 0;
    for (const Group& g : groups) {
        if (g.edits.empty()) continue;
        if (!g.enabled) {
            XSL_INFO(CH, "%s: [%s] disabled", usedA, g.name.c_str());
            continue;
        }
        // Verify the whole group first: it applies completely or not at all.
        const Edit* bad = nullptr;
        for (const Edit& e : g.edits)
            if (!in_image(image, e.rva, e.from.size()) || memcmp(image + e.rva, e.from.data(), e.from.size()) != 0) {
                bad = &e;
                break;
            }
        if (bad) {
            XSL_WARN(CH, "%s: [%s] skipped: line %d's original bytes do not match this executable (another build?)", usedA,
                     g.name.c_str(), bad->line);
            skipped++;
            continue;
        }
        bool ok = true;
        for (const Edit& e : g.edits) ok &= write(image + e.rva, e.to);
        XSL_INFO(CH, "%s: [%s] %s (%zu edit(s))", usedA, g.name.c_str(), ok ? "applied" : "FAILED to write", g.edits.size());
        applied += ok;
    }
    XSL_INFO(CH, "%s: %u group(s) applied, %u skipped", usedA, applied, skipped);
}
}  // namespace xsl::platform
