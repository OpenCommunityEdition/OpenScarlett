// OpenScarlett: minimal precompiled-header replacement for building libHttpClient's task queue
// (Source/Task) on its own. The upstream pch_common.h pulls in the HTTP stack.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(args) (void)(args);
#endif
#define UNREFERENCED_LOCAL(args) (void)(args);
#ifndef ARRAYSIZE
#define ARRAYSIZE(x) sizeof(x) / sizeof(x[0])
#endif
// Upstream traces through its HTTP logging; the layer drops those messages.
#define HC_TRACE_ERROR(area, ...) ((void)0)
#define HC_TRACE_ERROR_HR(area, hr, ...) ((void)(hr))
#include "ResultMacros.h"
#include <XTaskQueue.h>
#include <XAsync.h>
#include <XAsyncProvider.h>
#ifndef ASSERT
#define ASSERT(x) assert(x)
#endif
// Definitions the task queue takes from libHttpClient's pch_common.h / pal.h, restated minimally.
#ifndef E_NOT_SUPPORTED
#define E_NOT_SUPPORTED _HRESULT_TYPEDEF_(0x80070032L)
#endif
#define E_GAMERUNTIME_INVALID_HANDLE E_HANDLE
#define SYSTEM_HANDLE_DEFINE_HELPERS(a, b)
#define SystemHandleAssert(h)
#define SystemHandleMarkCreated(h)
#define SystemHandleMarkDestroyed(h)
#define USE_UNIQUE_HANDLES() (true)
#define GetCurrentRuntimeIteration() 0
#define ASYNC_LIB_TRACE(result, message) ((void)(result))
using DefaultUnnamedMutex = std::mutex;
using DefaultUnnamedConditionVariable = std::condition_variable;
#define CATCH_RETURN() \
    catch (std::bad_alloc const&) { return E_OUTOFMEMORY; } \
    catch (...) { return E_FAIL; }
#define CATCH_RETURN_WITH(errCode) \
    catch (...) { return errCode; }
