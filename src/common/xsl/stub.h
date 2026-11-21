// SPDX-License-Identifier: AGPL-3.0-or-later
// Hook used by generated function-table stubs: an unimplemented entry logs its
// own name (with a per-entry counter) and returns zero.
#pragma once
#include <cstdint>
using XSL_STUB_RET = uintptr_t;
XSL_STUB_RET xsl_stub_hit(int id, const char* name, void* self);
// Result of unimplemented HRESULT entries: E_NOTIMPL, or S_OK with XSL_STUB_HRESULT=ok (the old
// behaviour, for comparisons).
long xsl_stub_hresult();
