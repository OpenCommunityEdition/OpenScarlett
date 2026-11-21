// SPDX-License-Identifier: MIT
// Synthetic Series X|S title: console memory semantics through xmem.lib.
#include <windows.h>
#include <xmem.h>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); g_failures++; } else std::printf("ok   %s\n", #cond); } while (0)

int main() {
    // XMemAlloc with 64 KB pages and 4 KB alignment.
    ULONGLONG attr = MAKE_XALLOC_ATTRIBUTES(eXALLOCAllocatorId_GameMin, 0, XALLOC_MEMTYPE_GRAPHICS_WRITECOMBINE,
                                            XALLOC_PAGESIZE_64KB, XALLOC_ALIGNMENT_4K, 0);
    void* a = XMemAlloc(300000, attr);
    CHECK(a != nullptr && ((uintptr_t)a & 0xFFF) == 0);
    if (a) { memset(a, 0xAB, 300000); XMemFree(a, attr); }

    // Graphics-visible virtual allocation with console flags.
    void* g = XMemVirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE | MEM_64K_PAGES, XMEM_GRAPHICS,
                               PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_GRAPHICS_READWRITE);
    CHECK(g != nullptr);
    if (g) { ((volatile char*)g)[12345] = 7; CHECK(((volatile char*)g)[12345] == 7); }

    // Physical pages: map the same page at two addresses and check aliasing.
    ULONG_PTR pages[2] = {}, n = 2;
    CHECK(XMemAllocatePhysicalPages(XMEM_GRAPHICS, &n, pages) && n == 2);
    char* r = (char*)XMemVirtualAlloc(nullptr, 4 * 65536, MEM_RESERVE | MEM_64K_PAGES, XMEM_GRAPHICS, PAGE_READWRITE);
    CHECK(r != nullptr);
    if (r) {
        ULONG_PTR alias[4] = {pages[0], pages[1], pages[0], pages[1]};
        CHECK(XMemMapPhysicalPages(r, 4, alias) == r);
        strcpy(r + 10, "console");
        r[65536 + 5] = 42;
        CHECK(strcmp(r + 2 * 65536 + 10, "console") == 0);   // page 0 seen through its second mapping
        CHECK(r[3 * 65536 + 5] == 42);                          // page 1 likewise
        CHECK(XMemMapPhysicalPages(r, 4, nullptr) == r);       // unmap
        MEMORY_BASIC_INFORMATION m{};
        VirtualQuery(r, &m, sizeof m);
        CHECK(m.State == MEM_RESERVE);
    }
    CHECK(XMemFreePhysicalPages(2, pages));
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}
