// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The console's display size, as titles see it. A console title's window always has the
// console's output resolution as its client area, and titles size their display planes from it
// while rendering at their own fixed resolution. A PC window's client area is whatever the
// desktop leaves (1904x1041 on a 1080p screen with a title bar and taskbar), and a plane of that
// size next to 1920x1080 render targets leads to copies and resolves PC D3D12 rejects. So the
// title is told the console size and the swap chain stretches it onto the real window.
//
// XSL_DISPLAY_SIZE=<w>x<h> chooses another size; XSL_DISPLAY_SIZE=0 reports the real client area.
#pragma once
#include <windows.h>

#include <cstdlib>

namespace xsl {
inline bool display_size(LONG* w, LONG* h) {
    static LONG sw = -1, sh = -1;
    if (sw < 0) {
        char b[32];
        LONG cw = 1920, ch = 1080;
        if (GetEnvironmentVariableA("XSL_DISPLAY_SIZE", b, sizeof b)) {
            char* x = nullptr;
            long a = strtol(b, &x, 10);
            long c = (x && (*x == 'x' || *x == 'X')) ? strtol(x + 1, nullptr, 10) : 0;
            cw = a > 0 && c > 0 ? a : 0;
            ch = a > 0 && c > 0 ? c : 0;
        }
        sh = ch;
        sw = cw;
    }
    *w = sw;
    *h = sh;
    return sw > 0;
}
}  // namespace xsl
