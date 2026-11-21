// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <windows.h>
namespace xsl::platform {
// Replace every import of `func` from any DLL whose name starts with `dllPrefix`
// (case-insensitive) in `module`. Returns the number of slots patched.
int patch_import(HMODULE module, const char* dllPrefix, const char* func, void* replacement);
// Install all interposers into the process's main executable. Idempotent.
void install_into_title();
void install_windowing(HMODULE exe);
// Apply the user's patches.txt next to the executable (docs/patches.md). Called once, before
// the title's entry point.
void apply_user_patches();
// Redirect title loads of xgameruntime.dll to the layer's copy and answer the ConsoleMode registry
// read from XSL_CONSOLE_MODE (host_windows.cpp).
void install_host_redirects();
// XSL_NET_TRACE=1: log the module's socket calls (net_trace.cpp).
void install_net_trace(HMODULE module);
}
