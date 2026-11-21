// SPDX-License-Identifier: AGPL-3.0-or-later
// Async helpers shared by runtime classes (implemented over libHttpClient's XAsync in rt_task.cpp).
#pragma once
#include <windows.h>
#include <cstddef>
struct XAsyncBlock;
// Complete `async` on its queue with `status` and a copy of `result` (may be null / 0 bytes).
HRESULT xsl_async_immediate(XAsyncBlock* async, const void* identity, const char* name, HRESULT status, const void* result, size_t size);
// Fetch the result of an async started by xsl_async_immediate (wraps XAsyncGetResult).
HRESULT xsl_async_result(XAsyncBlock* async, const void* identity, size_t size, void* buffer, size_t* used);
HRESULT xsl_async_result_size(XAsyncBlock* async, size_t* size);
