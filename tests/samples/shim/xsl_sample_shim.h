// SPDX-License-Identifier: MIT
#pragma once
// Feasibility-build shim: xwin's Windows SDK/CRT hide these in WINAPI_FAMILY_GAMES.
#define _CRT_USE_WINAPI_FAMILY_DESKTOP_APP
#include <hstring.h>
#include <restrictederrorinfo.h>
extern "C" __declspec(dllimport) BOOL __stdcall RoOriginateError(HRESULT, HSTRING);
extern "C" __declspec(dllimport) BOOL __stdcall RoTransformError(HRESULT, HRESULT, HSTRING);
extern "C" __declspec(dllimport) ULONG __stdcall CoAddRefServerProcess(void);
extern "C" __declspec(dllimport) ULONG __stdcall CoReleaseServerProcess(void);
