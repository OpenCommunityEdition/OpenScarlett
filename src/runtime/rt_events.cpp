// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Event registrations that have no source on the host: invites and activation, streaming,
// broadcasting, capture, store licences, package installs, audio endpoints, network port and
// connectivity changes. Registration succeeds with a unique token and the callback never fires
// (none of these events can happen on an offline host); unregistration succeeds. Titles commonly
// treat a failed registration as fatal (Minecraft Preview aborts on XGameInviteRegisterForEvent).
#include <windows.h>

#include <cstdint>

#include "runtime_classes.h"

namespace {
volatile LONG64 g_next = 0x58534C00;  // tokens are opaque; never 0

HRESULT reg(uint64_t* token) {
    if (!token) return E_POINTER;
    *token = (uint64_t)InterlockedIncrement64(&g_next);
    return S_OK;
}
// (queue|handle, context, callback, token*)
HRESULT M_Reg4(void*, void*, void*, void*, uint64_t* token) { return reg(token); }
// (handle, queue, context, callback, token*)
HRESULT M_Reg5(void*, void*, void*, void*, void*, uint64_t* token) { return reg(token); }
// (token, wait) and (handle, token, wait)
bool M_Unreg2(void*, uint64_t, bool) { return true; }
bool M_Unreg3(void*, void*, uint64_t, bool) { return true; }
}  // namespace

XSL_RT_REGISTER(XGameInviteRegisterForEvent, &M_Reg4);
XSL_RT_REGISTER(XGameInviteUnregisterForEvent, &M_Unreg2);
XSL_RT_REGISTER(XGameInviteRegisterForPendingEvent, &M_Reg4);
XSL_RT_REGISTER(XGameInviteUnregisterForPendingEvent, &M_Unreg2);
XSL_RT_REGISTER(XGameActivationRegisterForEvent, &M_Reg4);
XSL_RT_REGISTER(XGameActivationUnregisterForEvent, &M_Unreg2);
XSL_RT_REGISTER(XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged, &M_Reg4);
XSL_RT_REGISTER(XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged, &M_Unreg2);
XSL_RT_REGISTER(XNetworkingRegisterConnectivityHintChanged, &M_Reg4);
XSL_RT_REGISTER(XNetworkingUnregisterConnectivityHintChanged, &M_Unreg2);
XSL_RT_REGISTER(XGameStreamingRegisterConnectionStateChanged, &M_Reg4);
XSL_RT_REGISTER(XGameStreamingUnregisterConnectionStateChanged, &M_Unreg2);
XSL_RT_REGISTER(XGameStreamingRegisterClientPropertiesChanged, &M_Reg5);
XSL_RT_REGISTER(XGameStreamingUnregisterClientPropertiesChanged, &M_Unreg3);
XSL_RT_REGISTER(XAppBroadcastRegisterIsAppBroadcastingChanged, &M_Reg4);
XSL_RT_REGISTER(XAppBroadcastUnregisterIsAppBroadcastingChanged, &M_Unreg2);
XSL_RT_REGISTER(XAppCaptureRegisterMetadataPurged, &M_Reg4);
XSL_RT_REGISTER(XAppCaptureUnRegisterMetadataPurged, &M_Unreg2);
XSL_RT_REGISTER(XStoreRegisterGameLicenseChanged, &M_Reg5);
XSL_RT_REGISTER(XStoreUnregisterGameLicenseChanged, &M_Unreg3);
XSL_RT_REGISTER(XStoreRegisterPackageLicenseLost, &M_Reg5);
XSL_RT_REGISTER(XStoreUnregisterPackageLicenseLost, &M_Unreg3);
XSL_RT_REGISTER(XUserRegisterForDeviceAssociationChanged, &M_Reg4);
XSL_RT_REGISTER(XUserUnregisterForDeviceAssociationChanged, &M_Unreg2);
XSL_RT_REGISTER(XUserRegisterForDefaultAudioEndpointUtf16Changed, &M_Reg4);
XSL_RT_REGISTER(XUserUnregisterForDefaultAudioEndpointUtf16Changed, &M_Unreg2);
XSL_RT_REGISTER(XPackageRegisterInstallationProgressChanged, &M_Reg4);
XSL_RT_REGISTER(XPackageUnregisterInstallationProgressChanged, &M_Unreg3);
XSL_RT_REGISTER(XPackageRegisterPackageInstalled, &M_Reg4);
XSL_RT_REGISTER(XPackageUnregisterPackageInstalled, &M_Unreg2);
