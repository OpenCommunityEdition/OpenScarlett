// SPDX-License-Identifier: AGPL-3.0-or-later
#include "xs_abi.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "xs_abi_layouts.h"  // generated
#include "xsl/log.h"

#define CH "gfx"

namespace xs::abi {
namespace {
// The pinned SDK's own edition: titles at or after it use the native layout.
constexpr unsigned kPinnedEdition = XSL_PINNED_GXDK_EDITION;

// Looks for "_xbld_edition_sdktype=GXDK,NNNNNN" (or GRDK,) in the main executable's .xbld section.
unsigned read_edition() {
    if (const char* e = getenv("XSL_GXDK_EDITION")) return (unsigned)strtoul(e, nullptr, 10);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (memcmp(sec->Name, ".xbld", 6) != 0) continue;
        std::string s((const char*)base + sec->VirtualAddress, sec->Misc.VirtualSize);
        // GDK releases up to 2020 name the kit "GRDK"; later ones "GXDK". The edition number
        // (YYMM00) is the same scale either way.
        for (const char* key : {"_xbld_edition_sdktype=GXDK,", "_xbld_edition_sdktype=GRDK,"}) {
            size_t at = s.find(key);
            if (at != std::string::npos) return (unsigned)strtoul(s.c_str() + at + strlen(key), nullptr, 10);
        }
    }
    return 0;
}

const XslAbiEdition* g_edition = nullptr;
std::once_flag g_once;

void choose() {
    unsigned t = title_edition();
    if (t && t < kPinnedEdition) {
        // The oldest known layout at or after the title's edition: between two releases, methods
        // were only ever added, so the newer one keeps every slot the title calls.
        for (unsigned i = 0; i < kXslAbiEditionCount; i++) {
            const XslAbiEdition& e = kXslAbiEditions[i];
            if (e.edition >= t && (!g_edition || e.edition < g_edition->edition)) g_edition = &e;
        }
    }
    XSL_INFO(CH, "title GXDK edition %u: D3D12.X vtables %s", t,
             g_edition ? ("rearranged to the " + std::to_string(g_edition->edition) + " layout").c_str()
                       : (t && t < kPinnedEdition ? "native (no older layout known, calls may misroute)" : "native"));
}

HRESULT __stdcall missing_slot() {
    static LONG once;
    if (!InterlockedExchange(&once, 1)) XSL_WARN("stub", "title called a D3D12.X method its GDK edition has and the layer does not");
    return E_NOTIMPL;
}

std::mutex g_lock;
std::map<void*, void**> g_tables;
}  // namespace

unsigned title_edition() {
    static unsigned e = read_edition();
    return e;
}

void* select(void* native, const char* name, size_t bytes) {
    std::call_once(g_once, choose);
    if (!g_edition || !name) return native;
    const XslAbiTable* t = nullptr;
    for (unsigned i = 0; i < g_edition->count; i++)
        if (!strcmp(g_edition->tables[i].name, name)) t = &g_edition->tables[i];
    if (!t) return native;  // unchanged, or only appended to since
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_tables.find(native);
    if (it != g_tables.end()) return it->second;
    auto** src = (void**)native;
    size_t nslots = bytes / sizeof(void*);
    auto** out = new void*[t->slots];
    for (unsigned s = 0; s < t->slots; s++) {
        int m = t->map[s];
        out[s] = (m >= 0 && (size_t)m < nslots && src[m]) ? src[m] : (void*)&missing_slot;
    }
    g_tables[native] = out;
    return out;
}
}  // namespace xs::abi
