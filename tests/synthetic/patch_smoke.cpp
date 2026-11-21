// SPDX-License-Identifier: MIT
// Synthetic Series X|S title for user patches (docs/patches.md): markers in read-only data that a
// generated patch_smoke.patches.txt next to the executable rewrites before the entry point.
#include <windows.h>

#include <d3d12_xs.h>

#include <cstdio>
#include <cstring>

// Read through volatile so the compiler cannot fold the comparisons at build time.
static const volatile char kApply[] = "XSLPATCH_APPLY_ORIGINAL";    // [Apply]: rewritten
static const volatile char kMismatch[] = "XSLPATCH_MISMT_ORIGINAL";  // [Mismatch]: wrong original bytes, skipped
static const volatile char kOff[] = "XSLPATCH_DISAB_ORIGINAL";       // [Disabled] off: untouched
static const volatile char kRva[] = "XSLPATCH_RVAXX_ORIGINAL";       // [RVA]: rewritten, RVA address form

// Expected values are written in lowercase so the markers stay unique in the binary (the patch
// generator looks them up by their exact bytes); compare case-insensitively.
// [Code]: the generated file rewrites this function's 'mov eax, 1234567' to return 7654321.
__declspec(noinline) int answer() { return 1234567; }
int (*volatile g_answer)() = answer;  // called through this so the compiler cannot fold the result

static bool is(const volatile char* s, const char* want) {
    for (size_t i = 0; want[i] || s[i]; i++) {
        char a = s[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

int main() {
    // A d3d12_xs.dll import, as every console title has: its DllMain applies the patches.
    volatile auto flags = D3D12XboxGetProcessDebugFlags();
    (void)flags;
    int fails = 0;
    auto check = [&](const char* what, const volatile char* s, const char* want) {
        bool ok = is(s, want);
        char buf[64];
        size_t i = 0;
        for (; s[i] && i < sizeof buf - 1; i++) buf[i] = s[i];
        buf[i] = 0;
        std::printf("%s %-9s %s\n", ok ? "ok  " : "FAIL", what, buf);
        fails += !ok;
    };
    check("apply", kApply, "xslpatch_apply_patched!");
    check("mismatch", kMismatch, "xslpatch_mismt_original");
    check("disabled", kOff, "xslpatch_disab_original");
    check("rva", kRva, "xslpatch_rvaxx_patched!");
    int a = g_answer();
    std::printf("%s code      answer() = %d\n", a == 7654321 ? "ok  " : "FAIL", a);
    fails += a != 7654321;
    std::printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails;
}
