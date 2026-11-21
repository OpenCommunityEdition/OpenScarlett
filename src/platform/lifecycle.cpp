// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Process lifetime management (PLM) notifications. On the console these deliver
// suspend/resume and constrained/unconstrained transitions. The layer never suspends a
// title, so registration succeeds and no transition
// is delivered; handles are tracked so Unregister matches.
#include <windows.h>

#include <atomic>

#include "xsl/log.h"
#include "xsl_platform.h"

namespace {
typedef void(CALLBACK* PAPPSTATE_CHANGE_ROUTINE)(BOOLEAN quiesced, PVOID context);
typedef void(CALLBACK* PAPPCONSTRAIN_CHANGE_ROUTINE)(BOOLEAN constrained, PVOID context);
struct Registration { void* routine; void* context; };

ULONG WINAPI Xsl_RegisterAppStateChangeNotification(PAPPSTATE_CHANGE_ROUTINE r, PVOID ctx, void** reg) {
    if (!reg) return ERROR_INVALID_PARAMETER;
    *reg = new Registration{(void*)r, ctx};
    XSL_INFO("platform", "RegisterAppStateChangeNotification (no suspend will be delivered)");
    return ERROR_SUCCESS;
}
void WINAPI Xsl_UnregisterAppStateChangeNotification(void* reg) { delete (Registration*)reg; }
ULONG WINAPI Xsl_RegisterAppConstrainedChangeNotification(PAPPCONSTRAIN_CHANGE_ROUTINE r, PVOID ctx, void** reg) {
    if (!reg) return ERROR_INVALID_PARAMETER;
    *reg = new Registration{(void*)r, ctx};
    return ERROR_SUCCESS;
}
void WINAPI Xsl_UnregisterAppConstrainedChangeNotification(void* reg) { delete (Registration*)reg; }
}  // namespace

namespace xsl::platform {
void install_into_title() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    HMODULE exe = GetModuleHandleW(nullptr);
    struct { const char* fn; void* p; } table[] = {
        {"RegisterAppStateChangeNotification", (void*)Xsl_RegisterAppStateChangeNotification},
        {"UnregisterAppStateChangeNotification", (void*)Xsl_UnregisterAppStateChangeNotification},
        {"RegisterAppConstrainedChangeNotification", (void*)Xsl_RegisterAppConstrainedChangeNotification},
        {"UnregisterAppConstrainedChangeNotification", (void*)Xsl_UnregisterAppConstrainedChangeNotification},
    };
    for (auto& e : table) patch_import(exe, "api-ms-win-core-psm-appnotify", e.fn, e.p);
    install_windowing(exe);
    install_host_redirects();
    install_net_trace(exe);
    apply_user_patches();
}
}  // namespace xsl::platform
