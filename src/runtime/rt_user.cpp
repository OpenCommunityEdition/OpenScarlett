// SPDX-License-Identifier: AGPL-3.0-or-later
// XUser: one local, signed-in, offline user (online services stay offline by construction). Token
// requests fail deterministically, so no Xbox services call can succeed or leave the machine.
#include <windows.h>

#include <XGameErr.h>
#include <XUser.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "rt_async.h"
#include "runtime_classes.h"
#include "xsl/log.h"

#define CH "rt"

namespace {
struct LocalUser { std::atomic<long> refs{1}; };
LocalUser g_user;
const uint64_t kXuid = 0x0009000000000001ull;
const uint64_t kLocalId = 1;
XUserHandle handle() { g_user.refs++; return reinterpret_cast<XUserHandle>(&g_user); }
bool ours(XUserHandle u) { return reinterpret_cast<LocalUser*>(u) == &g_user; }
const char* gamertag() {
    static char tag[64] = "";
    if (!tag[0] && !GetEnvironmentVariableA("XSL_GAMERTAG", tag, sizeof tag)) strcpy(tag, "Player One");
    return tag;
}
const char kAddIdentity[] = "XUserAddAsync";
const char kTokenIdentity[] = "XUserGetTokenAndSignatureAsync";

HRESULT M_XUserAddAsync(void*, XUserAddOptions, XAsyncBlock* async) {
    XSL_INFO(CH, "XUserAddAsync: signing in local user '%s' (offline)", gamertag());
    XUserHandle h = handle();
    return xsl_async_immediate(async, kAddIdentity, "XUserAddAsync", S_OK, &h, sizeof h);
}
HRESULT M_XUserAddResult(void*, XAsyncBlock* async, XUserHandle* out) { return xsl_async_result(async, kAddIdentity, sizeof *out, out, nullptr); }
HRESULT M_XUserDuplicateHandle(void*, XUserHandle h, XUserHandle* dup) {
    if (!ours(h)) return E_INVALIDARG;
    *dup = handle();
    return S_OK;
}
void M_XUserCloseHandle(void*, XUserHandle h) { if (ours(h)) g_user.refs--; }
int32_t M_XUserCompare(void*, XUserHandle a, XUserHandle b) { return a == b ? 0 : (a < b ? -1 : 1); }
HRESULT M_XUserGetMaxUsers(void*, uint32_t* n) { *n = 1; return S_OK; }
HRESULT M_XUserGetId(void*, XUserHandle, uint64_t* id) { *id = kXuid; return S_OK; }
HRESULT M_XUserGetLocalId(void*, XUserHandle, XUserLocalId* id) { id->value = kLocalId; return S_OK; }
HRESULT M_XUserFindUserById(void*, uint64_t id, XUserHandle* h) { if (id != kXuid) return E_GAMEUSER_USER_NOT_FOUND; *h = handle(); return S_OK; }
HRESULT M_XUserFindUserByLocalId(void*, XUserLocalId id, XUserHandle* h) {
    if (id.value != kLocalId) return E_GAMEUSER_USER_NOT_FOUND;
    *h = handle();
    return S_OK;
}
// Every input device belongs to the one local user (titles ask which user a controller belongs
// to and treat "none" as the user's controller having been lost).
HRESULT M_XUserFindForDevice(void*, const APP_LOCAL_DEVICE_ID* id, XUserHandle* h) {
    if (!id || !h) return E_INVALIDARG;
    *h = handle();
    return S_OK;
}
// "Pick up a controller": answers at once with the first gamepad, in GameInput.dll's device-id
// scheme ("openscarlett-gin", byte 30 = device type (0 = gamepad), byte 31 = XInput slot).
const char kFindControllerIdentity[] = "XUserFindControllerForUserWithUiAsync";
HRESULT M_XUserFindControllerForUserWithUiAsync(void*, XUserHandle, XAsyncBlock* async) {
    APP_LOCAL_DEVICE_ID id{};
    const char tag[] = "openscarlett-gin";
    memcpy(&id, tag, sizeof tag - 1);
    return xsl_async_immediate(async, kFindControllerIdentity, "XUserFindControllerForUserWithUiAsync", S_OK, &id, sizeof id);
}
HRESULT M_XUserFindControllerForUserWithUiResult(void*, XAsyncBlock* async, APP_LOCAL_DEVICE_ID* id) {
    return xsl_async_result(async, kFindControllerIdentity, sizeof *id, id, nullptr);
}
HRESULT M_XUserGetIsGuest(void*, XUserHandle, bool* g) { *g = false; return S_OK; }
HRESULT M_XUserGetState(void*, XUserHandle, XUserState* s) { *s = XUserState::SignedIn; return S_OK; }
HRESULT M_XUserGetGamertag(void*, XUserHandle, XUserGamertagComponent, size_t size, char* out, size_t* used) {
    size_t n = strlen(gamertag()) + 1;
    if (used) *used = n;
    if (size < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(out, gamertag(), n);
    return S_OK;
}
HRESULT M_XUserGetAgeGroup(void*, XUserHandle, XUserAgeGroup* g) { *g = XUserAgeGroup::Adult; return S_OK; }
HRESULT M_XUserCheckPrivilege(void*, XUserHandle, XUserPrivilegeOptions, XUserPrivilege, bool* has, XUserPrivilegeDenyReason* why) {
    *has = true;
    if (why) *why = XUserPrivilegeDenyReason::None;
    return S_OK;
}
HRESULT M_XUserRegisterForChangeEvent(void*, XTaskQueueHandle, void*, XUserChangeEventCallback*, XTaskQueueRegistrationToken* t) {
    if (t) t->token = 1;
    return S_OK;
}
bool M_XUserUnregisterForChangeEvent(void*, XTaskQueueRegistrationToken, bool) { return true; }
HRESULT M_XUserGetTokenAndSignatureAsync(void*, XUserHandle, XUserGetTokenAndSignatureOptions opts, const char* method, const char* url, size_t,
                                         const XUserGetTokenAndSignatureHttpHeader*, size_t, const void*, XAsyncBlock* async) {
    static volatile long n = 0;
    if (InterlockedIncrement(&n) <= 8)
        XSL_INFO(CH, "XUserGetTokenAndSignatureAsync(%s %s, options %#x): offline, failing deterministically", method ? method : "?",
                 url ? url : "?", (unsigned)opts);
    return xsl_async_immediate(async, kTokenIdentity, "XUserGetTokenAndSignatureAsync", E_GAMEUSER_RESOLVE_USER_ISSUE_REQUIRED, nullptr, 0);
}
bool M_XUserIsStoreUser(void*, XUserHandle) { return true; }
}  // namespace

XSL_RT_REGISTER(XUserAddAsync, &M_XUserAddAsync);
XSL_RT_REGISTER(XUserAddResult, &M_XUserAddResult);
XSL_RT_REGISTER(XUserDuplicateHandle, &M_XUserDuplicateHandle);
XSL_RT_REGISTER(XUserCloseHandle, &M_XUserCloseHandle);
XSL_RT_REGISTER(XUserCompare, &M_XUserCompare);
XSL_RT_REGISTER(XUserGetMaxUsers, &M_XUserGetMaxUsers);
XSL_RT_REGISTER(XUserGetId, &M_XUserGetId);
XSL_RT_REGISTER(XUserGetLocalId, &M_XUserGetLocalId);
XSL_RT_REGISTER(XUserFindUserById, &M_XUserFindUserById);
XSL_RT_REGISTER(XUserFindUserByLocalId, &M_XUserFindUserByLocalId);
XSL_RT_REGISTER(XUserGetIsGuest, &M_XUserGetIsGuest);
XSL_RT_REGISTER(XUserGetState, &M_XUserGetState);
XSL_RT_REGISTER(XUserGetGamertag, &M_XUserGetGamertag);
XSL_RT_REGISTER(XUserGetAgeGroup, &M_XUserGetAgeGroup);
XSL_RT_REGISTER(XUserCheckPrivilege, &M_XUserCheckPrivilege);
XSL_RT_REGISTER(XUserRegisterForChangeEvent, &M_XUserRegisterForChangeEvent);
XSL_RT_REGISTER(XUserUnregisterForChangeEvent, &M_XUserUnregisterForChangeEvent);
XSL_RT_REGISTER(XUserGetTokenAndSignatureAsync, &M_XUserGetTokenAndSignatureAsync);
XSL_RT_REGISTER(XUserFindForDevice, &M_XUserFindForDevice);
XSL_RT_REGISTER(XUserFindControllerForUserWithUiAsync, &M_XUserFindControllerForUserWithUiAsync);
XSL_RT_REGISTER(XUserFindControllerForUserWithUiResult, &M_XUserFindControllerForUserWithUiResult);
XSL_RT_REGISTER(XUserIsStoreUser, &M_XUserIsStoreUser);
