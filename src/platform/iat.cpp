// SPDX-License-Identifier: AGPL-3.0-or-later
#include <cstring>

#include "xsl/log.h"
#include "xsl_platform.h"

namespace xsl::platform {
int patch_import(HMODULE module, const char* dllPrefix, const char* func, void* replacement) {
    auto* base = (BYTE*)module;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    size_t plen = strlen(dllPrefix);
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char* dll = (const char*)(base + imp->Name);
        if (_strnicmp(dll, dllPrefix, plen) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA64*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* slots = (IMAGE_THUNK_DATA64*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            slots->u1.Function = (ULONGLONG)replacement;
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            patched++;
            XSL_INFO("platform", "interposed %s!%s", dll, func);
        }
    }
    return patched;
}
}  // namespace xsl::platform
